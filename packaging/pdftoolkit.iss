; Pdf-Toolkit 安装包脚本（Inno Setup 6.7.3）
; 用法：ISCC.exe packaging\pdftoolkit.iss
; 产物：dist\Pdf-Toolkit-0.0.1-win64-setup.exe
;
; 默认装到 %LOCALAPPDATA%\Programs\Pdf-Toolkit（PrivilegesRequired=lowest，不弹 UAC）。
; 要改成装进 Program Files，把 PrivilegesRequired 改成 admin 即可。
;
; 打包内容来自 packaging\_app（由 packaging\build-installer.ps1 从 publish 复制，
; 只保留界面程序与它需要的 Qt/MuPDF 运行库，不含 pdfword_cli.exe / pdfword.dll）。

#define MyAppName "Pdf-Toolkit"
#ifndef MyAppVersion
  #define MyAppVersion "0.0.1"
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
VersionInfoVersion=0.0.1.0
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

[Languages]
Name: "chinesesimp"; MessagesFile: "langs\ChineseSimplified.isl"
Name: "english"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"

[Files]
; _app 是干净运行目录：主程序 + Qt DLL + 插件子目录（platforms / imageformats / styles / tls / ...）
Source: "_app\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs

[Icons]
Name: "{group}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"; Comment: "PDF / Word 转换与预览"
Name: "{autodesktop}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"; Comment: "PDF / Word 转换与预览"; Tasks: desktopicon

[Run]
Filename: "{app}\{#MyAppExeName}"; Description: "{cm:LaunchProgram,{#MyAppName}}"; Flags: nowait postinstall skipifsilent
