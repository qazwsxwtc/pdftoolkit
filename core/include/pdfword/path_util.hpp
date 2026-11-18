#pragma once

// 全工程统一的路径编码约定：string 一律 UTF-8，进 fs::path 必须走 fsPath()。
//
// 背景：std::filesystem::path 的窄字符构造函数在 Windows 上按「当前 ANSI 代码页」
// 解释字节（中文系统 CP936、英文系统 CP1252），而 CLI 的 argv 是 ANSI、Qt 的
// QString::toStdString() 也是 ANSI，接口层拿到的却是 UTF-8 —— 一旦代码页对不上，
// 含中文的文件名会被解成乱码，表现为「文件明明在，却报文件不存在」。
// 所以这里显式按 UTF-8 解码成宽字符再构造 path，与系统代码页无关。
//
// 三条规定，全工程照此执行：
//   1. std::string 里放路径，一律是 UTF-8（业务层、C ABI、日志、配置）；
//   2. 进 fs::path 走 fsPath()；进 Win32/CRT 的文件 API 走 fsPathLong()；
//   3. 出到消息/日志走 fsUtf8()。
//
// 为什么还有 fsPathLong()：Win32 与 CRT 的路径上限是 MAX_PATH(260)，
// 中文/日文文件名很容易把长路径顶爆。超过阈值时它加 "\\?\" 前缀（UNC 走 "\\?\UNC\"），
// 彻底绕开 260 限制，且不依赖系统的 LongPathsEnabled 开关。

#include <filesystem>
#include <string>

namespace pw {

// UTF-8 字符串 → fs::path（Windows 上经 MultiByteToWideChar(CP_UTF8)）。
// 若字节不是合法 UTF-8（例如用户用记事本把 ini 存成了 GBK），退化为按当前 ANSI 代码页解释，
// 保证"旧配置也能打开"而不是把路径解成空串。
std::filesystem::path fsPath(const std::string& utf8Path);
std::filesystem::path fsPath(const char* utf8Path);

// fs::path → UTF-8 字符串（错误消息 / 日志 / C ABI 出参统一用这个）。
std::string fsUtf8(const std::filesystem::path& path);

// 本地 ANSI 编码（CLI 的 argv、getenv 的返回值）→ UTF-8。
std::string ansiToUtf8(const std::string& ansi);

// UTF-8 ⇄ 宽字符，给需要 wchar_t 的系统调用/MuPDF 用。
std::wstring utf8ToWide(const std::string& utf8);
std::string wideToUtf8(const std::wstring& wide);

// 给文件 API 用的宽路径：超过 MAX_PATH 时加 "\\?\" 前缀（UNC 转 "\\?\UNC\"）。
// 只用于真正的 IO 调用（CreateFile / _wfopen / fstream），不要拿去显示。
std::wstring longPathW(const std::filesystem::path& path);
std::filesystem::path fsPathLong(const std::filesystem::path& path);

// 路径是否存在（内部用长路径判断，>260 字符也能正确回答）。
bool existsLong(const std::filesystem::path& path);

// 控制台输出切到 UTF-8（Windows 的 cmd 默认是本地代码页，中文/日文会乱码）。
void enableUtf8Console();

}  // namespace pw
