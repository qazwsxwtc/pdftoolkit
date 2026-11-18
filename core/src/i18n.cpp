#include "pdfword/i18n.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <unordered_map>
#include <vector>

#include "pdfword/path_util.hpp"

#ifdef _WIN32
#include <windows.h>
#endif

namespace pw {
namespace i18n {

namespace {

Lang gSetting = Lang::Auto;
Lang gEffective = Lang::Zh;
const std::unordered_map<std::string, const char*>* gEnTable = nullptr;
bool gLoaded = false;

const std::unordered_map<std::string, const char*>& enTable() {
    if (gEnTable == nullptr) {
        auto* m = new std::unordered_map<std::string, const char*>();
        m->reserve(static_cast<size_t>(detail::kEntryCount) * 2);
        for (int i = 0; i < detail::kEntryCount; ++i) {
            (*m)[detail::kEntries[i].zh] = detail::kEntries[i].en;
        }
        gEnTable = m;
    }
    return *gEnTable;
}

std::string envOr(const char* name) {
    const char* v = std::getenv(name);
    return (v != nullptr && *v != '\0') ? std::string(v) : std::string();
}

// 与 Config 一致的 ini 位置：PW_CONFIG > exe 同目录 pdfword.ini
std::string iniPath() {
    std::string env = envOr("PW_CONFIG");
    if (!env.empty()) return env;
#ifdef _WIN32
    wchar_t exe[MAX_PATH * 4] = {0};
    if (::GetModuleFileNameW(nullptr, exe, MAX_PATH * 4) > 0) {
        std::string path = wideToUtf8(exe);
        const std::size_t slash = path.find_last_of("\\/");
        if (slash != std::string::npos) return path.substr(0, slash + 1) + "pdfword.ini";
    }
#endif
    return std::string();
}

Lang detectSystemLang() {
#ifdef _WIN32
    // GetUserDefaultUILanguage 拿到的是系统显示语言；中文（含简繁、港台）一律用中文界面
    const LANGID id = ::GetUserDefaultUILanguage();
    if (PRIMARYLANGID(id) == LANG_CHINESE) return Lang::Zh;
    return Lang::En;
#else
    const std::string lang = envOr("LANG");
    if (lang.rfind("zh", 0) == 0) return Lang::Zh;
    return Lang::En;
#endif
}

void recompute() {
    gEffective = (gSetting == Lang::Auto) ? detectSystemLang() : gSetting;
}

// 读 ini 里的 language= 这一行；找不到就 Auto
Lang readFromIni() {
    const std::string path = iniPath();
    if (path.empty()) return Lang::Auto;
    std::ifstream in(fsPathLong(fsPath(path)), std::ios::binary);
    if (!in) in.open(path.c_str(), std::ios::binary);
    if (!in) return Lang::Auto;
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const std::size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string key = line.substr(0, eq);
        const std::size_t b = key.find_first_not_of(" \t");
        const std::size_t e = key.find_last_not_of(" \t[");
        if (b == std::string::npos || e == std::string::npos || e < b) continue;
        key = key.substr(b, e - b + 1);
        if (key != "language") continue;
        return parseLang(line.substr(eq + 1));
    }
    return Lang::Auto;
}

// 只改 language= 一行，其余内容（含注释、其它键）原样保留
bool writeToIni(Lang lang) {
    const std::string path = iniPath();
    if (path.empty()) return false;

    std::vector<std::string> lines;
    {
        std::ifstream in(fsPathLong(fsPath(path)), std::ios::binary);
        if (!in) in.open(path.c_str(), std::ios::binary);
        if (in) {
            std::string line;
            while (std::getline(in, line)) {
                if (!line.empty() && line.back() == '\r') line.pop_back();
                lines.push_back(line);
            }
        }
    }

    const char* code = (lang == Lang::Zh) ? "zh" : (lang == Lang::En) ? "en" : "auto";
    const std::string newLine = std::string("language=") + code;
    bool replaced = false;
    for (auto& line : lines) {
        std::size_t i = line.find_first_not_of(" \t");
        if (i == std::string::npos) continue;
        const std::size_t eq = line.find('=');
        if (eq == std::string::npos || eq < i) continue;
        std::string key = line.substr(i, eq - i);
        key.erase(key.find_last_not_of(" \t") + 1);
        if (key == "language") {
            line = newLine;
            replaced = true;
            break;
        }
    }
    if (!replaced) lines.push_back(newLine);

    std::ostringstream oss;
    for (std::size_t i = 0; i < lines.size(); ++i) {
        oss << lines[i];
        if (i + 1 < lines.size()) oss << "\n";
    }
    oss << "\n";

    std::ofstream out(fsPathLong(fsPath(path)), std::ios::binary | std::ios::trunc);
    if (!out) out.open(path.c_str(), std::ios::binary | std::ios::trunc);
    if (!out) return false;
    out.write(oss.str().data(), static_cast<std::streamsize>(oss.str().size()));
    return static_cast<bool>(out);
}

}  // namespace

Lang parseLang(const std::string& text) {
    std::string v = text;
    // 去掉引号与空白，统一小写
    v.erase(std::remove_if(v.begin(), v.end(),
                           [](unsigned char c) { return c == '"' || c == '\'' || c == ' ' || c == '\t'; }),
            v.end());
    std::transform(v.begin(), v.end(), v.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (v.rfind("zh", 0) == 0) return Lang::Zh;
    if (v.rfind("en", 0) == 0) return Lang::En;
    return Lang::Auto;
}

void load() {
    if (gLoaded) return;
    gSetting = readFromIni();
    recompute();
    gLoaded = true;
}

Lang setting() {
    if (!gLoaded) load();
    return gSetting;
}

Lang effective() {
    if (!gLoaded) load();
    return gEffective;
}

const char* effectiveCode() { return (effective() == Lang::En) ? "en" : "zh"; }

bool setSetting(Lang lang) {
    gSetting = lang;
    recompute();
    gLoaded = true;
    return writeToIni(lang);
}

const char* t(const char* zh) {
    if (zh == nullptr) return "";
    if (effective() != Lang::En) return zh;
    const auto& table = enTable();
    const auto it = table.find(zh);
    return (it == table.end()) ? zh : it->second;
}

std::string t(const std::string& zh) { return std::string(t(zh.c_str())); }

}  // namespace i18n
}  // namespace pw
