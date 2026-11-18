#include "pdfword/converter.hpp"
#include "pdfword/i18n.hpp"

#include <atomic>
#include <chrono>
#include <sstream>
#include <string>
#include <vector>

#include "pdfword/docx_writer.hpp"
#include "pdfword/ocr.hpp"
#include "pdfword/path_util.hpp"

namespace pw {

namespace {

std::string extOf(const fs::path& p) {
    std::string e = fsUtf8(p.extension());
    if (!e.empty() && e[0] == '.') e.erase(0, 1);
    std::transform(e.begin(), e.end(), e.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return e;
}

std::vector<std::string> splitLines(const std::string& text) {
    std::vector<std::string> lines;
    std::string current;
    for (char c : text) {
        if (c == '\n') {
            lines.push_back(current);
            current.clear();
        } else if (c != '\r') {
            current.push_back(c);
        }
    }
    if (!current.empty()) lines.push_back(current);
    return lines;
}

std::string trim(const std::string& s) {
    std::size_t b = 0;
    std::size_t e = s.size();
    auto isSpace = [](unsigned char c) { return c == ' ' || c == '\t'; };
    while (b < e && isSpace(static_cast<unsigned char>(s[b]))) ++b;
    while (e > b && isSpace(static_cast<unsigned char>(s[e - 1]))) --e;
    return s.substr(b, e - b);
}

// 目标路径解析规则：
//   - 空           -> 源文件同目录、同名、换扩展名
//   - 已存在目录   -> 目录 + 源 stem + 目标扩展名
//   - 其它         -> 视为文件路径，强制换成目标扩展名
fs::path resolveOutput(const fs::path& source, const fs::path& requested, const std::string& targetExt) {
    std::error_code ec;
    if (requested.empty()) {
        return source.parent_path() / (fsUtf8(source.stem()) + "." + targetExt);
    }
    if (fs::is_directory(requested, ec)) {
        return requested / (fsUtf8(source.stem()) + "." + targetExt);
    }
    fs::path out = requested;
    if (extOf(out) != targetExt) out.replace_extension(targetExt);
    return out;
}

// 唯一临时输出目录：避免同 stem 的并发任务互相覆盖
fs::path makeTempOutDir(const Config& config) {
    static std::atomic<unsigned long> counter{0};
    std::error_code ec;
    auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    fs::path dir = config.workDir / "lo-out" /
                   (std::to_string(stamp) + "-" + std::to_string(++counter));
    fs::create_directories(dir, ec);
    return dir;
}

Status moveFile(const fs::path& from, const fs::path& to) {
    std::error_code ec;
    fs::create_directories(to.parent_path(), ec);
    fs::rename(from, to, ec);
    if (!ec) return Status::success();
    // 跨卷 rename 会失败，退化为 copy + remove
    fs::copy_file(from, to, fs::copy_options::overwrite_existing, ec);
    if (ec) {
        return Status::error(ErrorCode::IoError,
                             pw::i18n::t("无法写入目标文件: ") + fsUtf8(to) + " (" + ec.message() + ")");
    }
    fs::remove(from, ec);
    return Status::success();
}

void report(const ConvertOptions& options, int percent, const std::string& message) {
    if (options.progress) options.progress(percent, message);
    if (options.log) options.log(message);
}

}  // namespace

Converter::Converter(Config config)
    : config_(std::move(config)), office_(config_), backend_(makePdfBackend(config_)) {}

Status Converter::toPdf(const fs::path& source, const ConvertOptions& options, fs::path* produced) {
    std::error_code ec;
    if (!existsLong(source)) {
        return Status::error(ErrorCode::IoError, pw::i18n::t("源文件不存在: ") + fsUtf8(source));
    }
    if (extOf(source) == "pdf") {
        return Status::error(ErrorCode::InvalidArgument, pw::i18n::t("源文件已经是 PDF: ") + fsUtf8(source));
    }
    if (!office_.available()) {
        return Status::error(ErrorCode::BackendMissing,
                             pw::i18n::t("LibreOffice 未配置，无法把 ") + extOf(source) + pw::i18n::t(" 转成 PDF") +
                                 libreOfficeSetupHint(config_));
    }

    const fs::path target = resolveOutput(source, options.output, "pdf");
    const fs::path tempDir = makeTempOutDir(config_);

    report(options, 5, pw::i18n::t("启动 LibreOffice headless: ") + fsUtf8(source.filename()));

    fs::path loOutput;
    LogFn innerLog = options.log;
    Status st = office_.convertOne(source, tempDir, OfficeRunner::filterForPdf(), &loOutput, innerLog);
    if (!st.ok()) {
        std::error_code rmEc;
        fs::remove_all(tempDir, rmEc);
        return st;
    }

    report(options, 90, pw::i18n::t("移动产物 -> ") + fsUtf8(target));
    st = moveFile(loOutput, target);
    fs::remove_all(tempDir, ec);
    if (!st.ok()) return st;

    if (produced != nullptr) *produced = target;
    report(options, 100, pw::i18n::t("PDF 生成完成: ") + fsUtf8(target));
    return Status::success();
}

Status Converter::toDocx(const fs::path& source, const ConvertOptions& options, fs::path* produced) {
    std::error_code ec;
    if (!existsLong(source)) {
        return Status::error(ErrorCode::IoError, pw::i18n::t("源文件不存在: ") + fsUtf8(source));
    }
    std::string ext = extOf(source);
    if (ext == "pdf") {
        return pdfToDocx(source, options, produced);
    }
    if (ext == "docx") {
        return Status::error(ErrorCode::InvalidArgument, pw::i18n::t("源文件已经是 DOCX: ") + fsUtf8(source));
    }
    if (!office_.available()) {
        return Status::error(ErrorCode::BackendMissing,
                             pw::i18n::t("LibreOffice 未配置，无法把 ") + ext + pw::i18n::t(" 转成 DOCX") +
                                 libreOfficeSetupHint(config_));
    }

    const fs::path target = resolveOutput(source, options.output, "docx");
    const fs::path tempDir = makeTempOutDir(config_);

    report(options, 5, pw::i18n::t("启动 LibreOffice headless: ") + fsUtf8(source.filename()));
    fs::path loOutput;
    Status st = office_.convertOne(source, tempDir, OfficeRunner::filterForDocx(), &loOutput, options.log);
    if (!st.ok()) {
        fs::remove_all(tempDir, ec);
        return st;
    }

    st = moveFile(loOutput, target);
    fs::remove_all(tempDir, ec);
    if (!st.ok()) return st;

    if (produced != nullptr) *produced = target;
    report(options, 100, pw::i18n::t("DOCX 生成完成: ") + fsUtf8(target));
    return Status::success();
}

Status Converter::pdfToDocx(const fs::path& source, const ConvertOptions& options, fs::path* produced) {
    std::error_code ec;
    if (!existsLong(source)) {
        return Status::error(ErrorCode::IoError, pw::i18n::t("源文件不存在: ") + fsUtf8(source));
    }
    if (extOf(source) != "pdf") {
        return Status::error(ErrorCode::InvalidArgument, pw::i18n::t("pdf -> docx 只接受 PDF 输入"));
    }
    if (!backend_->available()) {
        return Status::error(ErrorCode::BackendMissing,
                             pw::i18n::t("PDF 后端不可用（") + backend_->name() + pw::i18n::t("）: ") + backend_->unavailableReason());
    }

    auto opened = backend_->open(source, options.pdfPassword);
    if (!opened.ok()) return opened.status();
    auto document = opened.value();
    if (!document) return Status::error(ErrorCode::Internal, pw::i18n::t("打开 PDF 失败"));

    const int pageCount = document->info().pageCount;
    if (pageCount <= 0) return Status::error(ErrorCode::IoError, pw::i18n::t("PDF 页数为 0"));

    const fs::path target = resolveOutput(source, options.output, "docx");
    report(options, 0, pw::i18n::t("提取文字: ") + std::to_string(pageCount) + pw::i18n::t(" 页"));

    DocxWriter writer;
    writer.setTitle(fsUtf8(source.stem()));
    writer.addParagraph(fsUtf8(source.stem()), 1);

    for (int i = 0; i < pageCount; ++i) {
        std::string text;
        Status st = document->pageText(i, &text);
        if (!st.ok()) {
            if (options.log) options.log(pw::i18n::t("第 ") + std::to_string(i + 1) + pw::i18n::t(" 页提取失败: ") + st.toString());
        } else {
            for (const auto& raw : splitLines(text)) {
                std::string line = trim(raw);
                if (line.empty()) {
                    writer.addEmptyParagraph();
                } else {
                    writer.addParagraph(line);
                }
            }
        }
        if (i + 1 < pageCount) writer.addPageBreak();
        report(options, static_cast<int>(90.0 * (i + 1) / pageCount),
               pw::i18n::t("已处理第 ") + std::to_string(i + 1) + "/" + std::to_string(pageCount) + pw::i18n::t(" 页"));
    }

    Status st = writer.save(target);
    if (!st.ok()) return st;

    if (produced != nullptr) *produced = target;
    report(options, 100, pw::i18n::t("DOCX 生成完成（文字已重排，版式有损）: ") + fsUtf8(target));
    return Status::success();
}

Status Converter::convert(const fs::path& source, const ConvertOptions& options,
                          const std::string& target, fs::path* produced) {
    std::string t;
    for (char c : target) {
        t.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    }
    if (t == "pdf") return toPdf(source, options, produced);
    if (t == "docx" || t == "doc") return toDocx(source, options, produced);
    return Status::error(ErrorCode::Unsupported, pw::i18n::t("不支持的目标格式: ") + target);
}

std::string Converter::capabilityReport() const {
    std::ostringstream oss;
    oss << pw::i18n::t("PDF 后端: ") << backend_->name() << (backend_->available() ? pw::i18n::t("（可用）") : pw::i18n::t("（不可用）")) << "\n";
    if (!backend_->available()) oss << pw::i18n::t("  原因: ") << backend_->unavailableReason() << "\n";
    oss << pw::i18n::t("OCR 引擎: ") << ocrCapabilityReport() << "\n";
    oss << describeLibreOffice(config_) << "\n";
    if (office_.available()) {
        // 启动时不跑 soffice --version：那会拉起一个 LibreOffice 进程（首次还会弹它自己的向导），
        // 白白拖慢开窗。版本等第一次真正转换时再取。
        const std::string cached = office_.cachedVersion();
        oss << pw::i18n::t("LibreOffice 版本: ")
            << (cached.empty() ? std::string(pw::i18n::t("未检测（第一次转换时自动获取）")) : cached) << "\n";
    }
    oss << pw::i18n::t("工作目录: ") << fsUtf8(config_.workDir) << "\n";
    return oss.str();
}

}  // namespace pw
