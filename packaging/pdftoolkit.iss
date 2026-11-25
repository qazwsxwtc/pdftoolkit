; Pdf-Toolkit 安装包脚本（Inno Setup 6.7.3）
; 用法：ISCC.exe packaging\pdftoolkit.iss
; 产物：dist\Pdf-Toolkit-0.0.4-win64-setup.exe
;
; 默认装到 %LOCALAPPDATA%\Programs\Pdf-Toolkit（PrivilegesRequired=lowest，不弹 UAC）。
; 要改成装进 Program Files，把 PrivilegesRequired 改成 admin 即可。
;
; 打包内容来自 packaging\_app（由 packaging\build-installer.ps1 从 publish 复制，
; 只保留界面程序与它需要的 Qt/MuPDF 运行库，不含 pdfword_cli.exe / pdfword.dll）。

#define MyAppName "Pdf-Toolkit"
#ifndef MyAppVersion
  #define MyAppVersion "0.0.4"
#endif
#define MyAppPublisher "wangtc"
#define MyAppExeName "pdftoolkit.exe"

[Setup]
AppId={{7C4A9E52-3B6D-4F1A-9E2C-5A8B7D3F1C60}
AppName={#MyAppName}
AppVersion={#MyAppVersion}
AppVerName={#MyAppName} {#MyAppVersion}
AppPublisher={#MyAppPublisher}
; 免管理员：lowest 下 {autopf} 解析成 %LOCALAPPDATA%\Programs
DefaultDirName={autopf}\Pdf-Toolkit
DefaultGroupName={#MyAppName}
DisableProgramGroupPage=yes
PrivilegesRequired=lowest
OutputDir=..\dist
OutputBaseFilename=Pdf-Toolkit-{#MyAppVersion}-win64-setup
SetupIconFile=..\ui\resources\pdftoolkit.ico
UninstallDisplayIcon={app}\{#MyAppExeName}
UninstallDisplayName={#MyAppName} {#MyAppVersion}
VersionInfoVersion=0.0.4.0
VersionInfoCompany={#MyAppPublisher}
VersionInfoDescription={#MyAppName} installer
VersionInfoProductName={#MyAppName}
VersionInfoProductVersion={#MyAppVersion}
Compression=lzma2/max
SolidCompression=yes
WizardStyle=modern
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
; 升级时如果程序还开着，先让用户关掉，避免 DLL 被占用
CloseApplications=yes
RestartApplications=no
AllowNoIcons=yes
; 注册了 .pdf 的"打开方式"候选，让安装器通知 Shell 刷新缓存（否则右键菜单要等下次登录）
ChangesAssociations=yes

[Languages]
Name: "chinesesimp"; MessagesFile: "langs\ChineseSimplified.isl"
Name: "english"; MessagesFile: "compiler:Default.isl"

[CustomMessages]
chinesesimp.AssocTaskTitle=把 Pdf-Toolkit 加入 PDF 的“打开方式”菜单（不改变当前默认程序）
english.AssocTaskTitle=Add Pdf-Toolkit to the "Open with" menu for PDF files (does not change your current default)

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"
Name: "pdfassoc"; Description: "{cm:AssocTaskTitle}"; GroupDescription: "{cm:AdditionalIcons}"; Flags: checkedonce

[Files]
; _app 是干净运行目录：主程序 + Qt DLL + 插件子目录（platforms / imageformats / styles / tls / ...）
Source: "_app\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs

[Icons]
Name: "{group}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"; Comment: "PDF / Word 转换与预览"
Name: "{autodesktop}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"; Comment: "PDF / Word 转换与预览"; Tasks: desktopicon

[Registry]
; ---- PDF 关联：让右键「打开方式」里出现 Pdf-Toolkit ----
; 只登记为"可选程序"，不动用户当前的默认程序（要设成默认由用户在 Windows「默认应用」里自己选）。
; 三条关键项：
;   1) Applications\pdftoolkit.exe —— 「打开方式」列表靠它认出这个 exe，FriendlyAppName 决定显示的名字
;   2) .pdf\OpenWithProgids      —— 把我们的 ProgId 挂到 .pdf 的候选程序里
;   3) PdfToolkit.Document       —— ProgId 本体：类型名 + 图标 + 打开命令
Root: HKA; Subkey: "Software\Classes\Applications\{#MyAppExeName}"; ValueType: string; ValueName: "FriendlyAppName"; ValueData: "{#MyAppName}"; Tasks: pdfassoc; Flags: uninsdeletekey
Root: HKA; Subkey: "Software\Classes\Applications\{#MyAppExeName}"; ValueType: string; ValueName: "ApplicationCompany"; ValueData: "{#MyAppPublisher}"; Tasks: pdfassoc
Root: HKA; Subkey: "Software\Classes\Applications\{#MyAppExeName}\DefaultIcon"; ValueType: string; ValueName: ""; ValueData: "{app}\{#MyAppExeName},0"; Tasks: pdfassoc
Root: HKA; Subkey: "Software\Classes\Applications\{#MyAppExeName}\shell\open\command"; ValueType: string; ValueName: ""; ValueData: """{app}\{#MyAppExeName}"" ""%1"""; Tasks: pdfassoc; Flags: uninsdeletekey
Root: HKA; Subkey: "Software\Classes\.pdf\OpenWithProgids"; ValueType: string; ValueName: "PdfToolkit.Document"; ValueData: ""; Tasks: pdfassoc; Flags: uninsdeletevalue
Root: HKA; Subkey: "Software\Classes\PdfToolkit.Document"; ValueType: string; ValueName: ""; ValueData: "PDF 文档"; Tasks: pdfassoc; Flags: uninsdeletekey
Root: HKA; Subkey: "Software\Classes\PdfToolkit.Document\DefaultIcon"; ValueType: string; ValueName: ""; ValueData: "{app}\{#MyAppExeName},0"; Tasks: pdfassoc
Root: HKA; Subkey: "Software\Classes\PdfToolkit.Document\shell\open\command"; ValueType: string; ValueName: ""; ValueData: """{app}\{#MyAppExeName}"" ""%1"""; Tasks: pdfassoc

[Run]
Filename: "{app}\{#MyAppExeName}"; Description: "{cm:LaunchProgram,{#MyAppName}}"; Flags: nowait postinstall skipifsilent
