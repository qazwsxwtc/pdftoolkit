# Pdf-Toolkit

**简体中文** | [English](README_EN.md)

C++ + Qt 的 PDF / Word 转换与预览工具。**核心能力全部在被 Qt 隔离的 `core` 里**，
Qt 只负责界面，同一条链路另有 CLI 和 C ABI 两种入口，方便以后直接套 web 壳。

> 产品名是 **Pdf-Toolkit**；工程里的技术标识仍沿用 `pdfword`（target / exe 名
> `pdftoolkit.exe`、`pdfword_cli.exe`、`pdfword.dll`、头文件目录 `pdfword/`、
> CMake 选项 `PDFWORD_*`、配置 `pdfword.ini`、环境变量 `PDFWORD_*`、
> 仓库目录 `D:\media\pdfword`）。改产品名不动这些标识，是为了不给 web 壳的
> C ABI、脚本和既有构建目录制造连锁改动。

```
pdfword/
├── core/          纯 C++17，无 Qt、无第三方库（MuPDF 可选）
│   ├── include/pdfword/    公共头（result / config / pdf_backend / converter / job ...）
│   ├── src/                实现（LibreOffice 调用、OOXML 生成、PDF 后端、OCR 引擎）
│   └── capi/               稳定 C ABI（pdfword.dll），web 壳的接入点
├── ui/            Qt 6 Widgets：可停靠文件面板 + PDF 预览 + 菜单化工具（只调 core）
│   └── resources/           logo（pdftoolkit.png / .ico）+ icons.qrc + pdftoolkit.rc
├── cli/           命令行：doctor / convert / info / text / render / merge / split / watermark / imgwatermark / ocr / outline / set-outline
├── tests/         冒烟测试（无外部依赖，ctest 可跑）
├── cmake/         FindMuPDF.cmake
├── config/        pdfword.ini 模板（外部依赖路径都在这里注入）
└── docs/          架构说明 + 依赖接入步骤
```

> **发布前必读**：`-DPDFWORD_WITH_MUPDF=ON` 会把 **MuPDF（AGPL-3.0 / Artifex 商业双授权）**
> 静态链进 `pdftoolkit.exe` / `pdfword.dll` / `pdfword_cli.exe`。要对外分发这套二进制，
> 就得按 AGPL-3.0 把整个软件的源码一起给出去，或者先找 Artifex 买商业许可。
> 详见文末「[许可与 MuPDF 约束](#许可与-mupdf-约束发布前必读)」。

## 界面语言（中文 / English）

界面自带中文和英文两套文案，**默认跟随系统语言**：Windows 主界面语言是中文就显示中文，
其它语言一律显示 English。也可以在界面里手动指定，并且会被记住。

- **菜单切换**：菜单栏最右一个顶级菜单 `语言(L)` → `Language(L)`，三项互斥单选：
  `跟随系统` / `简体中文` / `English`，当前生效的那一项打勾。
  切换后**立刻整窗换文案**，不用重启进程；文件列表、当前预览文件、当前页、缩放比例都会原样保留。
- **配置文件**：`pdfword.ini` 里的一行 `language=auto|zh|en`（`auto` 就是跟随系统；
  也认 `zh_CN` / `en_US` 这种写法）。ini 的查找顺序：环境变量 `PW_CONFIG` 指向的文件 >
  exe 同目录下的 `pdfword.ini`。菜单切换只会改写这一行，其余配置原样不动。
- **覆盖范围**：不只界面控件。菜单、对话框、按钮、状态栏、日志窗口、书签面板、
  以及 core 层返回的错误/状态消息（"PDF 后端: …"、"页码 N 超出范围（本文件共 M 页）"、
  "_已编辑.pdf" 这类产物名后缀）都在同一张表里，所以 CLI 和 C ABI 的输出也跟着语言走。
- **文案表**：`core/src/i18n_table.cpp`（中文原文 → English，一条 `{zh, en}` 一行）。
  查表以**中文原文为 key**，查不到就回落中文原文，因此漏译只会显示中文，不会空白、不会崩。
  新增文案时把中文字面量用 `pw::i18n::t("…")`（core）或 `TR("…")`（ui）包起来，
  再往表里补一行英文即可。

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

产物在 `build/msvc-x64/bin`：`pdftoolkit.exe`、`pdfword_cli.exe`、`pdfword.dll`、`pdfword_tests.exe`。
（界面 exe 的 target 名是 `pdfword_ui`，产物文件名由 `OUTPUT_NAME` 定成产品名 `pdftoolkit.exe`；
命令行与 DLL 沿用工程标识 `pdfword_cli.exe` / `pdfword.dll`。）

Qt 运行库：`D:\Qt\6.12.0\msvc2022_64\bin\windeployqt.exe --release build\msvc-x64\bin\pdftoolkit.exe`

### 应用图标（logo）

图标是编进二进制里的，分发时不用额外带图片文件：

| 文件 | 作用 |
| --- | --- |
| `ui/resources/pdftoolkit.png` | 256×256 透明底，运行时 `QApplication::setWindowIcon()` / 主窗口 / 关于窗口都用它（`ui/AppIcon.cpp` 里取 `:/icons/pdftoolkit.png`） |
| `ui/resources/pdftoolkit.ico` | 16/24/32/48/64/128/256 七个尺寸，给任务栏、标题栏、Alt-Tab 用 |
| `ui/resources/pdftoolkit.rc` | `IDI_ICON1 ICON "pdftoolkit.ico"`，只编进 `pdftoolkit.exe` —— 这是**资源管理器里看到的文件图标** |
| `ui/resources/icons.qrc` | Qt 资源清单（`set(CMAKE_AUTORCC ON)` 自动编译） |

换 logo 就替换那两张图（保持 256×256 透明底、ico 多尺寸），重新构建即可，代码不用动。
冒烟里有一条断言会检查窗口图标取得到 32×32 的位图，换图换错了会直接 FAIL。

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

### OCR 引擎的编译前提（Windows 内置 OCR）

`core/src/ocr_windows.cpp` 用 C++/WinRT 调 `Windows.Media.Ocr`，编译时 CMake 会自己找 Windows SDK 里的
`cppwinrt\winrt\base.h`：候选根目录是 `%WindowsSdkDir%\Include`、
`%SystemDrive%\Program Files (x86)\Windows Kits\10\Include`、`%SystemDrive%\Program Files\Windows Kits\10\Include`
以及本机这种把 SDK 装在 `D:\Windows Kits\10\Include` 的情况，逐个 glob `10.*` 倒序取最新版本。

- 找到 → 定义 `PDFWORD_HAVE_WINRT_OCR=1`、链 `WindowsApp.lib`，配置阶段打印
  `OCR: 使用 Windows 内置 OCR（cppwinrt=…）`；
- 找不到 → 打印 `OCR: 没找到 Windows SDK 的 cppwinrt 目录，跳过 Windows OCR`，
  `makeOcrEngine()` 返回空，`ocr()` 直接回 `BackendMissing`（其余 PDF 能力不受影响）。

### 路径编码：任何语言的文件名都能打开

整条链路只认 UTF-8，并且从**入口**就开始保证无损 —— 这一步错了，后面怎么修都没用：

- **CLI**：Windows 上 `main(char**)` 拿到的是本地代码页（中文系统 CP936），文件名里只要有该代码页表示不了的字符
  （日文假名、西里尔字母、泰文、emoji…）就会变成 `?`，**不可逆**。所以 CLI 用 `wmain` 直接拿 UTF-16，
  再转 UTF-8；控制台输出也切到 `SetConsoleOutputCP(CP_UTF8)`，否则中文日志在 cmd 里是乱码。
- **Qt**：`QString::toStdString()` 返回的是本地 8 位编码，不是 UTF-8。路径必须写成
  `pw::fsPath(QString.toUtf8().toStdString())`。
- **core**：`std::string` 只当 UTF-8 用；进 `fs::path` 走 `pw::fsPath()`；出到日志/消息走 `pw::fsUtf8()`。
- **C ABI**：`pdfword.dll` 的契约就是“字符串一律 UTF-8”，写在 `core/capi/pdfword_c.h`。
- **MuPDF 1.26**：窄路径接口底层是 `fopen`，按系统 ANSI 解释文件名。所以一律绕开：
  打开 = `fz_open_file_w` → `pdf_open_document_with_stream`（通用文档 `fz_open_document_with_stream` + magic），
  保存 = `_wfopen(L"wb")` + `fz_new_output_with_file_ptr` + `pdf_write_document`（1.26 没有
  `pdf_save_document_to_output`），读图 = `fz_open_file_w` + `fz_read_all` + `fz_new_image_from_buffer`。
  保存后先 `fz_close_output` 再 `fz_drop_output`，否则 MuPDF 会警告 `dropping unclosed output`、尾部可能没落盘。
- **`fstream`**：Windows 上 `std::ofstream(path)` 同样是窄路径，`zip_writer`、`office_runner`、CLI 的文本/图片导出
  都用宽路径打开。

### 超长路径与单段名上限

中文/日文文件名很容易把路径顶过 260 字符（`MAX_PATH`）。所有 IO 走 `pw::fsPathLong()`：
**超过 240 字符自动加 `\\?\` 前缀**（UNC 路径转 `\\?\UNC\`），不依赖系统的 `LongPathsEnabled` 开关。
实测 267 字符的十层中文目录 + 150 字符文件名，读、写、渲染全部正常。

要注意 Windows 自己的另一条线：**单个文件名（一段）最多 255 字符**，这个用 `\\?\` 也突破不了。
超了会明确报 `无法写文件，名称 256 字符，超过 255 或路径非法`，而不是甩一串被截断的乱码路径。
（Emoji 在 UTF-16 下占 2 个码元，最容易顶爆长度，报错里的字符数就是按 UTF-16 计。）

### 特殊符号与 LibreOffice 的 URI

文件名里的空格、`#`、`%`、`&`、`+`、`'`、括号、emoji 都能原样处理，因为：

- 外部进程参数按 MSVCRT 规则加引号（`\` 在引号内紧邻引号或结尾时才转义，所以 `C:\\dir\\` 这种
  以反斜杠结尾的目录参数也不会把引号弄成未闭合）；
- LibreOffice 的 `-env:UserInstallation` 是 `file:///` URL，路径里所有非保留字符都做 **percent-encode**。
  用户名是中文（`C:\\Users\\张三\\…`）、或工作目录带空格 `#` 时，不编码 soffice 会当成非法 URL 直接退出。
  实测日志：`-env:UserInstallation=file:///D:/.../%E4%B8%AD%E6%96%87%20%E5%B7%A5%E4%BD%9C%23%E7%9B%AE%E5%BD%95/lo-profile`。

### 兼容性实测（本机 Windows，中文代码页）

| 用例 | 结果 |
| --- | --- |
| 日文 / 韩文 / 俄文 / 阿拉伯文 / 泰文 / 越南文 文件名：`info` + `extract` + `render` | 21/21 通过 |
| `emoji😀&%#+'()【】文件.pdf` 同三项 | 通过（含 emoji 与全部特殊符号） |
| 267 字符深路径（十层中文目录 + 150 字符名）读 / 写 / 渲染 | 通过 |
| 单段 256 字符输出名 | 明确报错（字符数 256 > 255） |
| 日文 docx → pdf（工作目录含中文、空格、`#`） | 通过，436 KB |
| `emoji😀&%#+'` 名 docx → pdf | 通过，443 KB |
| UI 冒烟（日文名 PDF） | `failures=0`，26 张截图 |

### LibreOffice 定位（docx ⇄ pdf 转换的前提）

界面/CLI 里那句 `LibreOffice: …` 就是探测结果。程序按下面顺序找 `soffice.exe`，第一个能跑通的就用：

1. `pdfword.ini` 里的 `libre_office=<soffice.exe 或 LibreOffice 目录>`（ini 放 **exe 同目录**）；
2. 环境变量 `PW_LIBREOFFICE_PATH`；
3. 便携版：exe 同目录下的 `LibreOffice\program\soffice.exe`，或 exe 同目录的 `soffice.exe`；
4. 注册表 `HKLM` / `HKCU` × 64/32 位视图的
   `SOFTWARE\LibreOffice\UNO\InstallPath` 和 `...\CurrentVersion\App Paths\soffice.exe`；
5. `%ProgramFiles%`、`%ProgramFiles(x86)%`、`%ProgramW6432%` 下的 `LibreOffice\program\soffice.exe`；
6. **C–H 六个盘符 × {`\LibreOffice`、`\Program Files\LibreOffice`、`\Program Files (x86)\LibreOffice`}**；
7. `%ProgramFiles%`、`%ProgramFiles(x86)%`、`%LOCALAPPDATA%\Programs` 下所有 `LibreOffice*` 子目录
   （覆盖 `LibreOffice 7.6` 这类带版本号的目录名）；
8. 最后扫 `PATH` 里的 `soffice.exe` / `soffice.com`。

第 6 条不能省：LibreOffice 允许装到任意盘，本机就装在 `D:\LibreOffice`，
只查 `Program Files` 的旧版本会把它判成“未配置”，于是 docx ⇄ pdf 直接不可用。

没找到时，提示会把 ini 的绝对路径和已经试过的路径一起写出来，照着填就行：

```
LibreOffice: 未配置（docx ⇄ pdf 转换不可用）
  填法：在 D:\media\pdfword\publish\pdfword.ini 里加一行  libre_office=<soffice.exe 或 LibreOffice 目录>
  或设环境变量 PW_LIBREOFFICE_PATH
  已尝试：C:\LibreOffice\program\soffice.exe、C:\Program Files\LibreOffice\program\soffice.exe…等 16 处
```

最小可用的 `pdfword.ini`：

```ini
libre_office=D:\LibreOffice\program\soffice.exe
```

**不装 LibreOffice 也能用**：PDF 预览、拆分、合并、抽取、旋转、裁剪、插页、水印、加解密、压缩、
OCR、书签、批注、就地改字、图片转 PDF 全部走 MuPDF，和 LibreOffice 无关；`pdf → docx` 也是
MuPDF + 自带的 `docx_writer`，同样不需要它。**只有 `docx / doc → pdf` 需要 LibreOffice。**

想彻底关掉探测（纯 PDF 部署，或确认这台机器永远不做文档转换），设一个环境变量即可：

```powershell
setx PW_LIBREOFFICE_DISABLE 1     # 新开的终端/重启程序后生效；删掉该变量即恢复探测
```

此时报告显示“LibreOffice: 已关闭…（PDF 预览等其他功能不受影响）”，并且因为完全不扫盘，
启动探测从 ~440 ms 降到 ~150 ms。点“开始转换 docx→pdf”会给出带 ini 路径的提示，不会卡住。

锚点（已实测）：`pdfword_cli.exe doctor` 在装了 26.8 的机器上输出
`LibreOffice: D:\LibreOffice\program\soffice.exe`；
`pdfword_cli.exe convert 中文名.pdf 中文名.docx --to docx` 与回程 `docx → pdf` 都能出件。

## 跑界面：先认准哪个 bin 目录

两个构建目录的产物**同名**，双击错了就会看到“PDF 后端: stub（不可用）”：

| 目录 | CMake 选项 | `pdftoolkit.exe` 体积 | 界面里的 PDF 后端 |
| --- | --- | --- | --- |
| `build\mupdf` | `PDFWORD_WITH_MUPDF=ON` | ~37.8 MB（37,752,832 B） | `mupdf（可用）` |
| `build\msvc-x64` | 默认（OFF） | ~0.5 MB | `stub（不可用）` |

```powershell
pwsh -File scripts\run-ui.ps1              # 直接起带 MuPDF 的界面（36 MB 那个）
pwsh -File scripts\run-ui.ps1 -Build skeleton   # 骨架界面，只能转 docx/pdf
```

体积是最快的判断依据：几十 MB = 静态链入了 MuPDF；几百 KB = 骨架。

窗口标题是 `Pdf-Toolkit — PDF/Word 转换与预览`。**第一次启动**按主屏可用区域算位置并居中；
本机这种 1536×864（任务栏上方 816 高）的屏幕上，窗口尺寸会自动收缩到 1280×752，
不会露出任务栏或被切成两半。之后启动直接用上次关闭时记住的窗口几何（见下文“视图状态会记住”），
想回到居中就把 `%APPDATA%\Pdf-Toolkit\Pdf-Toolkit.ini` 删掉。
工作目录默认落在 `%LOCALAPPDATA%\Pdf-Toolkit\work`
（旧版本是 `%LOCALAPPDATA%\PdfWord\work`，改名后不自动搬迁，旧目录留着不删）。

> 如果还留着 `publish\pdftoolkit.exe` 那份独立分发副本（带 Qt DLL，可以整个文件夹拷给别人），
> 它不会跟着 `build` 自动更新：改完代码要手动
> `Copy-Item build\mupdf\bin\pdftoolkit.exe publish\pdftoolkit.exe -Force`。
> 覆盖前先把正在跑的旧窗口关掉，否则会报“文件被占用”。

**界面冒烟测试**（不需要屏幕，真实构造主窗口与四个对话框，并跑一次带密码的转换任务）：

```powershell
# 一次性开启测试目标，然后构建并运行
& "C:\Program Files\CMake\bin\cmake.exe" -S "D:\media\pdfword" -B "D:\media\pdfword\build\mupdf" -DPDFWORD_BUILD_UI_SMOKE=ON
& "C:\Program Files\CMake\bin\cmake.exe" --build "D:\media\pdfword\build\mupdf" --config Release --target pdfword_ui_smoke --parallel
.\build\mupdf\bin\pdfword_ui_smoke.exe "C:\Users\Lenovo\Desktop\tst\iw8ben.pdf"
# 再带上密码参数，验证 JobQueue::enqueue 的 password 真到了 ConvertOptions
.\build\mupdf\bin\pdfword_ui_smoke.exe ".\build\mupdf\out-test\enc_888888.pdf" 888888 ".\build\mupdf\out-test\smoke_pw.docx"
```

输出全是 `[ OK ]` 且退出码 0 即通过（**198 项断言 + 27 张截图**）。带密码样本时多 3 项任务断言、
少的 2 项“合法范围”校验会换成 1 项“加密件读不到页数必须拦下”。覆盖点：

- 主窗口 6 个顶级菜单（文件 / 转换 / PDF 工具 / 编辑与批注 / 视图 / 帮助）；窗口标题以 `Pdf-Toolkit` 开头；启动位置居中（与主屏可用区中心差 ≤12px）；
  **顶部没有预览工具栏**（`findChild<QToolBar*>()` 为空，导航已挪进状态栏）；
  十六个入口（开始转换 / 合并 / 拆分 / 抽取 / 页面编辑 ‖ 元数据 / 加密解密 / 压缩 / 水印 / 图片转 PDF / OCR 文字识别 ‖ 修改文字图片 / 注释批注 / 添加超链接 / 查找替换 / 表单处理）
  都在菜单里，且 PDF 工具菜单里不再有“文档工具…”；
  视图菜单是两级：`文件列表`（显示 / 隐藏 ‖ 靠左 / 靠右）、`日志窗口`（显示 / 隐藏）、`书签目录`（显示 / 隐藏）、`缩放`（放大 / 缩小 ‖ 适应宽度 / 适应窗口 / 实际大小）
- **底部导航条**：`pageNav` 的父控件就是 `statusBar()`（和 `0%` 进度条同一个容器），两者中心线差 ≤14px
  且都贴窗口最底；里面有 4 个按钮 + 页码框 + 折叠按钮；页码框范围是 1..总页数，
  `setValue` 到最后一页后 `currentPage` 真的变过去，再填 1 又跳回第一页；
  打开文档后“下一页”“放大”确实是 enabled（不是灰的）
- **折叠按钮**：`navFoldButton` 在最右；点一下后 4 个按钮 + 页码框 + 百分比全部 `!isVisible()`，再点全部回来
- **文件列表显隐**：点“隐藏文件列表”后 `fileDock` `isHidden()`，点“显示文件列表”回来且勾选跟着切
- **日志窗口显隐**：中央是竖直 QSplitter，默认日志在下半区（初始 640:140，可自己拖高度）；
  点“隐藏”后日志 `isHidden()` 且 splitter 里高度归零、预览区铺满，点“显示”后回到原高度
- **连续滚动**：画布里排着全部页（`pageItemCount() == pageCount()`，每页一个 `pageItem` 控件）；
  整叠页面比视口高，`verticalScrollBar()->maximum() > 0`；把滚条拉到最底 `currentPage` 自动变成最后一页，
  拉回顶部又回到第 1 页；只渲染视口附近那几页，远处的页会把位图放掉（80 页文档滚一遍也不吃内存）
- **缩放与滚动**：放大到 300% 后竖直/水平滚动条 `maximum() > 0` 且能拖到页面中部；
  缩到 50% 横向归零；“适应宽度”后横向 `maximum() == 0` 且画布不超视口；
  “适应窗口”后横向不出条、单页整页高度装进视口（后面几页继续往下滚）；向 viewport 投一个
  `Ctrl + wheel` 事件后缩放比例确实变大
- **视图状态持久化**：冒烟把 QSettings 指到 `%TEMP%\pdfword-ui-smoke-settings`（不碰用户真实布局），
  先切靠右 → 收起导航条 → 隐藏日志 → 隐藏文件列表 → 隐藏书签面板，`window.close()` 触发保存，
  再造一个 `MainWindow` 重开：断言日志仍隐藏、文件列表仍隐藏、书签面板仍隐藏、`dockRightAction` 仍勾选、
  `isNavCollapsed()` 仍为真，且 INI 目录确实生成（收尾把书签面板显示回来，免得下次启动少一块面板）
- **拖放**：主窗口 `acceptDrops`；把 PDF 的 `QDropEvent` 直接投给窗口后真的出现在列表里；
  同一文件重复拖入被去重；`.ini` 这类不支持的类型不入列，但日志里有“忽略”提示；
  合并窗口与图片转 PDF 窗口的列表都是 `DropListWidget`（只收各自阵营的扩展名）
- 待处理文件是 QDockWidget，只允许左/右停靠；用菜单选“靠右”后停靠区真的变到右侧，
  选“靠左”再回左侧；列表的 `contextMenuPolicy` 是 `CustomContextMenu`，
  且向列表投一个 `customContextMenuRequested` 后 `QMenu` 真的弹出（测试里定时关掉）
- 合并窗口的文件列表、输出路径、默认输出名
- 抽取窗口的源文件下拉、默认输出名，以及实时校验：`1-2` 算出 2 页；`1-99999` 当场报越界并清缓存；
  加密件（界面读不到页数）填任何范围都被拦下，不会放行一个荒唐页数
- 转换窗口的目标格式 2 项、摘要随 pdf/docx 切换而变化（PDF 跳过 / 非 PDF 不能转 docx）
- 页面编辑器与十一个文档级窗口（元数据 / 加解密 / 压缩 / 水印 / 图片转 PDF / OCR ‖ 批注 / 超链接 / 查找替换 / 替换图片 / 表单）都能被构造，
  各自的代表性控件（标题框、打开密码框、源大小标签、水印文字框、图片列表、OCR 语言下拉）存在
- 水印窗口 `watermarkTabs` 有文字/图片两个页签，图片页有路径框、宽度比例、平铺开关，不透明度两个页签共用；
  文字页签的「位置」下拉正好 12 项、第一项是 `top-left`（左上角）
- OCR 窗口 `ocrLanguageCombo` 至少有「自动」+ 一个真实语言（本机是 `zh-Hans-CN`）；
  `ocrPdfCheck` / `ocrTxtCheck` 两个开关都在且默认勾选；默认输出名带 `_可检索` / `_识别` 后缀；
  `ocrEngineLabel` 显示引擎能力（可用语言或不可用原因），`ocrStartButton` 存在
- 带密码的 PDF→docx 任务经 JobQueue 完成且产物落盘
- **书签 / 目录面板**：`bookmarkDock` 与目录树都在，视图菜单里有「书签目录」的显示 / 隐藏两项；
  隐藏后截一张 `main-window-bookmark-hidden.png` 再显示回来；打开样本时树里的条目数与 core `outline()`
  对账（不写死条数，换个样本也成立）；树里既有顶级也有嵌套子项；**双击条目真的跳到那一页**（页码框跟着变）；
  上移 / 下移换位再复位；删掉一条顶级后点「应用」，另存出 `<原名>_书签.pdf`，
  读回的条数 / 页码 / 层级与面板完全一致

第 5 个参数是截图目录，会在那里把主窗口和各对话框各 `grab()` 成一张 PNG（共 27 张）：

```powershell
.\build\mupdf\bin\pdfword_ui_smoke.exe "C:\Users\Lenovo\Desktop\tst\iw8ben.pdf" "" "" "D:\media\pdfword\build\mupdf\ui-shots"
```

`main-window / main-window-scroll / main-window-right / main-window-log-hidden / main-window-zoom /
main-window-nav-collapsed / main-window-bookmarks / main-window-bookmark-hidden /
merge-dialog / extract-dialog / convert-dialog / page-editor-dialog /
metadata-dialog / security-dialog / compress-dialog / watermark-dialog / watermark-dialog-image /
imgtopdf-dialog / ocr-dialog / annot-dialog / link-dialog / find-replace-dialog / replace-image-dialog /
form-dialog` **二十六张图**可以直接打开看布局，
不需要真实桌面（`main-window-scroll` 是滚到第 1、2 页接缝处的样子——中间那道浅灰缝就是页与页的分界，
`main-window-log-hidden` 是隐藏日志后预览铺满的样子，
`main-window-zoom` 是放大到 300% 后能看到右/下滚动条的样子，
`main-window-nav-collapsed` 是底部导航条收起后只剩“◂ 展开”一颗按钮的样子，
`main-window-bookmarks` 是右侧「书签 / 目录」面板展开的样子（两列、多级缩进、三行按钮），
`main-window-bookmark-hidden` 是视图菜单里点“隐藏书签目录”之后的样子——面板没了，预览区变宽，
`main-window-inline-edit` 是在预览里就地改完一块字之后的样子：预览已经切到 `<原名>_已编辑.pdf`，
还停在原来那一页，日志写着“已在预览里改好这块文字（第 2 页）”）
（`QWidget::grab()` 自己渲染，绕开了本机 CU 截图拿不到窗口的问题）。`watermark-dialog*`
两张分别是文字页签和图片页签，改界面时用来对照。

界面没有常驻的“开始转换/合并/拆分”按钮条了，全部收进菜单栏；“待处理文件”是一个可停靠面板。

**待处理文件列表的右键菜单**（`Qt::CustomContextMenu`，右键点在未选中的行上会先选中它）：

| 项 | 行为 |
| --- | --- |
| 预览 | 仅对 PDF 可用，效果同双击 |
| 删除 | 从列表移除选中的条目（可多选）——**只动列表，磁盘上的文件不动**，日志会写明删了几个 |
| 清空 | 清空整个待处理列表——同样**不动磁盘文件** |
| 重命名文件… | 改磁盘上的真实文件名（仅单选时可用）。新名字里 `\ / : * ? " < > \|` 与结尾空格/句点会被本地拦下；目标已存在则拒绝；成功后列表文本/`Qt::UserRole`/tooltip 一起更新，若该文件正在预览就自动重开新路径 |

**菜单结构**

| 菜单 | 菜单项 | 作用 |
| --- | --- | --- |
| 文件(&F) | 添加文件… / 移除选中 / 清空列表 / 退出 | 往待处理列表里加文件（PDF、docx、doc、odt、rtf、txt、xlsx、pptx） |
| 转换(&C) | 开始转换…（`Ctrl+Enter`） | 先弹**目标格式对话框**：文件清单 + `pdf` / `docx` 选择 + 输出目录（留空=源同目录）+ PDF 密码，确认后才入队 |
| PDF 工具(&P) | 合并 PDF… / 拆分 PDF… / 抽取页面… / 页面编辑… ‖ 元数据… / 加密 / 解密… / 压缩… / 水印… / 图片转 PDF… / OCR 文字识别…（前四项是页面级操作，分线后六项是文档级操作，每个都开独立窗口） | 见下表 |
| 编辑与批注(&E) | 修改文字、图片… / 注释批注… / 添加超链接… / 查找替换… / 表单处理… | 见下节「编辑与批注菜单项」 |
| 视图(&V) | **两级子菜单**：`文件列表 ▸`（显示文件列表 / 隐藏文件列表 ‖ 靠左 / 靠右）+ `日志窗口 ▸`（显示 / 隐藏）+ `书签目录 ▸`（显示书签目录 / 隐藏书签目录）+ `缩放 ▸`（放大一档 / 缩小一档 ‖ 适应宽度 / 适应窗口 / 实际大小 100%） | 文件面板显隐与停靠位置（也可直接把面板拖成浮动窗口）；日志窗口显隐；书签面板显隐；缩放与适应 |
| 帮助(&H) | 能力报告 | 弹出后端 / LibreOffice / OCR 诊断（LibreOffice 版本取自缓存，还没转换过就写"未检测"） |
| 帮助(&H) | 关于 Pdf-Toolkit… | 简介 + 版本号（取自 `project(Pdf-Toolkit VERSION …)`，现为 0.0.2）+ 联系人 / 邮箱（邮箱可点，走默认邮件客户端） |

**PDF 工具菜单项**

| 菜单项 | 作用 |
| --- | --- |
| `合并 PDF…` | 弹**独立合并窗口**：列表顺序即拼接顺序（可 添加 PDF… / 移除选中 / 上移 / 下移），输出路径 + 浏览，OK 前强制校验 ≥2 个文件；默认输出名 `<首文件名>_等N个_合并.pdf` |
| `拆分 PDF…` | 选源 PDF → 填每文件页数 → 选输出目录，按 `文件名_p1-3.pdf` 命名 |
| `抽取页面…` | 弹**抽取窗口**：源 PDF 下拉（可用列表里的 PDF，也可浏览新文件）+ 页码范围（`1-3,5`，中文逗号也认）+ 「全部页」按钮；实时显示“共 N 页，将抽取 M 页（1-3,9）”并实时校验越界 |
| `页面编辑…` | 删除 / 上移下移置顶置底重排 / 旋转 / 裁剪 / 插入空白页 / 插入其他 PDF 页，最后 `另存为…` |
| `元数据…` | 单独窗口：标题 / 作者 / 主题 / 关键词，「读取当前」自动带出原值，「写入并另存为…」输出 `_元数据.pdf` |
| `加密 / 解密…` | 单独窗口：AES-128 加密（打开密码 + 所有者密码 + 允许打印/复制/修改/批注四个权限位）；解密读“打开密码”字段，另存 `_解密.pdf` |
| `压缩…` | 单独窗口：显示源文件大小，一键「压缩并另存为…」（字体子集化 + 流压缩 + 垃圾回收），完成后在同一窗口显示 before → after |
| `水印…` | 单独窗口，**文字 / 图片两个页签**，不透明度共用（0.02–1.00，文字默认 0.15）：文字页填内容（默认「内部资料」）+ **位置下拉（12 种，默认左上角）**；图片页选 PNG/JPG/BMP/TIFF + 宽度占页宽比例（0.05–1.00，默认 0.30）+ 「网格平铺」（间距 = 水印宽的 1.6 倍）。「加水印并另存为…」输出 `_水印.pdf` |
| `图片转 PDF…` | 单独窗口：多张图片列表（顺序即页序）+ 添加/移除 + 「等比缩放放进 A4」，合成一个 PDF |
| `OCR 文字识别…` | 单独窗口：顶部列出引擎能力（本机 `Windows OCR（可用）：语言 zh-Hans-CN`）+ **识别语言下拉**（自动 / 引擎已装语言，有中文时默认选中文）+ 两个开关「输出可检索 PDF（不可见文字层）」`_可检索.pdf`、「导出纯文本（.txt，UTF-8）」`_识别.txt`，路径都能浏览。点「开始识别」后跑在线程池里（进度条忙碌态），期间窗口不许关 |

**编辑与批注菜单项**

| 菜单项 | 作用 |
| --- | --- |
| `修改文字、图片…` | 先问「增删改文字 / 替换图片」：文字走查找替换窗口，图片走替换图片窗口（白底盖掉原图后等比铺新图） |
| `注释批注…` | 7 种批注（高亮 / 下划线 / 删除线 / 波浪线 / 便签 / 文本框 / 手绘），可设颜色、不透明度、作者；文本框与便签填内容 |
| `添加超链接…` | 页内跳转（`第3页` / `P3` / `3` 自动规整成 `#page=N`）或外链网址（`https://…` / `mailto:…`） |
| `查找替换…` | 先「查找」看命中分布，再批量替换；可只删除、区分大小写、限条数、改新字颜色与字号 |
| `表单处理…` | 三个页签：填写（读字段 → 改「要填成的值」→ 另存）、制作（字段名/类型/页/矩形/候选项 → 生成可交互表单）、提取（表格 + 导出 CSV，UTF-8 BOM 可直接用 Excel 打开） |

五个窗口全是**非模态**的（`show()` 打开，不是 `exec()`）：弹着的时候主窗口照常能点、预览照常能滚，
不会被"模态阻塞"把输入吞掉（这正是之前"框选点了没反应"的根子）。同一时间只留一个编辑窗口。

四个带位置的窗口（批注 / 超链接 / 替换图片 / 制作表单）都有「在预览里框选位置」按钮：
点一下，正在编辑的那个窗口会先**自动收起来**（它本身是模态窗口，留着就把预览挡住了），
主窗口随即可以操作；预览进入框选模式（十字光标），按住左键拖一个框，松手后页码与四个坐标
自动填进窗口，窗口自己弹回来。30 秒不拖框会自动返回，不会把窗口卡在隐藏状态；
框选前会核对「预览里打开的是不是正在编辑的这个文件」，不一致直接提示、不给拖，免得坐标落到别的页面上。
想精确指定坐标时也可以完全不框选，直接在四个坐标框里输入（显示空间 pt，左上原点、y 向下）。

**在预览里直接改**（不用开窗口、不用填坐标：在页面上右键选一项，拖个框就完事）

| 右键菜单项 | 拖框之后 | 打哪里 |
| --- | --- | --- |
| `高亮` / `下划线` / `删除线` / `波浪线` | 直接标上，不打扰 | `annotate` |
| `手绘标记` | 沿框画一笔 | `annotate`（ink） |
| `文本框注释…` / `便签…` | 弹一行输入，写字（**这是批注，不是正文**） | `annotate` |
| `超链接…` | 读「地址」字段（`第3页` / `https://…`） | `addLink` |
| `改这里的文字…` | 输入框**默认带出框里的原文**，改完回车写回 | `replaceTextAt` |
| `删掉这里的文字（拖框）` | 白底盖掉这一块，不写字 | `replaceTextAt`（`deleteOnly`） |
| `在这里加文字…（拖框后打字）` | 直接打字插进正文（中文、空格、数字都行） | `addTextAt` |
| `换这里的图片…` | 选图，等比铺进框里 | `replaceImage` |
| `加书签（记到这一页）` | 往「书签 / 目录」面板里加一条 | `setOutline` |

- 拖框是**页面显示空间**的矩形（`PdfViewWidget` 按当前缩放换算成 pt），所以 150% 缩放、滚到第 9 页都不会错位；
- 写回是**按矩形**，不是按文字全篇搜索：同一句话在文档里出现好几次时，只动你框住的那一块
  （`textInRect` 取出的文本只用来给输入框当默认值，带换行/断词也不影响写回）；
- 结果落在 `<原名>_已编辑.pdf`（源文件不动），写完自动重载预览、停在原来那一页，
  同时把这个派生件补进待处理列表，后面合并/拆分可以直接选它；已经在改 `_已编辑.pdf` 时，
  这一轮先写到 `_编辑中_<毫秒>.pdf` 再换名盖回去——**预览此刻正打开那个文件**，Windows 上删不掉，
  所以换名前会先把预览放掉（`viewer_->closeDocument()`）；
- 加文字的框高就是字号（`fontSize=0` 时按框高 0.82 倍算），字比框宽会缩字号，不横向压扁；
- **代价（按框改字的固有代价，别当 bug）**：白底只是"视觉覆盖"，底层旧文字还在 PDF 里，
  `find` / 复制粘贴仍能捞到旧词（要真抹掉得走 redact，这类文档上 redact 会连带删掉别的字形）。
  真正想清干净，用「查找替换」的"只删除"或页面编辑重做那一页。

同一套能力在别处也能调，方便脚本和 web 壳：

| 层 | 入口 |
| --- | --- |
| core | `IPdfBackend::replaceTextAt(input, output, page, rect, text, opts, password)`、`addTextAt(...)`（`page` 是 0 基，`opts.deleteOnly` 只盖不写，`opts.fontSize=0` 自适应） |
| CLI | `pdfword_cli replace-at <in> <out> <页号1基> "x0,y0,x1,y1" "新文字" [--delete-only] [--color #rrggbb] [--font-size N]`、`add-text <in> <out> <页号> "rect" "文字" [--font-size N]` |
| C ABI | `pw_pdf_replace_text_at(ctx, in, out, page, x0, y0, x1, y1, text, delete_only, color, font_size, password)`、`pw_pdf_add_text_at(...)` |

**书签 / 目录面板**（`视图 → 书签目录`，默认在右侧，可以拖到左侧或拖成独立窗口）

PDF 里的“书签”和“目录”是同一个东西（文档里那棵 `/Outlines` 树），所以这里只有一棵树、一套操作：

| 按钮 | 作用 |
| --- | --- |
| `新增同级` / `新增子级` | 在当前选中的条目后面 / 下面插一条：标题 + 页码。页码留 0 表示“没有跳转目标”，用来做纯分组标题 |
| `修改` | 改选中条目的标题与页码 |
| `删除` | 删掉选中条目（连同它的子项） |
| `上移` / `下移` | 在同层级里换位置，不跨层级 |
| `应用` | 把面板里的树写回 PDF，另存 `<原名>_书签.pdf`，日志写明写了几条 |
| `导出…` | 存 `.txt`（每行 `层级<TAB>页码<TAB>标题`，与 CLI `outline --txt` 同格式）或 `.json`（字段与 C ABI 一致） |

- 预览打开哪个 PDF 就在树里铺开哪个的目录，双击文件列表换文档也会重新铺；
- **双击条目直接跳到那一页**，底部页码框同步过去；
- 层级随便加减，写回时“跳级”会被夹成“上一项层级 + 1”、第一条强制顶级，不会写出断层目录；
- 面板窄时标题会被省略号截断，鼠标悬停看全文；
- 面板显隐记进 INI，下次启动沿用（`saveState()` 会把隐藏的 dock 又显示出来，所以显隐另存 `view/bookmarkVisible`）；
- 写回走的是 MuPDF 的 outline iterator（1.26 没有 `pdf_document_update_toc`）：先删光旧树再逐条插，
  `uri` 填 `#page=N` 让 MuPDF 自己生成 `/Dest`，所以外链类书签（`http://…`）在写回后会变成纯分组标题。


**布局**：中央是竖直 `QSplitter`——上半区 PDF 预览、下半区只读日志（初始 640:140，可自己拖分隔条调高度）。
`视图 → 日志窗口 → 隐藏` 会把日志整块收起，预览区随即铺满整个中央区域；再点`显示`回到上次的高度。
`视图 → 文件列表 → 隐藏文件列表` 则把左侧面板整块收掉，预览区随之变宽；`显示文件列表` 让它回来。
**窗口顶部不再有工具栏**，导航全部收进主窗口**最底下那一行**——就是状态栏，和右下角 `0%` 进度条同排：
左边是常驻的后端状态文字（`PDF 后端: mupdf（可用）`），中间偏右是 `上一页 / 下一页 / 放大 / 缩小 / [页码] / 总页数 / 百分比`，
最右是 `收起 ▸` 按钮和进度条。预览区不为它单独让出一行高度，页面上也不会被任何浮条压住。
点 `收起 ▸`：那几个按钮和页码框全部隐藏，只剩 `◂ 展开`（行高不变，免得最底下那行来回跳）；再点回来。

| 底部那一行 | 作用 |
| --- | --- |
| `PDF 后端: mupdf（可用）` | 常驻后端状态，一眼看出跑的是 MuPDF 版还是 stub 骨架版 |
| `上一页` / `下一页`（也可 `PageUp` / `PageDown`） | 翻页，翻完回到页顶；到首页/末页时对应按钮变灰 |
| `放大` / `缩小`（`Ctrl++` / `Ctrl+-`） | 按固定档位走：8/10/15/25/33/50/67/75/100/125/150/175/200/250/300/400/600/800% |
| 页码输入框 + ` / N` | **直接输入页码，回车（或失焦）就跳到那一页**，范围自动锁在 1..总页数 |
| 缩放百分比 | 当前比例，一眼看出是 150% 还是 300% |
| 进度条 + `0%` | 任务进度（转换/合并/拆分等），和上面的按钮同一行 |
| `收起 ▸` / `◂ 展开` | 收起或展开上面这些按钮（状态会被记住，下次启动沿用） |

进程条与后端状态在状态栏；“待处理文件”面板内部带 添加文件 / 移除选中 两个按钮和
“双击 PDF 可预览；可直接把文件拖到这里”提示；用 `视图 → 文件列表` 可把它停到左侧或右侧。

**视图状态会记住**：关窗口的时候把当前布局写进 INI，下次启动原样恢复，不用每次重新调。

| 记住的项 | 存在哪 | 说明 |
| --- | --- | --- |
| 窗口大小/位置 | `window/geometry` | 上次关闭时的窗口几何；没有记录时才走"启动居中" |
| 文件列表显隐 / 停靠侧 / 宽度 / 是否浮动 | `window/dockState` + `view/fileListVisible` | `saveState()` 不存被隐藏的 dock，所以显隐另存一份 |
| 日志窗口显隐 / 高度 | `view/logVisible`、`view/logHeight` | 隐藏时把当时的高度记下来，重新显示或下次启动都回到这个高度 |
| 底部导航条（状态栏那一行）收起 / 展开 | `view/navCollapsed` | 收起状态下重开还是收起，只剩一个 `◂ 展开` |
| 书签 / 目录面板显隐 | `view/bookmarkVisible` | 与 `window/dockState` 配合：`saveState()` 不存隐藏的 dock，显隐另存一份 |

落盘位置 `%APPDATA%\Pdf-Toolkit\Pdf-Toolkit.ini`（跟工作目录 `%LOCALAPPDATA%\Pdf-Toolkit\work` 分开）。
**删掉这个文件就等于恢复默认布局**：窗口居中、文件列表靠左且显示、日志窗口显示（640:140）、书签面板显示在右侧、导航条展开。

**缩放与连续滚动**：所有页竖着排成一条画布（页与页之间 14px 浅灰缝），拿滚轮或拖右侧滚动条就能一路往下看，
不是"一次只显示一页、靠按钮翻页"。画布里只有视口附近的页真正渲染成位图，滚过去自动补、滚远了自动放，
所以 80 页的文档也不会一次吃掉几百 MB。当前页跟着"视口顶部落在哪一页"自动更新，页码框和总页数一起走；
放大超过视口宽度后底部出现横向滚动条，缩小到整页装得下时它消失。

| 操作 | 效果 |
| --- | --- |
| 底部横条 `放大` / `缩小`（`Ctrl++` / `Ctrl+-`） | 固定档位，反复点击停在整档上 |
| `Ctrl + 滚轮`（在预览区里） | 以**鼠标所在位置**为锚点缩放，指针下的内容不动 |
| `适应宽度`（`Ctrl+1`） | 页面宽度铺满视口，横向不再需要滚动（会预留竖直滚条宽度和页边距，不会算了又冒一条横向条） |
| `适应窗口`（`Ctrl+0`） | 单页整页高度装进视口；其它页仍在下面，滚着看 |
| `实际大小`（`Ctrl+Alt+0`） | 回到 100%（zoom=1 一页 72dpi 的原始像素） |

**拖放**：把文件从资源管理器拖到主窗口**任意位置**（预览区、日志区、左侧面板都行）就会进待处理列表；
拖入单个 PDF 时会顺带打开预览。扩展名按 `pdf/docx/doc/odt/rtf/txt/xlsx/pptx` 过滤，重复路径自动去重，
不支持的类型不会入列、但会在日志里写“忽略 N 个不支持的文件”，不会静默吞掉。
`合并 PDF…` 的列表只收 PDF、`图片转 PDF…` 的列表只收 PNG/JPG/BMP/TIFF/GIF，都支持直接拖入。

需要“某个 PDF”来操作的菜单项（拆分 / 抽取 / 页面编辑 / 元数据 / 加密解密 / 压缩 / 水印 / OCR）取源顺序是：
**当前预览的 PDF → 列表里唯一选中的 PDF → 多个时弹下拉让你选 → 都没有就在日志里提示**。
`图片转 PDF…` 不需要源 PDF，它只要求你先在窗口里添加图片。

`页面编辑…` 里那张“输出页顺序”列表就是最终 PDF 的页序列：删页 = 从列表删掉一条，
重排 = 挪动列表项，旋转/裁剪 = 给该条打标记，保存时才真正写文件。按 `Esc` 或 `关闭`
不会改动源文件。

## 打包与安装包

一条命令出两个交付物（`packaging\build-installer.ps1`）：

```powershell
# 前提：publish\ 已经 windeployqt 部署过（界面 exe + Qt DLL + 插件目录齐全）
pwsh -File packaging\build-installer.ps1
```

| 产物 | 内容 | 用途 |
| --- | --- | --- |
| `dist\Pdf-Toolkit-<ver>-win64-portable.zip` | 主程序 + 运行库（目录结构原样） | 绿色版，解压双击 `pdftoolkit.exe` 就能用 |
| `dist\Pdf-Toolkit-<ver>-win64-setup.exe` | Inno Setup 编译的单文件安装包 | 常规安装：向导、开始菜单 / 桌面快捷方式、卸载项 |

细节：

- 版本号自动从 `CMakeLists.txt` 的 `project(Pdf-Toolkit VERSION …)` 读，跟「关于」窗口显示的
  是同一个值，不用改两处。
- 打包只带**界面程序**：`pdfword_cli.exe` / `pdfword.dll` 不进包（它们是给命令行和 web 壳的，
  要一起发就改 `build-installer.ps1` 里的 `$exclude`）。
- 安装默认落 `%LOCALAPPDATA%\Programs\Pdf-Toolkit`，`PrivilegesRequired=lowest`，
  **不弹 UAC**。要装进 Program Files，把 `packaging\pdftoolkit.iss` 的
  `PrivilegesRequired` 改成 `admin`。
- 安装界面中英双语（`packaging\langs\ChineseSimplified.isl` 是 Inno 官方仓库的翻译，
  安装时按系统语言自动选）。
- 安装包图标用 `ui\resources\pdftoolkit.ico`，和程序图标同一个。
- 只重编安装包、跳过 zip：`pwsh -File packaging\build-installer.ps1 -SkipPortable`。
- 静默装到指定目录（自测用）：

  ```powershell
  .\dist\Pdf-Toolkit-0.0.2-win64-setup.exe /VERYSILENT /SUPPRESSMSGBOXES /NOICONS /DIR="D:\tmp\pdt"
  ```

## 当前状态（说清楚哪些是真跑通了，哪些还没）

以下“已验证”均指在 `build\mupdf`（`PDFWORD_WITH_MUPDF=ON`，MuPDF 1.26.10 + LibreOffice 26.8.3.1）
上真机跑过，样例是 `D:\media\lecture_1.pdf`（63 页横版）与 `D:\media\lecture_10.pdf`（82 页竖版）：

| 能力 | 走哪个模块 | 状态 |
| --- | --- | --- |
| 工程编译 / Qt 界面启动 | ui | ✅ 已验证（窗口标题 `Pdf-Toolkit — PDF/Word 转换与预览`，启动居中） |
| 界面菜单化重构（六入口进菜单 + 可停靠文件面板 + 三个弹窗） | ui | ✅ 已验证（`pdfword_ui_smoke` **198 项断言全过**；`QWidget::grab()` 出的 27 张界面图逐个核对过布局） |
| 预览里直接改（右键 → 拖框 → 改字 / 加字 / 批注 / 超链接 / 换图） | core + CLI + C ABI + ui | ✅ 已验证（`iw8ben.pdf` 第 2 页框住 `instructions for` 改成 `SMOKE-REPLACED`：产物 `<原名>_已编辑.pdf`、`findText` 命中第 2 页、**第 9 页文本逐字未变**；再往第 3 页空白处拖框打 `加字冒烟 ABC 123`，中文能搜到；CLI `replace-at` / `add-text` 与 C ABI 同名函数各跑一遍） |
| 启动居中 / 产品名 Pdf-Toolkit | ui | ✅ 已验证（实测窗口落在 (128,32)，与可用区中心一致；标题、CLI 首行、docx 元数据 creator 均为 Pdf-Toolkit） |
| 书签 / 目录管理（读 / 新增 / 修改 / 删除 / 移动 / 导出） | core + CLI + C ABI + ui | ✅ 已验证（core 侧 `fz_load_outline` 拍平后页码统一用 `fz_page_number_from_location`（外链解析不到记 0，不再靠 `uri == nullptr` 判空）、写回用 outline iterator 删旧树再逐条插；`iw8ben.pdf` 读回 12 条含 3 层、写入 5 条含中文再读回层级与页码一致，pypdf 交叉核对 `/Outlines` 的 `/First`–`/Next` 链确实是真实 `/Dest`；CLI `outline` / `set-outline`、C ABI `pw_pdf_outline` / `pw_pdf_set_outline` 各跑一遍；UI 侧见上面冒烟断言） |
| 视图状态持久化（文件列表 / 日志 / 书签面板显隐、停靠侧、日志高度、窗口几何） | ui / QSettings | ✅ 已验证（关窗写 `%APPDATA%\Pdf-Toolkit\Pdf-Toolkit.ini`，重开同一进程内造第二个窗口，隐藏的面板与靠右停靠都回来了） |
| 预览就地编辑的连续操作（连改 12 次 / 拖框写回 12 轮 / 完整交互 6 轮，都不崩、不跳回首页） | ui / PdfViewWidget + MainWindow | ✅ 已验证（三件事：页顶改按 `pageHpt_` 累加算（不再读布局未生效的 `QLabel::geometry()`）、`reloadAfterInline` 带 `scrollValue` 还原滚动位置、橡皮筋 `QPointer` + `clearPages()` 清理 + 跨页重建；反向验证见 `docs/ARCHITECTURE.md`） |
| 预览连续滚动（所有页竖排 + 懒渲染 + 滚动反推当前页） | ui / PdfViewWidget | ✅ 已验证（9 页与 63 页文档各跑一遍冒烟：页数对齐、滚到底当前页自动跳末页、只渲染视口 ±400px 内的页；63 页整轮 106 项断言 10s 跑完） |
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
| PDF 水印（文字） | core / MuPDF 后端 | ✅ 已验证（graft + Form XObject 叠加：82 页全部可加水印，中文水印可提取到文字层，原页渲染像素级不变；12 种位置全部出件，九宫格 + 居中斜放 + 水平铺满 + 45° 铺满逐张渲染核对过方位） |
| PDF 水印（图片） | core / MuPDF 后端 | ✅ 已验证（透明底 PNG 居中/平铺：9 页 190,688 B → 193,149 B（居中，每页一张）与 194,670 B（平铺，每页 30 格）；82 页 4,859,511 B → 4,934,195 B，用时 0.3 s；输出里只有 1 个 `/Image` + 1 个 `/SMask`，平铺不会复制像素） |
| OCR 文字识别（可检索 PDF + 纯文本） | core / Windows.Media.Ocr（C++/WinRT） | ✅ 已验证（图片型 PDF 单页 460 个词：字号/基线/横向缩放逐词算准，文字层用 `3 Tr` 于是**渲染像素 0 差异**；`text` 能从输出 PDF 里抽出“iw8ben.pdf”“添加文件”这些词，说明 Ctrl+F 与选中复制可用；3 页中文扫描件 1.2 s（约 0.4 s/页），识别出“数字图像处理 / 第一讲 / 绪论 / 提纲 / 基本概念” |
| pdf → docx（文字重排，版式有损） | MuPDF + DocxWriter | ✅ 已验证（82 页 → 726 个非空段落，中文正常） |
| C ABI 供 web 壳调用 | core / capi | ✅ 已验证（Python `ctypes` 加载 `pdfword.dll`：建 context、取能力报告、错误码与 last_error 都对；28 个页面/文档级/编辑批注/书签函数逐个调通（批注 `pw_pdf_annotate`、超链接 `pw_pdf_add_link`、查找 `pw_pdf_find_text`、替换 `pw_pdf_replace_text`、换图 `pw_pdf_replace_image`、表单 `pw_pdf_form_fields` / `pw_pdf_form_fill` / `pw_pdf_form_create`、书签 `pw_pdf_outline` / `pw_pdf_set_outline`、按矩形改字 `pw_pdf_replace_text_at` / `pw_pdf_add_text_at` 都在其中），图片水印 `pw_pdf_image_watermark`、带位置的 `pw_pdf_watermark_position`、`pw_pdf_ocr` / `pw_ocr_capability` 都在其中） |

已知限制（不是 bug，是当前实现的取舍）：

- **PDF → DOCX 只搬文字，不搬版式**：段落按行重排，图片、表格、分栏不保留。要保版式得走
  `docx → pdf` 方向，或者另外接版面分析。
- **加密 PDF 需要调用方提供密码**：`IPdfBackend::open(path, password)`、`--password`、
  C ABI 的 `pw_set_pdf_password()` 三条入口都支持，且能区分“需要密码”和“密码错误”。
  界面里 `开始转换…` 对话框有 “PDF 密码” 输入框（会透传到 `ConvertOptions.pdfPassword`）；
  预览、页面编辑、抽取页面这些入口目前不自带密码框，打开加密 PDF 会直接提示需要密码；
  `抽取页面…` 读不到页数时会明确提示“读不到页数（文件打不开或已加密，先解密再抽取）”，
  不会放过一个荒唐的页码范围；`加密 / 解密…` 窗口里的“打开密码”字段就是解密入口，
  填对才会另存 `_解密.pdf`。
- **文字水印 12 种位置**：`top-left`（默认）/ `top-center` / `top-right` / `middle-left` / `center` /
  `middle-right` / `bottom-left` / `bottom-center` / `bottom-right` / `center-diagonal` /
  `tile-horizontal` / `tile-diagonal`，未知取值按 `top-left` 处理。单条字号是页宽短边的 8%，
  平铺时降到 5.5%（同一字号一页放几十条会叠成一片）。
  **45° 两档（`center-diagonal` / `tile-diagonal`）走位图，不走 PDF 文字**：
  把文字用 4 倍超采样渲染成一张透明底 PNG（整份文档只嵌一份，同字号的页共用），
  再按图片水印的 `cm` 旋转贴上去。原因是 MuPDF 渲染"文字矩阵里带旋转"的内容流时会把字号
  放大 2–3 倍（45° 的单条水印能糊掉半页，像素红覆盖 27%，平铺能到 85%）；
  换成位图后同样的内容降到 0.6% / 2.9%，与水平摆放的观感一致。代价是斜向水印不可选中、
  不可搜索，水平摆放的 10 档仍然是真文字（可选中、可提取）。
- **文字宽度不能用 `fz_measure_string`**：它对 CJK 字体拿不到可靠 advance（返回矩阵的
  `e` 分量偏小），中文水印的平铺步长会算错。现用 `fz_bound_text` 探一条再取包围盒宽度。
- **文字水印会多出一个 CJK 字体子集**：水印要嵌一个字体子集（微软雅黑按实际字形裁剪后约 190 KB），
  所以输出一定比源文件略大，但不会再成倍膨胀：82 页样例源 4.63 MB → 输出 4.70 MB
  （源 9 页样例 190,688 B → 167,623 B，因去重与字体子集化反而略小；有 `/Rotate`/CropBox
  的页也走同一条 graft 路径，只改页面对象不改原页内容）。
  早先逐页 `pdf_graft_page` 的写法会让源页字体/图片被每页各拷一份，输出曾到 8.39 MB，已修复。
- **图片水印只嵌一份像素数据**：实现上先 `pdf_add_image` 在输出文档里建一个 image XObject，
  每页的 Form XObject 里只写 `q /GS0 gs <w> 0 0 <h> <x> <y> cm /PwImg Do Q`。
  第一版用 `pdf_new_pdf_device + fz_fill_image` 逐格画，pdf device 不会去重，
  9 页平铺件涨到 5.48 MB（648 个 Image 对象）；改成单份 XObject 后降到 194,670 B
  （1 个 Image + 1 个 SMask）。PNG 的透明通道会写成 `/SMask`，整体透明度走 `/ExtGState` 的 `/ca`。
  平铺按「页内网格、间距 = 水印宽的 1.6 倍」铺满整页，不做 45° 旋转；竖长图会被压到页高的 90% 以内。
- **OCR 走系统内置引擎，先支持 Windows**：`core/src/ocr_windows.cpp` 用 C++/WinRT 调
  `Windows.Media.Ocr`（离线、不额外下发模型、只认系统已装语言——本机就是 `zh-Hans-CN`，中文引擎也能认英文）。
  文字层实现取最快的一条路：每个页面一个 Form XObject，`/Matrix` 取 `inv(pageCtm)∘[1 0 0 -1 0 H]`
  （两次 y 翻转正好抵消，等于直接用页面用户空间坐标），于是 `/Rotate` 页也能落对位置；
  每个词 `BT /PwOcr <size> Tf <tz> Tz 3 Tr … Tm <hexGlyphIDs> Tj ET`，`3 Tr` 是**不可见渲染**，
  码值走 `fz_encode_character` 取 glyph index 写进 Identity-H 字体（`pdf_add_cid_font` 自带 ToUnicode，
  所以复制/搜索能还原成原文），字数少时再 `pdf_subset_fonts` 裁字体（否则会多背 19MB 的微软雅黑）。
  已知取舍：只做“文字层 + 纯文本”，不做版面还原（表格/分栏会按行拼平）；
  引擎第二选择 Tesseract 还没接（见下条）。
- **编辑与批注：MuPDF 的 redact 在这类文档上删不掉文字，替换走「覆盖 + 重写」**：
  逐词实测 `pdf_redact_page` / `pdf_apply_redaction` / 补 `pdf_update_page` 四套组合都无效
  （Redact 注释建好了、也被删掉了，但原文仍能被 `find` 命中，判断文字藏在嵌套 Form 里没被实例化）。
  最终 `replace` / `--delete-only` / `replace-image` 统一改成**白底矩形覆盖**：
  在原命中框上画 `1 1 1 rg … re f`（左右各外扩 1pt、**只向下**外扩 2pt——向上外扩会把
  上一行字的下半截切掉），需要时再压新文字。
  新文字比原框长时**缩字号**而不是横向压扁（压扁会糊成一条线），字号按框高 × 0.82 起，
  再按"实际字宽 / 框宽"折算缩小。
  字宽分两套模型：拉丁字母走 base14 Helvetica（字宽真实），汉字走非嵌入 CJK 字体（一字一 em）——
  全用 CJK 字体时每个字母都占满一个全角，`BOOK` 会压到右邻的 `records` 上。
  代价：底层旧文字仍在（可被搜索到），要彻底抹除得等 MuPDF redact 修好或换引擎；
  这一步在 README 与源码注释里都写明了。
- **偏「重排版面」的文字编辑没做**：只做「按命中区域整块替换 / 删除」，
  不做逐字插入、改字号字距、断行重排（那需要 PDF → 可编辑文档 → 回排的完整链路）。
- **文本框批注用位图外观，不用嵌入字体**：直接给 FreeText 挂 CJK 字体时 MuPDF 会把整套
  19MB 的 OpenType 字体程序塞进 PDF（224KB → 19MB）。改为把文字渲染成白底位图当 `/AP`，
  体积回到 272~292KB，中文显示正常。
- **仍未做**：数字签名与远程签名、PDF 版本对比。这两项都需要额外的签名/比对引擎，
  不在 MuPDF + LibreOffice 的能力范围内。
- **Tesseract 引擎未接入（装库这步被网络卡住）**：`vcpkg install tesseract:x64-windows`
  会拉起 10 个包（curl / leptonica / tiff / giflib / libwebp / zlib …），本机实测：
  `ghproxy.net` 镜像对 GitHub 包有效，但 **giflib 在 SourceForge**，
  镜像前缀返回 403、直连 `downloads.sourceforge.net` 超时（TUNA/USTC 的 sourceforge 镜像路径也都 404），
  于是卡在 `Downloading giflib-6.1.3.tar.gz` 这一步，进程已手动终止。
  要接 Tesseract 有两条路：(1) 手工把缺的包（先 giflib-6.1.3.tar.gz）拷进 `D:\vcpkg\downloads`，
  vcpkg 命中缓存后会继续往下编；(2) 换成预编译的 tesseract 二进制（自带头文件与 `tesseract.dll`）。
  库到位后只需实现 `TesseractOcrEngine`（`IOcrEngine` 三件套：`name/available/languages/recognize`），
  在 `makeOcrEngine()` 里按「Windows 优先、失败回退 Tesseract」串起来，backend / CLI / UI / C ABI 一行都不用改。
  `chi_sim.traineddata`（约 45MB）放 `tessdata/` 即可。
- **页面编辑器是列表式，不是缩略图拖拽**：`页面编辑…` 对话框里用文本列表 + 上移/下移/置顶/置底
  来重排，不做缩略图网格。缩略图需要额外缓存与线程调度，属于纯界面工作量。
- **右键菜单只挂在主窗口的待处理列表**：合并窗口、图片转 PDF 窗口的列表还是按钮操作。
  “删除 / 清空”只动列表不动磁盘（日志里明写），要删磁盘文件请自己在资源管理器里删；
  “重命名文件…”改的是真实文件名（可以连扩展名一起改，但不校验改后是否仍被支持），单文件、不批量。

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
.\build\msvc-x64\bin\pdfword_cli.exe watermark in.pdf out.pdf "内部资料" --opacity 0.3 --position tile-diagonal
.\build\msvc-x64\bin\pdfword_cli.exe watermark in.pdf out.pdf "内部资料" --position center-diagonal
.\build\msvc-x64\bin\pdfword_cli.exe imgwatermark in.pdf out.pdf logo.png --scale 0.3 --opacity 0.15
.\build\msvc-x64\bin\pdfword_cli.exe imgwatermark in.pdf tiled.pdf logo.png --scale 0.2 --opacity 0.08 --tile
.\build\msvc-x64\bin\pdfword_cli.exe ocr scan.pdf searchable.pdf extracted.txt --lang zh-Hans-CN
.\build\msvc-x64\bin\pdfword_cli.exe ocr scan.pdf searchable.pdf -            # 只要可检索 PDF
.\build\msvc-x64\bin\pdfword_cli.exe ocr scan.pdf - extracted.txt              # 只要纯文本
.\build\msvc-x64\bin\pdfword_cli.exe info .\enc.pdf --password 888888
.\build\msvc-x64\bin\pdfword_cli.exe convert .\enc.pdf .\enc.docx --to docx --password 888888

# 编辑与批注（坐标是页面显示空间 pt：左上原点、y 向下；页码一律 1 基）
.\build\mupdf\bin\pdfword_cli.exe annotate in.pdf out.pdf highlight 9 "386,56,410,75"
.\build\mupdf\bin\pdfword_cli.exe annotate in.pdf out.pdf freetext 9 "40,40,240,90" --text "这里要签字"
.\build\mupdf\bin\pdfword_cli.exe annotate in.pdf out.pdf ink 9 "40,200,300,260" --ink "40,200;80,230;120,210;160,250"
.\build\mupdf\bin\pdfword_cli.exe link in.pdf out.pdf 9 "380,40,560,60" https://example.com
.\build\mupdf\bin\pdfword_cli.exe link in.pdf out.pdf 9 "380,40,560,60" 第3页
.\build\mupdf\bin\pdfword_cli.exe find in.pdf book --page 9
.\build\mupdf\bin\pdfword_cli.exe replace in.pdf out.pdf book BOOK --color "#C00000"
.\build\mupdf\bin\pdfword_cli.exe replace in.pdf out.pdf book "" --delete-only
.\build\mupdf\bin\pdfword_cli.exe replace-image in.pdf out.pdf 9 "380,40,560,120" logo.png
.\build\mupdf\bin\pdfword_cli.exe form-fields form.pdf
.\build\mupdf\bin\pdfword_cli.exe form-fill form.pdf filled.pdf customer=张三 agree=on city=上海
.\build\mupdf\bin\pdfword_cli.exe form-create in.pdf form.pdf "customer:text:1:80,80,300,110" "agree:checkbox:1:80,130,100,150" "city:choice:1:80,180,300,210:上海|北京|广州"

# 书签 / 目录（level 从 0 起，page 是 1 基；page 写 0 表示“没有跳转目标”的分组标题）
.\build\mupdf\bin\pdfword_cli.exe outline in.pdf
.\build\mupdf\bin\pdfword_cli.exe outline in.pdf --txt ol.txt
.\build\mupdf\bin\pdfword_cli.exe set-outline in.pdf out.pdf "0:1:封面" "0:2:第一节" "1:3:小节 A" "0:6:第二节"
.\build\mupdf\bin\pdfword_cli.exe set-outline in.pdf empty.pdf --from ol.txt     # 读文件里的层级/页码/标题
.\build\mupdf\bin\pdfword_cli.exe set-outline in.pdf empty.pdf                    # 不给条目 = 清空目录
```

`--password` 对所有命令通用（info / text / render / convert 都认）；加密文档不开密码会报
“文档已加密，需要密码”，密码不对报“文档已加密，密码错误”。

数值参数（页号、角度、裁剪量、opacity）一律是**位置参数、纯数字**：写 `0.3`，不要写
`--opacity 0.3`。例外有两处：`imgwatermark` 的 `--scale` / `--opacity` / `--tile`
按带参数形式写，也可以只给一个位置参数当 scale；`watermark` 既认 `--opacity 0.3` /
`--position tile-diagonal`，也兼容老写法（第 4 个位置参数当 opacity）。写错时 CLI 会打一行
“参数错误: …”并返回退出码 2，不会再让进程直接崩掉（早期版本会因 `std::stof` 抛异常而 abort）。

## 架构要点

细节见 [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md)。三条铁律：

1. `core` 不 include 任何 Qt 头文件 —— 换 web 壳时界面层整个丢掉，核心零改动。
2. 外部依赖一律走接口（`IPdfBackend`、`OfficeRunner`），路径全部来自 `Config`，代码里不出现硬编码。
3. 跨边界只传 `Status`/`ErrorCode`，不抛异常；C ABI 把错误码映射成整数返回。

## 许可与 MuPDF 约束（发布前必读）

**本仓库自己的代码是 MIT**（见 [LICENSE](LICENSE)，Copyright (c) 2026 wangtc）。
但**构建出来的二进制不止包含本仓库的代码**——一旦打开 `PDFWORD_WITH_MUPDF=ON`，
`libmupdf` 会被静态链接进 `pdftoolkit.exe` / `pdfword_cli.exe` / `pdfword.dll`，
那几个文件的授权就由 MuPDF 说了算。

### 依赖的授权一览

| 组件 | 授权 | 对你意味着什么 |
| --- | --- | --- |
| **MuPDF**（`libmupdf`，vcpkg 的 `unofficial-libmupdf`） | **AGPL-3.0** 或 **Artifex 商业许可**（二选一） | 链接了它，整个软件按 AGPL；闭源分发必须先买商业许可 |
| LibreOffice | MPL-2.0 | 本工程只以**子进程**调用 `soffice --convert-to`，不链接、不修改，MPL 的文件级传染不落到本项目上 |
| Qt 6 | LGPL-3.0 / 商业 | 动态链接 + 随包分发 Qt DLL 即可，别静态链接闭源首发（静态要 LGPL 的例外条款或商业许可） |
| Windows 内置 OCR（本工程用的就是它） | 系统能力，不随包分发 | 不影响本项目授权；若将来接 Tesseract 等引擎，那是 Apache-2.0，保留 NOTICE 即可 |

### 三条路：AGPL 开源 / 买商业许可 / 干脆不发 MuPDF

MuPDF 由 Artifex Software 开发，官方双授权（<https://mupdf.com/> 的 “LICENSING” 段、
<https://mupdf.readthedocs.io/en/latest/license.html>）。**没有第三种**：不遵守 AGPL 又没买商业许可，
就不能分发带 MuPDF 的软件，也不能拿它对外提供在线服务。

#### 路径 A：按 AGPL-3.0 用（免费，但要开源整包）

AGPL 管的是**整个软件**，不是“只有那个库”。把 MuPDF 链进 `pdftoolkit.exe`，
就得把 `pdftoolkit.exe` 这个作品整体的源码给出去。具体要求：

- **分发即给源码**：任何人拿到你的 exe/dll，你都要能提供**完整对应源码**
  （本仓库的全部源码 + 你对 MuPDF 的任何修改 + 构建脚本），且不能对源码额外收费
  （工本费除外）。
- **保留声明**：不能去掉 MuPDF 的版权头、AGPL 文本和“可以找 Artifex 买商业许可”的说明。
- **允许替换 MuPDF**：终端用户要能拿更新版的 MuPDF 重新链接/替换（AGPL 的 anti-tivoization 条款），
  所以别把版本写死到无法替换，也别加锁阻止替换。
- **网络服务也管**（AGPL §13，比 GPL 多出来的那一条）：如果把本工具做成 web 壳对外提供服务
  （用户通过网络与它交互），必须向这些使用者提供获取完整源码的方式——页面上放源码地址即可，
  但必须有。
- **没有担保和支持**：AGPL 下 MuPDF 是 AS-IS。
- 本仓库的 MIT 声明要保留（MIT 与 AGPL 兼容，最终产物整体按 AGPL-3.0 分发，
  里面仍注明哪些文件来自 MIT 部分）。

#### 路径 B：买 Artifex 商业许可（闭源、商用、免源码义务）

要闭源卖、随硬件出货、做 SaaS 又不想开源，就走这条：商业许可会解除 AGPL 的全部约束
（不必开源、不必给源码、不必允许替换），通常还带支持。联系方式：

- 官方许可页 <https://artifex.com/licensing/>，或邮件 `sales@artifex.com`
- 询价时说明清楚：**MuPDF 作为静态库链接进 Windows 桌面程序**、是否需要 SaaS 场景、
  预计发行量——报价按场景定，不同场景价格差别很大

**注意**：商业许可**按项目/产品**授权，不是买一次终身覆盖所有产品；换产品线要重新确认条款。

#### 路径 C：不发 MuPDF（保持纯 MIT 分发）

不需要 PDF 能力、或只想先发个壳，用 stub 后端构建：

```powershell
cmake -S . -B build/msvc-x64 -G "Visual Studio 17 2022" -A x64 `
      -DCMAKE_PREFIX_PATH=D:/Qt/6.12.0/msvc2022_64 `
      -DPDFWORD_WITH_MUPDF=OFF
```

这样编出来的产物里没有 MuPDF，可以按 MIT 分发；代价是 PDF 预览 / 文本 / 合并 / 拆分 /
抽取 / 页面编辑 / 水印 / 加密全部报 `BackendMissing`（`doctor` 会写
“PDF 后端: stub（不可用）”），只剩 LibreOffice 那条 docx ⇄ pdf 转换链可用。

### 容易踩的四个坑

1. **“我只发了 exe，没发源码，应该没事”** —— 不行。AGPL 的义务是“给接收者源码”，
   不是“仓库里放着源码就行”；对方要，你就得能给。
2. **“我用的 MIT LICENSE，所以我的软件是 MIT”** —— 那只是**本仓库代码**的授权。
   链接了 AGPL 库的产物，产物本身按 AGPL。两个许可要分开看。
3. **“我在内网用，不对外”** —— 纯自用（不把软件给任何别人、也不对外提供服务）没问题；
   一旦交付给客户、装到别人的机器上、或者放到公网给外部用户点，就触发上面的义务。
4. **别用 MuPDF / Artifex 的商标**：`MuPDF`、`Artifex` 及其 logo 是注册商标，
   产品名、宣传页、图标里别借用（本工程叫 `Pdf-Toolkit`，与 Artifex 无隶属关系）。

### 免责

上面是对官方许可要点的转述与工程实践提醒，**不构成法律意见**。
正式对外发布（尤其商业化）前，请以 <https://mupdf.com/> 与
<https://www.gnu.org/licenses/agpl-3.0.html> 的原文为准，或咨询你自己的法务。
