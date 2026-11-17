#include "pdfword/page_selection.hpp"

#include <algorithm>
#include <cctype>

namespace pw {

namespace {

void replaceAll(std::string* s, const std::string& from, const std::string& to) {
    if (from.empty()) return;
    std::size_t pos = 0;
    while ((pos = s->find(from, pos)) != std::string::npos) {
        s->replace(pos, from.size(), to);
        pos += to.size();
    }
}

std::string trim(const std::string& s) {
    std::size_t b = 0;
    std::size_t e = s.size();
    while (b < e && std::isspace(static_cast<unsigned char>(s[b])) != 0) ++b;
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1])) != 0) --e;
    return s.substr(b, e - b);
}

bool parseInt(const std::string& s, int* out) {
    if (s.empty()) return false;
    for (char c : s) {
        if (std::isdigit(static_cast<unsigned char>(c)) == 0) return false;
    }
    try {
        const long v = std::stol(s);
        if (v < 0 || v > 100000000L) return false;
        *out = static_cast<int>(v);
        return true;
    } catch (...) {
        return false;
    }
}

}  // namespace

Result<std::vector<int>> parsePageSpec(const std::string& spec, int pageCount) {
    std::string s = spec;
    // 中文标点常见于从文档里复制出来的页码串，先归一化，省得用户被"格式错误"绕晕
    replaceAll(&s, "，", ",");
    replaceAll(&s, "、", ",");
    replaceAll(&s, "；", ",");
    replaceAll(&s, "－", "-");
    replaceAll(&s, "—", "-");
    replaceAll(&s, "–", "-");

    if (trim(s).empty()) {
        return Status::error(ErrorCode::InvalidArgument,
                             "页码范围为空，示例：1-3,5,8-10（从 1 开始、闭区间）");
    }

    std::vector<int> pages;   // 0 基
    std::size_t pos = 0;
    while (pos <= s.size()) {
        std::size_t comma = s.find(',', pos);
        std::string token = trim(s.substr(pos, comma == std::string::npos ? std::string::npos : comma - pos));
        if (!token.empty()) {
            const std::size_t dash = token.find('-');
            int first = 0;
            int last = 0;
            if (dash == std::string::npos) {
                if (!parseInt(token, &first)) {
                    return Status::error(ErrorCode::InvalidArgument, "无法识别的页码: " + token);
                }
                last = first;
            } else {
                std::string a = trim(token.substr(0, dash));
                std::string b = trim(token.substr(dash + 1));
                // "5-" 这种半开区间按 5 到末尾处理，对用户更宽容
                if (a.empty()) a = "1";
                if (b.empty()) b = pageCount > 0 ? std::to_string(pageCount) : a;
                if (!parseInt(a, &first) || !parseInt(b, &last)) {
                    return Status::error(ErrorCode::InvalidArgument, "无法识别的页码区间: " + token);
                }
            }
            if (first < 1) {
                return Status::error(ErrorCode::InvalidArgument, "页码从 1 开始，不能是 " + std::to_string(first));
            }
            if (last < first) {
                return Status::error(ErrorCode::InvalidArgument,
                                     "区间起点大于终点: " + token);
            }
            if (pageCount > 0 && last > pageCount) {
                return Status::error(ErrorCode::InvalidArgument,
                                     "页码 " + std::to_string(last) + " 超出范围（本文件共 " +
                                         std::to_string(pageCount) + " 页）");
            }
            for (int p = first; p <= last; ++p) {
                const int idx = p - 1;
                if (std::find(pages.begin(), pages.end(), idx) == pages.end()) pages.push_back(idx);
            }
        }
        if (comma == std::string::npos) break;
        pos = comma + 1;
    }

    if (pages.empty()) {
        return Status::error(ErrorCode::InvalidArgument,
                             "页码范围为空，示例：1-3,5,8-10（从 1 开始、闭区间）");
    }
    return pages;
}

std::string formatPageSpec(const std::vector<int>& pages0) {
    if (pages0.empty()) return std::string();
    std::string out;
    std::size_t i = 0;
    while (i < pages0.size()) {
        std::size_t j = i;
        while (j + 1 < pages0.size() && pages0[j + 1] == pages0[j] + 1) ++j;
        if (!out.empty()) out += ",";
        out += std::to_string(pages0[i] + 1);
        if (j > i) out += "-" + std::to_string(pages0[j] + 1);
        i = j + 1;
    }
    return out;
}

}  // namespace pw
