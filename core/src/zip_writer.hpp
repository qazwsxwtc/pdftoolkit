#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "pdfword/config.hpp"
#include "pdfword/result.hpp"

namespace pw {
namespace detail {

// 只用 store（不压缩）方式的 zip 写入器。
// 目的：生成 .docx 时不引入 zlib/libzip 之类的第三方依赖，
// OOXML 只要求合法 zip 容器，store 方式完全合规，体积稍大但优先零依赖。
class ZipWriter {
public:
    void add(const std::string& name, const std::string& data);

    // 目标目录不存在时自动创建
    Status write(const fs::path& path);

private:
    struct Entry {
        std::string name;
        std::string data;
        std::uint32_t crc = 0;
        std::uint32_t offset = 0;
    };

    std::vector<Entry> entries_;
};

std::uint32_t crc32(const std::string& data);

}  // namespace detail
}  // namespace pw
