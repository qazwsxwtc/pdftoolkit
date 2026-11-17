# 架构说明（产品名 Pdf-Toolkit，工程标识沿用 pdfword）

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

图片水印（`writeImageWatermarked`）复用这条经验，但多一层页面嵌套：先把水印图片在**输出文档**里
建一份 image XObject（`pdf_add_image`），每页只造一个 Form XObject，内容是若干条
`q /GS0 gs <w> 0 0 <h> <x> <y> cm /PwImg Do Q`，Form 自带 `/Resources`（`/XObject` 指那份图片、
`/ExtGState` 给整体透明度 `/ca`），再按 `q ... Q` 包住原内容追加到 `/Contents`。
这样平铺 30 格也只是 30 条 `Do`，整份文档只有 1 个 `/Image` + 1 个 `/SMask`
（9 页样例 194,670 B）。第一版拿 `pdf_new_pdf_device` + `fz_fill_image` 逐格画，
pdf device 不去重，同一页每格都嵌一份像素，输出直接到 5.48 MB（648 个 Image 对象），已改掉。
坐标按「显示方向」算（`dispbox`），再用 `fz_invert_matrix(pageCtm)` 折回页面用户空间写 `cm`，
所以带 `/Rotate` 的页面也能正着铺。

文字水印（`writeWatermarked`）在同一套骨架上多了"位置"这一维：`computeTextSpots()` 在显示空间里
算出每条水印的基线起点和旋转角（九宫格 9 档、居中斜放、水平铺满、45° 铺满，共 12 种，
未知取值退回 `top-left`），再把 spot 逐条塞进 fz_text 画到 device 里。

**45° 两档必须走位图**：MuPDF 渲染"文字矩阵里带旋转"的内容流时会把字号放大 2–3 倍
（单条 45° 水印像素红覆盖 27%、平铺 85%，而水平居中只有 0.7%）。原因是 pdf device 把旋转写进
`cm`、把 `1 0 0 -1` 写进 `Tm`，渲染端对这两个矩阵组合后的字号估计出错——PDF 结构本身是对的
（LibreOffice 渲染同一文件尺寸正常），所以只能绕开：`renderTextStamp()` 用 4 倍超采样把整条
文字渲染成透明底 pixmap（alpha=0、颜色铺白，避免边缘发黑），`pdf_add_image` 建成 image XObject
（整份文档一份，同字号页共用），每页只写若干条
`q /GS0 gs <a> <b> <c> <d> <e> <f> cm /PwTxt Do Q`，旋转由 `cm` 承担。实测覆盖降到
`center-diagonal` 0.6% / `tile-diagonal` 2.9%，与水平摆放同量级。

字号与步长：单条 `max(18, 短边*8%)`，平铺 `max(16, 短边*5.5%)`；
水平铺满 stepX = `1.5*tw + 0.5*size`、stepY = `3.0*size`；45° 铺满 stepU 同 stepX、
stepV = `(tw+size)*0.7071*1.45`，文字中点落在页内 ±2pt 才画，每页硬上限 400 条。
文字宽度用 `fz_bound_text` 探一条再取包围盒，**不要用 `fz_measure_string`**——它对 CJK 字体
拿不到可靠 advance，中文平铺的步长会算错。

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

## OCR 的两层抽象（引擎 + backend 方法）

```
IOcrEngine（ocr.hpp）      <- 只认 RGBA 位图，吐 OcrPageResult（pt 坐标 / y 向下 / UTF-8）
  ├─ WindowsOcrEngine      <- C++/WinRT → Windows.Media.Ocr，离线、语言取系统已装
  └─ (TesseractOcrEngine)  <- 规划中：vcpkg 装好后实现同一接口即可替换
IPdfBackend::ocr()        <- 渲染 → 识别 → 写回文字层 / 导出 txt，与引擎无关
```

- **为什么引擎只吃位图**：OCR 引擎的输入本质是像素。backend 负责渲染（知道 zoom、页面 pt、
  `/Rotate`、`/CropBox`），引擎只负责把像素里的字和框找出来。换引擎、换平台都不用碰 PDF 代码。
- **为什么识别结果是"显示空间 pt 坐标"**：引擎里 `sx = pageWidthPt / imageWidth`，
  `pt = px / zoom`，这样写文字层时不用再关心 DPI。页面 `/Rotate` 由 Form XObject 的 `/Matrix`
  统一吸收，词框不需要各自旋转。
- **为什么文字层走 Form XObject 而不是直接改页面内容流**：页面原有内容常常留着未闭合的 `q`
  （PPT 导出的 PDF 尤其常见），直接往后接会继承残留 CTM。一个 `q` 包原内容、`Q q /PwOcrLayer Do Q`
  画图层，既隔离状态又只给页面加一个 `/XObject` 引用。
- **为什么用 `3 Tr`（不可见）而不是白色字**：白字会盖掉扫描件里的浅色笔迹，且译打印仍可能显形；
  `3 Tr` 是 PDF 规范里的"只参与文本抽取与搜索、不绘制"，渲染像素与源文件完全一致（实测 0 差异）。
- **为什么码值用 glyph index（Identity-H）**：`pdf_add_cid_font` 会生成 `/Subtype/Type0` +
  `/Encoding/Identity_H` 并顺带写 ToUnicode（从字体 cmap 逐 glyph 反查），所以内容流里直接写
  gid 就能让复制/搜索还原成原文，不用自己维护 CMap。代价是显示字体必须真的含这些字形，
  所以优先嵌微软雅黑（`C:/Windows/Fonts/msyh.ttc`），失败才退 MuPDF 内置 CJK 字体。
- **字号与横向缩放怎么定**：词框高当字号、词框宽对自然宽（`Σ fz_advance_glyph`）算 `Tz`（clamp 20–500），
  基线取 `y1 - 0.25 * size`。这样选中高亮基本落在字上，而不是浮在字上方/下方。
- **Tesseract 回退的落点**：`makeOcrEngine()` 里"Windows 可用就用 Windows，否则找 Tesseract，
  都没有才返回空"。上层看到的是同一个 `Status`，UI 的引擎能力标签会直接显示当前命中的引擎。

## 编辑与批注的三条约定

```
edit.hpp（纯数据类型）       <- AnnotSpec / LinkSpec / FormField / FieldSpec / TextHit / ReplaceOptions
IPdfBackend 的 10 个方法      <- annotate / addLink / formFields / fillForm / createForm /
                                  findText / replaceText / replaceImage /
                                  outline / setOutline
PageTransform + 逆变换        <- ctm / inv / mediabox / dispbox，全部换算的入口
```

- **坐标只有一套**：对外（UI、CLI、C ABI、edit.hpp）一律是"页面显示空间 pt"——原点在页面左上角、
  y 向下，跟预览拖框、OCR 词框完全一致。backend 内部用 `rectToUser()`（`inv` 矩阵）换成 PDF 用户空间，
  所以 `/Rotate`、`/CropBox` 都不用上层操心。曾经踩过 `fz_new_stext_page_from_page` 后再乘一次 ctm
  的坑（坐标被翻两次，命中的 y 跑到页面外），现在直接 `fz_rect_from_quad(ch->quad)` 就用。
- **批注分两类**：高亮/下划线/删除线/波浪线**没有 `/Rect`**（只有 `/QuadPoints`），
  `pdf_set_annot_rect` 会直接抛 "Highlight annotations have no Rect property"，必须按类型分支。
  文本框（FreeText）的外观不走字体，而是把文字渲染成一张白底位图当 `/AP`——
  挂 CJK 字体会让 MuPDF 把 19MB 的 OpenType 字体程序写进 PDF（实测 224KB → 19MB）。
- **替换是覆盖式的**：MuPDF 的 `pdf_redact_page` / `pdf_apply_redaction` 在这类文档上删不掉文字
  （Redact 注释确实被识别并被删除，但原文仍能被 search 到，判断文字藏在嵌套 Form 里没被实例化）。
  于是 `replaceText` / `replaceImage` 统一改成"白底矩形覆盖 + 可选重写"：
  先 `1 1 1 rg … re f` 盖掉原区域（左右各 1pt、只向下 2pt），再压新文字或新图。
- **重写文字用两套字体**：拉丁字母走 base14 `/Helv`（`fz_new_base14_font` +
  `pdf_add_simple_font`，字宽真实、不进字库），汉字走 `pdf_add_cjk_font` 的非嵌入字体（一字 1em）。
  只用 CJK 字体时 ASCII 也按全角推进，`BOOK` 会溢出命中框、压到右边 `records` 上（实测过）。
  字号先按框高 0.82 起，再按"实际字宽 / 框宽"折减，最低 4pt。
  代价写在 README 里：**底层旧文字仍在**，需要彻底抹除要等 redact 修好。

## 书签 / 目录的三条约定

```
edit.hpp                  <- OutlineItem{ title, page(1 基), level(0 起) }，正文只有这一种形状
IPdfBackend              <- outline(in, password) / setOutline(in, out, items, password)
树 ↔ 拍平                 <- 存的是一棵 /Outlines，给出去的是带 level 的顺序表
```

- **书签和目录是同一个东西**：PDF 里没有"目录"这个独立概念，两者都是 `/Outlines`
  那棵树。所以对外只有一棵树、一套读写，UI 面板标题写"书签 / 目录"，不拆成两个功能。
  拍平模型只有 `{title, page, level}`：`level` 0 = 顶级，`page` 1 基，写 0 表示
  "没有跳转目标"（纯分组标题），读回来解析不到目标的也记 0。
- **页码不能靠 `uri` 判空**（踩过的坑）：`fz_load_outline` 内部走 `load_outline_sub`，
  它对每个节点执行 `onode->page = fz_resolve_link(ctx, doc, onode->uri, ...)`，
  **`uri` 一定被填成 `#page=N`**，所以按 `cur->uri == nullptr` 判"没有目标"会把整棵树的
  页码全读成 0。现在的做法是统一 `fz_page_number_from_location(ctx, &doc->super, cur->page)`，
  返回 `>= 0` 才 +1 存成 1 基，`-1`（外链、坏目标）直接记 0。
- **写回只能走 outline iterator**：MuPDF 1.26.10 没有 `pdf_document_update_toc`。
  `pdf_new_outline_iterator` 之后先 `while (item(iter) != nullptr) { if (delete(iter) < 0) break; }`
  删光旧树（删完迭代器自动停在可插入位置），再逐项 `insert`。层级移动有两个必须遵守的细节：
  `insert` 之后迭代器处于追加模式（`MOD_AFTER`），**想往下钻必须先 `prev()` 复位再 `down()`**；
  回上一层用 `up()` 再 `next()`。`level` 跳级一律夹成"上一项 + 1"、首项强制 0，
  免得写出一个中间断层的目录。每项的 `uri` 填 `#page=N`，交给 `pdf_new_dest_from_link`
  生成真实 `/Dest`，最后 `pdf_save_document`。
- **代价**：外链书签（`http://…`、`mailto:`）写回后会退化成"没有跳转目标的分组标题"，
  因为统一按 `#page=N` 重建。要保留外链得把 `OutlineItem` 扩成带 uri 的结构，
  目前只有"页内跳转"这一种目标。

三层复用同一份拍平数据，交换格式各按各的：CLI 用 `level<TAB>page<TAB>title` 的文本
（`outline --txt` 出来的文件能直接喂给 `set-outline --from`），C ABI 用 JSON 数组
（`pw_pdf_outline` 出）与 `level:page:title` 字符串（前两个冒号切，标题里可以有冒号），
UI 直接把树 ↔ 顺序表互转（`collectBookmarks()` 递归拍平、`rebuildBookmarkTree()` 用
`chain[k]` 记住每层"最近节点"再挂子项）。

## 编辑窗口为什么不用 exec()

5 个编辑窗口（批注 / 超链接 / 查找替换 / 替换图片 / 表单）都是 `show()` 打开的**非模态**窗口，
`MainWindow` 用一个 `QPointer<QDialog> editDialog_` 管"同一时间只有一个"。

原因是这些窗口天生要"一边看预览一边填"：`QDialog::exec()` 是模态，模态期间 Qt 会把主窗口的
鼠标键盘事件全部拦掉（判据是**模态窗口是否可见**），于是用户在预览里拖框时事件根本到不了
`PdfViewWidget`——表现就是"点了框选没反应、窗口还挡着预览"。改成非模态后主窗口随时可用；
点「在预览里框选」时窗口仍然 `hide()`（否则它自己会盖住预览），拖完框 `finishPickMode()` 再
`show()+raise()` 回来，30 秒不拖由单发 `QTimer` 兜底返回。

## 下一步（按优先级）

1. 接 Tesseract（`vcpkg install tesseract:x64-windows` + `chi_sim.traineddata`），
   实现 `TesseractOcrEngine` 并接进 `makeOcrEngine()` 的回退链。
2. OCR 的进度回调：现在 `IPdfBackend` 的方法都没有进度参数，长文档（80+ 页）只能看忙碌条；
   要按页报进度得给 `ocr()` 加回调或走 JobQueue。
3. OCR 版面还原：现在按行拼平，表格/分栏会乱；要保版式得起"列检测 + 单元格重建"。
4. UI：页缩略图侧栏、批量 OCR（多文件一次提交）、批量输出目录选择。
5. 书签：`OutlineItem` 扩出 `uri` 字段，写回时保留外链与已有书签的颜色/斜体等样式位
   （现在重建只保留标题、页码、层级）。
5. `pdf → docx` 增强：按 pageText 的行间距做简单段落合并（现在是一行一段）。
6. 打包：Inno Setup + windeployqt，LibreOffice 走便携目录随包分发的可行性验证（许可证需确认）。
