// 骨架冒烟测试：验证不依赖 LibreOffice / MuPDF 的部分确实能工作。
// - DOCX 生成（自研 zip + OOXML）是不是合法容器
// - 配置解析与能力报告是否正常
// 有依赖的部分（docx->pdf / pdf 预览）在 docs/DEPENDENCIES.md 里给出手动验证步骤。

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

#include "pdfword/converter.hpp"
#include "pdfword/docx_writer.hpp"

namespace fs = std::filesystem;
using namespace pw;

namespace {

int g_failures = 0;

void check(bool cond, const std::string& what) {
    if (cond) {
        std::cout << "  [ok]   " << what << "\n";
    } else {
        std::cout << "  [FAIL] " << what << "\n";
        ++g_failures;
    }
}

}  // namespace

int main(int argc, char** argv) {
    const bool keep = argc > 1 && std::string(argv[1]) == "--keep";
    fs::path out = fs::temp_directory_path() / "pdfword_smoke.docx";

    std::cout << "DOCX 生成\n";
    DocxWriter writer;
    writer.setTitle("PdfWord 冒烟测试");
    writer.addParagraph("第一章 测试", 1);
    writer.addParagraph("正文包含 & <特殊> \"字符\" 与 '引号'");
    writer.addPageBreak();
    writer.addParagraph("第二页正文");

    Status st = writer.save(out);
    check(st.ok(), "保存 docx: " + (st.ok() ? out.string() : st.toString()));
    check(writer.paragraphCount() == 4, "段落计数 = 4");

    std::string bytes;
    {
        std::ifstream in(out, std::ios::binary);
        bytes.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }
    check(bytes.size() > 4 && bytes.compare(0, 4, "PK\x03\x04") == 0, "zip 魔数 PK\\x03\\x04");
    check(bytes.find("[Content_Types].xml") != std::string::npos, "包含 [Content_Types].xml");
    check(bytes.find("word/document.xml") != std::string::npos, "包含 word/document.xml");
    check(bytes.find("&amp;") != std::string::npos, "XML 转义 & -> &amp;");
    check(bytes.find("&lt;特殊") != std::string::npos, "XML 转义 < -> &lt;");
    check(bytes.find("<w:br w:type=\"page\"/>") != std::string::npos, "分页符已写入");

    std::cout << "配置与能力报告\n";
    Config config = loadConfig(fs::path("C:/fake/pdfword.exe"));
    check(!config.workDir.empty(), "workDir 有默认值: " + config.workDir.string());
    check(!config.executableDir.empty(), "executableDir 非空");

    Converter converter(config);
    std::string report = converter.capabilityReport();
    check(!report.empty(), "capabilityReport 非空");
    check(report.find("PDF 后端") != std::string::npos, "报告包含 PDF 后端状态");

    std::cout << "\n--- capabilityReport ---\n" << report;

    // 未配置 LibreOffice 时，docx->pdf 应给出 BackendMissing 而不是崩溃
    ConvertOptions options;
    Status convertStatus = converter.toPdf(fs::path("C:/nonexistent/a.docx"), options, nullptr);
    check(!convertStatus.ok(), "缺失输入文件时返回失败而不是崩溃");
    check(convertStatus.code() == ErrorCode::IoError, "错误码为 IoError");

    if (keep) {
        std::cout << "\n保留产物用于外部校验: " << out.string() << "\n";
    } else {
        fs::remove(out);
    }
    std::cout << (g_failures == 0 ? "\n全部通过\n" : "\n有失败项\n");
    return g_failures == 0 ? 0 : 1;
}
