#include "pdfword/docx_writer.hpp"

#include <map>
#include <string>
#include <vector>

#include "zip_writer.hpp"

namespace pw {

namespace {

std::string xmlEscape(const std::string& in) {
    std::string out;
    out.reserve(in.size() + 16);
    for (char c : in) {
        switch (c) {
            case '&': out += "&amp;"; break;
            case '<': out += "&lt;"; break;
            case '>': out += "&gt;"; break;
            case '"': out += "&quot;"; break;
            case '\'': out += "&apos;"; break;
            default: out.push_back(c); break;
        }
    }
    return out;
}

const char* kDocHeader =
    "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>"
    "<w:document xmlns:w=\"http://schemas.openxmlformats.org/wordprocessingml/2006/main\">"
    "<w:body>";

const char* kSectPr =
    "<w:sectPr>"
    "<w:pgSz w:w=\"11906\" w:h=\"16838\"/>"
    "<w:pgMar w:top=\"1440\" w:right=\"1800\" w:bottom=\"1440\" w:left=\"1800\" "
    "w:header=\"851\" w:footer=\"992\" w:gutter=\"0\"/>"
    "</w:sectPr>"
    "</w:body></w:document>";

const char* kStylesXml =
    "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>"
    "<w:styles xmlns:w=\"http://schemas.openxmlformats.org/wordprocessingml/2006/main\">"
    "<w:docDefaults><w:rPrDefault><w:rPr>"
    "<w:rFonts w:ascii=\"Calibri\" w:hAnsi=\"Calibri\" w:eastAsia=\"宋体\"/>"
    "<w:sz w:val=\"22\"/></w:rPr></w:rPrDefault>"
    "<w:pPrDefault><w:pPr><w:spacing w:after=\"120\"/></w:pPr></w:pPrDefault></w:docDefaults>"
    "<w:style w:type=\"paragraph\" w:default=\"1\" w:styleId=\"Normal\">"
    "<w:name w:val=\"Normal\"/></w:style>"
    "<w:style w:type=\"paragraph\" w:styleId=\"Heading1\">"
    "<w:name w:val=\"heading 1\"/><w:basedOn w:val=\"Normal\"/>"
    "<w:rPr><w:b/><w:sz w:val=\"36\"/></w:rPr></w:style>"
    "<w:style w:type=\"paragraph\" w:styleId=\"Heading2\">"
    "<w:name w:val=\"heading 2\"/><w:basedOn w:val=\"Normal\"/>"
    "<w:rPr><w:b/><w:sz w:val=\"30\"/></w:rPr></w:style>"
    "<w:style w:type=\"paragraph\" w:styleId=\"Heading3\">"
    "<w:name w:val=\"heading 3\"/><w:basedOn w:val=\"Normal\"/>"
    "<w:rPr><w:b/><w:sz w:val=\"26\"/></w:rPr></w:style>"
    "</w:styles>";

const char* kContentTypes =
    "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>"
    "<Types xmlns=\"http://schemas.openxmlformats.org/package/2006/content-types\">"
    "<Default Extension=\"rels\" ContentType=\"application/vnd.openxmlformats-package.relationships+xml\"/>"
    "<Default Extension=\"xml\" ContentType=\"application/xml\"/>"
    "<Override PartName=\"/word/document.xml\" "
    "ContentType=\"application/vnd.openxmlformats-officedocument.wordprocessingml.document.main+xml\"/>"
    "<Override PartName=\"/word/styles.xml\" "
    "ContentType=\"application/vnd.openxmlformats-officedocument.wordprocessingml.styles+xml\"/>"
    "<Override PartName=\"/docProps/core.xml\" "
    "ContentType=\"application/vnd.openxmlformats-package.core-properties+xml\"/>"
    "<Override PartName=\"/docProps/app.xml\" "
    "ContentType=\"application/vnd.openxmlformats-officedocument.extended-properties+xml\"/>"
    "</Types>";

const char* kRootRels =
    "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>"
    "<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">"
    "<Relationship Id=\"rId1\" "
    "Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/officeDocument\" "
    "Target=\"word/document.xml\"/>"
    "<Relationship Id=\"rId2\" "
    "Type=\"http://schemas.openxmlformats.org/package/2006/relationships/metadata/core-properties\" "
    "Target=\"docProps/core.xml\"/>"
    "<Relationship Id=\"rId3\" "
    "Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/extended-properties\" "
    "Target=\"docProps/app.xml\"/>"
    "</Relationships>";

const char* kDocumentRels =
    "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>"
    "<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">"
    "<Relationship Id=\"rId1\" "
    "Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/styles\" "
    "Target=\"styles.xml\"/>"
    "</Relationships>";

}  // namespace

struct DocxWriter::Impl {
    std::string title;
    std::vector<std::string> bodyXml;
    std::size_t paragraphs = 0;
};

DocxWriter::DocxWriter() : impl_(new Impl()) {}
DocxWriter::~DocxWriter() = default;

void DocxWriter::setTitle(const std::string& title) {
    impl_->title = title;
}

void DocxWriter::addParagraph(const std::string& text, int headingLevel) {
    std::string pPr;
    if (headingLevel >= 1 && headingLevel <= 3) {
        pPr = "<w:pPr><w:pStyle w:val=\"Heading" + std::to_string(headingLevel) + "\"/></w:pPr>";
    }
    if (text.empty()) {
        impl_->bodyXml.push_back("<w:p>" + pPr + "</w:p>");
    } else {
        impl_->bodyXml.push_back("<w:p>" + pPr + "<w:r><w:t xml:space=\"preserve\">" +
                                 xmlEscape(text) + "</w:t></w:r></w:p>");
    }
    ++impl_->paragraphs;
}

void DocxWriter::addEmptyParagraph() {
    impl_->bodyXml.push_back("<w:p/>");
    ++impl_->paragraphs;
}

void DocxWriter::addPageBreak() {
    impl_->bodyXml.push_back("<w:p><w:r><w:br w:type=\"page\"/></w:r></w:p>");
    ++impl_->paragraphs;
}

std::size_t DocxWriter::paragraphCount() const {
    return impl_->paragraphs;
}

Status DocxWriter::save(const fs::path& path) {
    std::string document = kDocHeader;
    for (const auto& p : impl_->bodyXml) {
        document += p;
    }
    document += kSectPr;

    std::string core =
        "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>"
        "<cp:coreProperties xmlns:cp=\"http://schemas.openxmlformats.org/package/2006/metadata/core-properties\" "
        "xmlns:dc=\"http://purl.org/dc/elements/1.1/\">"
        "<dc:title>" + xmlEscape(impl_->title) + "</dc:title>"
        "<dc:creator>PdfWord</dc:creator>"
        "</cp:coreProperties>";

    std::string app =
        "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>"
        "<Properties xmlns=\"http://schemas.openxmlformats.org/officeDocument/2006/extended-properties\">"
        "<Application>PdfWord</Application>"
        "</Properties>";

    detail::ZipWriter zip;
    zip.add("[Content_Types].xml", kContentTypes);
    zip.add("_rels/.rels", kRootRels);
    zip.add("word/document.xml", document);
    zip.add("word/styles.xml", kStylesXml);
    zip.add("word/_rels/document.xml.rels", kDocumentRels);
    zip.add("docProps/core.xml", core);
    zip.add("docProps/app.xml", app);

    return zip.write(path);
}

}  // namespace pw
