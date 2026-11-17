#pragma once

#include <memory>
#include <string>

#include "pdfword/config.hpp"
#include "pdfword/office_runner.hpp"
#include "pdfword/pdf_backend.hpp"
#include "pdfword/result.hpp"

namespace pw {

struct ConvertOptions {
    // 目标文件路径，或目录（目录时按源文件名推导）
    fs::path output;
    // 加密 PDF 的密码；只在 pdf -> docx 等需要读取 PDF 的方向使用
    std::string pdfPassword;
    LogFn log;
    ProgressFn progress;
};

// 转换门面。UI / CLI / C ABI 都只跟它打交道，不直接碰 LibreOffice 或 MuPDF。
class Converter {
public:
    explicit Converter(Config config);

    const Config& config() const { return config_; }

    // docx/doc/odt/rtf/xlsx/pptx -> pdf（LibreOffice headless）
    Status toPdf(const fs::path& source, const ConvertOptions& options, fs::path* produced = nullptr);

    // doc/odt/rtf -> docx（LibreOffice headless）
    Status toDocx(const fs::path& source, const ConvertOptions& options, fs::path* produced = nullptr);

    // pdf -> docx（MuPDF 提取文字 + DocxWriter 重排；版式有损）
    Status pdfToDocx(const fs::path& source, const ConvertOptions& options, fs::path* produced = nullptr);

    // 按目标扩展名自动分派："pdf" / "docx"
    Status convert(const fs::path& source, const ConvertOptions& options, const std::string& target,
                   fs::path* produced = nullptr);

    std::shared_ptr<IPdfBackend> pdfBackend() const { return backend_; }
    OfficeRunner& office() { return office_; }

    // 一行一条，给 CLI doctor / UI 状态栏用
    std::string capabilityReport() const;

private:
    Config config_;
    OfficeRunner office_;
    std::shared_ptr<IPdfBackend> backend_;
};

}  // namespace pw
