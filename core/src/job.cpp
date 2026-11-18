#include "pdfword/job.hpp"
#include "pdfword/i18n.hpp"

#include <utility>
#include "pdfword/path_util.hpp"

namespace pw {

JobQueue::JobQueue(std::shared_ptr<Converter> converter) : converter_(std::move(converter)) {
    worker_ = std::thread(&JobQueue::workerLoop, this);
}

JobQueue::~JobQueue() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stop_ = true;
    }
    cv_.notify_all();
    if (worker_.joinable()) worker_.join();
}

void JobQueue::setListener(Listener listener) {
    std::lock_guard<std::mutex> lock(mutex_);
    listener_ = std::move(listener);
}

int JobQueue::enqueue(const fs::path& source, const fs::path& output, const std::string& target,
                      const std::string& password) {
    int id = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        id = nextId_++;
        JobInfo info;
        info.id = id;
        info.source = source;
        info.output = output;
        info.target = target;
        info.state = JobState::Pending;
        info.message = pw::i18n::t("排队中");
        info.password = password;
        jobs_[id] = info;
        pending_.push_back(id);
    }
    cv_.notify_one();
    return id;
}

// 注意：只有还在排队（Pending）的任务能被真正取消。
// 已经开始转换的任务（LibreOffice 子进程）在骨架阶段不强杀，会跑完并保持 Done 状态。
void JobQueue::cancel(int jobId) {
    JobInfo copy;
    bool hasCopy = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = jobs_.find(jobId);
        if (it == jobs_.end()) return;
        if (it->second.state == JobState::Pending) {
            cancelled_.insert(jobId);
            it->second.state = JobState::Cancelled;
            it->second.message = pw::i18n::t("已取消");
            copy = it->second;
            hasCopy = true;
        }
    }
    if (hasCopy) notify(copy);
}

void JobQueue::cancelAll() {
    std::vector<JobInfo> copies;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto& kv : jobs_) {
            if (kv.second.state == JobState::Pending) {
                cancelled_.insert(kv.first);
                kv.second.state = JobState::Cancelled;
                kv.second.message = pw::i18n::t("已取消");
                copies.push_back(kv.second);
            }
        }
    }
    for (const auto& c : copies) notify(c);
}

std::vector<JobInfo> JobQueue::snapshot() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<JobInfo> out;
    out.reserve(jobs_.size());
    for (const auto& kv : jobs_) out.push_back(kv.second);
    return out;
}

void JobQueue::waitAll() {
    std::unique_lock<std::mutex> lock(mutex_);
    idleCv_.wait(lock, [this] { return pending_.empty() && !running_; });
}

void JobQueue::notify(const JobInfo& info) {
    Listener listener;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        listener = listener_;
    }
    if (listener) listener(info);
}

void JobQueue::workerLoop() {
    while (true) {
        int id = -1;
        fs::path source;
        fs::path output;
        std::string target;
        std::string password;

        {
            std::unique_lock<std::mutex> lock(mutex_);
            cv_.wait(lock, [this] { return stop_ || !pending_.empty(); });
            if (stop_ && pending_.empty()) break;
            if (pending_.empty()) continue;

            id = pending_.front();
            pending_.erase(pending_.begin());

            auto it = jobs_.find(id);
            if (it == jobs_.end()) continue;

            if (cancelled_.count(id) != 0) {
                it->second.state = JobState::Cancelled;
                it->second.message = pw::i18n::t("已取消");
                JobInfo copy = it->second;
                lock.unlock();
                notify(copy);
                idleCv_.notify_all();
                continue;
            }

            it->second.state = JobState::Running;
            it->second.progress = 0;
            it->second.message = pw::i18n::t("转换中");
            source = it->second.source;
            output = it->second.output;
            target = it->second.target;
            password = it->second.password;

            JobInfo runningCopy = it->second;
            running_ = true;
            lock.unlock();
            notify(runningCopy);
        }

        ConvertOptions options;
        options.output = output;
        options.pdfPassword = password;
        options.progress = [this, id](int percent, const std::string& message) {
            JobInfo copy;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                auto it = jobs_.find(id);
                if (it == jobs_.end()) return;
                it->second.progress = percent;
                it->second.message = message;
                copy = it->second;
            }
            notify(copy);
        };

        fs::path produced;
        Status st = converter_ ? converter_->convert(source, options, target, &produced)
                               : Status::error(ErrorCode::Internal, pw::i18n::t("Converter 为空"));

        JobInfo finished;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            auto it = jobs_.find(id);
            if (it != jobs_.end()) {
                if (st.ok()) {
                    it->second.state = JobState::Done;
                    it->second.progress = 100;
                    it->second.message = pw::i18n::t("完成: ") + fsUtf8(produced);
                } else {
                    it->second.state = JobState::Failed;
                    it->second.message = st.toString();
                }
                finished = it->second;
            }
            running_ = false;
        }
        idleCv_.notify_all();
        if (finished.id != 0) notify(finished);
    }
}

}  // namespace pw
