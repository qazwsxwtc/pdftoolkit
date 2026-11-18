#include "zip_writer.hpp"
#include "pdfword/i18n.hpp"

#include <cstdint>
#include <fstream>
#include <vector>
#include "pdfword/path_util.hpp"

namespace pw {
namespace detail {

namespace {

const std::uint32_t kLocalSig = 0x04034b50u;
const std::uint32_t kCentralSig = 0x02014b50u;
const std::uint32_t kEndSig = 0x06054b50u;

void append16(std::vector<std::uint8_t>& out, std::uint16_t v) {
    out.push_back(static_cast<std::uint8_t>(v & 0xFF));
    out.push_back(static_cast<std::uint8_t>((v >> 8) & 0xFF));
}

void append32(std::vector<std::uint8_t>& out, std::uint32_t v) {
    for (int i = 0; i < 4; ++i) {
        out.push_back(static_cast<std::uint8_t>((v >> (8 * i)) & 0xFF));
    }
}

// 固定时间戳 2020-01-01 00:00:00，保证同样的输入产出字节一致的 docx，方便做 diff/回归。
const std::uint16_t kDosTime = 0x0000;
const std::uint16_t kDosDate = 0x5021;

}  // namespace

std::uint32_t crc32(const std::string& data) {
    static std::uint32_t table[256];
    static bool init = false;
    if (!init) {
        for (std::uint32_t i = 0; i < 256; ++i) {
            std::uint32_t c = i;
            for (int k = 0; k < 8; ++k) {
                c = (c & 1u) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            }
            table[i] = c;
        }
        init = true;
    }
    std::uint32_t crc = 0xFFFFFFFFu;
    for (unsigned char ch : data) {
        crc = table[(crc ^ static_cast<std::uint32_t>(ch)) & 0xFFu] ^ (crc >> 8);
    }
    return crc ^ 0xFFFFFFFFu;
}

void ZipWriter::add(const std::string& name, const std::string& data) {
    Entry e;
    e.name = name;
    e.data = data;
    e.crc = crc32(data);
    entries_.push_back(e);
}

Status ZipWriter::write(const fs::path& path) {
    std::error_code ec;
    if (!path.parent_path().empty()) {
        fs::create_directories(path.parent_path(), ec);
    }

    std::vector<std::uint8_t> file;
    file.reserve(64 * 1024);

    for (auto& e : entries_) {
        e.offset = static_cast<std::uint32_t>(file.size());
        append32(file, kLocalSig);
        append16(file, 20);                            // version needed
        append16(file, 0);                             // flags
        append16(file, 0);                             // method = store
        append16(file, kDosTime);
        append16(file, kDosDate);
        append32(file, e.crc);
        append32(file, static_cast<std::uint32_t>(e.data.size()));
        append32(file, static_cast<std::uint32_t>(e.data.size()));
        append16(file, static_cast<std::uint16_t>(e.name.size()));
        append16(file, 0);                             // extra len
        file.insert(file.end(), e.name.begin(), e.name.end());
        file.insert(file.end(), e.data.begin(), e.data.end());
    }

    const std::uint32_t centralStart = static_cast<std::uint32_t>(file.size());
    for (const auto& e : entries_) {
        append32(file, kCentralSig);
        append16(file, 20);                            // version made by
        append16(file, 20);                            // version needed
        append16(file, 0);                             // flags
        append16(file, 0);                             // method
        append16(file, kDosTime);
        append16(file, kDosDate);
        append32(file, e.crc);
        append32(file, static_cast<std::uint32_t>(e.data.size()));
        append32(file, static_cast<std::uint32_t>(e.data.size()));
        append16(file, static_cast<std::uint16_t>(e.name.size()));
        append16(file, 0);                             // extra
        append16(file, 0);                             // comment
        append16(file, 0);                             // disk
        append16(file, 0);                             // internal attr
        append32(file, 0);                             // external attr
        append32(file, e.offset);
        file.insert(file.end(), e.name.begin(), e.name.end());
    }
    const std::uint32_t centralSize = static_cast<std::uint32_t>(file.size()) - centralStart;

    append32(file, kEndSig);
    append16(file, 0);
    append16(file, 0);
    append16(file, static_cast<std::uint16_t>(entries_.size()));
    append16(file, static_cast<std::uint16_t>(entries_.size()));
    append32(file, centralSize);
    append32(file, centralStart);
    append16(file, 0);

    #ifdef _WIN32
    std::ofstream out(fsPathLong(path).wstring().c_str(), std::ios::binary | std::ios::trunc);
#else
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
#endif
    if (!out) {
        return Status::error(ErrorCode::IoError, pw::i18n::t("无法写入文件: ") + fsUtf8(path));
    }
    out.write(reinterpret_cast<const char*>(file.data()), static_cast<std::streamsize>(file.size()));
    if (!out) {
        return Status::error(ErrorCode::IoError, pw::i18n::t("写入过程中失败: ") + fsUtf8(path));
    }
    return Status::success();
}

}  // namespace detail
}  // namespace pw
