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
#include "pdfword/path_util.hpp"
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
    if (!local.empty()) return fsPath(local) / "Pdf-Toolkit" / "work";
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
    if (!fs::exists(candidate, ec)) return fail("路径不存在: " + fsUtf8(candidate));

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
        return fail("目录内找不到 soffice: " + fsUtf8(candidate));
    }

    std::string name = toLower(fsUtf8(candidate.filename()));
    if (name != "soffice.exe" && name != "soffice.com" && name != "soffice") {
        return fail("文件名不是 soffice 可执行入口: " + fsUtf8(candidate));
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
            if (!envCfg.empty()) iniPath = fsPath(envCfg);
        }
    }
    std::map<std::string, std::string> kv =
        iniPath.empty() ? std::map<std::string, std::string>() : parseIni(iniPath);

    auto get = [&kv](const char* key) -> std::string {
        auto it = kv.find(key);
        return it == kv.end() ? std::string() : it->second;
    };

    // 2) LibreOffice 路径：显式配置 > 便携目录 > 注册表 > 常见安装位置（含各盘符）> PATH
    std::string lo = get("libre_office");
    if (lo.empty()) lo = envOr("PW_LIBREOFFICE_PATH", "");

    // PW_LIBREOFFICE_DISABLE=1：纯 PDF 模式，跳过全部 LibreOffice 探测。
    // 明确不装 LibreOffice 的机器上启动更快，也能验证"预览与 LibreOffice 无关"。
    const bool officeDisabled = !envOr("PW_LIBREOFFICE_DISABLE", "").empty();
    cfg.libreOfficeDisabled = officeDisabled;

    std::vector<fs::path> candidates;
    if (!officeDisabled) {
        if (!lo.empty()) candidates.push_back(fsPath(lo));

        // 便携版：程序目录旁边带一份 LibreOffice（整包拷到 U 盘也能用）
        candidates.push_back(cfg.executableDir / "LibreOffice" / "program" / "soffice.exe");
        candidates.push_back(cfg.executableDir / "soffice.exe");

    #ifdef _WIN32
        // 注册表：UNO\InstallPath（HKLM/HKCU × 64/32 位视图）+ App Paths
        for (HKEY hive : {HKEY_LOCAL_MACHINE, HKEY_CURRENT_USER}) {
            for (DWORD view : {KEY_WOW64_64KEY, KEY_WOW64_32KEY}) {
                std::string install =
                    readRegistryValue(hive, L"SOFTWARE\\LibreOffice\\UNO\\InstallPath", L"", view);
                if (!install.empty()) candidates.push_back(fsPath(install) / "soffice.exe");
                std::string appPath = readRegistryValue(
                    hive, L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\App Paths\\soffice.exe", L"",
                    view);
                if (!appPath.empty()) candidates.push_back(fsPath(appPath));
            }
        }
    #endif

        // 环境变量给出的标准安装位置
        for (const char* envName : {"ProgramFiles", "ProgramFiles(x86)", "ProgramW6432"}) {
            std::string root = envOr(envName, "");
            if (!root.empty()) candidates.push_back(fsPath(root) / "LibreOffice" / "program" / "soffice.exe");
        }

        // 常见盘符 × 常见目录：LibreOffice 允许装到任意盘，实测本机就在 D:\LibreOffice，
        // 只查 C:/D: 的 "Program Files" 会漏掉它。
        for (const char* drive : {"C", "D", "E", "F", "G", "H"}) {
            std::string base = std::string(1, drive[0]) + ":\\";
            candidates.push_back(fsPath(base + "LibreOffice\\program\\soffice.exe"));
            candidates.push_back(fsPath(base + "Program Files\\LibreOffice\\program\\soffice.exe"));
            candidates.push_back(fsPath(base + "Program Files (x86)\\LibreOffice\\program\\soffice.exe"));
        }

        // 目录扫描：Program Files / Programs 下所有 LibreOffice*（覆盖 "LibreOffice 7.6" 之类的版本目录名）
        {
            std::vector<fs::path> scanRoots;
            for (const char* envName : {"ProgramFiles", "ProgramFiles(x86)"}) {
                std::string root = envOr(envName, "");
                if (!root.empty()) scanRoots.push_back(fsPath(root));
            }
            std::string local = envOr("LOCALAPPDATA", "");
            if (!local.empty()) scanRoots.push_back(fsPath(local) / "Programs");
            for (const auto& root : scanRoots) {
                std::error_code ec;
                fs::directory_iterator it(root, ec);
                if (ec) continue;
                for (const auto& entry : it) {
                    std::error_code ec2;
                    if (!entry.is_directory(ec2)) continue;
                    std::wstring name = entry.path().filename().wstring();
                    if (name.rfind(L"LibreOffice", 0) != 0) continue;
                    candidates.push_back(entry.path() / "program" / "soffice.exe");
                }
            }
        }
    }

    for (const auto& c : candidates) {
        if (c.empty()) continue;
        fs::path resolved;
        if (probeLibreOffice(c, &resolved, nullptr)) {
            cfg.libreOfficePath = resolved;
            break;
        }
        if (cfg.libreOfficeCandidates.size() < 16) cfg.libreOfficeCandidates.push_back(fsUtf8(c));
    }

#ifdef _WIN32
    if (!officeDisabled && cfg.libreOfficePath.empty()) {
        std::string fromPath = searchOnPath(L"soffice.exe");
        if (fromPath.empty()) fromPath = searchOnPath(L"soffice.com");
        if (!fromPath.empty()) cfg.libreOfficePath = fsPath(fromPath);
    }
#endif

    // 3) MuPDF 便携目录（仅动态链接的多后端场景需要）
    std::string mupdfDir = get("mupdf_dir");
    if (mupdfDir.empty()) mupdfDir = envOr("PW_MUPDF_DIR", "");
    if (!mupdfDir.empty()) cfg.mupdfRuntimeDir = fsPath(mupdfDir);

    // 4) 工作目录
    std::string work = get("work_dir");
    if (work.empty()) work = envOr("PW_WORK_DIR", "");
    cfg.workDir = work.empty() ? defaultWorkDir(cfg.executableDir) : fsPath(work);

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

std::string libreOfficeSetupHint(const Config& config) {
    if (config.libreOfficeDisabled) {
        return "（PW_LIBREOFFICE_DISABLE 已开启，去掉该环境变量后重启即可启用文档转换）";
    }
    return "（填法：在 " + fsUtf8(config.executableDir / "pdfword.ini") +
           " 里加一行 libre_office=<soffice.exe 或 LibreOffice 目录>，或设环境变量 PW_LIBREOFFICE_PATH）";
}

std::string describeLibreOffice(const Config& config) {
    if (config.libreOfficeDisabled) {
        return "LibreOffice: 已关闭（PW_LIBREOFFICE_DISABLE 生效，docx ⇄ pdf 转换不可用；PDF 预览等其他功能不受影响）";
    }
    if (!config.libreOfficeConfigured()) {
        const std::string ini = fsUtf8(config.executableDir / "pdfword.ini");
        std::string msg = "LibreOffice: 未配置（docx ⇄ pdf 转换不可用）";
        msg += "\n  填法：在 " + ini + " 里加一行  libre_office=<soffice.exe 或 LibreOffice 目录>";
        msg += "\n  或设环境变量 PW_LIBREOFFICE_PATH";
        if (!config.libreOfficeCandidates.empty()) {
            msg += "\n  已尝试：";
            const std::size_t show = std::min<std::size_t>(config.libreOfficeCandidates.size(), 6);
            for (std::size_t i = 0; i < show; ++i) {
                if (i != 0) msg += "、";
                msg += config.libreOfficeCandidates[i];
            }
            if (config.libreOfficeCandidates.size() > show) {
                msg += " 等 " + std::to_string(config.libreOfficeCandidates.size()) + " 处";
            }
        }
        return msg;
    }
    std::error_code ec;
    if (!fs::exists(config.libreOfficePath, ec)) {
        return "LibreOffice: 路径无效 " + fsUtf8(config.libreOfficePath);
    }
    return "LibreOffice: " + fsUtf8(config.libreOfficePath);
}

}  // namespace pw
