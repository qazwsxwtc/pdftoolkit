# 外部依赖接入

骨架里两个外部依赖都是**运行时/构建时可选**的：

- 缺 **LibreOffice** → 编译照常，UI 能开，只有 docx/pdf 转换报 `BackendMissing`。
- 缺 **MuPDF** → 编译照常（用 stub 后端），UI 能开，只有 PDF 预览/文本/合并/拆分/水印报 `BackendMissing`。

不给路径也能跑起来，这是刻意的设计：新人拿到代码第一天就能编译、能看界面。

---

## 一、LibreOffice（便携/免安装）

### 步骤 1：拿到一个不需要安装的 LibreOffice

任选一种：

**A. MSI 管理式安装（推荐，最稳）**

```powershell
# /a 是 administrative install，只解包到目录，不写系统注册表、不建快捷方式
msiexec /a "D:\download\LibreOffice_25.2_Win_x86-64.msi" /qb TARGETDIR=D:\Portable\LibreOffice
```

**B. 7-Zip 直接解包 MSI**

```powershell
7z x "D:\download\LibreOffice_25.2_Win_x86-64.msi" -o"D:\Portable\LibreOffice"
```

两种方式都应该得到：`D:\Portable\LibreOffice\program\soffice.exe`。

> 官方下载页：`https://www.libreoffice.org/download/download-libreoffice/`
> 只装 Writer + 转换过滤器即可，但官方 MSI 是整包，直接整包解出来最省事。

### 步骤 2：写进 pdfword.ini

把 `build\msvc-x64\bin\pdfword.ini.example` 复制成 `pdfword.ini`，放在 exe 同目录：

```ini
; 写 exe 绝对路径
libre_office = D:/Portable/LibreOffice/program/soffice.exe

; 或者只写目录，程序会自己拼 program/soffice.exe
; libre_office = D:/Portable/LibreOffice
```

也可以不写 ini，用环境变量：`$env:PW_LIBREOFFICE_PATH = "D:/Portable/LibreOffice/program/soffice.exe"`

查找优先级（`core/src/config.cpp`）：显式传入 → 环境变量 → exe 同目录 ini → 注册表
`HKLM\SOFTWARE\LibreOffice\UNO\InstallPath` → Program Files 常见目录 → PATH。

### 步骤 3：验证

```powershell
.\build\msvc-x64\bin\pdfword_cli.exe doctor
# 期望看到: LibreOffice: D:\Portable\LibreOffice\program\soffice.exe
#           LibreOffice 版本: LibreOffice 25.2.x.x ...

.\build\msvc-x64\bin\pdfword_cli.exe convert .\sample.docx .\sample.pdf --to pdf
# 期望: 产物: ...\sample.pdf
```

如果报"LibreOffice 转换超时"，先把 ini 里 `timeout_ms` 调大（首次启动要建隔离 profile，会慢）。

### 关于 LibreOffice SDK（UNO）

`D:\LibreOffice_26.8_SDK\sdk` 里有 `include`（com/cppu/cppuhelper/LibreOfficeKit/osl/rtl/sal/typelib/uno）、
`lib`（icppu / icppuhelper / isal / isalhelper 等 import lib）、`bin`（cppumaker/climaker 等工具）。
SDK 本身不含 office 运行时，必须配一份同版本的 office 才有意义——本机 `D:\LibreOffice\program\soffice.exe`
是 **26.8.3.1**，和 26.8 的 SDK 版本对得上，已经具备走 UNO 的条件。

当前 `OfficeRunner` 走的是 `soffice.exe --convert-to` 子进程（隔离 profile + JobObject 超时杀进程树 +
产物 `<stem>.<ext>` 校验），不依赖 SDK。要换成 UNO（cppumaker 生成类型头 + 链 `icppu.lib`/`isal.lib`
连本地实例）只需替换 `OfficeRunner` 的实现，`Converter` 和 `IPdfBackend` 接口都不用动。

---

## 二、MuPDF

### 本机现状（vcpkg 装的 1.26.10，已验证）

```
D:\vcpkg\installed\x64-windows\include\mupdf\          82 个头文件（fitz.h / pdf.h / fitz\*.h）
D:\vcpkg\installed\x64-windows\lib\libmupdf.lib        静态库，约 73 MB
D:\vcpkg\installed\x64-windows\share\unofficial-libmupdf\   CMake 配置（含传递依赖）
```

工程优先 `find_package(unofficial-libmupdf CONFIG)`：这个目标自己带
`freetype / harfbuzz / jbig2dec / openjpeg / libjpeg-turbo / zlib / gumbo` 的链接关系，
静态库才不会缺符号；找不到才退回 `cmake/FindMuPDF.cmake`（那条路只适合自己编的单体 `mupdf.lib`）。

### 装法一：vcpkg（GitHub 直连不通时套镜像）

vcpkg 的每个 port 都在 portfile 里写死了 GitHub 地址，官方**没有**国内资产镜像，
所以清华/中科大/阿里云的 PyPI 镜像对 vcpkg 无效。能用的是 vcpkg 自己的资产源机制：

```
X_VCPKG_ASSET_SOURCES = "x-script,<命令>"
# 命令里可用占位符：{url} {sha512} {dst}
```

工程里已封装成 `scripts/vcpkg-mirror.ps1`：用 curl 先把 URL 套上 `https://ghproxy.net/`，
失败的再回落原始地址（`X_VCPKG_ASSET_SOURCES` 允许把下载外包给任意命令）。

```powershell
pwsh -File scripts\vcpkg-mirror.ps1 -Packages libmupdf:x64-windows
```

实测：7 个包（vcpkg-tool-meson → harfbuzz → jbig2dec → libspng → libjpeg-turbo → openjpeg → libmupdf）
共用 21 分钟装完；其中 harfbuzz 走 meson 编 9 分钟，libmupdf 本体 5.8 分钟。

装完直接构建：

```powershell
cmake -S . -B build/mupdf -G "Visual Studio 17 2022" -A x64 `
      -DCMAKE_PREFIX_PATH=D:/Qt/6.12.0/msvc2022_64 `
      -DCMAKE_TOOLCHAIN_FILE=D:/vcpkg/scripts/buildsystems/vcpkg.cmake `
      -DPDFWORD_WITH_MUPDF=ON
cmake --build build/mupdf --config Release --parallel
```

`x64-windows` 是动态库三元组：harfbuzz.dll / freetype.dll / jpeg62.dll / openjp2.dll / z.dll / bz2.dll
等会在构建时由 vcpkg applocal 自动拷到 `bin`，不用手工复制。

### 装法二：官方源码自己编

```powershell
git clone https://github.com/ArtifexSoftware/mupdf.git
# 打开 platform/win32/mupdf.sln，Release x64 编译
```

编译产物里拿到 `mupdf.lib` 和 `include/mupdf/*.h`，按下面这种布局摆好：

```
D:\sdk\mupdf\
├── include\mupdf\fitz.h ...
└── lib\mupdf.lib
```

然后：

```powershell
cmake -S . -B build/mupdf -G "Visual Studio 17 2022" -A x64 `
      -DCMAKE_PREFIX_PATH=D:/Qt/6.12.0/msvc2022_64 `
      -DPDFWORD_WITH_MUPDF=ON `
      -DPDFWORD_MUPDF_ROOT=D:/sdk/mupdf
```

> 官网页面上那个 `mupdf-*-windows.zip` 是**成品二进制**（mutool.exe 等），不含头文件和 lib，
> 不能直接用来编译，别下错。

### 版本对齐：1.26.10 实际踩到的坑

`core/src/pdf_backend_mupdf.cpp` 已经按 **MuPDF 1.26.10** 编译通过，当时报错只有两处，
都是 1.24+ 改签名导致的：

| 调用 | 1.24+ 之前的写法 | 1.26.10 实际签名 |
| --- | --- | --- |
| `fz_show_string` | 8 参（末尾只有 wmode/bidi） | 9 参：`..., int wmode, int bidi_level, fz_bidi_direction markup_dir, fz_text_language language` |
| `fz_fill_text` | 7 参 | 8 参，末尾多一个 `fz_color_params`（传 `fz_default_color_params`） |

另外两处是**主动改**的，不是编译错误：

| 能力 | 做法 | 原因 |
| --- | --- | --- |
| 合并 / 拆分 | `pdf_create_document` + `pdf_graft_page(-1 追加)` + `pdf_save_document` | 原来走 `fz_new_document_writer` 的“重绘+蒸馏”，日志会刷 `the pdf device does not support image masks`，带透明蒙版的页面会失真；graft 直接搬页面对象，保真且快 |
| 文字提取 | 按 UTF-8 编码 `fz_stext_char::c` | 原实现 `c < 256 ? c : '?'` 把中文全变成 `?` |
| 水印字体 | 非 ASCII 文本先试 `C:/Windows/Fonts/msyh.ttc`，再退 `fz_new_cjk_font(FZ_ADOBE_GB)`，最后才 base14 Helvetica | Helvetica 只覆盖 ASCII，中文水印会空白 |
| 水印路径 | `pdf_create_document` + `pdf_graft_page` + 每页一个 **Form XObject**（自带 `/Resources`）叠加，最后 `pdf_subset_fonts` + `pdf_save_document` | 重绘路径丢 image mask；直接把资源并进页面会顶掉页面原有的 `/Font`、`/ExtGState`（表现为 `cannot find ExtGState resource 'GS1'` + 原文乱码），Form XObject 把水印资源隔离在自己的 `/Resources` 里 |
| 水印方向 | 用 `pdf_page_obj_transform` 取页面变换，水印按“显示方向”摆放；文字的 `trm` 的 y 取负 | 页面带 `/Rotate 90` 时（PPT 导出的 PDF 很常见），按页面空间画会变成竖排且跑位；`topctm` 含 y 翻转，不取负水印会上下镜像 |
| 原页图形状态 | 内容流首部插 `q`、尾部插 `Q` 再 `Do` 水印 | 原页内容常留下未闭合的 `cm`，水印直接接在后面会继承残留 CTM |
| 加密 PDF | `IPdfBackend::open(path, password)`；`fz_needs_password` + `fz_authenticate_password`，区分“需要密码”/“密码错误”；CLI `--password`、C ABI `pw_set_pdf_password` | 原来只能直接报“需要密码”，无法打开加密文档 |
| 保存压缩 | 统一 `pdf_write_options` 打开 `do_compress` | `pdf_default_write_options` 各项为 0（不压缩），graft 之后体积会膨胀 5 倍以上 |

> 水印现在也是 graft 路径，带透明蒙版的 PDF 加水印不再失真（原页渲染像素级不变，仅水印区域有差异）。

### 验证（本机实测结果）

```powershell
.\build\mupdf\bin\pdfword_cli.exe doctor
# PDF 后端: mupdf（可用）
# LibreOffice: D:\LibreOffice\program\soffice.exe
# LibreOffice 版本: LibreOffice 26.8.3.1 ...

.\build\mupdf\bin\pdfword_cli.exe info  D:\media\lecture_1.pdf        # 63 页, 842 x 595.22 pt
.\build\mupdf\bin\pdfword_cli.exe text  D:\media\lecture_1.pdf 1      # 中文正常：数字图像处理 / 第一讲 / 绪论
.\build\mupdf\bin\pdfword_cli.exe render D:\media\lecture_1.pdf 1 --zoom 1.5 --out page1.bmp   # 1263x893
.\build\mupdf\bin\pdfword_cli.exe merge out.pdf a.pdf b.pdf           # 20+20 -> 40 页
.\build\mupdf\bin\pdfword_cli.exe split big.pdf outdir 20             # 63 页 -> 4 个文件
.\build\mupdf\bin\pdfword_cli.exe watermark in.pdf out.pdf "内部资料" 0.3   # 文字层可提取到「内部资料」
.\build\mupdf\bin\pdfword_cli.exe info   enc.pdf                        # 报「文档已加密，需要密码」
.\build\mupdf\bin\pdfword_cli.exe info   enc.pdf --password 888888      # 3 页
.\build\mupdf\bin\pdfword_cli.exe convert enc.pdf enc.docx --to docx --password 888888
```

页面装配 / 文档级操作实测（`C:\Users\Lenovo\Desktop\tst\iw8ben.pdf` 9 页、`fw8ben.pdf` 1 页）：

| 命令 | 校验点 |
| --- | --- |
| `rotate in.pdf out.pdf "1,3" 90` | 输出 9 页，`/Rotate` = `[90,0,90,0,...]`，其余页不变 |
| `crop in.pdf out.pdf "1" 20 20 20 20` | 第 1 页 `/CropBox = [20,20,592,772]`，第 2 页无裁剪 |
| `delete in.pdf out.pdf "2,4-6"` | 9 页 → 5 页 |
| `pages in.pdf out.pdf "3,1,2"` | 3 页，且逐页文字与原第 3/1/2 页**完全相同** |
| `insert-blank in.pdf out.pdf 2 1` | 10 页，第 3 页文字长度 0（真空页） |
| `insert-pdf in.pdf out.pdf 1 fw8ben.pdf "1"` | 10 页，第 2 页文字与 `fw8ben.pdf` 第 1 页**完全相同** |
| `img2pdf album.pdf a.png b.jpg --a4` | 每页内嵌图为 918×1188 原始像素、未重采样 |
| `encrypt` / `decrypt` / `compress` | 加密件 `is_encrypted=True`、无密码报“需要密码”、解密回 9 页；压缩 274 KB → 220 KB 且首像素一致 |

同一批操作也通过 C ABI 用 Python `ctypes` 直接调 `pdfword.dll` 验证过一遍
（`pw_pdf_rotate_pages` / `pw_pdf_delete_pages` / `pw_pdf_crop_pages` / `pw_pdf_insert_blank` /
`pw_pdf_insert_pages` / `pw_images_to_pdf` / `pw_pdf_metadata` / `pw_pdf_encrypt` /
`pw_pdf_decrypt` / `pw_pdf_compress` 全部返回 0），web 壳可以照这组签名接。

水印相关实测（82 页 `lecture_10.pdf`，源 4.63 MB / 4,859,511 B）：

| 版本 | 输出体积 | 说明 |
| --- | --- | --- |
| 旧（整页重绘 + 蒸馏） | 25.8 MB | 日志刷 image mask 警告，透明蒙版失真 |
| graft + 直接并资源 | 20.7 MB | 顶掉页面 `/Font`、`/ExtGState`，原文变乱码（已废弃） |
| graft + Form XObject + 字体子集化（逐页 `pdf_graft_page`） | 8.39 MB | 原页渲染像素级不变（差异仅水印区域 0.73% 像素），文字层含原文 + `水印测试`；体积翻倍来自逐页各拷一份源页字体/图片 |
| **当前：共用 `pdf_graft_map` + `do_garbage>=3`** | **4.70 MB**（4,924,131 B） | 源页字体/图片只拷一次，输出仅比源大 63 KB（水印内容流 + 字体子集）；9 页 `iw8ben.pdf` 190,688 B → 167,623 B |

`do_garbage` 的语义要注意：`1` = 回收未引用对象、`2` = 重新编号、`3` = **额外去掉重复对象**。
只写 `2` 时"去重"完全不会发生，体积不会有任何变化；装配/合并/拆分/抽取/水印五条写入路径统一用 `3`。

`render` 目前输出 BMP（骨架阶段零依赖）；接 web 壳时改成由 C ABI 直接吐 RGBA8888 给前端，
不需要落盘。

---

## 三、两件都装好之后

```powershell
.\build\mupdf\bin\pdfword_cli.exe doctor
```

本机当前输出（三项都绿）：

```
PDF 后端: mupdf（可用）
LibreOffice: D:\LibreOffice\program\soffice.exe
LibreOffice 版本: LibreOffice 26.8.3.1 861effc5289d539f15c4c18d1ba8579ffd78728b
```

此时 UI 里：双击 PDF 能预览，选文件点"开始转换"能出 pdf/docx，进度条和日志走的是
`JobQueue` 的同一套回调。整条 pdf → docx → pdf 往返已实测（82 页 → 726 段 → 82 页）。
