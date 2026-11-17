#pragma once

#include <string>
#include <utility>

namespace pw {

// 统一错误码。任何跨模块边界（core -> C ABI -> web 壳）的错误都只走这里，
// 不允许抛异常穿透到调用方，方便 C#/Go/Python 之类的宿主安全接住。
enum class ErrorCode : int {
    Ok = 0,
    InvalidArgument = 1,
    IoError = 2,
    Unsupported = 3,     // 当前后端不支持该操作（例如 stub 后端）
    BackendMissing = 4,  // MuPDF / LibreOffice 未就绪
    ProcessFailed = 5,   // 外部进程返回非 0
    Timeout = 6,
    Cancelled = 7,
    Internal = 8,
};

const char* errorCodeName(ErrorCode code);

class Status {
public:
    Status() = default;
    Status(ErrorCode code, std::string message)
        : code_(code), message_(std::move(message)) {}

    // 注意：不能叫 ok()，会与下面的成员函数 ok() 重载冲突
    static Status success() { return Status(); }
    static Status error(ErrorCode code, std::string message) {
        return Status(code, std::move(message));
    }

    bool ok() const { return code_ == ErrorCode::Ok; }
    explicit operator bool() const { return ok(); }

    ErrorCode code() const { return code_; }
    const std::string& message() const { return message_; }

    // "BackendMissing: LibreOffice 未配置" 这类可直接写日志的一行文本
    std::string toString() const;

private:
    ErrorCode code_ = ErrorCode::Ok;
    std::string message_;
};

// 轻量 Result<T>：不依赖 std::expected，MSVC 的 /std:c++17 就能编。
template <class T>
class Result {
public:
    Result(T value) : value_(std::move(value)) {}              // NOLINT: 故意隐式
    Result(Status status) : status_(std::move(status)) {}      // NOLINT: 故意隐式

    bool ok() const { return status_.ok(); }
    explicit operator bool() const { return ok(); }

    T& value() { return value_; }
    const T& value() const { return value_; }
    T valueOr(T fallback) const { return ok() ? value_ : std::move(fallback); }

    const Status& status() const { return status_; }
    ErrorCode code() const { return status_.code(); }
    const std::string& message() const { return status_.message(); }

private:
    T value_{};
    Status status_{};
};

}  // namespace pw
