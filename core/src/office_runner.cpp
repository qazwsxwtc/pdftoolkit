#include "pdfword/office_runner.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <sstream>
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

std::string toLower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

std::string readTextFile(const fs::path& path, std::size_t maxBytes) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return std::string();
    std::string s;
    s.resize(maxBytes);
    in.read(s.data(), static_cast<std::streamsize>(maxBytes));
    s.resize(static_cast<std::size_t>(in.gcount()));
    return s;
}

// LibreOffice 的 -env:UserInstallation 需要 file:/// URL 形式，且路径分隔符要转成 /
std::string toFileUrl(const fs::path& p) {
#ifdef _WIN32
    std::string s = p.string();
#else
    std::string s = p.string();
#endif
    std::replace(s.begin(), s.end(), '\\', '/');
    if (!s.empty() && s[0] != '/') s.insert(s.begin(), '/');
    return "file://" + s;
}

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

std::wstring toWide(const std::string& s) {
    if (s.empty()) return std::wstring();
    int n = ::MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), nullptr, 0);
    if (n <= 0) return std::wstring();
    std::wstring w(static_cast<std::size_t>(n), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

// 启动外部进程并把 stdout/stderr 落到 logFile。
// 超时时通过 JobObject 杀整棵进程树（soffice.exe 会再拉起 soffice.bin）。
RunResult runProcess(const fs::path& exe, const std::vector<std::string>& args, int timeoutMs,
                     const fs::path& logFile) {
    RunResult r;

    std::wstring cmd = L"\"" + toWide(exe.string()) + L"\"";
    for (const auto& a : args) {
        cmd += L" \"";
        cmd += toWide(a);
        cmd += L"\"";
    }
    std::vector<wchar_t> cmdBuf(cmd.begin(), cmd.end());
    cmdBuf.push_back(L'\0');

    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    sa.lpSecurityDescriptor = nullptr;

    std::wstring logPath = toWide(logFile.string());
    HANDLE hLog = ::CreateFileW(logPath.c_str(), GENERIC_WRITE, 0, &sa, CREATE_ALWAYS,
                                FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hLog == INVALID_HANDLE_VALUE) {
        r.output = "无法创建日志文件: " + logFile.string();
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
        r.output = "CreateProcess 失败, error=" + std::to_string(::GetLastError());
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
    std::string cmd = shellQuote(exe.string());
    for (const auto& a : args) cmd += " " + shellQuote(a);
    cmd += " > " + shellQuote(logFile.string()) + " 2>&1";
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
    return fs::exists(config_.libreOfficePath, ec);
}

fs::path OfficeRunner::isolatedProfileDir() const {
    fs::path profile = config_.workDir / "lo-profile";
    std::error_code ec;
    fs::create_directories(profile, ec);
    return profile;
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
        versionCache_ = "未配置";
        return versionCache_;
    }
    std::vector<std::string> args = {"--version"};
    fs::path logFile = config_.workDir / "lo-version.log";
    std::error_code ec;
    fs::create_directories(config_.workDir, ec);
    RunResult r = runProcess(config_.libreOfficePath, args, 60000, logFile);
    std::string text = r.output;
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) text.pop_back();
    if (text.empty()) {
        // 不写缓存，下次调用/刷新会重试
        return r.timedOut ? "未知（soffice --version 超时，稍后重试）"
                          : "未知（soffice --version 无输出，稍后重试）";
    }
    versionCache_ = text;
    return versionCache_;
}

Status OfficeRunner::convert(const OfficeRequest& request, std::vector<fs::path>* outputs) {
    if (request.inputs.empty()) {
        return Status::error(ErrorCode::InvalidArgument, "输入文件列表为空");
    }
    if (!available()) {
        return Status::error(ErrorCode::BackendMissing,
                             "LibreOffice 未配置或路径不存在: " +
                                 config_.libreOfficePath.string() +
                                 "（在 pdfword.ini 里设置 libre_office=便携目录）");
    }

    std::error_code ec;
    fs::create_directories(request.outDir, ec);
    fs::create_directories(config_.workDir, ec);

    for (const auto& in : request.inputs) {
        if (!fs::exists(in, ec)) {
            return Status::error(ErrorCode::IoError, "输入文件不存在: " + in.string());
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
    args.push_back("-env:UserInstallation=" + toFileUrl(isolatedProfileDir()));
    args.push_back("--convert-to");
    args.push_back(filter);
    args.push_back("--outdir");
    args.push_back(request.outDir.string());
    for (const auto& in : request.inputs) args.push_back(in.string());

    if (request.log) {
        std::string line = "run: " + config_.libreOfficePath.filename().string();
        for (const auto& a : args) {
            // 只记录关键参数，避免把整串路径刷屏
            if (a.rfind("--convert-to", 0) == 0 || a.rfind("-env:", 0) == 0) line += " " + a;
        }
        request.log(line);
    }

    fs::path logFile = config_.workDir / "lo-convert.log";
    RunResult r = runProcess(config_.libreOfficePath, args, timeoutMs, logFile);

    if (r.timedOut) {
        return Status::error(ErrorCode::Timeout,
                             "LibreOffice 转换超时（" + std::to_string(timeoutMs) + " ms）");
    }
    if (r.exitCode != 0) {
        return Status::error(ErrorCode::ProcessFailed,
                             "LibreOffice 退出码 " + std::to_string(r.exitCode) + "；输出: " +
                                 tail(r.output, 2000));
    }

    std::string ext = extensionFromFilter(filter);
    std::vector<fs::path> produced;
    for (const auto& in : request.inputs) {
        fs::path expected = request.outDir / (in.stem().string() + "." + ext);
        if (!fs::exists(expected, ec)) {
            return Status::error(ErrorCode::ProcessFailed,
                                 "转换后未找到产物: " + expected.string() + "；LibreOffice 输出: " +
                                     tail(r.output, 1500));
        }
        produced.push_back(expected);
    }

    if (outputs != nullptr) *outputs = produced;
    if (request.log) {
        request.log("完成: " + std::to_string(produced.size()) + " 个文件 -> " +
                    request.outDir.string());
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
