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

// 多线程任务队列：一个工作线程同一时刻只转换一个文件。
// 为什么要有它：UI 和未来 web 壳都需要「提交任务 -> 收进度 -> 查状态」这套语义，
// 放在 core 里两边共用，避免 UI 线程里各写一套。
//
// 并行规则（与批量处理窗口一致）：
//   * 工作线程按需增长，总数不超过 floor(逻辑内核数 × 1.3)，至少 1；
//   * 空闲线程自行退出，来新任务再补，常驻线程数始终 ≤ 上限；
//   * 每个文件由一个线程从头跑到尾，文件之间互不共享状态。
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

    // 工作线程上限 = floor(逻辑内核数 × 1.3)，至少 1（诊断 / 测试用）
    int workerLimit() const { return maxWorkers_; }

private:
    void workerLoop();
    void notify(const JobInfo& info);
    // 持锁调用：把活线程数补到 min(上限, 待处理 + 在跑)，多出来的任务才有线程接手
    void spawnWorkersLocked();

    std::shared_ptr<Converter> converter_;
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::condition_variable idleCv_;
    std::vector<int> pending_;                 // FIFO
    std::map<int, JobInfo> jobs_;
    std::set<int> cancelled_;
    Listener listener_;
    bool stop_ = false;
    int runningCount_ = 0;                     // 正在转换的任务数
    int liveWorkers_ = 0;                      // 当前存活的工作线程数
    int maxWorkers_ = 1;                       // floor(逻辑核数 × 1.3)
    int nextId_ = 1;
};

}  // namespace pw
