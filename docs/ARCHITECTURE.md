# 架构说明

## 分层与依赖方向

```
┌──────────────┐   ┌──────────────┐   ┌────────────────────────┐
│  ui (Qt)     │   │  cli         │   │  web 壳（未来）        │
│  只画界面    │   │  批处理/调试  │   │  HTTP 服务 / WebView   │
└──────┬───────┘   └──────┬───────┘   └──────────┬─────────────┘
       │                  │                      │
       │  C++ 直接调用     │                      │  C ABI (pdfword.dll)
       └────────┬─────────┘                      │
                ▼                                ▼
        ┌───────────────────────────────────────────────┐
        │                 core（纯 C++17）              │
        │  Converter  Converter  JobQueue  DocxWriter   │
        │  IPdfBackend(抽象)  OfficeRunner  Config      │
        └───────┬───────────────────────┬───────────────┘
                │                       │
        ┌───────▼────────┐      ┌───────▼──────────┐
        │ MuPDF 后端     │      │ LibreOffice      │
        │ 渲染/文本/合并 │      │ headless 子进程  │
        └────────────────┘      └──────────────────┘
```

依赖只能从上往下走。`core` 里出现 `#include <Q...>` 就是违规——这是整个方案能换 web 壳的前提。

## 关键决策与取舍

### 1. PDF 能力为什么做成 `IPdfBackend` 抽象而不是直接调 MuPDF

- 没 SDK 时能编译、能跑 UI（返回 `BackendMissing` 而不是崩溃），团队其他人不用先装依赖。
- 以后想换 pdfium（渲染更快）或加密库，加一个 backend 实现即可，Converter/UI/web 壳都不用动。
- 代价：多一层虚函数和一次 `shared_ptr` 间接，可以忽略。

### 2. docx → pdf 为什么必须外挂 LibreOffice

纯 C++ 做 Word 排版引擎（分页、表格、断行、字体度量）是另一个量级的工程。LibreOffice headless
是唯一低成本且版式可接受的路径。三个必须踩的坑，代码里已处理：

- **隔离 profile**：`-env:UserInstallation=file:///.../lo-profile`。否则会和用户已打开的
  LibreOffice 抢锁，或者首次启动弹"欢迎/恢复"向导卡住。
- **超时杀进程树**：soffice.exe 会再拉起 soffice.bin，`TerminateProcess` 只杀外壳。
  用 JobObject + `JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE` 整棵树一起收。
- **产物校验**：退出码 0 不等于转换成功（过滤器不认的格式也会返回 0）。所以按
  `<stem>.<目标扩展名>` 检查文件是否真出现，再回读 stdout 报错。

### 3. pdf → docx 为什么用自研 zip + OOXML，而不是引 zlib/libzip

OOXML 只要求合法 zip 容器。`docx` 里我们用 store（不压缩）方式写，零第三方依赖，
换来的是体积大一点（纯文本可以忽略）。成本：约 150 行 zip writer + 120 行 OOXML 生成，
已经用 python-docx 反向解析验证过（Heading 样式、`& < >` 转义、分页符都正确）。

**明确的局限**：这条路只搬运文字、按行重组段落，**版式一定丢失**（表格变段落、图片丢失、
段落缩进不做）。要保版式必须走"PDF → 图片 → OCR/版式分析"或者商业 SDK，不在本工程范围。

### 3.5 页面操作为什么全部收敛成 `assemble(std::vector<PageRef>)`

删除、重排、旋转、裁剪、插入空白页、插入其他 PDF 页、合并、拆分、抽取，看起来是七八个功能，
本质都是同一件事：**输出 PDF 的每一页从哪来、带什么变换**。所以只有一个后端原语：

```cpp
struct PageRef {
    fs::path source;      // 空 = 空白页
    int pageIndex;        // 0 基
    int rotateDelta;      // 叠加到页面已有 /Rotate 上
    bool hasCrop; double cropLeft, cropBottom, cropRight, cropTop;   // 从四边裁掉多少 pt
    double blankWidthPt, blankHeightPt;
};
```

`assemble()` 的做法是源文件按路径缓存只打开一次 → **每个源文件建一个 `pdf_graft_map`**，
逐条 `pdf_graft_mapped_page(map, -1, src, idx)` 追加到目标文档末尾 →
在目标页对象上写 `/Rotate` 与 `/CropBox` → 一次保存（`do_garbage>=3`）。好处：

- **一次实现覆盖九种功能**：CLI、C ABI、界面三处只是构造不同的 `PageRef` 表。
- **不重绘、不蒸馏**：graft 直接搬页对象，原页像素、字体、透明蒙版全部保持原样（水印那套坑不会再踩）。
- **源文件只解析一次**：整本 800 页重排也只开一次文档，不会每页重开。
- **源页资源只拷一份**：走 graft map 而不是裸 `pdf_graft_page`。后者每 graft 一页就把该页用到的
  字体、图片各拷一份到目标文档；82 页文档重排会从 4.63 MB 涨到 5 MB 以上，删页输出比源文件还大
  （实测 60 页 5.37 MB）。graft map 让同一个源文件的所有页共享同一份资源副本，配合
  `pdf_write_options::do_garbage = 3`（`1`=gc、`2`=重编号、`3`=去重复，写 `2` 体积不会有任何变化）
  再走一遍去重，实测 82 页裁剪 7.84 MB → 4.87 MB、60 页删页 5.37 MB → 3.34 MB、9 页旋转
  931 KB → 172 KB。提取/拆分（`writeGraftedPages`）、合并（`writeGraftedRange`）、水印
  （`writeWatermarked`）四条写入路径都按这个写法统一了，水印 82 页也从 8.39 MB 降到 4.70 MB。

图片转 PDF 走的也是同一个思路：`pdf_add_image` 把图片作为 XObject 直接嵌入，内容流只写
`q <w> 0 0 <h> <x> <y> cm /Im0 Do Q`，不经过重采样，像素无损。

加密 / 解密 / 压缩不需要装配，统一走 `resaveWithOptions()`：打开 → 改 `pdf_write_options`
（`do_encrypt` / `permissions` / `upwd_utf8` / `opwd_utf8`，压缩额外 `pdf_subset_fonts` +
`do_garbage=3`）→ 另存。

### 4. 为什么单独做一个 C ABI 层
web 壳可能用 Rust/Go/C#/Python，也可能只是一个本地 HTTP 服务进程。暴露 C++ 类会带来
ABI 不稳定（STL 版本、异常、内存归属）三大问题。C ABI 这层把这些问题按死：

- 只传 `const char*` / POD，字符串 UTF-8；
- 返回值统一是 `int`（`pw::ErrorCode` 的数值），0 表示成功；
- 调用方拿到需要释放的内存，统一用 `pw_free()`；
- 每个线程各自 `pw_context_create()`，不做全局状态。

### 5. 线程模型

- `JobQueue` 单 worker 串行执行。为什么串行：LibreOffice 本身就倾向单实例复用 profile，
  并发跑多个 headless 会互相抢 profile（或需要多个 profile 目录，内存吃不消）。
  真要并发，按 context 拆多份 `workDir/lo-profile-N` 再开多 worker。
- UI 侧不在 worker 线程碰控件，一律 `QMetaObject::invokeMethod(..., QueuedConnection)` 回主线程。
- 取消语义：只有排队中的任务能真取消；已进入 LibreOffice 的任务会跑完（骨架阶段不强杀）。

## 错误模型

```cpp
enum class ErrorCode : int { Ok, InvalidArgument, IoError, Unsupported,
                             BackendMissing, ProcessFailed, Timeout, Cancelled, Internal };
```

`BackendMissing` 与 `IoError` 必须区分开：前者是"环境没配好"（提示用户去装依赖/填 ini），
后者是"文件本身有问题"。UI 和 web 壳的错误提示直接依赖这个区分。

## web 壳怎么接（三条路，按需选）

1. **本地 HTTP 服务**（最稳）：写一个几十行的 C++/Rust 服务进程，`dlopen`/链接 `pdfword.dll`，
   暴露 `POST /convert`、`GET /pdf/{id}/pages/{n}.png`、`POST /merge`。前端只吃 HTTP。
   需要补的是：任务持久化、文件生命周期回收、鉴权、并发上限。
2. **WebView 桥**：Qt WebEngine / 系统 WebView 里，用 `QWebChannel` 或 URL scheme handler
   把 `converter` 的调用桥接给 JS。适合"还是本地桌面应用，只是界面改成 HTML"。
3. **FFI 直连**：Go(cgo)/C#(PInvoke)/Python(ctypes) 直接调 `pdfword.dll`。

三条路都只需要 `capi/pdfword_c.h`，不需要知道 core 里长什么样。

## 下一步（按优先级）

1. 接 MuPDF：`-DPDFWORD_WITH_MUPDF=ON`，按编译报错对齐 SDK 版本（详见 DEPENDENCIES.md）。
2. 接入了 MuPDF 后要补真机验证：大页面渲染内存、加密 PDF、扫描版 PDF 的文字提取质量。
3. UI：拖拽入队、批量输出目录选择、页缩略图侧栏、水印/合并/拆分的图形入口。
4. `pdf → docx` 增强：按 pageText 的行间距做简单段落合并（现在是一行一段）。
5. 打包：Inno Setup + windeployqt，LibreOffice 走便携目录随包分发的可行性验证（许可证需确认）。
