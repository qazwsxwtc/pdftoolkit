#include "pdfword/path_util.hpp"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#include <vector>

namespace pw {

#ifdef _WIN32

std::wstring utf8ToWide(const std::string& utf8) {
    if (utf8.empty()) return std::wstring();
    // 先按严格 UTF-8 解；失败说明调用方给的是本地代码页字节（老 ini 存成 GBK、
    // 老脚本、以及 Windows 的 getenv），退化成 CP_ACP。宁可猜，也不能把路径变成空串。
    int n = ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.c_str(),
                                  static_cast<int>(utf8.size()), nullptr, 0);
    if (n > 0) {
        std::wstring w(static_cast<std::size_t>(n), L'\0');
        if (::MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), static_cast<int>(utf8.size()), w.data(), n) > 0) {
            return w;
        }
    }
    int m = ::MultiByteToWideChar(CP_ACP, 0, utf8.c_str(), static_cast<int>(utf8.size()), nullptr, 0);
    if (m <= 0) return std::wstring();
    std::wstring w(static_cast<std::size_t>(m), L'\0');
    ::MultiByteToWideChar(CP_ACP, 0, utf8.c_str(), static_cast<int>(utf8.size()), w.data(), m);
    return w;
}

std::string wideToUtf8(const std::wstring& wide) {
    if (wide.empty()) return std::string();
    int n = ::WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), static_cast<int>(wide.size()), nullptr, 0,
                                  nullptr, nullptr);
    if (n <= 0) return std::string();
    std::string s(static_cast<std::size_t>(n), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), static_cast<int>(wide.size()), s.data(), n, nullptr,
                          nullptr);
    return s;
}

std::string ansiToUtf8(const std::string& ansi) {
    if (ansi.empty()) return std::string();
    int n = ::MultiByteToWideChar(CP_ACP, 0, ansi.c_str(), static_cast<int>(ansi.size()), nullptr, 0);
    if (n <= 0) return ansi;  // 转不动就原样返回，至少不吞数据
    std::wstring w(static_cast<std::size_t>(n), L'\0');
    ::MultiByteToWideChar(CP_ACP, 0, ansi.c_str(), static_cast<int>(ansi.size()), w.data(), n);
    return wideToUtf8(w);
}

std::wstring longPathW(const std::filesystem::path& path) {
    std::wstring w = path.wstring();
    // 短路径不动：加了前缀的路径不支持 "."/".."、相对路径和以空格结尾，反而更容易出错。
    if (w.size() < 240) return w;
    if (w.rfind(L"\\\\?\\", 0) == 0) return w;
    if (w.size() >= 2 && w[1] == L':') return L"\\\\?\\" + w;
    if (w.rfind(L"\\\\", 0) == 0) return L"\\\\?\\UNC\\" + w.substr(2);
    return w;
}

bool existsLong(const std::filesystem::path& path) {
    if (path.empty()) return false;
    DWORD attr = ::GetFileAttributesW(longPathW(path).c_str());
    return attr != INVALID_FILE_ATTRIBUTES;
}

void enableUtf8Console() {
    ::SetConsoleOutputCP(CP_UTF8);
    ::SetConsoleCP(CP_UTF8);
}

std::filesystem::path fsPathLong(const std::filesystem::path& path) {
    return std::filesystem::path(longPathW(path));
}

#else

// POSIX：窄字符本来就是 UTF-8，宽字符只做逐字节扩展（仅用于偶尔需要 wchar_t 的接口）。
std::wstring utf8ToWide(const std::string& utf8) {
    return std::wstring(utf8.begin(), utf8.end());
}

std::string wideToUtf8(const std::wstring& wide) {
    return std::string(wide.begin(), wide.end());
}

std::string ansiToUtf8(const std::string& ansi) { return ansi; }

std::wstring longPathW(const std::filesystem::path& path) { return path.wstring(); }

std::filesystem::path fsPathLong(const std::filesystem::path& path) { return path; }

bool existsLong(const std::filesystem::path& path) {
    std::error_code ec;
    return std::filesystem::exists(path, ec);
}

void enableUtf8Console() {}

#endif

std::filesystem::path fsPath(const std::string& utf8Path) {
    if (utf8Path.empty()) return std::filesystem::path();
#ifdef _WIN32
    return std::filesystem::path(utf8ToWide(utf8Path));
#else
    return std::filesystem::path(utf8Path);
#endif
}

std::filesystem::path fsPath(const char* utf8Path) {
    if (utf8Path == nullptr || *utf8Path == '\0') return std::filesystem::path();
    return fsPath(std::string(utf8Path));
}

std::string fsUtf8(const std::filesystem::path& path) {
#ifdef _WIN32
    return wideToUtf8(path.wstring());
#else
    return path.string();
#endif
}

}  // namespace pw
