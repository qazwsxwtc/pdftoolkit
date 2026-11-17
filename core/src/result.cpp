#include "pdfword/result.hpp"

namespace pw {

const char* errorCodeName(ErrorCode code) {
    switch (code) {
        case ErrorCode::Ok: return "Ok";
        case ErrorCode::InvalidArgument: return "InvalidArgument";
        case ErrorCode::IoError: return "IoError";
        case ErrorCode::Unsupported: return "Unsupported";
        case ErrorCode::BackendMissing: return "BackendMissing";
        case ErrorCode::ProcessFailed: return "ProcessFailed";
        case ErrorCode::Timeout: return "Timeout";
        case ErrorCode::Cancelled: return "Cancelled";
        case ErrorCode::Internal: return "Internal";
    }
    return "Unknown";
}

std::string Status::toString() const {
    if (ok()) return "Ok";
    return std::string(errorCodeName(code_)) + ": " + message_;
}

}  // namespace pw
