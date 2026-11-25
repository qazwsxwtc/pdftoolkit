#pragma once

#include <filesystem>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

#include "pdfword/config.hpp"
#include "pdfword/result.hpp"

namespace pw {

using LogFn = std::function<void(const std::string&)>;
// 进度回调：percent 0..100，message 为人类可读阶段描述
using ProgressFn = std::function<void(int percent, const std::string& message)>;

struct OfficeRequest {
    std::vector<fs::path> inputs;  // 批处理时一次性传给 soffice，能省掉多次进程启动
    fs::path outDir;
    std::string filter;            // 例如 "pdf:writer_pdf_Export"
    int timeoutMs = 0;             // 0 = 使用 Config::officeTimeoutMs
    LogFn log;
};

// LibreOffice headless 调用器。
// 关键点：每次调用都用 -env:UserInstallation 指向独立 profile，
// 避免和用户已打开的 LibreOffice 抢锁、也避免首次启动向导弹窗。
class OfficeRunner {
public:
    explicit OfficeRunner(Config config);

    const Config& config() const { return config_; }
    bool available() const;
    // 真的去跑一次 soffice --version（会起进程，别放在启动路径上）
    std::string versionString() const;
    // 只看已经探测到的版本（没探测过就是空），启动写日志用这个，不起进程
    std::string cachedVersion() const { return versionCache_; }

    Status convert(const OfficeRequest& request, std::vector<fs::path>* outputs = nullptr);
    Status convertOne(const fs::path& input, const fs::path& outDir, const std::string& filter,
                      fs::path* output = nullptr, LogFn log = nullptr);

    // 隔离 profile 目录（首次调用时创建）
    fs::path isolatedProfileDir() const;

    static std::string filterForPdf() { return "pdf:writer_pdf_Export"; }
    static std::string filterForDocx() { return "docx:MS Word 2007 XML"; }

    // 从 filter 里取目标扩展名："pdf:writer_pdf_Export" -> "pdf"
    static std::string extensionFromFilter(const std::string& filter);

private:
    // 并发隔离：每次 convert 独占一个 profile，跑完归还池子复用。
    // 同时跑的多个 soffice 各用各的 profile，不会抢同一把 profile 锁；
    // 顺序调用则复用已建好的 warm profile，省掉重复冷启动。
    fs::path acquireProfile();
    void releaseProfile(const fs::path& profile);

    Config config_;
    mutable std::string versionCache_;
    std::mutex profileMutex_;
    std::vector<fs::path> freeProfiles_;   // 空闲可复用的 profile 目录
    int profileCount_ = 0;                 // 已创建的 profile 总数
};

}  // namespace pw
