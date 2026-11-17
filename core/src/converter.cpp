#include "pdfword/converter.hpp"

#include <atomic>
#include <chrono>
#include <sstream>
#include <string>
#include <vector>

#include "pdfword/docx_writer.hpp"
#include "pdfword/ocr.hpp"

namespace pw {

namespace {

std::string extOf(const fs::path& p) {
    std::string e = p.extension().string();
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
        return source.parent_path() / (source.stem().string() + "." + targetExt);
    }
    if (fs::is_directory(requested, ec)) {
        return requested / (source.stem().string() + "." + targetExt);
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
                             "无法写入目标文件: " + to.string() + " (" + ec.message() + ")");
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
    if (!fs::exists(source, ec)) {
        return Status::error(ErrorCode::IoError, "源文件不存在: " + source.string());
    }
    if (extOf(source) == "pdf") {
        return Status::error(ErrorCode::InvalidArgument, "源文件已经是 PDF: " + source.string());
    }
    if (!office_.available()) {
        return Status::error(ErrorCode::BackendMissing,
                             "LibreOffice 未配置，无法把 " + extOf(source) +
                                 " 转成 PDF（在 pdfword.ini 里填 libre_office=便携目录）");
    }

    const fs::path target = resolveOutput(source, options.output, "pdf");
    const fs::path tempDir = makeTempOutDir(config_);

    report(options, 5, "启动 LibreOffice headless: " + source.filename().string());

    fs::path loOutput;
    LogFn innerLog = options.log;
    Status st = office_.convertOne(source, tempDir, OfficeRunner::filterForPdf(), &loOutput, innerLog);
    if (!st.ok()) {
        std::error_code rmEc;
        fs::remove_all(tempDir, rmEc);
        return st;
    }

    report(options, 90, "移动产物 -> " + target.string());
    st = moveFile(loOutput, target);
    fs::remove_all(tempDir, ec);
    if (!st.ok()) return st;

    if (produced != nullptr) *produced = target;
    report(options, 100, "PDF 生成完成: " + target.string());
    return Status::success();
}

Status Converter::toDocx(const fs::path& source, const ConvertOptions& options, fs::path* produced) {
    std::error_code ec;
    if (!fs::exists(source, ec)) {
        return Status::error(ErrorCode::IoError, "源文件不存在: " + source.string());
    }
    std::string ext = extOf(source);
    if (ext == "pdf") {
        return pdfToDocx(source, options, produced);
    }
    if (ext == "docx") {
        return Status::error(ErrorCode::InvalidArgument, "源文件已经是 DOCX: " + source.string());
    }
    if (!office_.available()) {
        return Status::error(ErrorCode::BackendMissing,
                             "LibreOffice 未配置，无法把 " + ext + " 转成 DOCX");
    }

    const fs::path target = resolveOutput(source, options.output, "docx");
    const fs::path tempDir = makeTempOutDir(config_);

    report(options, 5, "启动 LibreOffice headless: " + source.filename().string());
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
    report(options, 100, "DOCX 生成完成: " + target.string());
    return Status::success();
}

Status Converter::pdfToDocx(const fs::path& source, const ConvertOptions& options, fs::path* produced) {
    std::error_code ec;
    if (!fs::exists(source, ec)) {
        return Status::error(ErrorCode::IoError, "源文件不存在: " + source.string());
    }
    if (extOf(source) != "pdf") {
        return Status::error(ErrorCode::InvalidArgument, "pdf -> docx 只接受 PDF 输入");
    }
    if (!backend_->available()) {
        return Status::error(ErrorCode::BackendMissing,
                             "PDF 后端不可用（" + backend_->name() + "）: " + backend_->unavailableReason());
    }

    auto opened = backend_->open(source, options.pdfPassword);
    if (!opened.ok()) return opened.status();
    auto document = opened.value();
    if (!document) return Status::error(ErrorCode::Internal, "打开 PDF 失败");

    const int pageCount = document->info().pageCount;
    if (pageCount <= 0) return Status::error(ErrorCode::IoError, "PDF 页数为 0");

    const fs::path target = resolveOutput(source, options.output, "docx");
    report(options, 0, "提取文字: " + std::to_string(pageCount) + " 页");

    DocxWriter writer;
    writer.setTitle(source.stem().string());
    writer.addParagraph(source.stem().string(), 1);

    for (int i = 0; i < pageCount; ++i) {
        std::string text;
        Status st = document->pageText(i, &text);
        if (!st.ok()) {
            if (options.log) options.log("第 " + std::to_string(i + 1) + " 页提取失败: " + st.toString());
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
               "已处理第 " + std::to_string(i + 1) + "/" + std::to_string(pageCount) + " 页");
    }

    Status st = writer.save(target);
    if (!st.ok()) return st;

    if (produced != nullptr) *produced = target;
    report(options, 100, "DOCX 生成完成（文字已重排，版式有损）: " + target.string());
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
    return Status::error(ErrorCode::Unsupported, "不支持的目标格式: " + target);
}

std::string Converter::capabilityReport() const {
    std::ostringstream oss;
    oss << "PDF 后端: " << backend_->name() << (backend_->available() ? "（可用）" : "（不可用）") << "\n";
    if (!backend_->available()) oss << "  原因: " << backend_->unavailableReason() << "\n";
    oss << "OCR 引擎: " << ocrCapabilityReport() << "\n";
    oss << describeLibreOffice(config_) << "\n";
    if (office_.available()) {
        // 启动时不跑 soffice --version：那会拉起一个 LibreOffice 进程（首次还会弹它自己的向导），
        // 白白拖慢开窗。版本等第一次真正转换时再取。
        const std::string cached = office_.cachedVersion();
        oss << "LibreOffice 版本: "
            << (cached.empty() ? std::string("未检测（第一次转换时自动获取）") : cached) << "\n";
    }
    oss << "工作目录: " << config_.workDir.string() << "\n";
    return oss.str();
}

}  // namespace pw
