# Pdf-Toolkit

**English** | [简体中文](README.md)

A PDF / Word conversion and preview tool built with C++ and Qt. **All the capability lives in
`core`, which is isolated from Qt**; Qt only draws the UI, and the same pipeline is also exposed
through a CLI and a C ABI so a web shell can be dropped on top later.

> The product name is **Pdf-Toolkit**; the technical identifiers in the project still use
> `pdfword` (target / exe names `pdftoolkit.exe`, `pdfword_cli.exe`, `pdfword.dll`, the header
> directory `pdfword/`, the CMake options `PDFWORD_*`, the config `pdfword.ini`, the environment
> variables `PDFWORD_*`, the repo directory `D:\media\pdfword`). The product rename deliberately
> left those identifiers alone so the web shell's C ABI, the scripts and the existing build
> directories did not have to change.

```
pdfword/
├── core/           pure C++17, no Qt, no third-party libs (MuPDF optional)
│   ├── include/pdfword/    public headers (result / config / pdf_backend / converter / job ...)
│   ├── src/                implementation (LibreOffice call, OOXML writing, PDF backend, OCR engine)
│   └── capi/               stable C ABI (pdfword.dll), the web shell's entry point
├── ui/            Qt 6 Widgets: dockable file panel + PDF preview + menu-based tools (calls core only)
│   └── resources/           logo (pdftoolkit.png / .ico) + icons.qrc + pdftoolkit.rc
├── cli/           command line: doctor / convert / info / text / render / merge / split / watermark / imgwatermark / ocr / outline / set-outline
├── tests/         smoke tests (no external dependencies, runnable via ctest)
├── cmake/         FindMuPDF.cmake
├── config/        pdfword.ini template (all external dependency paths are injected here)
└── docs/          architecture notes + dependency setup steps
```

> **Read before releasing**: `-DPDFWORD_WITH_MUPDF=ON` statically links **MuPDF (AGPL-3.0 or a
> commercial Artifex license)** into `pdftoolkit.exe` / `pdfword.dll` / `pdfword_cli.exe`. If you
> distribute those binaries you must release the complete source of the whole application under
> AGPL-3.0, or buy a commercial license from Artifex first.
> See "[License and the MuPDF constraint](#license-and-the-mupdf-constraint-read-before-releasing)" at the end.

## UI language (Chinese / English)

The app ships with both a Chinese and an English UI. **It follows the system language by
default**: a Chinese Windows shows Chinese, anything else shows English. You can also pin a
language by hand, and the choice is remembered.

- **Menu switch**: the rightmost top-level menu is `语言(L)` → `Language(L)` with three
  mutually exclusive radio items: `Follow system` / `简体中文` / `English`, the active one ticked.
  Switching **rebuilds the whole window's captions on the spot** — no restart; the file list,
  the previewed file, the current page and the zoom level are carried over unchanged.
- **Config file**: one line in `pdfword.ini`, `language=auto|zh|en` (`auto` means follow system;
  `zh_CN` / `en_US` style values are accepted too). The ini is looked up as: the file named by
  the `PW_CONFIG` environment variable > `pdfword.ini` next to the exe. The menu switch rewrites
  only that one line and leaves the rest of the config untouched.
- **Coverage**: not just widgets. Menus, dialogs, buttons, the status bar, the log pane, the
  bookmark panel, plus the error/status messages produced by `core` (`PDF backend: …`,
  `page N out of range (this file has M page(s))`, the `_edited.pdf` output-name suffix) all live
  in the same table — so CLI and C ABI output follow the language too.
- **Translation table**: `core/src/i18n_table.cpp` (Chinese source text → English, one
  `{zh, en}` entry per line). Lookup is **keyed by the Chinese source text**; a miss falls back to
  the Chinese original, so an untranslated string shows Chinese instead of going blank or crashing.
  To add a string, wrap the Chinese literal in `pw::i18n::t("…")` (core) or `TR("…")` (ui) and
  append one English row to the table.

## Building

```powershell
# Verified: VS2022 x64 + Qt 6.12.0 msvc2022_64
cmake -S . -B build/msvc-x64 -G "Visual Studio 17 2022" -A x64 `
      -DCMAKE_PREFIX_PATH=D:/Qt/6.12.0/msvc2022_64
cmake --build build/msvc-x64 --config Release --parallel

# or use the presets
cmake --preset msvc-x64
cmake --build --preset release
```

Artifacts land in `build/msvc-x64/bin`: `pdftoolkit.exe`, `pdfword_cli.exe`, `pdfword.dll`,
`pdfword_tests.exe`.
(The GUI target is still called `pdfword_ui`; `OUTPUT_NAME` makes the artifact
`pdftoolkit.exe`. The CLI and the DLL keep the project identifier `pdfword_cli.exe` /
`pdfword.dll`.)

Qt runtime: `D:\Qt\6.12.0\msvc2022_64\bin\windeployqt.exe --release build\msvc-x64\bin\pdftoolkit.exe`

### Application icon (logo)

The icon is compiled into the binary, so no extra image files travel with a release:

| File | Purpose |
| --- | --- |
| `ui/resources/pdftoolkit.png` | 256×256, transparent background. Used by `QApplication::setWindowIcon()`, the main window and the About box (`ui/AppIcon.cpp` loads `:/icons/pdftoolkit.png`) |
| `ui/resources/pdftoolkit.ico` | 16/24/32/48/64/128/256 in one file, for the taskbar, title bar and Alt-Tab |
| `ui/resources/pdftoolkit.rc` | `IDI_ICON1 ICON "pdftoolkit.ico"`, compiled into `pdftoolkit.exe` only — this is **the file icon Explorer shows** |
| `ui/resources/icons.qrc` | Qt resource manifest (`set(CMAKE_AUTORCC ON)` compiles it) |

To change the logo, replace those two images (keep 256×256 with a transparent background, and a
multi-size ico) and rebuild; no code changes needed. One smoke assertion checks that a 32×32
pixmap can be produced from the window icon, so a broken image fails immediately.

### Full build with MuPDF (all PDF capability enabled)

MuPDF comes from vcpkg's `libmupdf` port; build with `PDFWORD_WITH_MUPDF=ON` once it is installed:

```powershell
D:\vcpkg\vcpkg.exe install libmupdf:x64-windows

cmake -S . -B build/mupdf -G "Visual Studio 17 2022" -A x64 `
      -DCMAKE_PREFIX_PATH=D:/Qt/6.12.0/msvc2022_64 `
      -DCMAKE_TOOLCHAIN_FILE=D:/vcpkg/scripts/buildsystems/vcpkg.cmake `
      -DPDFWORD_WITH_MUPDF=ON
cmake --build build/mupdf --config Release --parallel
```

When GitHub is unreachable, `scripts/vcpkg-mirror.ps1` routes the vcpkg downloads through a mirror:

```powershell
pwsh -File scripts\vcpkg-mirror.ps1 -Packages libmupdf:x64-windows
```

vcpkg's `x64-windows` is the dynamic triple: dependent DLLs (harfbuzz, freetype, jpeg62, openjp2,
z, bz2 …) are copied next to the exe during the build, no manual steps needed.

### OCR engine build prerequisite (Windows built-in OCR)

`core/src/ocr_windows.cpp` calls `Windows.Media.Ocr` through C++/WinRT; during configuration CMake
looks for `cppwinrt\winrt\base.h` inside the Windows SDK. Candidate roots are
`%WindowsSdkDir%\Include`, `%SystemDrive%\Program Files (x86)\Windows Kits\10\Include`,
`%SystemDrive%\Program Files\Windows Kits\10\Include`, plus machines like this one where the SDK
lives in `D:\Windows Kits\10\Include`; it globs `10.*` in each and takes the newest.

- Found → defines `PDFWORD_HAVE_WINRT_OCR=1`, links `WindowsApp.lib`, and prints
  `OCR: using the built-in Windows OCR (cppwinrt=…)` during configuration;
- Not found → prints `OCR: cppwinrt directory not found in the Windows SDK, skipping Windows OCR`,
  `makeOcrEngine()` returns null and `ocr()` answers `BackendMissing` (every other PDF feature is
  unaffected).

### Path encoding: filenames in any language open correctly

The whole pipeline is UTF-8, and it stays lossless **from the entry point** — if that step drops
characters, nothing downstream can recover them:

- **CLI**: on Windows `main(char**)` receives the system ANSI code page (CP936 on Chinese systems).
  Any character that code page cannot express (kana, Cyrillic, Thai, emoji …) becomes `?` and the
  conversion is **irreversible**. The CLI therefore uses `wmain`, which receives UTF-16, and converts
  to UTF-8. Console output is switched to `SetConsoleOutputCP(CP_UTF8)` as well.
- **Qt**: `QString::toStdString()` returns the local 8-bit encoding, not UTF-8. Paths go through
  `pw::fsPath(QString.toUtf8().toStdString())`.
- **core**: `std::string` is UTF-8 only; into `fs::path` via `pw::fsPath()`; out to logs via `pw::fsUtf8()`.
- **C ABI**: `pdfword.dll` promises “all strings are UTF-8” (`core/capi/pdfword_c.h`).
- **MuPDF 1.26**: the narrow-path API is `fopen` underneath and reads names in the system code page,
  so it is bypassed entirely: open = `fz_open_file_w` → `pdf_open_document_with_stream` (generic:
  `fz_open_document_with_stream` + magic); save = `_wfopen(L"wb")` + `fz_new_output_with_file_ptr` +
  `pdf_write_document` (1.26 has no `pdf_save_document_to_output`); images = `fz_open_file_w` +
  `fz_read_all` + `fz_new_image_from_buffer`. Call `fz_close_output` before `fz_drop_output`.
- **`fstream`**: `std::ofstream(path)` is a narrow-path call too, so `zip_writer`, `office_runner`
  and the CLI exports open with wide paths.

### Very long paths and the 255-character segment limit

Chinese or Japanese filenames push a path past the 260-character `MAX_PATH` very easily. All IO
uses `pw::fsPathLong()`, which **prepends `\\?\` once the path exceeds 240 characters** (UNC becomes
`\\?\UNC\`) and does not depend on the system-wide `LongPathsEnabled` switch. Verified with a
267-character path (ten Chinese-named directories plus a 150-character filename): read, write and
render all work.

Windows has a second limit that `\\?\` cannot lift: **one path segment (one file name) may hold at
most 255 characters**. Exceeding it now reports
`无法写文件，名称 256 字符，超过 255 或路径非法` instead of dumping a truncated, mangled path.
(Emoji take two UTF-16 code units and blow the limit first; the reported count is UTF-16 code units.)

### Special characters and LibreOffice URIs

Spaces, `#`, `%`, `&`, `+`, `'`, brackets and emoji in filenames all work, because:

- arguments to external processes are quoted by the MSVCRT rules (`\` only escapes inside quotes when
  adjacent to a quote or the end, so a directory argument ending in `\\` no longer breaks the quote);
- LibreOffice's `-env:UserInstallation` is a `file:///` URL, and every non-reserved character in the
  path is **percent-encoded**. With a Chinese user name (`C:\\Users\\张三\\…`) or a working directory
  containing spaces or `#`, soffice rejects an unencoded URL outright. Observed log:
  `-env:UserInstallation=file:///D:/.../%E4%B8%AD%E6%96%87%20%E5%B7%A5%E4%BD%9C%23%E7%9B%AE%E5%BD%95/lo-profile`.

### Compatibility test results (this machine, Chinese code page)

| Case | Result |
| --- | --- |
| Japanese / Korean / Russian / Arabic / Thai / Vietnamese filenames: `info` + `extract` + `render` | 21/21 passed |
| `emoji😀&%#+'()【】文件.pdf`, same three steps | passed |
| 267-character deep path (ten Chinese directories + 150-char name): read / write / render | passed |
| 256-character output segment | clear error (256 > 255) |
| Japanese docx → pdf with a working dir containing Chinese, spaces and `#` | passed, 436 KB |
| `emoji😀&%#+'` docx → pdf | passed, 443 KB |
| UI smoke with a Japanese-named PDF | `failures=0`, 26 screenshots |

### Locating LibreOffice (the prerequisite for docx ⇄ pdf)

The `LibreOffice: …` line in the GUI and CLI is the probe result. Candidates are tried in this order,
and the first one that actually runs wins:

1. `libre_office=<soffice.exe or the LibreOffice directory>` in `pdfword.ini`, **next to the exe**;
2. the `PW_LIBREOFFICE_PATH` environment variable;
3. portable layouts: `LibreOffice\program\soffice.exe` beside the exe, or `soffice.exe` beside it;
4. the registry, `HKLM` / `HKCU` × 64/32-bit views:
   `SOFTWARE\LibreOffice\UNO\InstallPath` and `...\CurrentVersion\App Paths\soffice.exe`;
5. `LibreOffice\program\soffice.exe` under `%ProgramFiles%`, `%ProgramFiles(x86)%`,
   `%ProgramW6432%`;
6. **drives C–H × {`\LibreOffice`, `\Program Files\LibreOffice`,
   `\Program Files (x86)\LibreOffice`}**;
7. every `LibreOffice*` subdirectory under `%ProgramFiles%`, `%ProgramFiles(x86)%` and
   `%LOCALAPPDATA%\Programs` (covers versioned folder names such as `LibreOffice 7.6`);
8. finally `soffice.exe` / `soffice.com` on `PATH`.

Step 6 is not optional: LibreOffice may live on any drive. On this machine it is installed at
`D:\LibreOffice`, and an older build that only checked `Program Files` reported it as
“not configured”, which made docx ⇄ pdf unavailable.

When nothing is found, the message states the absolute ini path plus the candidates already tried:

```
LibreOffice: not configured (docx <-> pdf conversion unavailable)
  put: libre_office=<soffice.exe or the LibreOffice directory> into D:\media\pdfword\publish\pdfword.ini
  or set PW_LIBREOFFICE_PATH
  tried: C:\LibreOffice\program\soffice.exe, C:\Program Files\LibreOffice\program\soffice.exe ... 16 places
```

The smallest usable `pdfword.ini`:

```ini
libre_office=D:\LibreOffice\program\soffice.exe
```

**LibreOffice is optional**: PDF preview, split, merge, extract, rotate, crop, insert pages,
watermark, encrypt/decrypt, compress, OCR, bookmarks, annotations, in-place text editing and
image-to-PDF all run on MuPDF and do not touch LibreOffice; `pdf → docx` is MuPDF plus the bundled
`docx_writer` and needs it neither. **Only `docx / doc → pdf` requires LibreOffice.**

To turn the probe off entirely (pure PDF deployment, or a machine that will never convert documents),
set one environment variable:

```powershell
setx PW_LIBREOFFICE_DISABLE 1     # takes effect in a new terminal / after restarting the app
```

The report then reads “LibreOffice: disabled … (PDF preview and everything else unaffected)”, and
because nothing is scanned the startup probe drops from ~440 ms to ~150 ms. Asking for
`docx → pdf` returns a message with the ini path instead of hanging.

Verified anchors: `pdfword_cli.exe doctor` reports
`LibreOffice: D:\LibreOffice\program\soffice.exe` on a machine with 26.8 installed, and
`pdfword_cli.exe convert 中文名.pdf 中文名.docx --to docx` plus the return trip `docx → pdf`
both produce files.

## Running the GUI: pick the right bin directory first

The two build directories produce artifacts with the **same names**, so double-clicking the wrong
one shows "PDF backend: stub (unavailable)":

| Directory | CMake option | `pdftoolkit.exe` size | PDF backend shown in the UI |
| --- | --- | --- | --- |
| `build\mupdf` | `PDFWORD_WITH_MUPDF=ON` | ~37.8 MB (37,752,832 B) | `mupdf (available)` |
| `build\msvc-x64` | default (OFF) | ~0.5 MB | `stub (unavailable)` |

```powershell
pwsh -File scripts\run-ui.ps1              # starts the GUI with MuPDF (the 36 MB one)
pwsh -File scripts\run-ui.ps1 -Build skeleton   # skeleton GUI, only docx/pdf conversion
```

Size is the quickest way to tell them apart: tens of MB = MuPDF statically linked; a few hundred
KB = skeleton.

The window title is `Pdf-Toolkit — PDF/Word 转换与预览`. **On first start** the window is placed by
measuring the primary screen's available area and centred; on a 1536×864 screen like this one
(816 px above the taskbar) the window shrinks to 1280×752 so it never covers the taskbar or hangs
off the edge. Later starts reuse the geometry saved on the previous close (see "View state is
remembered" below); to go back to centred, delete `%APPDATA%\Pdf-Toolkit\Pdf-Toolkit.ini`.
The working directory defaults to `%LOCALAPPDATA%\Pdf-Toolkit\work`
(it used to be `%LOCALAPPDATA%\PdfWord\work`; the rename does not migrate anything, the old
directory is simply left alone).

> If you keep the standalone `publish\pdftoolkit.exe` copy (Qt DLLs included, the whole folder can
> be handed to someone), remember that it does **not** follow `build` automatically: after changing
> code you have to
> `Copy-Item build\mupdf\bin\pdftoolkit.exe publish\pdftoolkit.exe -Force`.
> Close any running instance first, otherwise you get "file in use".

**GUI smoke test** (no screen needed; it really constructs the main window and the four dialogs and
runs one password-protected conversion job):

```powershell
# turn the test target on once, then build and run
& "C:\Program Files\CMake\bin\cmake.exe" -S "D:\media\pdfword" -B "D:\media\pdfword\build\mupdf" -DPDFWORD_BUILD_UI_SMOKE=ON
& "C:\Program Files\CMake\bin\cmake.exe" --build "D:\media\pdfword\build\mupdf" --config Release --target pdfword_ui_smoke --parallel
.\build\mupdf\bin\pdfword_ui_smoke.exe "C:\Users\Lenovo\Desktop\tst\iw8ben.pdf"
# then with the password argument, to prove JobQueue::enqueue really passes it into ConvertOptions
.\build\mupdf\bin\pdfword_ui_smoke.exe ".\build\mupdf\out-test\enc_888888.pdf" 888888 ".\build\mupdf\out-test\smoke_pw.docx"
```

All output `[ OK ]` and exit code 0 means it passed (**198 assertions + 27 screenshots**). With the
password sample there are 3 extra job assertions, and the 2 "valid range" checks are replaced by 1
"an encrypted file whose page count cannot be read must be rejected". Coverage:

- The main window has 6 top-level menus (File / Convert / PDF Tools / Edit & Annotate / View /
  Help); the title starts with `Pdf-Toolkit`; the startup position is centred (within 12 px of the
  available-area centre); **there is no toolbar at the top** (`findChild<QToolBar*>()` is null, the
  navigation moved into the status bar); all sixteen entries (Start conversion / Merge / Split /
  Extract / Page editor ‖ Metadata / Encrypt-decrypt / Compress / Watermark / Images to PDF / OCR ‖
  Edit text & images / Annotations / Add hyperlink / Find & replace / Forms) are in the menus, and
  the PDF Tools menu no longer has "Document tools…"; the View menu is two-level: `File list`
  (show / hide ‖ left / right), `Log window` (show / hide), `Bookmarks` (show / hide), `Zoom`
  (zoom in / zoom out ‖ fit width / fit window / actual size)
- **Bottom navigation row**: `pageNav`'s parent is `statusBar()` (the same container as the `0%`
  progress bar), their centre lines are within 14 px and both hug the bottom of the window; it
  holds 4 buttons + the page box + the fold button; the page box range is 1..pageCount,
  `setValue` to the last page really changes `currentPage`, and setting 1 jumps back to the first
  page; after opening a document "Next page" and "Zoom in" are really enabled (not greyed out)
- **Fold button**: `navFoldButton` sits at the far right; one click makes the 4 buttons + page box +
  percentage all `!isVisible()`, another click brings them back
- **File list show/hide**: "Hide file list" makes `fileDock` `isHidden()`; "Show file list" brings
  it back and the check mark follows
- **Log window show/hide**: the centre is a vertical QSplitter, the log sits in the lower half by
  default (initial 640:140, draggable); "Hide" makes it `isHidden()`, the splitter height goes to
  zero and the preview fills the area; "Show" restores the previous height
- **Continuous scrolling**: all pages sit in the canvas (`pageItemCount() == pageCount()`, one
  `pageItem` widget per page); the stack is taller than the viewport, `verticalScrollBar()->maximum() > 0`;
  dragging to the bottom makes `currentPage` the last page, dragging back to the top returns to
  page 1; only pages near the viewport are rendered and distant pages drop their bitmaps (an 80-page
  document can be scrolled end to end without eating memory)
- **Zoom and scroll**: at 300% both scroll bars have `maximum() > 0` and can reach the middle of the
  page; at 50% the horizontal one is zero; after "fit width" the horizontal `maximum() == 0` and the
  canvas does not exceed the viewport; after "fit window" there is no horizontal bar and one whole
  page fits the viewport height (later pages continue below); posting a `Ctrl + wheel` event to the
  viewport really increases the zoom factor
- **View state persistence**: the smoke test points QSettings at
  `%TEMP%\pdfword-ui-smoke-settings` (the real layout is untouched); it switches to right-docked,
  folds the navigation row, hides the log, hides the file list and hides the bookmark panel, then
  `window.close()` triggers the save, and a second `MainWindow` is created: the assertions are that
  the log is still hidden, the file list is still hidden, the bookmark panel is still hidden,
  `dockRightAction` is still checked, `isNavCollapsed()` is still true, and the INI directory really
  exists (the bookmark panel is shown again at the end so the next start is not missing a panel)
- **Drag and drop**: the main window has `acceptDrops`; dropping a PDF `QDropEvent` straight onto the
  window really adds it to the list; the same file dropped twice is de-duplicated; unsupported types
  such as `.ini` are not added but the log says "ignored"; the merge window and the images-to-PDF
  window both use `DropListWidget` (each accepts only its own extensions)
- The pending-files panel is a QDockWidget and may only dock left/right; choosing "right" in the menu
  really moves the dock area to the right side and "left" moves it back; the list's
  `contextMenuPolicy` is `CustomContextMenu`, and posting a `customContextMenuRequested` really pops
  a `QMenu` (closed by a timer in the test)
- The merge window's file list, output path and default output name
- The extract window's source drop-down, default output name, and live validation: `1-2` resolves to
  2 pages; `1-99999` immediately reports out of range and clears the cache; for an encrypted file
  (the UI cannot read its page count) any range is rejected, so an absurd page number never gets
  through
- The conversion window's two target formats and the summary switching with pdf/docx (skip PDF /
  non-PDF cannot become docx)
- The page editor and the eleven document-level windows (Metadata / Encrypt-decrypt / Compress /
  Watermark / Images to PDF / OCR ‖ Annotations / Hyperlink / Find & replace / Replace image / Forms)
  all construct, and their representative widgets exist (title box, open-password box, source-size
  label, watermark text box, image list, OCR language combo)
- The watermark window's `watermarkTabs` has a text and an image tab; the image tab has a path box,
  width ratio and tile switch, the opacity control is shared by both tabs; the text tab's position
  drop-down has exactly 12 items and the first one is `top-left`
- The OCR window's `ocrLanguageCombo` has at least "auto" plus one real language (`zh-Hans-CN`
  here); both `ocrPdfCheck` / `ocrTxtCheck` exist and are checked by default; the default output
  names carry the `_可检索` / `_识别` suffixes; `ocrEngineLabel` shows the engine capability
  (available languages or the reason it is unavailable) and `ocrStartButton` exists
- A password-protected PDF→docx job finishes through JobQueue and the output is on disk
- **Bookmark / outline panel**: `bookmarkDock` and the tree exist, and the View menu has the show /
  hide pair for "Bookmarks"; hiding takes a `main-window-bookmark-hidden.png` screenshot and shows it
  again; when a sample is opened the number of entries in the tree is reconciled against core
  `outline()` (not hard-coded, so another sample still works); the tree has both top-level and nested
  children; **double-clicking an entry really jumps to that page** (the page box follows); move up /
  move down and back; delete one top-level entry, click "Apply", and `<name>_书签.pdf` is written —
  reading it back gives exactly the same count / pages / nesting as the panel

The fifth argument is the screenshot directory; the main window and each dialog are `grab()`bed there
as one PNG each (27 in total):

```powershell
.\build\mupdf\bin\pdfword_ui_smoke.exe "C:\Users\Lenovo\Desktop\tst\iw8ben.pdf" "" "" "D:\media\pdfword\build\mupdf\ui-shots"
```

`main-window / main-window-scroll / main-window-right / main-window-log-hidden / main-window-zoom /
main-window-nav-collapsed / main-window-bookmarks / main-window-bookmark-hidden / about-dialog /
merge-dialog / extract-dialog / convert-dialog / page-editor-dialog /
metadata-dialog / security-dialog / compress-dialog / watermark-dialog / watermark-dialog-image /
imgtopdf-dialog / ocr-dialog / annot-dialog / link-dialog / find-replace-dialog /
replace-image-dialog / form-dialog` **can be opened directly to inspect the layout** without a real
desktop (`main-window-scroll` is the seam between pages 1 and 2 — the light grey band in the middle
is the page boundary; `main-window-log-hidden` is the preview filling the area after the log is
hidden; `main-window-zoom` is 300% with the right/bottom scroll bars visible;
`main-window-nav-collapsed` is the bottom row folded down to a single `◂ 展开` button;
`main-window-bookmarks` is the "Bookmarks / Outline" panel open on the right (two columns, multi-level
indent, three rows of buttons); `main-window-bookmark-hidden` is after "Hide bookmarks" — the panel is
gone and the preview is wider; `main-window-inline-edit` is after editing a block of text in the
preview: the preview already switched to `<name>_已编辑.pdf`, it stays on the same page, and the log
says the block was rewritten on page 2)
(`QWidget::grab()` renders the widget itself, which sidesteps the fact that the local computer-use
screenshot path cannot capture this window). `watermark-dialog*` are the text and image tabs, kept
for visual comparison when the UI changes.

There is no permanent "start conversion / merge / split" button row any more; everything lives in the
menu bar, and "Pending files" is a dockable panel.

**Context menu of the pending file list** (`Qt::CustomContextMenu`; right-clicking an unselected row
selects it first):

| Item | Behaviour |
| --- | --- |
| Preview | PDF only, same as double-clicking |
| Delete | Removes the selected rows from the list (multi-select) — **the list only, nothing on disk**; the log states how many were removed |
| Clear | Empties the whole pending list — again **nothing on disk is touched** |
| Rename file… | Renames the real file on disk (single selection only). `\ / : * ? " < > \|` and trailing spaces/dots are rejected locally; an existing target name is refused; on success the list text, `Qt::UserRole` and the tooltip are all updated, and if that file is being previewed the preview reopens on the new path |

**Menu structure**

| Menu | Items | Purpose |
| --- | --- | --- |
| File(&F) | Add files… / Remove selected / Clear list / Exit | Add files to the pending list (PDF, docx, doc, odt, rtf, txt, xlsx, pptx) |
| Convert(&C) | Start conversion… (`Ctrl+Enter`) | First shows the **target format dialog**: file list + `pdf` / `docx` choice + output directory (empty = next to the source) + PDF password; only after confirming are jobs queued |
| PDF Tools(&P) | Merge PDFs… / Split PDF… / Extract pages… / Page editor… ‖ Metadata… / Encrypt / Decrypt… / Compress… / Watermark… / Images to PDF… / OCR text recognition… (the first four are page-level, the six after the separator are document-level, each opens its own window) | See the table below |
| Edit & Annotate(&E) | Edit text & images… / Annotations… / Add hyperlink… / Find & replace… / Forms… | See "Edit & Annotate menu items" below |
| View(&V) | **Two-level submenus**: `File list ▸` (show / hide ‖ left / right) + `Log window ▸` (show / hide) + `Bookmarks ▸` (show / hide) + `Zoom ▸` (zoom in / zoom out ‖ fit width / fit window / actual size 100%) | Panel visibility and dock side (you can also drag the panel out into a floating window); log visibility; bookmark panel visibility; zoom and fit |
| Help(&H) | Capability report | Shows backend / LibreOffice / OCR diagnostics (the LibreOffice version comes from the cache; before the first conversion it says "not detected") |
| Help(&H) | About Pdf-Toolkit… | Short description + version (taken from `project(Pdf-Toolkit VERSION …)`, currently 0.0.3) + contact / e-mail (the e-mail is a link that opens the default mail client) |

**PDF Tools menu items**

| Item | Purpose |
| --- | --- |
| `Merge PDFs…` | Opens a **standalone merge window**: list order is the concatenation order (Add PDFs… / Remove selected / Move up / Move down), output path + browse; at least 2 files are enforced before OK; the default name is `<first>_等N个_合并.pdf` |
| `Split PDF…` | Pick the source PDF → pages per file → output directory, named `filename_p1-3.pdf` |
| `Extract pages…` | Opens the **extract window**: source PDF drop-down (any PDF in the list, or browse for a new one) + page range (`1-3,5`, full-width commas accepted) + an "All pages" button; it shows "N pages total, M will be extracted (1-3,9)" live and validates out-of-range input as you type |
| `Page editor…` | Delete / move up, down, to top, to bottom / rotate / crop / insert blank page / insert pages from another PDF, then `Save as…` |
| `Metadata…` | Standalone window: title / author / subject / keywords; "Read current" fills in the existing values, "Write and save as…" produces `_元数据.pdf` |
| `Encrypt / Decrypt…` | Standalone window: AES-128 encryption (open password + owner password + four permission bits for print/copy/modify/annotate); decryption reads the "open password" field and saves `_解密.pdf` |
| `Compress…` | Standalone window: shows the source size, one-click "Compress and save as…" (font subsetting + stream compression + garbage collection), then the same window shows before → after |
| `Watermark…` | Standalone window with **text and image tabs**, shared opacity (0.02–1.00, text default 0.15): the text tab takes the content (default "内部资料") + a **position drop-down (12 options, default top-left)**; the image tab takes PNG/JPG/BMP/TIFF + width as a fraction of the page width (0.05–1.00, default 0.30) + "tile grid" (spacing = 1.6× the watermark width). "Add watermark and save as…" writes `_水印.pdf` |
| `Images to PDF…` | Standalone window: a list of images (order = page order) + add/remove + "scale proportionally into A4", producing one PDF |
| `OCR text recognition…` | Standalone window: the engine capability at the top (`Windows OCR (available): zh-Hans-CN` here) + a **recognition language drop-down** (auto / languages the engine has, Chinese preselected when present) + two switches "output a searchable PDF (invisible text layer)" `_可检索.pdf` and "export plain text (.txt, UTF-8)" `_识别.txt`, both paths browsable. "Start recognition" runs on a thread pool (busy progress bar) and the window cannot be closed while it runs |

**Edit & Annotate menu items**

| Item | Purpose |
| --- | --- |
| `Edit text & images…` | First asks "add/delete/change text / replace image": text goes to the find & replace window, images to the replace-image window (a white rectangle covers the old image, then the new one is scaled into it) |
| `Annotations…` | 7 annotation types (highlight / underline / strikeout / squiggly / sticky note / text box / ink) with colour, opacity and author; the text box and note take a body text |
| `Add hyperlink…` | In-document jumps (`第3页` / `P3` / `3` normalised to `#page=N`) or external URLs (`https://…` / `mailto:…`) |
| `Find & replace…` | "Find" first to see where hits are, then batch replace; delete-only, case-sensitive, a hit limit, and the colour/size of the new text |
| `Forms…` | Three tabs: fill (read fields → edit "value to set" → save as), create (field name/type/page/rect/choices → an interactive form), extract (table + CSV export, UTF-8 BOM so Excel opens it directly) |

All five windows are **modeless** (`show()`, not `exec()`): while they are open the main window still
responds and the preview still scrolls, so input is never swallowed by a modal loop (which is exactly
what caused the earlier "clicking does nothing" problem). Only one editing window exists at a time.

The four windows that need a position (annotations / hyperlink / replace image / create form) all have
a "select a rectangle in the preview" button: clicking it **folds the editing window away first**
(it is modal, so leaving it up would cover the preview) and the main window becomes usable; the
preview enters selection mode (cross cursor), you drag with the left button, and on release the page
number and four coordinates are filled into the window, which pops back up. If you do not drag within
30 seconds it returns automatically, so the window is never stuck hidden. Before selecting, it checks
"is the file open in the preview the one being edited" and refuses to drag on a mismatch, so the
coordinates cannot land on another page. You can also skip the rectangle entirely and type the four
coordinates (display space pt, origin top-left, y down).

**Editing directly in the preview** (no window, no coordinates: right-click on the page, pick an item,
drag a rectangle)

| Context menu item | After dragging | Calls |
| --- | --- | --- |
| `Highlight` / `Underline` / `Strikeout` / `Squiggly` | Applied straight away, no interruption | `annotate` |
| `Ink mark` | Draws a stroke along the rectangle | `annotate` (ink) |
| `Text box note…` / `Sticky note…` | One-line input, then written (**an annotation, not body text**) | `annotate` |
| `Hyperlink…` | Reads the "address" field (`第3页` / `https://…`) | `addLink` |
| `Edit the text here…` | The input box **is prefilled with the text inside the rectangle**; Enter writes it back | `replaceTextAt` |
| `Delete the text here (drag a box)` | Covers the area with white, writes nothing | `replaceTextAt` (`deleteOnly`) |
| `Add text here… (drag a box, then type)` | Types directly into the body text (Chinese, spaces, digits all fine) | `addTextAt` |
| `Replace the image here…` | Pick an image, scaled into the rectangle | `replaceImage` |
| `Add bookmark (pointing at this page)` | Adds an entry to the "Bookmarks / Outline" panel | `setOutline` |

- The rectangle is in **page display space** (`PdfViewWidget` converts it to pt using the current
  zoom), so 150% zoom or page 9 never shifts the target;
- Write-back is **by rectangle**, not by searching the whole document: if the same sentence appears
  several times only the block you boxed changes (`textInRect` is used only to prefill the input box;
  line breaks or hyphenation do not affect the write-back);
- The result goes to `<name>_已编辑.pdf` (the source is untouched); after writing the preview reloads
  automatically, stays on the same page, and the derived file is added to the pending list so merge /
  split can pick it straight away. When you are already editing `_已编辑.pdf`, this round is written to
  `_编辑中_<ms>.pdf` first and then renamed over the target — **the preview currently has that file
  open**, and on Windows it cannot be deleted, so the preview is released first
  (`viewer_->closeDocument()`);
- The height of an added-text box is the font size (`fontSize=0` uses 0.82× the box height); text
  wider than the box shrinks the font instead of squashing it horizontally;
- **The cost (inherent to rectangle-based text editing, not a bug)**: the white rectangle only covers
  visually, the old text is still in the PDF, so `find` and copy-paste still return the old words.
  Truly erasing needs redact, and on documents like these redact also deletes neighbouring glyphs.
  To really clean it, use "Find & replace" with delete-only, or rebuild that page in the page editor.

The same capability is callable elsewhere, which is what the scripts and the web shell use:

| Layer | Entry point |
| --- | --- |
| core | `IPdfBackend::replaceTextAt(input, output, page, rect, text, opts, password)`, `addTextAt(...)` (`page` is 0-based, `opts.deleteOnly` covers without writing, `opts.fontSize=0` auto-sizes) |
| CLI | `pdfword_cli replace-at <in> <out> <page 1-based> "x0,y0,x1,y1" "new text" [--delete-only] [--color #rrggbb] [--font-size N]`, `add-text <in> <out> <page> "rect" "text" [--font-size N]` |
| C ABI | `pw_pdf_replace_text_at(ctx, in, out, page, x0, y0, x1, y1, text, delete_only, color, font_size, password)`, `pw_pdf_add_text_at(...)` |

**Bookmarks / outline panel** (`View → Bookmarks`, right side by default, draggable to the left or
out into its own window)

A PDF's "bookmarks" and "outline" are the same thing (the `/Outlines` tree in the document), so there
is one tree and one set of operations:

| Button | Purpose |
| --- | --- |
| `Add sibling` / `Add child` | Inserts an entry after / below the selected one: title + page. A page of 0 means "no jump target", used for pure group headings |
| `Edit` | Changes the title and page of the selected entry |
| `Delete` | Removes the selected entry (and its children) |
| `Move up` / `Move down` | Reorders within the same level, never across levels |
| `Apply` | Writes the panel's tree back to the PDF as `<name>_书签.pdf`; the log states how many entries were written |
| `Export…` | Saves `.txt` (one `level<TAB>page<TAB>title` per line, same format as CLI `outline --txt`) or `.json` (fields matching the C ABI) |

- The tree shows the outline of whichever PDF the preview has open, and switching documents via the
  file list rebuilds it;
- **Double-clicking an entry jumps straight to that page**, and the page box at the bottom follows;
- Levels can be raised or lowered freely; when writing back, a skipped level is clamped to
  "previous level + 1" and the first entry is forced to the top level, so no broken outlines are
  written;
- In a narrow panel titles are ellipsised, hover shows the full text;
- Panel visibility is stored in the INI and reused next start (`saveState()` re-shows hidden docks,
  so visibility is stored separately as `view/bookmarkVisible`);
- Write-back goes through MuPDF's outline iterator (1.26 has no `pdf_document_update_toc`): the old
  tree is deleted and each entry inserted; `uri` is `#page=N` so MuPDF generates the `/Dest` itself,
  which is why external-link bookmarks (`http://…`) become plain group headings after write-back.

**Layout**: the centre is a vertical `QSplitter` — PDF preview on top, read-only log below (initial
640:140, drag the handle to resize). `View → Log window → Hide` collapses the log and the preview fills
the whole centre; `Show` restores the previous height. `View → File list → Hide file list` collapses
the left panel and the preview widens accordingly; `Show file list` brings it back.
**There is no toolbar at the top of the window**; all navigation moved into the **bottom row** — the
status bar — on the same line as the `0%` progress bar: on the left the permanent backend status
(`PDF backend: mupdf (available)`), towards the right
`Previous / Next / Zoom in / Zoom out / [page] / total pages / percentage`, and at the far right the
`收起 ▸` button plus the progress bar. The preview does not give up a row of height for it and no
floating bar covers the page.
Click `收起 ▸`: those buttons and the page box hide, leaving only `◂ 展开` (the row height is
unchanged so the bottom row does not jump); click again to bring them back.

| Bottom row | Purpose |
| --- | --- |
| `PDF backend: mupdf (available)` | Permanent backend status, so you can tell at a glance whether you are running the MuPDF build or the stub skeleton |
| `Previous` / `Next` (also `PageUp` / `PageDown`) | Page navigation, landing at the top of the page; the buttons grey out at the first/last page |
| `Zoom in` / `Zoom out` (`Ctrl++` / `Ctrl+-`) | Fixed steps: 8/10/15/25/33/50/67/75/100/125/150/175/200/250/300/400/600/800% |
| Page input box + ` / N` | **Type a page and press Enter (or leave the field) to jump there**; the range is clamped to 1..pageCount |
| Zoom percentage | Current factor, so 150% and 300% are obvious at a glance |
| Progress bar + `0%` | Job progress (conversion / merge / split …), same row as the buttons above |
| `收起 ▸` / `◂ 展开` | Folds or expands those buttons (the state is remembered for the next start) |

The progress bar and backend status live in the status bar; the "Pending files" panel has its own
Add files / Remove selected buttons and the hint "double-click a PDF to preview; you can drag files
here"; `View → File list` docks it on the left or right.

**View state is remembered**: the current layout is written to the INI when the window closes and
restored on the next start, so no re-arranging every time.

| Remembered | Stored in | Notes |
| --- | --- | --- |
| Window size/position | `window/geometry` | Geometry at the last close; only when nothing is stored does "centre on start" apply |
| File list visibility / dock side / width / floating | `window/dockState` + `view/fileListVisible` | `saveState()` does not store hidden docks, so visibility is stored separately |
| Log visibility / height | `view/logVisible`, `view/logHeight` | The height at the time of hiding is kept and restored on show or next start |
| Bottom navigation row folded / expanded | `view/navCollapsed` | Reopening while folded stays folded, with a single `◂ 展开` |
| Bookmark panel visibility | `view/bookmarkVisible` | Works together with `window/dockState`: `saveState()` does not store hidden docks, so visibility is stored separately |

The file lives at `%APPDATA%\Pdf-Toolkit\Pdf-Toolkit.ini` (separate from the working directory
`%LOCALAPPDATA%\Pdf-Toolkit\work`). **Deleting that file restores the default layout**: centred window,
file list on the left and visible, log visible (640:140), bookmark panel on the right, navigation row
expanded.

**Zoom and continuous scrolling**: all pages are stacked vertically in one canvas (14 px light grey
gaps between pages); the wheel or the right scroll bar takes you all the way down — it is not
"one page at a time with buttons". Only pages near the viewport are rendered into bitmaps; scrolling
fills them in and distant pages release theirs, so an 80-page document does not eat hundreds of MB.
The current page follows "which page the top of the viewport is on", and the page box and page count
move with it; above the viewport width a horizontal scroll bar appears at the bottom, and it
disappears when the whole page fits.

| Action | Effect |
| --- | --- |
| Bottom row `Zoom in` / `Zoom out` (`Ctrl++` / `Ctrl+-`) | Fixed steps, repeated clicks stop on a step |
| `Ctrl + wheel` (inside the preview) | Zooms around **the pointer position**, so what is under the cursor stays put |
| `Fit width` (`Ctrl+1`) | Page width fills the viewport and horizontal scrolling is no longer needed (the vertical scroll bar width and page margins are reserved, so no second bar pops in) |
| `Fit window` (`Ctrl+0`) | One whole page fits the viewport height; the other pages are still below, scroll to see them |
| `Actual size` (`Ctrl+Alt+0`) | Back to 100% (zoom=1, one page at 72 dpi, native pixels) |

**Drag and drop**: dropping files from Explorer onto **anywhere in the main window** (preview, log,
left panel) adds them to the pending list; dropping a single PDF also opens it in the preview.
Extensions are filtered to `pdf/docx/doc/odt/rtf/txt/xlsx/pptx`, duplicate paths are de-duplicated, and
unsupported types are not added but the log says "ignored N unsupported files" instead of swallowing
them silently. The `Merge PDFs…` list accepts PDFs only and the `Images to PDF…` list accepts
PNG/JPG/BMP/TIFF/GIF only; both accept drops directly.

Menu items that need "some PDF" (split / extract / page editor / metadata / encrypt-decrypt /
compress / watermark / OCR) pick the source in this order: **the PDF currently previewed → the only
selected PDF in the list → a drop-down when several → a log message when there is none**.
`Images to PDF…` needs no source PDF; it only asks you to add images in its window first.

The "output page order" list in `Page editor…` is the final page sequence: deleting a page removes a
row, reordering moves rows, rotating/cropping marks that row; files are only written when you save.
`Esc` or `Close` leaves the source untouched.

## Packaging and installers

One command produces both deliverables (`packaging\build-installer.ps1`):

```powershell
# Prerequisite: publish\ has been deployed by windeployqt (GUI exe + Qt DLLs + plugin directories)
pwsh -File packaging\build-installer.ps1
```

| Artifact | Contents | Use |
| --- | --- | --- |
| `dist\Pdf-Toolkit-<ver>-win64-portable.zip` | Main program + runtime (directory layout preserved) | Portable build: unzip and double-click `pdftoolkit.exe` |
| `dist\Pdf-Toolkit-<ver>-win64-setup.exe` | Single-file installer compiled by Inno Setup | Normal install: wizard, Start-menu / desktop shortcuts, uninstall entry |

Details:

- The version is read from `project(Pdf-Toolkit VERSION …)` in `CMakeLists.txt`, the same value the
  About box shows, so there is only one place to change.
- Only the **GUI program** is packaged: `pdfword_cli.exe` / `pdfword.dll` are not included (they exist
  for the command line and the web shell; to ship them too, edit `$exclude` in
  `build-installer.ps1`).
- The install defaults to `%LOCALAPPDATA%\Programs\Pdf-Toolkit` with `PrivilegesRequired=lowest`, so
  **no UAC prompt**. To install into Program Files, change `PrivilegesRequired` to `admin` in
  `packaging\pdftoolkit.iss`.
- The installer UI is Chinese/English (`packaging\langs\ChineseSimplified.isl` is the Inno official
  translation) and follows the system language.
- The installer icon is `ui\resources\pdftoolkit.ico`, the same icon as the program.
- Rebuild only the installer, skipping the zip:
  `pwsh -File packaging\build-installer.ps1 -SkipPortable`.
- Silent install into a given directory (for self-testing):

  ```powershell
  .\dist\Pdf-Toolkit-0.0.3-win64-setup.exe /VERYSILENT /SUPPRESSMSGBOXES /NOICONS /DIR="D:\tmp\pdt"
  ```

### PDF association: showing up in the right-click "Open with" menu

The wizard has a task **"Add Pdf-Toolkit to the Open with menu for PDF files"** (checked by default;
`/TASKS="pdfassoc"` for silent installs). After installing:

- right-click a `.pdf` → *Open with* lists **Pdf-Toolkit**, with the program's own logo as its icon;
- Windows *Default apps* also offers it as a candidate for the default PDF handler.

**It only registers itself as a candidate and never takes over your current default** — on purpose:
an installer should not change your default apps behind your back. If you install a second copy over
the same `Applications\pdftoolkit.exe`, the later path wins.

Three registry locations are touched (`HKCU\Software\Classes` when `PrivilegesRequired=lowest`):

| Key | Purpose |
| --- | --- |
| `Applications\pdftoolkit.exe` (`FriendlyAppName` + `DefaultIcon` + `shell\open\command`) | how the "Open with" list recognises this exe; the name shown is `Pdf-Toolkit` |
| value `PdfToolkit.Document` under `.pdf\OpenWithProgids` | attaches our ProgId to `.pdf` candidates |
| `PdfToolkit.Document` (type name / icon / open command) | the ProgId itself; the command is `"<install dir>\pdftoolkit.exe" "%1"` |

Uninstall removes only its own entries (`Applications\pdftoolkit.exe` and `PdfToolkit.Document` as
whole keys, and just the `PdfToolkit.Document` value from `.pdf\OpenWithProgids`); candidates
registered by other programs are left untouched.

The installer sets `ChangesAssociations=yes`, so the Shell association cache is refreshed right away;
a few machines still need a re-login or a restart of Explorer.

**The open command passes a file on the command line**: `pdftoolkit.exe "C:\a.pdf"`. On startup the
program adds every file from the command line to the pending list and previews the first PDF;
opening several files at once works too (arguments are read with the wide-character
`CommandLineToArgvW`, so Chinese / Japanese / Arabic paths are never mangled). Dropping a file onto
the `pdftoolkit.exe` icon or its shortcut behaves the same.

## Current status (what really works and what does not)

Everything marked "verified" was run on a real machine against `build\mupdf`
(`PDFWORD_WITH_MUPDF=ON`, MuPDF 1.26.10 + LibreOffice 26.8.3.1), with samples
`D:\media\lecture_1.pdf` (63 pages, landscape) and `D:\media\lecture_10.pdf` (82 pages, portrait):

| Capability | Module | Status |
| --- | --- | --- |
| Project build / Qt GUI start-up | ui | ✅ Verified (title `Pdf-Toolkit — PDF/Word 转换与预览`, centred on start) |
| Menu-based UI refactor (six entries into menus + dockable file panel + three dialogs) | ui | ✅ Verified (`pdfword_ui_smoke` **passes all 198 assertions**; the 27 screenshots from `QWidget::grab()` were checked one by one for layout) |
| Direct editing in the preview (right-click → drag a box → replace / add text / annotate / hyperlink / replace image) | core + CLI + C ABI + ui | ✅ Verified (`iw8ben.pdf` page 2, box around `instructions for` replaced with `SMOKE-REPLACED`: output `<name>_已编辑.pdf`, `findText` hits page 2, and **page 9 text is byte-for-byte unchanged**; then a box on an empty spot of page 3 with `加字冒烟 ABC 123`, Chinese is searchable; CLI `replace-at` / `add-text` and the same C ABI functions each run once) |
| Centred start-up / product name Pdf-Toolkit | ui | ✅ Verified (window lands at (128,32), matching the available-area centre; title, the first CLI line and the docx metadata creator are all Pdf-Toolkit) |
| Bookmarks / outline management (read / add / edit / delete / move / export) | core + CLI + C ABI + ui | ✅ Verified (on the core side `fz_load_outline` is flattened and pages come from `fz_page_number_from_location` (unresolvable external links become 0 instead of relying on `uri == nullptr`); write-back uses the outline iterator to delete the old tree and insert one by one; `iw8ben.pdf` reads 12 entries over 3 levels, and 5 entries including Chinese are written and read back with identical levels and pages; pypdf cross-checks that the `/First`–`/Next` chain of `/Outlines` really points at `/Dest`; CLI `outline` / `set-outline` and C ABI `pw_pdf_outline` / `pw_pdf_set_outline` each run once; the UI side is covered by the smoke assertions above) |
| View state persistence (file list / log / bookmark panel visibility, dock side, log height, window geometry) | ui / QSettings | ✅ Verified (closing writes `%APPDATA%\Pdf-Toolkit\Pdf-Toolkit.ini`; a second window created in the same process comes back with hidden panels and the right dock side) |
| Consecutive in-place preview edits (12 edits in a row / 12 drag-and-write rounds / 6 full interaction rounds, none crash, none jump to page 1) | ui / PdfViewWidget + MainWindow | ✅ Verified (three things: page tops now computed by summing `pageHpt_` (no longer reading `QLabel::geometry()` before layout runs), `reloadAfterInline` restores the scroll position via `scrollValue`, and the rubber band is a `QPointer` with `clearPages()` cleanup and cross-page recreation; the reverse verification is in `docs/ARCHITECTURE.md`) |
| Continuous preview scrolling (all pages stacked + lazy rendering + scroll-inferred current page) | ui / PdfViewWidget | ✅ Verified (9-page and 63-page documents both pass the smoke test: page count matches, scrolling to the bottom moves the current page to the last page, only pages within ±400 px of the viewport are rendered; the 63-page round of 106 assertions finishes in 10 s) |
| DOCX generation (OOXML + zip) | core / DocxWriter | ✅ Verified (python-docx parses it: paragraph styles and escaping are correct) |
| Config probing / capability report | core / Config | ✅ Verified (`pdfword_cli.exe doctor` reports all three green) |
| Job queue and progress callbacks | core / JobQueue | ✅ Verified (the UI log and progress bar go through this path) |
| docx → pdf | core / OfficeRunner → LibreOffice | ✅ Verified (82-page docx → 82-page PDF, isolated profile and timeout process-tree kill both work) |
| doc → docx and similar conversions | same | ✅ Same pipeline (LibreOffice `--convert-to`) |
| PDF info / render / text | core / MuPDF backend | ✅ Verified (63 pages 842×595 pt; render 1263×893; Chinese output as UTF-8) |
| PDF merge / split | core / MuPDF backend | ✅ Verified (graft moves pages: 20+20 → 40 pages; 63 pages split into 4 files of 20) |
| Page-level assembly: extract / delete / reorder / rotate / crop / insert blank page / insert pages from another PDF | core / MuPDF backend | ✅ Verified (all operations reduce to one `PageRef` table → `assemble()`; rotation writes `/Rotate`, cropping writes `/CropBox`; extracted text is byte-identical and rendering has 0 pixel difference) |
| Images to PDF | core / MuPDF backend | ✅ Verified (`pdf_add_image` + Form XObject, original pixels embedded losslessly; `--a4` centres proportionally) |
| Metadata read / write | core / MuPDF backend | ✅ Verified (trailer `/Info`, empty fields are deleted; read/write round-trips) |
| Encrypt / decrypt | core / MuPDF backend | ✅ Verified (AES-128; the encrypted file reports `is_encrypted=True`, without a password it says "password required", with the password it decrypts back to the original page count) |
| Compression | core / MuPDF backend | ✅ Verified (font subsetting + stream compression + garbage collection: 9 pages 274 KB → 220 KB, first pixel identical) |
| PDF watermark (text) | core / MuPDF backend | ✅ Verified (graft + Form XObject overlay: all 82 pages can be watermarked, the Chinese watermark is extractable from the text layer, the original page renders pixel-identically; all 12 positions produce output, the 3×3 grid + centred diagonal + horizontal tiling + 45° tiling were rendered one by one to check placement) |
| PDF watermark (image) | core / MuPDF backend | ✅ Verified (transparent PNG centred/tiled: 9 pages 190,688 B → 193,149 B (centred, one per page) and 194,670 B (tiled, 30 tiles per page); 82 pages 4,859,511 B → 4,934,195 B in 0.3 s; the output contains only 1 `/Image` + 1 `/SMask`, tiling never duplicates pixels) |
| OCR text recognition (searchable PDF + plain text) | core / Windows.Media.Ocr (C++/WinRT) | ✅ Verified (a single image-only page with 460 words: size/baseline/horizontal scaling computed per word, the text layer uses `3 Tr` so **rendering has 0 pixel difference**; `text` extracts "iw8ben.pdf" and "添加文件" from the output PDF, proving Ctrl+F and selection-copy work; a 3-page Chinese scan takes 1.2 s (~0.4 s/page) and recognises "数字图像处理 / 第一讲 / 绪论 / 提纲 / 基本概念") |
| pdf → docx (text reflow, layout lossy) | MuPDF + DocxWriter | ✅ Verified (82 pages → 726 non-empty paragraphs, Chinese intact) |
| C ABI for the web shell | core / capi | ✅ Verified (Python `ctypes` loads `pdfword.dll`: context creation, capability report, error codes and last_error are all correct; all 28 page/document/edit-annotation/bookmark functions were exercised one by one (annotations `pw_pdf_annotate`, hyperlinks `pw_pdf_add_link`, find `pw_pdf_find_text`, replace `pw_pdf_replace_text`, image replacement `pw_pdf_replace_image`, forms `pw_pdf_form_fields` / `pw_pdf_form_fill` / `pw_pdf_form_create`, bookmarks `pw_pdf_outline` / `pw_pdf_set_outline`, rectangle text edits `pw_pdf_replace_text_at` / `pw_pdf_add_text_at`; image watermark `pw_pdf_image_watermark`, positioned `pw_pdf_watermark_position`, `pw_pdf_ocr` / `pw_ocr_capability` are all included) |

Known limitations (trade-offs of the current implementation, not bugs):

- **PDF → DOCX moves text only, not layout**: paragraphs are reflowed by line; images, tables and
  columns are not preserved. To keep layout, go the `docx → pdf` direction, or add a layout analyser.
- **Encrypted PDFs need the caller to supply the password**: `IPdfBackend::open(path, password)`,
  `--password`, and the C ABI `pw_set_pdf_password()` all support it and distinguish "password
  required" from "wrong password". The `Start conversion…` dialog has a "PDF password" field that is
  passed through to `ConvertOptions.pdfPassword`; preview, page editor and extract pages currently
  have no password field, so opening an encrypted PDF says a password is required;
  `Extract pages…` explicitly says "page count unavailable (file cannot be opened or is encrypted —
  decrypt it first)" and never lets an absurd range through; the "open password" field in the
  `Encrypt / Decrypt…` window is the decryption entry point and only saves `_解密.pdf` when correct.
- **The 12 text watermark positions**: `top-left` (default) / `top-center` / `top-right` /
  `middle-left` / `center` / `middle-right` / `bottom-left` / `bottom-center` / `bottom-right` /
  `center-diagonal` / `tile-horizontal` / `tile-diagonal`; unknown values fall back to `top-left`.
  A single line is 8% of the page's short edge, dropping to 5.5% when tiling (dozens of lines at the
  same size would smear together).
  **The two 45° positions (`center-diagonal` / `tile-diagonal`) use bitmaps, not PDF text**: the text
  is rendered at 4× supersampling into one transparent PNG (one per document, shared by pages with
  the same size) and placed with the same `cm` rotation as an image watermark. The reason: when MuPDF
  renders a content stream whose text matrix contains a rotation, glyphs come out 2–3× too large
  (a single 45° watermark covers half a page, 27% of pixels red; tiling reaches 85%); as a bitmap the
  same content drops to 0.6% / 2.9%, matching the look of the horizontal ones. The cost is that
  diagonal watermarks cannot be selected or searched, while the 10 horizontal positions remain real
  text (selectable and extractable).
- **Text width cannot use `fz_measure_string`**: for CJK fonts it does not give a reliable advance
  (the `e` component of the returned matrix is too small), so the tiling step for Chinese watermarks
  was wrong. It now probes one string with `fz_bound_text` and takes the bounding-box width.
- **A text watermark adds one CJK font subset**: the watermark embeds a subset (Microsoft YaHei
  trimmed to the glyphs used, about 190 KB), so output is always slightly larger than the source but
  no longer multiplies: an 82-page sample goes 4.63 MB → 4.70 MB (the 9-page sample goes
  190,688 B → 167,623 B, slightly smaller thanks to de-duplication and subsetting; pages with
  `/Rotate`/CropBox take the same graft path, changing the page object but not the page content).
  The earlier per-page `pdf_graft_page` approach copied each source page's fonts and images once per
  page and reached 8.39 MB; that is fixed.
- **An image watermark embeds the pixel data once**: the implementation calls `pdf_add_image` once on
  the output document and each page's Form XObject only writes
  `q /GS0 gs <w> 0 0 <h> <x> <y> cm /PwImg Do Q`. The first version drew tiles with
  `pdf_new_pdf_device + fz_fill_image`, and the pdf device does not de-duplicate: a 9-page tiled file
  grew to 5.48 MB (648 Image objects); with a single XObject it is 194,670 B (1 Image + 1 SMask).
  PNG alpha becomes `/SMask` and overall transparency goes through `/ExtGState`'s `/ca`. Tiling is a
  grid on the page with spacing = 1.6× the watermark width, no 45° rotation; very tall images are
  capped at 90% of the page height.
- **OCR uses the built-in engine and supports Windows first**: `core/src/ocr_windows.cpp` calls
  `Windows.Media.Ocr` via C++/WinRT (offline, no extra models, only languages already installed —
  `zh-Hans-CN` here, and the Chinese engine also reads English). The text layer takes the fastest
  route: one Form XObject per page with `/Matrix` = `inv(pageCtm)∘[1 0 0 -1 0 H]` (two y-flips cancel
  out, which equals using page user-space coordinates), so `/Rotate` pages land correctly;
  each word is `BT /PwOcr <size> Tf <tz> Tz 3 Tr … Tm <hexGlyphIDs> Tj ET`, and `3 Tr` is **invisible
  rendering**; the codes go through `fz_encode_character` to glyph indices in an Identity-H font
  (`pdf_add_cid_font` writes ToUnicode, so copy/search recovers the original text), and for few
  glyphs `pdf_subset_fonts` trims the font (otherwise 19 MB of Microsoft YaHei would be carried).
  Known trade-off: text layer + plain text only, no layout reconstruction (tables/columns are
  flattened line by line); the second engine, Tesseract, is not wired up yet (see below).
- **Editing and annotations: MuPDF's redact cannot remove text on these documents, so replace uses
  "cover + rewrite"**: per-word tests of `pdf_redact_page` / `pdf_apply_redaction` plus
  `pdf_update_page` in four combinations all failed (the Redact annotation was created and then
  deleted, but `find` still hit the original text; the text appears to live in a nested Form that is
  never instantiated). So `replace` / `--delete-only` / `replace-image` all became **white rectangle
  covering**: `1 1 1 rg … re f` drawn over the hit box (1 pt outwards left/right, **only downwards**
  by 2 pt — extending upwards clips the bottom half of the previous line), then the new text on top
  when needed. When the new text is longer than the rectangle the font **shrinks** instead of
  squashing horizontally (squashing blurs into a line): start at box height × 0.82, then scale by
  "actual text width / box width". Two width models: Latin letters use base14 Helvetica (true widths),
  Chinese uses a non-embedded CJK font (one em per glyph) — with a CJK font only, every letter takes
  a full-width cell and `BOOK` would run into the neighbouring `records`. The cost: the old text
  remains (and is searchable); fully erasing requires MuPDF redact to be fixed or an engine change.
  Both README and source comments state this.
- **Text editing that re-flows layout is not implemented**: only whole-block replace / delete by hit
  region, no per-character insertion, no font-size or letter-spacing changes, no line re-breaking
  (that needs a complete PDF → editable document → re-typeset pipeline).
- **Text box annotations use a bitmap appearance instead of an embedded font**: attaching a CJK font
  to FreeText makes MuPDF embed the entire 19 MB OpenType program (224 KB → 19 MB). Rendering the
  text as a white-backed bitmap for `/AP` brings it back to 272–292 KB with correct Chinese.
- **Not implemented yet**: digital signatures and remote signing, PDF version comparison. Both need
  an extra signing/compare engine, outside the scope of MuPDF + LibreOffice.
- **The Tesseract engine is not wired up (the install step is blocked by the network)**:
  `vcpkg install tesseract:x64-windows` pulls in 10 packages (curl / leptonica / tiff / giflib /
  libwebp / zlib …). On this machine the `ghproxy.net` mirror works for GitHub packages, but
  **giflib lives on SourceForge**: the mirror prefix returns 403 and direct
  `downloads.sourceforge.net` times out (the TUNA/USTC sourceforge mirror paths are also 404), so it
  stalls at `Downloading giflib-6.1.3.tar.gz`; the process was terminated manually.
  Two ways forward for Tesseract: (1) manually copy the missing packages (starting with
  giflib-6.1.3.tar.gz) into `D:\vcpkg\downloads` and let vcpkg hit the cache and continue;
  (2) switch to a prebuilt Tesseract binary (headers and `tesseract.dll` included).
  Once the library is there only `TesseractOcrEngine` has to be implemented (the `IOcrEngine` trio
  `name/available/languages/recognize`), wired up in `makeOcrEngine()` as "Windows first, fall back to
  Tesseract"; backend / CLI / UI / C ABI need no changes. `chi_sim.traineddata` (~45 MB) goes into
  `tessdata/`.
- **The page editor is list-based, not thumbnail drag**: `Page editor…` reorders with a text list plus
  move up / down / to top / to bottom, not a thumbnail grid. Thumbnails would need extra caching and
  threading — pure UI work.
- **The context menu exists only on the main window's pending list**: the merge and images-to-PDF
  windows still use buttons. "Delete / Clear" touch the list only, not the disk (the log says so
  explicitly); to delete real files use Explorer. "Rename file…" changes the real name (extension
  included, with no check that the result is still supported), one file at a time, never batch.

Bringing dependencies in only needs `pdfword.ini`, no rebuild: see
[docs/DEPENDENCIES.md](docs/DEPENDENCIES.md).

## CLI quick verification

The `build\msvc-x64` below is the minimal skeleton build (stub PDF backend, docx/pdf conversion only).
To try the PDF features (info / render / merge / split / watermark) point the path at
`build\mupdf\bin`; the commands are identical.

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
.\build\msvc-x64\bin\pdfword_cli.exe setmeta in.pdf out.pdf --title "Contract" --author "Zhang San"
.\build\msvc-x64\bin\pdfword_cli.exe encrypt in.pdf enc.pdf 888888 owner --allow print,copy
.\build\msvc-x64\bin\pdfword_cli.exe decrypt enc.pdf dec.pdf --password 888888
.\build\msvc-x64\bin\pdfword_cli.exe compress in.pdf small.pdf
.\build\msvc-x64\bin\pdfword_cli.exe watermark in.pdf out.pdf "Internal" 0.3
.\build\msvc-x64\bin\pdfword_cli.exe watermark in.pdf out.pdf "Internal" --opacity 0.3 --position tile-diagonal
.\build\msvc-x64\bin\pdfword_cli.exe watermark in.pdf out.pdf "Internal" --position center-diagonal
.\build\msvc-x64\bin\pdfword_cli.exe imgwatermark in.pdf out.pdf logo.png --scale 0.3 --opacity 0.15
.\build\msvc-x64\bin\pdfword_cli.exe imgwatermark in.pdf tiled.pdf logo.png --scale 0.2 --opacity 0.08 --tile
.\build\msvc-x64\bin\pdfword_cli.exe ocr scan.pdf searchable.pdf extracted.txt --lang zh-Hans-CN
.\build\msvc-x64\bin\pdfword_cli.exe ocr scan.pdf searchable.pdf -            # searchable PDF only
.\build\msvc-x64\bin\pdfword_cli.exe ocr scan.pdf - extracted.txt              # plain text only
.\build\msvc-x64\bin\pdfword_cli.exe info .\enc.pdf --password 888888
.\build\msvc-x64\bin\pdfword_cli.exe convert .\enc.pdf .\enc.docx --to docx --password 888888

# Editing and annotations (coordinates are page display space pt: origin top-left, y down; pages are 1-based)
.\build\mupdf\bin\pdfword_cli.exe annotate in.pdf out.pdf highlight 9 "386,56,410,75"
.\build\mupdf\bin\pdfword_cli.exe annotate in.pdf out.pdf freetext 9 "40,40,240,90" --text "sign here"
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

# Bookmarks / outline (level starts at 0, page is 1-based; page 0 means a group heading with no jump target)
.\build\mupdf\bin\pdfword_cli.exe outline in.pdf
.\build\mupdf\bin\pdfword_cli.exe outline in.pdf --txt ol.txt
.\build\mupdf\bin\pdfword_cli.exe set-outline in.pdf out.pdf "0:1:Cover" "0:2:Section 1" "1:3:Subsection A" "0:6:Section 2"
.\build\mupdf\bin\pdfword_cli.exe set-outline in.pdf empty.pdf --from ol.txt     # levels/pages/titles from a file
.\build\mupdf\bin\pdfword_cli.exe set-outline in.pdf empty.pdf                    # no entries = clear the outline
```

`--password` works with every command (info / text / render / convert all accept it); an encrypted
document without a password reports "document is encrypted, a password is required", and a wrong
password reports "document is encrypted, wrong password".

Numeric parameters (page numbers, angles, crop amounts, opacity) are always **positional, plain
numbers**: write `0.3`, not `--opacity 0.3`. There are two exceptions: `imgwatermark` takes
`--scale` / `--opacity` / `--tile` in flag form, and a single positional argument also works as the
scale; `watermark` accepts both `--opacity 0.3` / `--position tile-diagonal` and the older form (the
fourth positional argument as opacity). Bad input makes the CLI print one line
`parameter error: …` and exit with code 2, instead of aborting the process (earlier versions aborted
because `std::stof` threw).

## Architecture notes

Details in [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md). Three rules:

1. `core` includes no Qt headers — when the web shell arrives the whole UI layer is dropped and the
   core is unchanged.
2. External dependencies always sit behind interfaces (`IPdfBackend`, `OfficeRunner`), all paths come
   from `Config`, and no path is hard-coded.
3. Across boundaries only `Status`/`ErrorCode` travel, nothing throws; the C ABI maps error codes to
   integers.

## License and the MuPDF constraint (read before releasing)

**This repository's own code is MIT** (see [LICENSE](LICENSE), Copyright (c) 2026 wangtc).
But **the binaries contain more than this repository's code** — as soon as `PDFWORD_WITH_MUPDF=ON` is
set, `libmupdf` is statically linked into `pdftoolkit.exe` / `pdfword_cli.exe` / `pdfword.dll`, and
the license of those files is dictated by MuPDF.

### Licenses of the dependencies

| Component | License | What it means for you |
| --- | --- | --- |
| **MuPDF** (`libmupdf`, vcpkg's `unofficial-libmupdf`) | **AGPL-3.0** or a **commercial Artifex license** (pick one) | Linking it makes the whole application AGPL; closed-source distribution requires buying the commercial license first |
| LibreOffice | MPL-2.0 | This project only calls `soffice --convert-to` as a **subprocess** — no linking, no modification — so MPL's file-level copyleft does not reach this project |
| Qt 6 | LGPL-3.0 / commercial | Dynamic linking plus shipping the Qt DLLs is enough; do not statically link for a closed-source release (static needs the LGPL exception or a commercial license) |
| Windows built-in OCR (what this project uses) | An OS capability, not redistributed | Does not affect this project's licensing; if Tesseract and similar engines are added later they are Apache-2.0 — keep the NOTICE |

### Three routes: AGPL open source / buy a commercial license / do not ship MuPDF

MuPDF is developed by Artifex Software and dual-licensed (the "LICENSING" section at
<https://mupdf.com/>, and <https://mupdf.readthedocs.io/en/latest/license.html>). **There is no third
option**: if you neither comply with the AGPL nor buy a commercial license you may not distribute
software containing MuPDF, nor offer it as an online service.

#### Route A: use it under AGPL-3.0 (free, but the whole package is open source)

AGPL covers **the whole application**, not "just that library". Linking MuPDF into `pdftoolkit.exe`
means handing out the source of the work as a whole. The requirements:

- **Distribution means source**: anyone who receives your exe/dll must be able to obtain the
  **complete corresponding source** (all of this repository's source + any changes you made to MuPDF
  + build scripts), and you may not charge extra for the source (beyond media cost).
- **Keep the notices**: do not remove MuPDF's copyright headers, the AGPL text, or the note that a
  commercial license is available from Artifex.
- **Allow replacing MuPDF**: end users must be able to relink/replace with an updated MuPDF (the AGPL
  anti-tivoization clause), so do not pin a version in a way that cannot be replaced and do not lock
  the replacement out.
- **Network use counts too** (AGPL §13, the clause GPL does not have): if this tool is turned into a
  web shell offered as a service (users interact with it over the network), those users must be able
  to obtain the complete source — a source link on the page is enough, but it must exist.
- **No warranty and no support**: under the AGPL, MuPDF is AS-IS.
- This repository's MIT notice must stay (MIT is AGPL-compatible; the combined work is distributed
  under AGPL-3.0 while still marking which files came from the MIT part).

#### Route B: buy the Artifex commercial license (closed source, commercial, no source obligation)

Selling closed source, shipping with hardware, or running SaaS without open-sourcing: this route
removes every AGPL constraint (no open source, no source delivery, no replacement obligation) and
usually includes support. Contacts:

- License page <https://artifex.com/licensing/>, or e-mail `sales@artifex.com`
- State clearly when asking: **MuPDF linked as a static library into a Windows desktop application**,
  whether a SaaS scenario is needed, and the expected volume — quotes depend on the scenario and vary
  a lot

**Note**: a commercial license is granted **per project/product**, not once for every product you
will ever make; a new product line means re-checking the terms.

#### Route C: do not ship MuPDF (stay purely MIT)

If you do not need PDF capability, or just want to ship a shell first, build with the stub backend:

```powershell
cmake -S . -B build/msvc-x64 -G "Visual Studio 17 2022" -A x64 `
      -DCMAKE_PREFIX_PATH=D:/Qt/6.12.0/msvc2022_64 `
      -DPDFWORD_WITH_MUPDF=OFF
```

That produces binaries without MuPDF, distributable under MIT; the cost is that PDF preview / text /
merge / split / extract / page editing / watermark / encryption all report `BackendMissing`
(`doctor` prints "PDF backend: stub (unavailable)") and only the LibreOffice docx ⇄ pdf chain works.

### Four easy mistakes

1. **"I only shipped the exe, not the source, so I'm fine"** — no. The AGPL obligation is "give the
   recipient the source", not "keep the source in a repository"; if they ask, you must be able to
   deliver.
2. **"I use the MIT LICENSE, so my software is MIT"** — that covers **this repository's code** only.
   An artifact linking an AGPL library is itself AGPL. The two licenses must be read separately.
3. **"I use it on an internal network, not publicly"** — pure self-use (no one else gets the software
   and no service is offered) is fine; the moment you deliver it to a customer, install it on someone
   else's machine, or put it on the public internet for outside users, the obligations above apply.
4. **Do not use the MuPDF / Artifex trademarks**: `MuPDF`, `Artifex` and their logos are registered
   trademarks; do not borrow them in a product name, a landing page or an icon (this project is called
   `Pdf-Toolkit` and has no affiliation with Artifex).

### Disclaimer

The above restates the official license highlights and adds engineering-practice reminders; it is
**not legal advice**. Before a real release (especially a commercial one), rely on the original texts
at <https://mupdf.com/> and <https://www.gnu.org/licenses/agpl-3.0.html>, or consult your own legal
counsel.
