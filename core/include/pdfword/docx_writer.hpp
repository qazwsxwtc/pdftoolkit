#pragma once

#include <filesystem>
#include <memory>
#include <string>

#include "pdfword/result.hpp"

namespace pw {

// 极简 OOXML 生成器：只负责「段落 + 分页 + 标题级别」，不追求排版保真。
// 用途是 PDF -> DOCX 的文本重组（版式必然有损，重排交给 Word）。
class DocxWriter {
public:
    DocxWriter();
    ~DocxWriter();
    DocxWriter(const DocxWriter&) = delete;
    DocxWriter& operator=(const DocxWriter&) = delete;

    void setTitle(const std::string& title);
    // headingLevel: 0 = 正文，1..3 = Heading1..3
    void addParagraph(const std::string& text, int headingLevel = 0);
    void addEmptyParagraph();
    void addPageBreak();

    std::size_t paragraphCount() const;

    Status save(const std::filesystem::path& path);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace pw
