#include "pdfword/office_runner.hpp"
#include "pdfword/i18n.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include "pdfword/path_util.hpp"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace pw {

namespace {

std::string toLower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

std::string readTextFile(const fs::path& path, std::size_t maxBytes) {
#ifdef _WIN32
    std::ifstream in(fsPathLong(path).wstring().c_str(), std::ios::binary);
#else
    std::ifstream in(path, std::ios::binary);
#endif
    if (!in) return std::string();
    std::string s;
    s.resize(maxBytes);
    in.read(s.data(), static_cast<std::streamsize>(maxBytes));
    s.resize(static_cast<std::size_t>(in.gcount()));
    return s;
}

// file:// URL 里除了 RFC3986 的保留字符，其余（中文、日文、空格、#、%、&…）都必须
// percent-encode，否则用户名或工作目录一含非 ASCII 字符，soffice 就把它当非法 URL 退出。
std::string percentEncodePath(const std::string& utf8) {
    std::string out;
    out.reserve(utf8.size() * 2);
    static const char* kHex = "0123456789ABCDEF";
    for (unsigned char c : utf8) {
        const bool keep = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                          (c >= '0' && c <= '9') || c == '-' || c == '.' || c == '_' ||
                          c == '~' || c == '/' || c == ':';
        if (keep) {
            out.push_back(static_cast<char>(c));
        } else {
            out.push_back('%');
            out.push_back(kHex[c >> 4]);
            out.push_back(kHex[c & 0x0F]);
        }
    }
    return out;
}

// LibreOffice 的 -env:UserInstallation 需要 file:/// URL 形式，且路径分隔符要转成 /
std::string toFileUrl(const fs::path& p) {
    std::string s = fsUtf8(p);
    std::replace(s.begin(), s.end(), '\\', '/');
    if (!s.empty() && s[0] != '/') s.insert(s.begin(), '/');
    return "file://" + percentEncodePath(s);
}

#ifdef _WIN32
// MSVCRT 的命令行解析规则：引号内的反斜杠只有在紧邻引号或结尾时才具转义含义，
// 所以 "C:\dir\" 会被当成未闭合引号 —— 目录参数几乎都以反斜杠结尾，必须按规则加倍。
std::wstring quoteWinArg(const std::wstring& arg) {
    if (arg.empty()) return L"\"\"";
    const bool needQuote = arg.find_first_of(L" \t\n\v\"") != std::wstring::npos;
    if (!needQuote) return arg;
    std::wstring out = L"\"";
    for (std::wstring::const_iterator it = arg.begin();;) {
        std::size_t backslashes = 0;
        while (it != arg.end() && *it == L'\\') {
            ++it;
            ++backslashes;
        }
        if (it == arg.end()) {
            out.append(backslashes * 2, L'\\');
            break;
        }
        if (*it == L'"') {
            out.append(backslashes * 2 + 1, L'\\');
            out.push_back(L'"');
        } else {
            out.append(backslashes, L'\\');
            out.push_back(*it);
        }
        ++it;
    }
    out.push_back(L'"');
    return out;
}
#endif

std::string tail(const std::string& s, std::size_t maxLen) {
    if (s.size() <= maxLen) return s;
    return "…" + s.substr(s.size() - maxLen);
}

struct RunResult {
    int exitCode = -1;
    bool timedOut = false;
    bool launched = false;
    std::string output;
};

#ifdef _WIN32

// 启动外部进程并把 stdout/stderr 落到 logFile。
// 超时时通过 JobObject 杀整棵进程树（soffice.exe 会再拉起 soffice.bin）。
RunResult runProcess(const fs::path& exe, const std::vector<std::string>& args, int timeoutMs,
                     const fs::path& logFile) {
    RunResult r;

    std::wstring cmd = quoteWinArg(longPathW(exe));
    for (const auto& a : args) {
        cmd += L" ";
        cmd += quoteWinArg(utf8ToWide(a));
    }
    std::vector<wchar_t> cmdBuf(cmd.begin(), cmd.end());
    cmdBuf.push_back(L'\0');

    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    sa.lpSecurityDescriptor = nullptr;

    std::wstring logPath = longPathW(logFile);
    HANDLE hLog = ::CreateFileW(logPath.c_str(), GENERIC_WRITE, 0, &sa, CREATE_ALWAYS,
                                FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hLog == INVALID_HANDLE_VALUE) {
        r.output = pw::i18n::t("无法创建日志文件: ") + fsUtf8(logFile);
        return r;
    }

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = hLog;
    si.hStdError = hLog;
    si.hStdInput = nullptr;

    PROCESS_INFORMATION pi{};
    BOOL ok = ::CreateProcessW(nullptr, cmdBuf.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW,
                              nullptr, nullptr, &si, &pi);
    ::CloseHandle(hLog);

    if (!ok) {
        r.output = pw::i18n::t("CreateProcess 失败, error=") + std::to_string(::GetLastError());
        return r;
    }
    r.launched = true;

    HANDLE job = ::CreateJobObjectW(nullptr, nullptr);
    if (job != nullptr) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION info{};
        info.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        ::SetInformationJobObject(job, JobObjectExtendedLimitInformation, &info, sizeof(info));
        ::AssignProcessToJobObject(job, pi.hProcess);
    }

    DWORD waitMethod = ::WaitForSingleObject(pi.hProcess, static_cast<DWORD>(timeoutMs));
    if (waitMethod == WAIT_TIMEOUT) {
        r.timedOut = true;
        if (job != nullptr) {
            ::TerminateJobObject(job, 1);
        } else {
            ::TerminateProcess(pi.hProcess, 1);
        }
        ::WaitForSingleObject(pi.hProcess, 5000);
        r.exitCode = -1;
    } else {
        DWORD code = 0;
        ::GetExitCodeProcess(pi.hProcess, &code);
        r.exitCode = static_cast<int>(code);
    }

    ::CloseHandle(pi.hThread);
    ::CloseHandle(pi.hProcess);
    if (job != nullptr) ::CloseHandle(job);

    r.output = readTextFile(logFile, 32u * 1024u);
    return r;
}

#else  // !_WIN32

std::string shellQuote(const std::string& s) {
    std::string out = "'";
    for (char c : s) {
        if (c == '\'') out += "'\\''";
        else out.push_back(c);
    }
    out += "'";
    return out;
}

// POSIX 分支保留可用实现，但不做超时强杀（骨架阶段够用，后续可换 posix_spawn + 定时器）
RunResult runProcess(const fs::path& exe, const std::vector<std::string>& args, int timeoutMs,
                     const fs::path& logFile) {
    (void)timeoutMs;
    RunResult r;
    std::string cmd = shellQuote(fsUtf8(exe));
    for (const auto& a : args) cmd += " " + shellQuote(a);
    cmd += " > " + shellQuote(fsUtf8(logFile)) + " 2>&1";
    r.launched = true;
    int rc = std::system(cmd.c_str());
    r.exitCode = rc;
    r.output = readTextFile(logFile, 32u * 1024u);
    return r;
}

#endif

}  // namespace

OfficeRunner::OfficeRunner(Config config) : config_(std::move(config)) {}

bool OfficeRunner::available() const {
    if (!config_.libreOfficeConfigured()) return false;
    std::error_code ec;
    return existsLong(config_.libreOfficePath);
}

fs::path OfficeRunner::isolatedProfileDir() const {
    fs::path profile = config_.workDir / "lo-profile";
    std::error_code ec;
    fs::create_directories(profile, ec);
    return profile;
}

fs::path OfficeRunner::acquireProfile() {
    std::lock_guard<std::mutex> lock(profileMutex_);
    if (!freeProfiles_.empty()) {
        fs::path p = freeProfiles_.back();
        freeProfiles_.pop_back();
        return p;
    }
    fs::path p = config_.workDir / "lo-profiles" / ("p" + std::to_string(profileCount_++));
    std::error_code ec;
    fs::create_directories(p, ec);
    return p;
}

void OfficeRunner::releaseProfile(const fs::path& profile) {
    std::lock_guard<std::mutex> lock(profileMutex_);
    freeProfiles_.push_back(profile);
}

std::string OfficeRunner::extensionFromFilter(const std::string& filter) {
    std::size_t pos = filter.find(':');
    std::string ext = (pos == std::string::npos) ? filter : filter.substr(0, pos);
    ext = toLower(ext);
    return ext.empty() ? std::string("pdf") : ext;
}

std::string OfficeRunner::versionString() const {
    // 缓存里只放成功结果：soffice 冷启动可能要几十秒（杀毒扫描 + 首次建 profile），
    // 一次没拿到就永久缓存"未知"，界面上再也修不回来。
    if (!versionCache_.empty()) return versionCache_;
    if (!available()) {
        versionCache_ = pw::i18n::t("未配置");
        return versionCache_;
    }
    // 和真正转换用同一套无窗口参数：裸 --version 会去碰默认 profile，
    // 首次运行会弹 LibreOffice 自己的启动向导/提示窗口
    std::vector<std::string> args;
    args.push_back("--headless");
    args.push_back("--invisible");
    args.push_back("--norestore");
    args.push_back("--nofirststartwizard");
    args.push_back("--nolockcheck");
    args.push_back("--nodefault");
    args.push_back("-env:UserInstallation=" + toFileUrl(isolatedProfileDir()));
    args.push_back("--version");
    fs::path logFile = config_.workDir / "lo-version.log";
    std::error_code ec;
    fs::create_directories(config_.workDir, ec);
    RunResult r = runProcess(config_.libreOfficePath, args, 60000, logFile);
    std::string text = r.output;
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) text.pop_back();
    if (text.empty()) {
        // 不写缓存，下次调用/刷新会重试
        return r.timedOut ? pw::i18n::t("未知（soffice --version 超时，稍后重试）")
                          : pw::i18n::t("未知（soffice --version 无输出，稍后重试）");
    }
    versionCache_ = text;
    return versionCache_;
}

Status OfficeRunner::convert(const OfficeRequest& request, std::vector<fs::path>* outputs) {
    if (request.inputs.empty()) {
        return Status::error(ErrorCode::InvalidArgument, pw::i18n::t("输入文件列表为空"));
    }
    if (!available()) {
        return Status::error(ErrorCode::BackendMissing,
                             pw::i18n::t("LibreOffice 未配置或路径不存在") + libreOfficeSetupHint(config_));
    }

    std::error_code ec;
    fs::create_directories(request.outDir, ec);
    fs::create_directories(config_.workDir, ec);

    // 本次转换独占一个 profile，函数退出（含任何提前 return）时归还池子
    fs::path profile = acquireProfile();
    std::unique_ptr<fs::path, std::function<void(fs::path*)>> profileGuard(
        &profile, [this](fs::path* p) { releaseProfile(*p); });

    for (const auto& in : request.inputs) {
        if (!existsLong(in)) {
            return Status::error(ErrorCode::IoError, pw::i18n::t("输入文件不存在: ") + fsUtf8(in));
        }
    }

    std::string filter = request.filter.empty() ? filterForPdf() : request.filter;
    int timeoutMs = request.timeoutMs > 0 ? request.timeoutMs : config_.officeTimeoutMs;

    std::vector<std::string> args;
    args.push_back("--headless");
    args.push_back("--invisible");
    args.push_back("--norestore");
    args.push_back("--nofirststartwizard");
    args.push_back("--nolockcheck");
    args.push_back("--nodefault");
    args.push_back("-env:UserInstallation=" + toFileUrl(profile));
    args.push_back("--convert-to");
    args.push_back(filter);
    args.push_back("--outdir");
    args.push_back(fsUtf8(request.outDir));
    for (const auto& in : request.inputs) args.push_back(fsUtf8(in));

    if (request.log) {
        std::string line = "run: " + fsUtf8(config_.libreOfficePath.filename());
        for (const auto& a : args) {
            // 只记录关键参数，避免把整串路径刷屏
            if (a.rfind("--convert-to", 0) == 0 || a.rfind("-env:", 0) == 0) line += " " + a;
        }
        request.log(line);
    }

    // 日志文件按调用唯一，避免并发转换互踩同一个文件；读完即删，不留垃圾
    static std::atomic<unsigned long> logSeq{0};
    fs::path logFile = config_.workDir / ("lo-convert-" + std::to_string(++logSeq) + ".log");
    RunResult r = runProcess(config_.libreOfficePath, args, timeoutMs, logFile);
    std::error_code logEc;
    fs::remove(logFile, logEc);

    if (r.timedOut) {
        return Status::error(ErrorCode::Timeout,
                             pw::i18n::t("LibreOffice 转换超时（") + std::to_string(timeoutMs) + pw::i18n::t(" ms）"));
    }
    if (r.exitCode != 0) {
        return Status::error(ErrorCode::ProcessFailed,
                             pw::i18n::t("LibreOffice 退出码 ") + std::to_string(r.exitCode) + pw::i18n::t("；输出: ") +
                                 tail(r.output, 2000));
    }

    std::string ext = extensionFromFilter(filter);
    std::vector<fs::path> produced;
    for (const auto& in : request.inputs) {
        fs::path expected = request.outDir / (fsUtf8(in.stem()) + "." + ext);
        if (!existsLong(expected)) {
            return Status::error(ErrorCode::ProcessFailed,
                                 pw::i18n::t("转换后未找到产物: ") + fsUtf8(expected) + pw::i18n::t("；LibreOffice 输出: ") +
                                     tail(r.output, 1500));
        }
        produced.push_back(expected);
    }

    if (outputs != nullptr) *outputs = produced;
    if (request.log) {
        request.log(pw::i18n::t("完成: ") + std::to_string(produced.size()) + pw::i18n::t(" 个文件 -> ") +
                    fsUtf8(request.outDir));
    }
    return Status::success();
}

Status OfficeRunner::convertOne(const fs::path& input, const fs::path& outDir,
                                const std::string& filter, fs::path* output, LogFn log) {
    OfficeRequest req;
    req.inputs.push_back(input);
    req.outDir = outDir;
    req.filter = filter;
    req.log = std::move(log);

    std::vector<fs::path> outputs;
    Status st = convert(req, &outputs);
    if (!st.ok()) return st;
    if (output != nullptr) *output = outputs.front();
    return Status::success();
}

}  // namespace pw
