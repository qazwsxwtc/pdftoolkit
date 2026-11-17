# PdfWord

C++ + Qt 的 PDF / Word 转换与预览工具。**核心能力全部在被 Qt 隔离的 `core` 里**，
Qt 只负责界面，同一条链路另有 CLI 和 C ABI 两种入口，方便以后直接套 web 壳。

```
pdfword/
├── core/          纯 C++17，无 Qt、无第三方库（MuPDF 可选）
│   ├── include/pdfword/    公共头（result / config / pdf_backend / converter / job ...）
│   ├── src/                实现（LibreOffice 调用、OOXML 生成、PDF 后端）
│   └── capi/               稳定 C ABI（pdfword.dll），web 壳的接入点
├── ui/            Qt 6 Widgets：文件列表 + PDF 预览 + 任务日志（只调 core）
├── cli/           命令行：doctor / convert / info / text / render / merge / split / watermark
├── tests/         冒烟测试（无外部依赖，ctest 可跑）
├── cmake/         FindMuPDF.cmake
├── config/        pdfword.ini 模板（外部依赖路径都在这里注入）
└── docs/          架构说明 + 依赖接入步骤
```

## 构建

```powershell
# 已验证：VS2022 x64 + Qt 6.12.0 msvc2022_64
cmake -S . -B build/msvc-x64 -G "Visual Studio 17 2022" -A x64 `
      -DCMAKE_PREFIX_PATH=D:/Qt/6.12.0/msvc2022_64
cmake --build build/msvc-x64 --config Release --parallel

# 或直接用预设
cmake --preset msvc-x64
cmake --build --preset release
```

产物在 `build/msvc-x64/bin`：`pdfword_ui.exe`、`pdfword_cli.exe`、`pdfword.dll`、`pdfword_tests.exe`。

Qt 运行库：`D:\Qt\6.12.0\msvc2022_64\bin\windeployqt.exe --release build\msvc-x64\bin\pdfword_ui.exe`

### 带 MuPDF 的完整构建（PDF 能力全开）

MuPDF 由 vcpkg 的 `libmupdf` 端口提供，装好后用 `PDFWORD_WITH_MUPDF=ON` 构建：

```powershell
D:\vcpkg\vcpkg.exe install libmupdf:x64-windows

cmake -S . -B build/mupdf -G "Visual Studio 17 2022" -A x64 `
      -DCMAKE_PREFIX_PATH=D:/Qt/6.12.0/msvc2022_64 `
      -DCMAKE_TOOLCHAIN_FILE=D:/vcpkg/scripts/buildsystems/vcpkg.cmake `
      -DPDFWORD_WITH_MUPDF=ON
cmake --build build/mupdf --config Release --parallel
```

GitHub 直连不通时，用 `scripts/vcpkg-mirror.ps1` 把 vcpkg 的下载接到镜像：

```powershell
pwsh -File scripts\vcpkg-mirror.ps1 -Packages libmupdf:x64-windows
```

vcpkg 的 `x64-windows` 是动态库三元组，依赖 DLL（harfbuzz、freetype、jpeg62、openjp2、z、bz2 …）
会在构建时自动拷到 exe 目录，不用手工处理。

## 跑界面：先认准哪个 bin 目录

两个构建目录的产物**同名**，双击错了就会看到“PDF 后端: stub（不可用）”：

| 目录 | CMake 选项 | `pdfword_ui.exe` 体积 | 界面里的 PDF 后端 |
| --- | --- | --- | --- |
| `build\mupdf` | `PDFWORD_WITH_MUPDF=ON` | ~35.7 MB | `mupdf（可用）` |
| `build\msvc-x64` | 默认（OFF） | ~0.3 MB | `stub（不可用）` |

```powershell
pwsh -File scripts\run-ui.ps1              # 直接起带 MuPDF 的界面（35 MB 那个）
pwsh -File scripts\run-ui.ps1 -Build skeleton   # 骨架界面，只能转 docx/pdf
```

体积是最快的判断依据：几十 MB = 静态链入了 MuPDF；几百 KB = 骨架。

界面上的 PDF 工具入口（都在左侧，操作对象是“当前预览的 PDF”，没预览时用列表里第一个 PDF）：

| 按钮 | 作用 |
| --- | --- |
| `合并 PDF` | 把列表里选中的多个 PDF 合成一个（Ctrl 多选） |
| `拆分 PDF` | 按每 N 页切分成多个文件 |
| 页码范围框 + `抽取页面` | 按 `1-3,5` 这类范围抽页另存 |
| `页面编辑…` | 删除 / 上移下移置顶置底重排 / 旋转 / 裁剪 / 插入空白页 / 插入其他 PDF 页，最后 `另存为…` |
| `文档工具…` | 元数据读写、AES-128 加密、解密、压缩、文字水印（文字 + 不透明度）、多张图片合成 PDF |

`页面编辑…` 里那张“输出页顺序”列表就是最终 PDF 的页序列：删页 = 从列表删掉一条，
重排 = 挪动列表项，旋转/裁剪 = 给该条打标记，保存时才真正写文件。按 `Esc` 或 `关闭`
不会改动源文件。

## 当前状态（说清楚哪些是真跑通了，哪些还没）

以下“已验证”均指在 `build\mupdf`（`PDFWORD_WITH_MUPDF=ON`，MuPDF 1.26.10 + LibreOffice 26.8.3.1）
上真机跑过，样例是 `D:\media\lecture_1.pdf`（63 页横版）与 `D:\media\lecture_10.pdf`（82 页竖版）：

| 能力 | 走哪个模块 | 状态 |
| --- | --- | --- |
| 工程编译 / Qt 界面启动 | ui | ✅ 已验证（窗口标题 `PdfWord — PDF/Word 转换与预览`） |
| DOCX 生成（OOXML + zip） | core / DocxWriter | ✅ 已验证（python-docx 能解析：段落样式、转义都对） |
| 配置探测 / 能力报告 | core / Config | ✅ 已验证（`pdfword_cli.exe doctor` 三项都绿） |
| 任务队列与进度回调 | core / JobQueue | ✅ 已验证（UI 日志与进度条走这条通路） |
| docx → pdf | core / OfficeRunner → LibreOffice | ✅ 已验证（82 页 docx → 82 页 PDF，隔离 profile + 超时杀进程树都生效） |
| doc → docx 等互转 | 同上 | ✅ 同一条链路（LibreOffice `--convert-to`） |
| PDF 信息 / 渲染 / 文本 | core / MuPDF 后端 | ✅ 已验证（63 页 842×595pt；渲染 1263×893；中文按 UTF-8 输出） |
| PDF 合并 / 拆分 | core / MuPDF 后端 | ✅ 已验证（graft 搬运页面：20+20 → 40 页；63 页按 20 页切 4 份） |
| 页面级装配：抽取 / 删除 / 重排 / 旋转 / 裁剪 / 插入空白页 / 插入其他 PDF 页 | core / MuPDF 后端 | ✅ 已验证（所有操作归结为一张 `PageRef` 表 → `assemble()`；旋转写 `/Rotate`、裁剪写 `/CropBox`；抽取件原文逐字一致、渲染逐像素 0 差异） |
| 图片转 PDF | core / MuPDF 后端 | ✅ 已验证（`pdf_add_image` + Form XObject，图片原始像素无损嵌入；`--a4` 等比居中） |
| 元数据读 / 写 | core / MuPDF 后端 | ✅ 已验证（trailer `/Info`，空字段即删除；读写往返一致） |
| 加密 / 解密 | core / MuPDF 后端 | ✅ 已验证（AES-128；加密件 `is_encrypted=True`、不带密码报“需要密码”、带密码可解密回原页数） |
| 压缩 | core / MuPDF 后端 | ✅ 已验证（字体子集 + 流压缩 + 垃圾回收：9 页 274 KB → 220 KB，首像素完全一致） |
| PDF 水印 | core / MuPDF 后端 | ✅ 已验证（graft + Form XObject 叠加：82 页全部可加水印，中文水印可提取到文字层，原页渲染像素级不变） |
| pdf → docx（文字重排，版式有损） | MuPDF + DocxWriter | ✅ 已验证（82 页 → 726 个非空段落，中文正常） |
| C ABI 供 web 壳调用 | core / capi | ✅ 已验证（Python `ctypes` 加载 `pdfword.dll`：建 context、取能力报告、错误码与 last_error 都对；新增的 12 个页面/文档级函数也逐个调通） |

已知限制（不是 bug，是当前实现的取舍）：

- **PDF → DOCX 只搬文字，不搬版式**：段落按行重排，图片、表格、分栏不保留。要保版式得走
  `docx → pdf` 方向，或者另外接版面分析。
- **加密 PDF 需要调用方提供密码**：`IPdfBackend::open(path, password)`、`--password`、
  C ABI 的 `pw_set_pdf_password()` 三条入口都支持，且能区分“需要密码”和“密码错误”。
  UI 仍没有密码输入框，界面里打开加密 PDF 会直接提示需要密码。
- **水印会多出一个 CJK 字体子集**：水印要嵌一个字体子集（微软雅黑按实际字形裁剪后约 190 KB），
  所以输出一定比源文件略大，但不会再成倍膨胀：82 页样例源 4.63 MB → 输出 4.70 MB
  （源 9 页样例 190,688 B → 167,623 B，因去重与字体子集化反而略小；有 `/Rotate`/CropBox
  的页也走同一条 graft 路径，只改页面对象不改原页内容）。
  早先逐页 `pdf_graft_page` 的写法会让源页字体/图片被每页各拷一份，输出曾到 8.39 MB，已修复。
- **明确不做的 PDF 功能**（都需要额外引擎，不在 MuPDF + LibreOffice 的能力范围内）：
  OCR 文字识别（要 Tesseract/OCRmyPDF）、批注与文字编辑（要重排版面）、表单填写与生成、
  数字签名与远程签名、PDF 版本对比。页面级操作与文档级操作已全部覆盖。
- **页面编辑器是列表式，不是缩略图拖拽**：`页面编辑…` 对话框里用文本列表 + 上移/下移/置顶/置底
  来重排，不做缩略图网格。缩略图需要额外缓存与线程调度，属于纯界面工作量。

依赖接进来只改 `pdfword.ini`，不用重新编译：见 [docs/DEPENDENCIES.md](docs/DEPENDENCIES.md)。

## CLI 快速验证

下面的 `build\msvc-x64` 是最小骨架构建目录（PDF 后端是 stub，只能转 docx/pdf）；要试 PDF
能力（info / render / merge / split / watermark）把路径换成 `build\mupdf\bin`，命令完全一样。

```powershell
.\build\msvc-x64\bin\pdfword_cli.exe doctor
.\build\msvc-x64\bin\pdfword_cli.exe convert .\a.docx .\a.pdf --to pdf
.\build\msvc-x64\bin\pdfword_cli.exe info .\a.pdf
.\build\msvc-x64\bin\pdfword_cli.exe text .\a.pdf 1
.\build\msvc-x64\bin\pdfword_cli.exe render .\a.pdf 1 --zoom 2.0 --out page1.bmp
.\build\msvc-x64\bin\pdfword_cli.exe merge out.pdf 1.pdf 2.pdf
.\build\msvc-x64\bin\pdfword_cli.exe split big.pdf outdir 2
.\build\msvc-x64\bin\pdfword_cli.exe extract big.pdf picked.pdf "1-3,5,8-10"
.\build\msvc-x64\bin\pdfword_cli.exe pages big.pdf reordered.pdf "3,1,2"
.\build\msvc-x64\bin\pdfword_cli.exe delete big.pdf trimmed.pdf "2,4-6"
.\build\msvc-x64\bin\pdfword_cli.exe rotate in.pdf out.pdf "1,3" 90
.\build\msvc-x64\bin\pdfword_cli.exe crop in.pdf out.pdf "1" 20 20 20 20
.\build\msvc-x64\bin\pdfword_cli.exe insert-blank in.pdf out.pdf 2 1
.\build\msvc-x64\bin\pdfword_cli.exe insert-pdf in.pdf out.pdf 1 other.pdf "1-2"
.\build\msvc-x64\bin\pdfword_cli.exe img2pdf album.pdf a.png b.jpg --a4
.\build\msvc-x64\bin\pdfword_cli.exe meta in.pdf
.\build\msvc-x64\bin\pdfword_cli.exe setmeta in.pdf out.pdf --title "合同" --author "张三"
.\build\msvc-x64\bin\pdfword_cli.exe encrypt in.pdf enc.pdf 888888 owner --allow print,copy
.\build\msvc-x64\bin\pdfword_cli.exe decrypt enc.pdf dec.pdf --password 888888
.\build\msvc-x64\bin\pdfword_cli.exe compress in.pdf small.pdf
.\build\msvc-x64\bin\pdfword_cli.exe watermark in.pdf out.pdf "内部资料" 0.3
.\build\msvc-x64\bin\pdfword_cli.exe info .\enc.pdf --password 888888
.\build\msvc-x64\bin\pdfword_cli.exe convert .\enc.pdf .\enc.docx --to docx --password 888888
```

`--password` 对所有命令通用（info / text / render / convert 都认）；加密文档不开密码会报
“文档已加密，需要密码”，密码不对报“文档已加密，密码错误”。

数值参数（页号、角度、裁剪量、opacity）一律是**位置参数、纯数字**：写 `0.3`，不要写
`--opacity 0.3`。写错时 CLI 会打一行“参数错误: …”并返回退出码 2，不会再让进程直接崩掉
（早期版本会因 `std::stof` 抛异常而 abort）。

## 架构要点

细节见 [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md)。三条铁律：

1. `core` 不 include 任何 Qt 头文件 —— 换 web 壳时界面层整个丢掉，核心零改动。
2. 外部依赖一律走接口（`IPdfBackend`、`OfficeRunner`），路径全部来自 `Config`，代码里不出现硬编码。
3. 跨边界只传 `Status`/`ErrorCode`，不抛异常；C ABI 把错误码映射成整数返回。
