#include "pdfword/config.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <map>
#include <string>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace pw {

namespace {

std::string trimCopy(const std::string& s) {
    std::size_t b = 0;
    std::size_t e = s.size();
    while (b < e && (std::isspace(static_cast<unsigned char>(s[b])) != 0)) ++b;
    while (e > b && (std::isspace(static_cast<unsigned char>(s[e - 1])) != 0)) --e;
    return s.substr(b, e - b);
}

std::string toLower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

// 极简 ini：只支持 `key = value` 与 # / ; 注释，够用且零依赖。
std::map<std::string, std::string> parseIni(const fs::path& file) {
    std::map<std::string, std::string> out;
    std::ifstream in(file);
    if (!in) return out;
    std::string line;
    bool first = true;
    while (std::getline(in, line)) {
        if (first) {
            first = false;
            // 去掉 UTF-8 BOM，避免第一个 key 变成 \xEF\xBB\xBFkey
            if (line.size() >= 3 && line[0] == '\xEF' && line[1] == '\xBB' && line[2] == '\xBF') {
                line.erase(0, 3);
            }
        }
        if (!line.empty() && line.back() == '\r') line.pop_back();
        std::string t = trimCopy(line);
        if (t.empty() || t[0] == '#' || t[0] == ';' || t[0] == '[') continue;
        std::size_t eq = t.find('=');
        if (eq == std::string::npos) continue;
        std::string key = toLower(trimCopy(t.substr(0, eq)));
        std::string value = trimCopy(t.substr(eq + 1));
        if (!key.empty()) out[key] = value;
    }
    return out;
}

std::string envOr(const char* name, const std::string& fallback) {
    const char* v = std::getenv(name);
    if (v != nullptr && *v != '\0') return std::string(v);
    return fallback;
}

fs::path defaultWorkDir(const fs::path& exeDir) {
    std::string local = envOr("LOCALAPPDATA", "");
    if (!local.empty()) return fs::path(local) / "PdfWord" / "work";
    return exeDir / "work";
}

#ifdef _WIN32

std::wstring toWide(const std::string& s) {
    if (s.empty()) return std::wstring();
    int n = ::MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), nullptr, 0);
    if (n <= 0) return std::wstring();
    std::wstring w(static_cast<std::size_t>(n), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

std::string toUtf8(const std::wstring& w) {
    if (w.empty()) return std::string();
    int n = ::WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), nullptr, 0,
                                  nullptr, nullptr);
    if (n <= 0) return std::string();
    std::string s(static_cast<std::size_t>(n), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), s.data(), n, nullptr,
                          nullptr);
    return s;
}

std::string readRegistryValue(HKEY root, const wchar_t* subKey, const wchar_t* valueName,
                              DWORD viewFlag) {
    HKEY key = nullptr;
    if (::RegOpenKeyExW(root, subKey, 0, KEY_READ | viewFlag, &key) != ERROR_SUCCESS) {
        return std::string();
    }
    wchar_t buf[1024];
    DWORD size = sizeof(buf);
    std::string out;
    if (::RegQueryValueExW(key, valueName, nullptr, nullptr, reinterpret_cast<LPBYTE>(buf), &size) ==
        ERROR_SUCCESS) {
        std::wstring w(buf, size / sizeof(wchar_t));
        while (!w.empty() && w.back() == L'\0') w.pop_back();
        out = toUtf8(w);
    }
    ::RegCloseKey(key);
    return out;
}

std::string searchOnPath(const std::wstring& exeName) {
    wchar_t buf[MAX_PATH];
    if (::SearchPathW(nullptr, exeName.c_str(), nullptr, MAX_PATH, buf, nullptr) > 0) {
        return toUtf8(std::wstring(buf));
    }
    return std::string();
}

#endif  // _WIN32

}  // namespace

bool probeLibreOffice(const fs::path& candidate, fs::path* resolved, std::string* why) {
    if (resolved != nullptr) *resolved = fs::path();
    auto fail = [&](const std::string& reason) {
        if (why != nullptr) *why = reason;
        return false;
    };

    if (candidate.empty()) return fail("路径为空");

    std::error_code ec;
    if (!fs::exists(candidate, ec)) return fail("路径不存在: " + candidate.string());

    std::vector<fs::path> tried;
    if (fs::is_directory(candidate, ec)) {
        for (const char* rel : {"program/soffice.exe", "soffice.exe", "soffice.com", "soffice"}) {
            fs::path t = candidate / rel;
            tried.push_back(t);
            if (fs::exists(t, ec) && !fs::is_directory(t, ec)) {
                if (resolved != nullptr) *resolved = fs::absolute(t, ec);
                return true;
            }
        }
        return fail("目录内找不到 soffice: " + candidate.string());
    }

    std::string name = toLower(candidate.filename().string());
    if (name != "soffice.exe" && name != "soffice.com" && name != "soffice") {
        return fail("文件名不是 soffice 可执行入口: " + candidate.string());
    }
    if (resolved != nullptr) *resolved = fs::absolute(candidate, ec);
    return true;
}

Config loadConfig(const fs::path& exePath, const fs::path& configFile) {
    Config cfg;
    cfg.executableDir = exePath.parent_path();
    if (cfg.executableDir.empty()) {
        std::error_code ec;
        cfg.executableDir = fs::current_path(ec);
    }

    // 1) ini：显式参数 > 同目录 pdfword.ini > 环境变量 PW_CONFIG
    fs::path iniPath = configFile;
    if (iniPath.empty()) {
        std::error_code ec;
        fs::path beside = cfg.executableDir / "pdfword.ini";
        if (fs::exists(beside, ec)) {
            iniPath = beside;
        } else {
            std::string envCfg = envOr("PW_CONFIG", "");
            if (!envCfg.empty()) iniPath = fs::path(envCfg);
        }
    }
    std::map<std::string, std::string> kv =
        iniPath.empty() ? std::map<std::string, std::string>() : parseIni(iniPath);

    auto get = [&kv](const char* key) -> std::string {
        auto it = kv.find(key);
        return it == kv.end() ? std::string() : it->second;
    };

    // 2) LibreOffice 路径：ini > 环境变量 > 注册表 > 常见安装目录 > PATH
    std::string lo = get("libre_office");
    if (lo.empty()) lo = envOr("PW_LIBREOFFICE_PATH", "");

    fs::path candidatesFirst;
    if (!lo.empty()) candidatesFirst = fs::path(lo);

    std::vector<fs::path> candidates;
    if (!candidatesFirst.empty()) candidates.push_back(candidatesFirst);

#ifdef _WIN32
    for (DWORD view : {KEY_WOW64_64KEY, KEY_WOW64_32KEY}) {
        std::string install = readRegistryValue(HKEY_LOCAL_MACHINE, L"SOFTWARE\\LibreOffice\\UNO\\InstallPath",
                                                L"", view);
        if (!install.empty()) candidates.push_back(fs::path(install) / "soffice.exe");
    }
#endif

    for (const char* envName : {"ProgramFiles", "ProgramFiles(x86)", "ProgramW6432"}) {
        std::string root = envOr(envName, "");
        if (!root.empty()) candidates.push_back(fs::path(root) / "LibreOffice" / "program" / "soffice.exe");
    }
    candidates.push_back(fs::path("C:\\Program Files\\LibreOffice\\program\\soffice.exe"));
    candidates.push_back(fs::path("D:\\Program Files\\LibreOffice\\program\\soffice.exe"));
    {
        std::string local = envOr("LOCALAPPDATA", "");
        if (!local.empty()) {
            candidates.push_back(fs::path(local) / "Programs" / "LibreOffice" / "program" / "soffice.exe");
        }
    }

    for (const auto& c : candidates) {
        fs::path resolved;
        if (probeLibreOffice(c, &resolved, nullptr)) {
            cfg.libreOfficePath = resolved;
            break;
        }
    }

#ifdef _WIN32
    if (cfg.libreOfficePath.empty()) {
        std::string fromPath = searchOnPath(L"soffice.exe");
        if (fromPath.empty()) fromPath = searchOnPath(L"soffice.com");
        if (!fromPath.empty()) cfg.libreOfficePath = fs::path(fromPath);
    }
#endif

    // 3) MuPDF 便携目录（仅动态链接的多后端场景需要）
    std::string mupdfDir = get("mupdf_dir");
    if (mupdfDir.empty()) mupdfDir = envOr("PW_MUPDF_DIR", "");
    if (!mupdfDir.empty()) cfg.mupdfRuntimeDir = fs::path(mupdfDir);

    // 4) 工作目录
    std::string work = get("work_dir");
    if (work.empty()) work = envOr("PW_WORK_DIR", "");
    cfg.workDir = work.empty() ? defaultWorkDir(cfg.executableDir) : fs::path(work);

    // 5) 数值项
    std::string timeout = get("timeout_ms");
    if (!timeout.empty()) {
        try {
            int v = std::stoi(timeout);
            if (v >= 5000 && v <= 3600000) cfg.officeTimeoutMs = v;
        } catch (...) {
            // 非法值忽略，保持默认
        }
    }
    std::string maxSide = get("render_max_side");
    if (!maxSide.empty()) {
        try {
            int v = std::stoi(maxSide);
            if (v >= 512 && v <= 16384) cfg.renderMaxSide = v;
        } catch (...) {
        }
    }

    return cfg;
}

std::string describeLibreOffice(const Config& config) {
    if (!config.libreOfficeConfigured()) {
        return "LibreOffice: 未配置（在 pdfword.ini 里填 libre_office=便携目录）";
    }
    std::error_code ec;
    if (!fs::exists(config.libreOfficePath, ec)) {
        return "LibreOffice: 路径无效 " + config.libreOfficePath.string();
    }
    return "LibreOffice: " + config.libreOfficePath.string();
}

}  // namespace pw
