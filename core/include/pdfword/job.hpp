#pragma once

#include <condition_variable>
#include <functional>
#include <map>
#include <set>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "pdfword/converter.hpp"

namespace pw {

enum class JobState { Pending, Running, Done, Failed, Cancelled };

struct JobInfo {
    int id = 0;
    fs::path source;
    fs::path output;
    std::string target;  // "pdf" / "docx"
    JobState state = JobState::Pending;
    int progress = 0;
    std::string message;
    // 加密源文件（PDF）的打开密码，转换时透传给 Converter::convert
    std::string password;
};

// 单工作线程的串行任务队列。
// 为什么要有它：UI 和未来 web 壳都需要「提交任务 -> 收进度 -> 查状态」这套语义，
// 放在 core 里两边共用，避免 UI 线程里各写一套。
class JobQueue {
public:
    using Listener = std::function<void(const JobInfo&)>;

    explicit JobQueue(std::shared_ptr<Converter> converter);
    ~JobQueue();
    JobQueue(const JobQueue&) = delete;
    JobQueue& operator=(const JobQueue&) = delete;

    void setListener(Listener listener);

    int enqueue(const fs::path& source, const fs::path& output, const std::string& target,
                const std::string& password = std::string());
    void cancel(int jobId);
    void cancelAll();

    std::vector<JobInfo> snapshot() const;

    // 阻塞直到队列清空（CLI 批处理用）
    void waitAll();

private:
    void workerLoop();
    void notify(const JobInfo& info);

    std::shared_ptr<Converter> converter_;
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::condition_variable idleCv_;
    std::vector<int> pending_;                 // FIFO
    std::map<int, JobInfo> jobs_;
    std::set<int> cancelled_;
    Listener listener_;
    std::thread worker_;
    bool stop_ = false;
    bool running_ = false;
    int nextId_ = 1;
};

}  // namespace pw
