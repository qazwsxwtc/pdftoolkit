// Pdf-Toolkit CLI —— 证明 core 与界面解耦：同一套 core，命令行也能完整跑通。
// 用法见 printUsage()。

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include "pdfword/converter.hpp"
#include "pdfword/docx_writer.hpp"
#include "pdfword/job.hpp"
#include "pdfword/page_selection.hpp"
#include "pdfword/path_util.hpp"

namespace fs = std::filesystem;
using namespace pw;

namespace {

// 自身可执行路径：用于定位同目录 pdfword.ini（core 不硬编码任何路径）
fs::path gSelf;

// 加密 PDF 的密码，来自 --password
std::string gPassword;

void printUsage() {
    std::cout <<
        "Pdf-Toolkit CLI（可执行文件仍名 pdfword_cli.exe）\n"
        "  pdfword doctor\n"
        "  pdfword convert <src> [dst] --to pdf|docx\n"
        "  pdfword info <file.pdf>\n"
        "  pdfword text <file.pdf> <page(1-based)>\n"
        "  pdfword render <file.pdf> <page> [--zoom 1.5] [--out out.bmp]\n"
        "  pdfword merge <out.pdf> <in1.pdf> <in2.pdf> ...\n"
        "  pdfword split <file.pdf> <out-dir> <pages-per-file>\n"
        "  pdfword extract <in.pdf> <out.pdf> <pages e.g. 1-3,5,8-10>\n"
        "  pdfword pages <in.pdf> <out.pdf> <pages>        按给定顺序重排（可重复页）\n"
        "  pdfword delete <in.pdf> <out.pdf> <pages>       删除这些页\n"
        "  pdfword rotate <in.pdf> <out.pdf> <pages> <deg> 旋转 90/180/270（可负）\n"
        "  pdfword crop <in.pdf> <out.pdf> <pages> <l> <b> <r> <t>   裁边，单位 pt\n"
        "  pdfword insert-blank <in.pdf> <out.pdf> <after-page> [count]\n"
        "  pdfword insert-pdf <in.pdf> <out.pdf> <after-page> <other.pdf> <pages>\n"
        "  pdfword img2pdf <out.pdf> <img1> [img2 ...] [--a4]\n"
        "  pdfword meta <file.pdf>\n"
        "  pdfword setmeta <in.pdf> <out.pdf> [--title T] [--author A] [--subject S] [--keywords K]\n"
        "  pdfword encrypt <in.pdf> <out.pdf> <user-pwd> [owner-pwd] [--allow print,copy,...]\n"
        "  pdfword decrypt <in.pdf> <out.pdf>             （加密件需 --password）\n"
        "  pdfword compress <in.pdf> <out.pdf>           字体子集 + 流压缩 + 垃圾回收\n"
        "  pdfword watermark <in.pdf> <out.pdf> <text> [--opacity 0..1] [--position top-left|center|tile-diagonal|...]\n"
        "  pdfword imgwatermark <in.pdf> <out.pdf> <image> [--scale 0.05..1] [--opacity O] [--tile]\n"
        "  pdfword ocr <in.pdf> <out.pdf|-> <out.txt|-> [--lang zh-Hans-CN]\n"
        "     识别扫描件，写不可见文字层（能 Ctrl+F / 选中复制）并导出纯文本；“-”表示跳过该输出\n"
        "\n"
        "  编辑与批注类：\n"
        "  pdfword annotate <in.pdf> <out.pdf> <kind> <page> <x0,y0,x1,y1> [--text T] [--color #RRGGBB]\n"
        "                   [--author A] [--opacity 0..1] [--ink \"x,y;x,y|...\"]\n"
        "      kind = highlight|underline|strikeout|squiggly|note|freetext|ink（坐标 pt，页面左上原点）\n"
        "  pdfword link <in.pdf> <out.pdf> <page> <x0,y0,x1,y1> <uri>\n"
        "      uri = https://… 或页内跳转 #page=3（也认 “第3页”）\n"
        "  pdfword form-fields <in.pdf>                    列出可填写字段\n"
        "  pdfword form-fill <in.pdf> <out.pdf> name=value ...\n"
        "  pdfword form-create <in.pdf> <out.pdf> <name:text|checkbox|choice:page:x0,y0,x1,y1[:opt1|opt2]> ...\n"
        "  pdfword find <in.pdf> <needle> [--page N]\n"
        "  pdfword replace <in.pdf> <out.pdf> <find> <replace> [--delete-only] [--size N] [--color C] [--limit N] [--case]\n"
        "  pdfword replace-image <in.pdf> <out.pdf> <page> <x0,y0,x1,y1> <image>\n"
  "  pdfword replace-at <in.pdf> <out.pdf> <page> <x0,y0,x1,y1> <text> [--delete-only] [--color #RRGGBB] [--font-size N]\n"
  "                                      按框改字：只动这一块，不整篇搜同一个词\n"
  "  pdfword add-text   <in.pdf> <out.pdf> <page> <x0,y0,x1,y1> <text> [--color #RRGGBB] [--font-size N]\n"
  "                                      往框里加字（不铺白底、不碰原文）\n"
        "  pdfword outline <in.pdf> [--txt out.txt]      打印 / 导出书签目录\n"
        "  pdfword set-outline <in.pdf> <out.pdf> <level:page:title> ... [--from out.txt]\n"
        "      level 从 0 起（0 = 顶级），page 从 1 起（0 = 没有跳转）；不给项等于清空目录\n"
        "  加密 PDF 在上述命令后追加 --password <pwd>\n";
}

// 骨架阶段的位图导出：写 24bit BMP，零依赖、Windows 直接双击可看。
Status writeBmp(const RasterImage& image, const fs::path& path) {
    if (image.empty()) return Status::error(ErrorCode::InvalidArgument, "位图为空");

    const int rowBytes = (image.width * 3 + 3) & ~3;
    const std::uint32_t pixelBytes = static_cast<std::uint32_t>(rowBytes) * static_cast<std::uint32_t>(image.height);
    const std::uint32_t fileSize = 54u + pixelBytes;

    std::vector<std::uint8_t> buf;
    buf.reserve(fileSize);

    auto put16 = [&buf](std::uint16_t v) {
        buf.push_back(static_cast<std::uint8_t>(v & 0xFF));
        buf.push_back(static_cast<std::uint8_t>((v >> 8) & 0xFF));
    };
    auto put32 = [&buf](std::uint32_t v) {
        for (int i = 0; i < 4; ++i) buf.push_back(static_cast<std::uint8_t>((v >> (8 * i)) & 0xFF));
    };

    buf.push_back('B');
    buf.push_back('M');
    put32(fileSize);
    put32(0);
    put32(54);
    put32(40);
    put32(static_cast<std::uint32_t>(image.width));
    put32(static_cast<std::uint32_t>(image.height));  // 正数 = 自下而上
    put16(1);
    put16(24);
    put32(0);
    put32(pixelBytes);
    put32(2835);
    put32(2835);
    put32(0);
    put32(0);

    buf.resize(54u + pixelBytes, 0);
    for (int y = 0; y < image.height; ++y) {
        const int srcRow = image.height - 1 - y;  // BMP 行序自下而上
        const std::uint8_t* src = image.rgba.data() + static_cast<std::size_t>(srcRow) * image.width * 4u;
        std::uint8_t* dst = buf.data() + 54u + static_cast<std::size_t>(y) * rowBytes;
        for (int x = 0; x < image.width; ++x) {
            dst[x * 3 + 0] = src[x * 4 + 2];  // B
            dst[x * 3 + 1] = src[x * 4 + 1];  // G
            dst[x * 3 + 2] = src[x * 4 + 0];  // R
        }
    }

    #ifdef _WIN32
        std::ofstream out(fsPathLong(path).wstring().c_str(), std::ios::binary | std::ios::trunc);
#else
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
#endif
    if (!out) return Status::error(ErrorCode::IoError, "无法写入: " + fsUtf8(path));
    out.write(reinterpret_cast<const char*>(buf.data()), static_cast<std::streamsize>(buf.size()));
    return Status::success();
}

int cmdConvert(const std::vector<std::string>& args) {
    if (args.empty()) return printUsage(), 1;
    std::string target = "pdf";
    fs::path src = args[0];
    fs::path dst;
    for (std::size_t i = 1; i < args.size(); ++i) {
        if (args[i] == "--to" && i + 1 < args.size()) {
            target = args[++i];
        } else {
            dst = args[i];
        }
    }

    Config cfg = loadConfig(gSelf);
    Converter converter(cfg);
    ConvertOptions options;
    options.output = dst;
    options.pdfPassword = gPassword;
    options.log = [](const std::string& m) { std::cout << "  " << m << "\n"; };

    fs::path produced;
    Status st = converter.convert(src, options, target, &produced);
    if (!st.ok()) {
        std::cerr << "失败: " << st.toString() << "\n";
        return static_cast<int>(st.code());
    }
    std::cout << "产物: " << fsUtf8(produced) << "\n";
    return 0;
}

int cmdInfo(const fs::path& file) {
    Config cfg = loadConfig(gSelf);
    Converter converter(cfg);
    auto backend = converter.pdfBackend();
    if (!backend->available()) {
        std::cerr << "PDF 后端不可用: " << backend->unavailableReason() << "\n";
        return static_cast<int>(ErrorCode::BackendMissing);
    }
    auto opened = backend->open(file, gPassword);
    if (!opened.ok()) {
        std::cerr << opened.message() << "\n";
        return static_cast<int>(opened.code());
    }
    auto doc = opened.value();
    std::cout << "文件: " << fsUtf8(file) << "\n"
              << "后端: " << doc->info().backendName << "\n"
              << "页数: " << doc->info().pageCount << "\n";
    PageInfo pi;
    if (doc->pageInfo(0, &pi).ok()) {
        std::cout << "首页尺寸: " << pi.widthPt << " x " << pi.heightPt << " pt\n";
    }
    return 0;
}

int cmdText(const fs::path& file, int page1) {
    Config cfg = loadConfig(gSelf);
    Converter converter(cfg);
    auto opened = converter.pdfBackend()->open(file, gPassword);
    if (!opened.ok()) {
        std::cerr << opened.message() << "\n";
        return static_cast<int>(opened.code());
    }
    std::string text;
    Status st = opened.value()->pageText(page1 - 1, &text);
    if (!st.ok()) {
        std::cerr << st.toString() << "\n";
        return static_cast<int>(st.code());
    }
    std::cout << text;
    return 0;
}

int cmdRender(const std::vector<std::string>& args) {
    if (args.size() < 2) return printUsage(), 1;
    fs::path file = args[0];
    int page1 = std::stoi(args[1]);
    float zoom = 1.5f;
    fs::path out;
    for (std::size_t i = 2; i < args.size(); ++i) {
        if (args[i] == "--zoom" && i + 1 < args.size()) {
            zoom = std::stof(args[++i]);
        } else if (args[i] == "--out" && i + 1 < args.size()) {
            out = args[++i];
        }
    }
    if (out.empty()) out = file.parent_path() / (fsUtf8(file.stem()) + "_p" + std::to_string(page1) + ".bmp");

    Config cfg = loadConfig(gSelf);
    Converter converter(cfg);
    auto opened = converter.pdfBackend()->open(file, gPassword);
    if (!opened.ok()) {
        std::cerr << opened.message() << "\n";
        return static_cast<int>(opened.code());
    }
    RasterImage image;
    Status st = opened.value()->renderPage(page1 - 1, zoom, &image);
    if (!st.ok()) {
        std::cerr << st.toString() << "\n";
        return static_cast<int>(st.code());
    }
    st = writeBmp(image, out);
    if (!st.ok()) {
        std::cerr << st.toString() << "\n";
        return static_cast<int>(st.code());
    }
    std::cout << "已输出 " << image.width << "x" << image.height << " -> " << fsUtf8(out) << "\n";
    return 0;
}

}  // namespace

// ---- 页面装配类命令的公共部分 ----

// 打不开 / 读不到页数时给出真正的原因：后端不可用比"读不到页数"更值得提示
int reportUnreadable(Converter& converter, const fs::path& path) {
    auto backend = converter.pdfBackend();
    if (!backend->available()) {
        std::cerr << "PDF 后端不可用（" << backend->name() << "）：" << backend->unavailableReason()
                  << "\n";
        return 1;
    }
    auto doc = backend->open(path, gPassword);
    if (!doc.ok()) {
        std::cerr << doc.status().toString() << "\n";
        return static_cast<int>(doc.code());
    }
    std::cerr << "无法读取页数: " << fsUtf8(path) << "\n";
    return 1;
}

int pageCountOf(Converter& converter, const fs::path& path) {
    auto doc = converter.pdfBackend()->open(path, gPassword);
    return doc.ok() ? doc.value()->info().pageCount : 0;
}

int reportStatus(const Status& st) {
    if (!st.ok()) {
        std::cerr << st.toString() << "\n";
        return static_cast<int>(st.code());
    }
    return 0;
}

// deleteMode=false: 按 spec 顺序重排/选页；true: 删掉 spec 里的页
int cmdPages(const fs::path& in, const fs::path& out, const std::string& spec, bool deleteMode) {
    Config cfg = loadConfig(gSelf);
    Converter converter(cfg);
    const int total = pageCountOf(converter, in);
    if (total <= 0) return reportUnreadable(converter, in);
    auto pages = parsePageSpec(spec, total);
    if (!pages.ok()) {
        std::cerr << pages.status().toString() << "\n";
        return static_cast<int>(pages.code());
    }

    std::vector<PageRef> refs;
    const auto& list = pages.value();
    for (int i = 0; i < total; ++i) {
        const bool inSpec = std::find(list.begin(), list.end(), i) != list.end();
        if (deleteMode) {
            if (inSpec) continue;
            PageRef ref;
            ref.source = in;
            ref.pageIndex = i;
            refs.push_back(ref);
        }
    }
    if (!deleteMode) {
        for (int idx : list) {
            PageRef ref;
            ref.source = in;
            ref.pageIndex = idx;
            refs.push_back(ref);
        }
    }
    if (refs.empty()) {
        std::cerr << "结果没有任何页面\n";
        return 1;
    }
    Status st = converter.pdfBackend()->assemble(refs, out, gPassword);
    if (!st.ok()) return reportStatus(st);
    std::cout << (deleteMode ? "已删除 " : "已重排 ") << refs.size() << " 页 -> " << fsUtf8(out) << "\n";
    return 0;
}

// 旋转 / 裁剪：对 spec 命中的页做变换，其余页原样保留
int cmdRotateCrop(const fs::path& in, const fs::path& out, const std::string& spec, int rotateDeg,
                  bool doCrop, double l, double b, double r, double t) {
    Config cfg = loadConfig(gSelf);
    Converter converter(cfg);
    const int total = pageCountOf(converter, in);
    if (total <= 0) return reportUnreadable(converter, in);
    auto pages = parsePageSpec(spec, total);
    if (!pages.ok()) {
        std::cerr << pages.status().toString() << "\n";
        return static_cast<int>(pages.code());
    }

    std::vector<PageRef> refs;
    for (int i = 0; i < total; ++i) {
        PageRef ref;
        ref.source = in;
        ref.pageIndex = i;
        const bool hit = std::find(pages.value().begin(), pages.value().end(), i) != pages.value().end();
        if (hit) {
            ref.rotateDelta = rotateDeg;
            if (doCrop) {
                ref.hasCrop = true;
                ref.cropLeft = l;
                ref.cropBottom = b;
                ref.cropRight = r;
                ref.cropTop = t;
            }
        }
        refs.push_back(ref);
    }
    Status st = converter.pdfBackend()->assemble(refs, out, gPassword);
    if (!st.ok()) return reportStatus(st);
    std::cout << "已处理 " << pages.value().size() << " 页 -> " << fsUtf8(out) << "\n";
    return 0;
}

int cmdInsert(const fs::path& in, const fs::path& out, int afterPage1, int blankCount,
              const fs::path& other, const std::string& otherSpec) {
    Config cfg = loadConfig(gSelf);
    Converter converter(cfg);
    const int total = pageCountOf(converter, in);
    if (total <= 0) return reportUnreadable(converter, in);
    if (afterPage1 < 0 || afterPage1 > total) {
        std::cerr << "插入位置超出范围（0.." << total << "）\n";
        return 1;
    }

    std::vector<PageRef> refs;
    for (int i = 0; i < afterPage1; ++i) {
        PageRef ref;
        ref.source = in;
        ref.pageIndex = i;
        refs.push_back(ref);
    }

    if (!other.empty()) {
        const int otherTotal = pageCountOf(converter, other);
        if (otherTotal <= 0) {
            std::cerr << "无法读取被插入文件的页数: " << fsUtf8(other) << "\n";
            return 1;
        }
        auto pages = parsePageSpec(otherSpec, otherTotal);
        if (!pages.ok()) {
            std::cerr << pages.status().toString() << "\n";
            return static_cast<int>(pages.code());
        }
        for (int idx : pages.value()) {
            PageRef ref;
            ref.source = other;
            ref.pageIndex = idx;
            refs.push_back(ref);
        }
    } else {
        for (int i = 0; i < blankCount; ++i) {
            PageRef ref;   // source 为空 = 空白页
            refs.push_back(ref);
        }
    }

    for (int i = afterPage1; i < total; ++i) {
        PageRef ref;
        ref.source = in;
        ref.pageIndex = i;
        refs.push_back(ref);
    }

    Status st = converter.pdfBackend()->assemble(refs, out, gPassword);
    if (!st.ok()) return reportStatus(st);
    std::cout << "已插入 -> " << fsUtf8(out) << "（共 " << refs.size() << " 页）\n";
    return 0;
}

int cmdImagesToPdf(const std::vector<std::string>& args) {
    if (args.size() < 2) return printUsage(), 1;
    bool fitA4 = false;
    std::vector<ImagePage> images;
    for (std::size_t i = 1; i < args.size(); ++i) {
        if (args[i] == "--a4") {
            fitA4 = true;
            continue;
        }
        ImagePage page;
        page.path = args[i];
        page.dpi = 96.0;
        images.push_back(page);
    }
    for (auto& img : images) img.fitA4 = fitA4;

    Config cfg = loadConfig(gSelf);
    Converter converter(cfg);
    Status st = converter.pdfBackend()->imagesToPdf(images, args[0]);
    if (!st.ok()) return reportStatus(st);
    std::cout << "已生成 " << args[0] << "（" << images.size() << " 页）\n";
    return 0;
}

int cmdMeta(const fs::path& file) {
    Config cfg = loadConfig(gSelf);
    Converter converter(cfg);
    auto meta = converter.pdfBackend()->metadata(file, gPassword);
    if (!meta.ok()) return reportStatus(meta.status());
    const DocumentMetadata& m = meta.value();
    std::cout << "标题: " << m.title << "\n"
              << "作者: " << m.author << "\n"
              << "主题: " << m.subject << "\n"
              << "关键词: " << m.keywords << "\n"
              << "创建工具: " << m.creator << "\n"
              << "生成工具: " << m.producer << "\n";
    return 0;
}

int cmdSetMeta(const std::vector<std::string>& args) {
    if (args.size() < 2) return printUsage(), 1;
    DocumentMetadata meta;
    for (std::size_t i = 2; i + 1 < args.size(); i += 2) {
        const std::string& key = args[i];
        const std::string& value = args[i + 1];
        if (key == "--title") meta.title = value;
        else if (key == "--author") meta.author = value;
        else if (key == "--subject") meta.subject = value;
        else if (key == "--keywords") meta.keywords = value;
        else if (key == "--creator") meta.creator = value;
        else if (key == "--producer") meta.producer = value;
        else {
            std::cerr << "未知参数: " << key << "\n";
            return 1;
        }
    }
    Config cfg = loadConfig(gSelf);
    Converter converter(cfg);
    Status st = converter.pdfBackend()->setMetadata(args[0], args[1], meta, gPassword);
    if (!st.ok()) return reportStatus(st);
    std::cout << "已写入元数据 -> " << args[1] << "\n";
    return 0;
}

int parsePermissions(const std::string& allow, int fallback) {
    if (allow.empty()) return fallback;
    if (allow == "all") return PermAll;
    if (allow == "none") return PermNone;
    int perms = PermNone;
    std::size_t pos = 0;
    while (pos <= allow.size()) {
        std::size_t comma = allow.find(',', pos);
        std::string token = allow.substr(pos, comma == std::string::npos ? std::string::npos : comma - pos);
        if (token == "print") perms |= PermPrint;
        else if (token == "copy") perms |= PermCopy;
        else if (token == "modify") perms |= PermModify;
        else if (token == "annotate") perms |= PermAnnotate;
        else if (token == "form") perms |= PermForm;
        else if (token == "assemble") perms |= PermAssemble;
        else if (token == "hq-print") perms |= PermPrintHq;
        if (comma == std::string::npos) break;
        pos = comma + 1;
    }
    return perms;
}

int cmdEncrypt(const std::vector<std::string>& args) {
    if (args.size() < 3) return printUsage(), 1;
    std::string allow;
    std::size_t end = args.size();
    for (std::size_t i = 3; i + 1 < args.size(); ++i) {
        if (args[i] == "--allow") {
            allow = args[i + 1];
            end = i;
            break;
        }
    }
    const std::string owner = (end > 3) ? args[3] : std::string();

    Config cfg = loadConfig(gSelf);
    Converter converter(cfg);
    Status st = converter.pdfBackend()->encrypt(args[0], args[1], args[2], owner,
                                                parsePermissions(allow, PermAll));
    if (!st.ok()) return reportStatus(st);
    std::cout << "已加密（AES-128）-> " << args[1] << "\n";
    return 0;
}

int cmdDecryptOrCompress(const std::string& cmd, const std::vector<std::string>& args) {
    if (args.size() < 2) return printUsage(), 1;
    Config cfg = loadConfig(gSelf);
    Converter converter(cfg);
    Status st = (cmd == "decrypt") ? converter.pdfBackend()->decrypt(args[0], args[1], gPassword)
                                   : converter.pdfBackend()->compress(args[0], args[1], gPassword);
    if (!st.ok()) return reportStatus(st);
    std::cout << "已输出 -> " << args[1] << "\n";
    return 0;
}

// OCR：识别扫描件 → 写不可见文字层（可检索 PDF）+ 导出纯文本。
// 两个输出路径都用 "-" 表示"这一类不要"。
int cmdOcr(const std::vector<std::string>& args) {
    std::string lang = "zh-Hans-CN";
    std::vector<std::string> pos;
    for (std::size_t i = 0; i < args.size(); ++i) {
        if (args[i] == "--lang" && i + 1 < args.size()) {
            lang = args[++i];
        } else {
            pos.push_back(args[i]);
        }
    }
    if (pos.size() < 3) return printUsage(), 1;

    const bool wantPdf = pos[1] != "-";
    const bool wantTxt = pos[2] != "-";
    Config cfg = loadConfig(gSelf);
    Converter converter(cfg);
    Status st = converter.pdfBackend()->ocr(
        pos[0], wantPdf ? fsPath(pos[1]) : fs::path(), wantTxt ? fsPath(pos[2]) : fs::path(),
        lang, wantPdf, wantTxt, gPassword);
    if (!st.ok()) return reportStatus(st);
    if (wantPdf) std::cout << "可检索 PDF -> " << pos[1] << "\n";
    if (wantTxt) std::cout << "纯文本 -> " << pos[2] << "\n";
    return 0;
}

// ---------------------------------------------------------------------------
// 编辑与批注类命令
// ---------------------------------------------------------------------------

// "x0,y0,x1,y1"（也认空格/分号分隔），单位 pt，页面左上原点、y 向下
RectD parseRectStr(const std::string& s) {
    RectD r;
    std::vector<double> vals;
    std::string cur;
    std::string body = s + ",";
    for (char c : body) {
        if (c == ',' || c == ';' || c == ' ' || c == '\t') {
            if (!cur.empty()) {
                vals.push_back(std::stod(cur));
                cur.clear();
            }
        } else {
            cur.push_back(c);
        }
    }
    if (vals.size() == 4) {
        r.x0 = vals[0];
        r.y0 = vals[1];
        r.x1 = vals[2];
        r.y1 = vals[3];
    }
    return r;
}

AnnotKind parseAnnotKind(const std::string& s, bool* ok) {
    if (ok) *ok = true;
    if (s == "highlight" || s == "高亮") return AnnotKind::Highlight;
    if (s == "underline" || s == "下划线") return AnnotKind::Underline;
    if (s == "strikeout" || s == "删除线") return AnnotKind::StrikeOut;
    if (s == "squiggly" || s == "波浪线") return AnnotKind::Squiggly;
    if (s == "note" || s == "便签") return AnnotKind::Note;
    if (s == "freetext" || s == "text" || s == "文本框") return AnnotKind::FreeText;
    if (s == "ink" || s == "手绘") return AnnotKind::Ink;
    if (ok) *ok = false;
    return AnnotKind::Highlight;
}

FieldKind parseFieldKind(const std::string& s) {
    if (s == "text" || s == "文本") return FieldKind::Text;
    if (s == "checkbox" || s == "勾选" || s == "check") return FieldKind::Checkbox;
    if (s == "choice" || s == "下拉") return FieldKind::Choice;
    if (s == "signature" || s == "签名") return FieldKind::Signature;
    return FieldKind::Text;
}

const char* fieldKindName(FieldKind k) {
    switch (k) {
        case FieldKind::Text: return "文本";
        case FieldKind::Checkbox: return "勾选";
        case FieldKind::Choice: return "下拉";
        case FieldKind::Signature: return "签名";
        default: return "未知";
    }
}

// annotate <in.pdf> <out.pdf> <kind> <page> <rect> [--text T] [--color #RRGGBB]
//          [--author A] [--opacity 0..1] [--ink "x,y;x,y;..."]（可多条折线，用 | 分隔）
int cmdAnnotate(const std::vector<std::string>& args) {
    std::string text;
    std::string color;
    std::string author;
    std::string inkSpec;
    double opacity = 1.0;
    std::vector<std::string> pos;
    for (std::size_t i = 0; i < args.size(); ++i) {
        if (args[i] == "--text" && i + 1 < args.size()) {
            text = args[++i];
        } else if (args[i] == "--color" && i + 1 < args.size()) {
            color = args[++i];
        } else if (args[i] == "--author" && i + 1 < args.size()) {
            author = args[++i];
        } else if (args[i] == "--opacity" && i + 1 < args.size()) {
            opacity = std::stod(args[++i]);
        } else if (args[i] == "--ink" && i + 1 < args.size()) {
            inkSpec = args[++i];
        } else {
            pos.push_back(args[i]);
        }
    }
    if (pos.size() < 5) return printUsage(), 1;

    bool kindOk = false;
    AnnotSpec spec;
    spec.kind = parseAnnotKind(pos[2], &kindOk);
    if (!kindOk) {
        std::cerr << "未知批注类型: " << pos[2]
                  << "（可用 highlight/underline/strikeout/squiggly/note/freetext/ink）\n";
        return 1;
    }
    spec.page = std::stoi(pos[3]) - 1;  // 命令行用 1-based
    if (spec.page < 0) {
        std::cerr << "页号从 1 开始\n";
        return 1;
    }
    spec.rect = parseRectStr(pos[4]);
    spec.text = text;
    spec.colorHex = color;
    spec.author = author;
    spec.opacity = opacity;

    if (!inkSpec.empty()) {
        // "x1,y1;x2,y2;...|x1,y1;..." → 多条折线
        std::vector<std::vector<PointD>> paths;
        std::string chunk;
        auto flush = [&]() {
            if (chunk.empty()) return;
            std::vector<PointD> path;
            std::string point;
            std::string body = chunk + ";";
            double x = 0.0;
            bool haveX = false;
            for (char c : body) {
                if (c == ';') {
                    if (!point.empty()) {
                        const double v = std::stod(point);
                        if (!haveX) {
                            x = v;
                            haveX = true;
                        } else {
                            PointD p;
                            p.x = x;
                            p.y = v;
                            path.push_back(p);
                            haveX = false;
                        }
                        point.clear();
                    }
                } else if (c == ',' || c == ' ') {
                    if (!point.empty()) {
                        const double v = std::stod(point);
                        if (!haveX) {
                            x = v;
                            haveX = true;
                        } else {
                            PointD p;
                            p.x = x;
                            p.y = v;
                            path.push_back(p);
                            haveX = false;
                        }
                        point.clear();
                    }
                } else {
                    point.push_back(c);
                }
            }
            if (!path.empty()) paths.push_back(path);
            chunk.clear();
        };
        for (char c : inkSpec) {
            if (c == '|') {
                flush();
            } else {
                chunk.push_back(c);
            }
        }
        flush();
        spec.inkPaths = paths;
    }

    Config cfg = loadConfig(gSelf);
    Converter converter(cfg);
    Status st = converter.pdfBackend()->annotate(pos[0], pos[1], {spec}, gPassword);
    if (!st.ok()) return reportStatus(st);
    std::cout << "已添加批注 -> " << pos[1] << "\n";
    return 0;
}

// link <in.pdf> <out.pdf> <page> <rect> <uri>
//   uri 可以是 https://… 或页内跳转 "#page=3" / "第3页"
int cmdLink(const std::vector<std::string>& args) {
    if (args.size() < 5) return printUsage(), 1;
    LinkSpec link;
    link.page = std::stoi(args[2]) - 1;
    if (link.page < 0) {
        std::cerr << "页号从 1 开始\n";
        return 1;
    }
    link.rect = parseRectStr(args[3]);
    link.uri = args[4];
    if (link.rect.empty()) {
        std::cerr << "链接区域为空，格式应为 x0,y0,x1,y1\n";
        return 1;
    }
    Config cfg = loadConfig(gSelf);
    Converter converter(cfg);
    Status st = converter.pdfBackend()->addLink(args[0], args[1], link, gPassword);
    if (!st.ok()) return reportStatus(st);
    std::cout << "已添加超链接 -> " << args[1] << "\n";
    return 0;
}

// form-fields <in.pdf>       列出所有字段
int cmdFormFields(const std::vector<std::string>& args) {
    if (args.size() < 1) return printUsage(), 1;
    Config cfg = loadConfig(gSelf);
    Converter converter(cfg);
    Result<std::vector<FormField>> res = converter.pdfBackend()->formFields(args[0], gPassword);
    if (!res.ok()) return reportStatus(res.status());
    const auto& fields = res.value();
    if (fields.empty()) {
        std::cout << "文档里没有可填写的表单字段\n";
        return 0;
    }
    std::cout << "共 " << fields.size() << " 个字段\n";
    for (const FormField& f : fields) {
        std::cout << "  [" << f.page + 1 << "页] " << f.name << "  类型="
                  << fieldKindName(f.kind);
        if (f.readOnly) std::cout << " (只读)";
        std::cout << "  值=\"" << f.value << "\"";
        if (!f.options.empty()) {
            std::cout << "  选项=";
            for (std::size_t i = 0; i < f.options.size(); ++i) {
                std::cout << (i > 0 ? "|" : "") << f.options[i];
            }
        }
        std::cout << "\n";
    }
    return 0;
}

// form-fill <in.pdf> <out.pdf> name=value [name=value ...]
int cmdFormFill(const std::vector<std::string>& args) {
    if (args.size() < 3) return printUsage(), 1;
    std::vector<FieldValue> values;
    for (std::size_t i = 2; i < args.size(); ++i) {
        const std::size_t eq = args[i].find('=');
        if (eq == std::string::npos) {
            std::cerr << "字段赋值要写成 name=value: " << args[i] << "\n";
            return 1;
        }
        FieldValue v;
        v.name = args[i].substr(0, eq);
        v.value = args[i].substr(eq + 1);
        values.push_back(v);
    }
    Config cfg = loadConfig(gSelf);
    Converter converter(cfg);
    Status st = converter.pdfBackend()->fillForm(args[0], args[1], values, gPassword);
    if (!st.ok()) return reportStatus(st);
    std::cout << "已填写 " << values.size() << " 个字段 -> " << args[1] << "\n";
    return 0;
}

// form-create <in.pdf> <out.pdf> <name:type:page:rect[:opt1|opt2]> ...
//   type = text | checkbox | choice
int cmdFormCreate(const std::vector<std::string>& args) {
    if (args.size() < 3) return printUsage(), 1;
    std::vector<FieldSpec> fields;
    for (std::size_t i = 2; i < args.size(); ++i) {
        std::vector<std::string> parts;
        std::string cur;
        for (char c : args[i]) {
            if (c == ':') {
                parts.push_back(cur);
                cur.clear();
            } else {
                cur.push_back(c);
            }
        }
        parts.push_back(cur);
        if (parts.size() < 4) {
            std::cerr << "字段定义至少要 name:type:page:rect，收到: " << args[i] << "\n";
            return 1;
        }
        FieldSpec spec;
        spec.name = parts[0];
        spec.kind = parseFieldKind(parts[1]);
        spec.page = std::stoi(parts[2]) - 1;
        if (spec.page < 0) {
            std::cerr << "页号从 1 开始\n";
            return 1;
        }
        spec.rect = parseRectStr(parts[3]);
        if (spec.rect.empty()) {
            std::cerr << "字段区域为空，格式应为 x0,y0,x1,y1: " << args[i] << "\n";
            return 1;
        }
        if (parts.size() >= 5 && !parts[4].empty()) {
            std::string opt;
            for (char c : parts[4]) {
                if (c == '|') {
                    if (!opt.empty()) spec.options.push_back(opt);
                    opt.clear();
                } else {
                    opt.push_back(c);
                }
            }
            if (!opt.empty()) spec.options.push_back(opt);
        }
        fields.push_back(spec);
    }
    Config cfg = loadConfig(gSelf);
    Converter converter(cfg);
    Status st = converter.pdfBackend()->createForm(args[0], args[1], fields, gPassword);
    if (!st.ok()) return reportStatus(st);
    std::cout << "已创建 " << fields.size() << " 个表单字段 -> " << args[1] << "\n";
    return 0;
}

// find <in.pdf> <needle> [--page N]
int cmdFind(const std::vector<std::string>& args) {
    if (args.size() < 2) return printUsage(), 1;
    int page = -1;
    std::vector<std::string> pos;
    for (std::size_t i = 0; i < args.size(); ++i) {
        if (args[i] == "--page" && i + 1 < args.size()) {
            page = std::stoi(args[++i]) - 1;
        } else {
            pos.push_back(args[i]);
        }
    }
    if (pos.size() < 2) return printUsage(), 1;
    Config cfg = loadConfig(gSelf);
    Converter converter(cfg);
    Result<std::vector<TextHit>> res =
        converter.pdfBackend()->findText(pos[0], pos[1], page, gPassword);
    if (!res.ok()) return reportStatus(res.status());
    const auto& hits = res.value();
    std::cout << "命中 " << hits.size() << " 处\n";
    for (const TextHit& h : hits) {
        std::cout << "  第 " << h.page + 1 << " 页  [" << h.rect.x0 << "," << h.rect.y0 << " - "
                  << h.rect.x1 << "," << h.rect.y1 << "]  \"" << h.text << "\"\n";
    }
    return 0;
}

// replace <in.pdf> <out.pdf> <find> <replace> [--delete-only] [--size N] [--color #RRGGBB] [--limit N]
int cmdReplace(const std::vector<std::string>& args) {
    ReplaceOptions opts;
    std::vector<std::string> pos;
    for (std::size_t i = 0; i < args.size(); ++i) {
        if (args[i] == "--delete-only") {
            opts.deleteOnly = true;
        } else if (args[i] == "--size" && i + 1 < args.size()) {
            opts.fontSize = std::stod(args[++i]);
        } else if (args[i] == "--color" && i + 1 < args.size()) {
            opts.colorHex = args[++i];
        } else if (args[i] == "--limit" && i + 1 < args.size()) {
            opts.limit = std::stoi(args[++i]);
        } else if (args[i] == "--case") {
            opts.caseSensitive = true;
        } else {
            pos.push_back(args[i]);
        }
    }
    if (pos.size() < 3) return printUsage(), 1;
    Config cfg = loadConfig(gSelf);
    Converter converter(cfg);
    Status st = converter.pdfBackend()->replaceText(pos[0], pos[1], pos[2],
                                                    opts.deleteOnly ? std::string() : pos[3], opts,
                                                    gPassword);
    if (!st.ok()) return reportStatus(st);
    std::cout << (opts.deleteOnly ? "已删除命中文字" : "已替换命中文字") << " -> " << pos[1] << "\n";
    return 0;
}

// replace-image <in.pdf> <out.pdf> <page> <rect> <image>
int cmdReplaceImage(const std::vector<std::string>& args) {
    if (args.size() < 5) return printUsage(), 1;
    const int page = std::stoi(args[2]) - 1;
    if (page < 0) {
        std::cerr << "页号从 1 开始\n";
        return 1;
    }
    const RectD rect = parseRectStr(args[3]);
    if (rect.empty()) {
        std::cerr << "替换区域为空，格式应为 x0,y0,x1,y1\n";
        return 1;
    }
    Config cfg = loadConfig(gSelf);
    Converter converter(cfg);
    Status st = converter.pdfBackend()->replaceImage(args[0], args[1], page, rect, args[4], gPassword);
    if (!st.ok()) return reportStatus(st);
    std::cout << "已替换图片 -> " << args[1] << "\n";
    return 0;
}

// replace-at <in.pdf> <out.pdf> <page> <rect> <text> [--delete-only] [--color ..] [--font-size ..]
int cmdReplaceTextAt(const std::vector<std::string>& args) {
    std::vector<std::string> pos;
    ReplaceOptions opts;
    for (std::size_t i = 0; i < args.size(); ++i) {
        if (args[i] == "--delete-only") {
            opts.deleteOnly = true;
        } else if (args[i] == "--color" && i + 1 < args.size()) {
            opts.colorHex = args[++i];
        } else if (args[i] == "--font-size" && i + 1 < args.size()) {
            opts.fontSize = std::stod(args[++i]);
        } else {
            pos.push_back(args[i]);
        }
    }
    if (pos.size() < 5) return printUsage(), 1;
    const int page = std::stoi(pos[2]) - 1;
    if (page < 0) {
        std::cerr << "页号从 1 开始\n";
        return 1;
    }
    const RectD rect = parseRectStr(pos[3]);
    if (rect.empty()) {
        std::cerr << "区域为空，格式应为 x0,y0,x1,y1\n";
        return 1;
    }
    Config cfg = loadConfig(gSelf);
    Converter converter(cfg);
    Status st = converter.pdfBackend()->replaceTextAt(pos[0], pos[1], page, rect, pos[4], opts,
                                                      gPassword);
    if (!st.ok()) return reportStatus(st);
    std::cout << (opts.deleteOnly ? "已删掉这块文字" : "已改好这块文字") << " -> " << pos[1] << "\n";
    return 0;
}

// add-text <in.pdf> <out.pdf> <page> <rect> <text> [--color ..] [--font-size ..]
int cmdAddText(const std::vector<std::string>& args) {
    std::vector<std::string> pos;
    ReplaceOptions opts;
    for (std::size_t i = 0; i < args.size(); ++i) {
        if (args[i] == "--color" && i + 1 < args.size()) {
            opts.colorHex = args[++i];
        } else if (args[i] == "--font-size" && i + 1 < args.size()) {
            opts.fontSize = std::stod(args[++i]);
        } else {
            pos.push_back(args[i]);
        }
    }
    if (pos.size() < 5) return printUsage(), 1;
    const int page = std::stoi(pos[2]) - 1;
    if (page < 0) {
        std::cerr << "页号从 1 开始\n";
        return 1;
    }
    const RectD rect = parseRectStr(pos[3]);
    if (rect.empty()) {
        std::cerr << "区域为空，格式应为 x0,y0,x1,y1\n";
        return 1;
    }
    Config cfg = loadConfig(gSelf);
    Converter converter(cfg);
    Status st = converter.pdfBackend()->addTextAt(pos[0], pos[1], page, rect, pos[4], opts,
                                                  gPassword);
    if (!st.ok()) return reportStatus(st);
    std::cout << "已在框里加了文字 -> " << pos[1] << "\n";
    return 0;
}

// outline <in.pdf> [--txt out.txt]
//   打印目录树（缩进表示层级）；给了 --txt 就连同层级、页码一起导出成文本
int cmdOutline(const std::vector<std::string>& args) {
    if (args.empty()) return printUsage(), 1;
    std::string txtOut;
    std::string in;
    for (std::size_t i = 0; i < args.size(); ++i) {
        if (args[i] == "--txt" && i + 1 < args.size()) {
            txtOut = args[++i];
        } else if (in.empty()) {
            in = args[i];
        }
    }
    Config cfg = loadConfig(gSelf);
    Converter converter(cfg);
    Result<std::vector<OutlineItem>> res = converter.pdfBackend()->outline(in, gPassword);
    if (!res.ok()) return reportStatus(res.status());
    const auto& items = res.value();
    if (items.empty()) {
        std::cout << "文档没有书签 / 目录\n";
        return 0;
    }
    std::cout << "共 " << items.size() << " 条书签 / 目录\n";
    std::string text;
    for (const OutlineItem& item : items) {
        const std::string indent(static_cast<std::size_t>(item.level) * 2, ' ');
        std::cout << indent << "- " << item.title;
        if (item.page > 0) std::cout << "  (第 " << item.page << " 页)";
        std::cout << "\n";
        // 导出格式：层级<TAB>页码<TAB>标题（page 0 表示没有跳转目标）
        text += std::to_string(item.level) + "\t" + std::to_string(item.page) + "\t" + item.title +
                "\n";
    }
    if (!txtOut.empty()) {
        #ifdef _WIN32
    std::ofstream out(fsPathLong(fsPath(txtOut)).wstring().c_str(), std::ios::binary);
#else
    std::ofstream out(txtOut.c_str(), std::ios::binary);
#endif
        if (!out) {
            std::cerr << "写不了: " << txtOut << "\n";
            return 1;
        }
        out << text;
        out.close();
        std::cout << "已导出书签 -> " << txtOut << "\n";
    }
    return 0;
}

// set-outline <in.pdf> <out.pdf> "level:page:title" ...
//   level 从 0 开始（0 = 顶级），page 从 1 开始（0 = 没有跳转目标）；不给任何项等于清空目录
//   --from <txt> 可以代替后面的一串参数，直接吃 outline --txt 导出的文件
int cmdSetOutline(const std::vector<std::string>& args) {
    if (args.size() < 2) return printUsage(), 1;
    std::vector<OutlineItem> items;
    std::string fromFile;
    std::vector<std::string> specs;
    for (std::size_t i = 2; i < args.size(); ++i) {
        if (args[i] == "--from" && i + 1 < args.size()) {
            fromFile = args[++i];
        } else {
            specs.push_back(args[i]);
        }
    }
    if (!fromFile.empty()) {
        #ifdef _WIN32
    std::ifstream in(fsPathLong(fsPath(fromFile)).wstring().c_str(), std::ios::binary);
#else
    std::ifstream in(fromFile.c_str(), std::ios::binary);
#endif
        if (!in) {
            std::cerr << "读不了: " << fromFile << "\n";
            return 1;
        }
        std::string line;
        while (std::getline(in, line)) {
            while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
            if (line.empty()) continue;
            const std::size_t t1 = line.find('\t');
            const std::size_t t2 = t1 == std::string::npos ? std::string::npos : line.find('\t', t1 + 1);
            if (t1 == std::string::npos || t2 == std::string::npos) continue;
            OutlineItem item;
            item.level = std::stoi(line.substr(0, t1));
            item.page = std::stoi(line.substr(t1 + 1, t2 - t1 - 1));
            item.title = line.substr(t2 + 1);
            items.push_back(item);
        }
        in.close();
    }
    for (const std::string& spec : specs) {
        const std::size_t c1 = spec.find(':');
        const std::size_t c2 = c1 == std::string::npos ? std::string::npos : spec.find(':', c1 + 1);
        if (c1 == std::string::npos || c2 == std::string::npos) {
            std::cerr << "书签定义要写成 level:page:title，收到: " << spec << "\n";
            return 1;
        }
        OutlineItem item;
        item.level = std::stoi(spec.substr(0, c1));
        item.page = std::stoi(spec.substr(c1 + 1, c2 - c1 - 1));
        item.title = spec.substr(c2 + 1);
        items.push_back(item);
    }
    Config cfg = loadConfig(gSelf);
    Converter converter(cfg);
    Status st = converter.pdfBackend()->setOutline(args[0], args[1], items, gPassword);
    if (!st.ok()) return reportStatus(st);
    std::cout << "已写入 " << items.size() << " 条书签 -> " << args[1] << "\n";
    return 0;
}

// 前置声明：main 里调用，实现放在 main 之后
int dispatch(const std::string& cmd, std::vector<std::string>& args);

// 参数统一成 UTF-8 之后再进 core。
// Windows 上绝不能走 main(char**) —— 那条路拿到的是本地代码页（中文系统 CP936），
// 文件名里只要有该代码页表示不了的字符（日文假名、西里尔字母、emoji…）就会变成 '?'，
// 而且不可逆。wmain 拿到的是 UTF-16，转 UTF-8 无损，任何语言的路径都能开。
static int runMain(const std::vector<std::string>& argv) {
    if (argv.empty()) {
        printUsage();
        return 1;
    }
    gSelf = fs::absolute(fsPath(argv[0]));

    if (argv.size() < 2) {
        printUsage();
        return 1;
    }

    std::string cmd = argv[1];
    std::vector<std::string> args;
    for (std::size_t i = 2; i < argv.size(); ++i) args.emplace_back(argv[i]);

    // --password 对所有命令通用：先摘出来，剩余参数按原位置语义分派
    for (std::size_t i = 0; i + 1 < args.size();) {
        if (args[i] == "--password") {
            gPassword = args[i + 1];
            args.erase(args.begin() + static_cast<std::ptrdiff_t>(i),
                       args.begin() + static_cast<std::ptrdiff_t>(i + 2));
        } else {
            ++i;
        }
    }

    try {
        return dispatch(cmd, args);
    } catch (const std::exception& e) {
        // 数值参数写错（例如把 --opacity 当成数字）不能让进程直接 abort
        std::cerr << "参数错误: " << e.what() << "（数值参数请用纯数字，例如 opacity 写 0.15）\n";
        return 2;
    }
}

#ifdef _WIN32
int wmain(int argc, wchar_t** wargv) {
    enableUtf8Console();  // cmd 默认是本地代码页，中文/日文输出会乱码
    std::vector<std::string> argv;
    argv.reserve(static_cast<std::size_t>(argc));
    for (int i = 0; i < argc; ++i) argv.push_back(wideToUtf8(std::wstring(wargv[i])));
    return runMain(argv);
}
#else
int main(int argc, char** argv) {
    std::vector<std::string> a;
    a.reserve(static_cast<std::size_t>(argc));
    for (int i = 0; i < argc; ++i) a.push_back(argv[i]);
    return runMain(a);
}
#endif

// 具体命令分派：抽出来是为了让 main 只负责参数清洗与异常兜底
int dispatch(const std::string& cmd, std::vector<std::string>& args) {
    if (cmd == "doctor") {
        Config cfg = loadConfig(gSelf);
        Converter converter(cfg);
        std::cout << converter.capabilityReport();
        return 0;
    }
    if (cmd == "convert") return cmdConvert(args);

    // 编辑与批注类
    if (cmd == "annotate") return cmdAnnotate(args);
    if (cmd == "link") return cmdLink(args);
    if (cmd == "form-fields") return cmdFormFields(args);
    if (cmd == "form-fill") return cmdFormFill(args);
    if (cmd == "form-create") return cmdFormCreate(args);
    if (cmd == "find") return cmdFind(args);
    if (cmd == "replace") return cmdReplace(args);
    if (cmd == "replace-image") return cmdReplaceImage(args);
    if (cmd == "replace-at") return cmdReplaceTextAt(args);
    if (cmd == "add-text") return cmdAddText(args);
    if (cmd == "outline") return cmdOutline(args);
    if (cmd == "set-outline") return cmdSetOutline(args);
    if (cmd == "info" && !args.empty()) return cmdInfo(args[0]);
    if (cmd == "text" && args.size() >= 2) return cmdText(args[0], std::stoi(args[1]));
    if (cmd == "render") return cmdRender(args);

    if (cmd == "merge" && args.size() >= 2) {
        fs::path out = args[0];
        std::vector<fs::path> inputs;
        for (std::size_t i = 1; i < args.size(); ++i) inputs.emplace_back(args[i]);
        Config cfg = loadConfig(gSelf);
        Converter converter(cfg);
        Status st = converter.pdfBackend()->merge(inputs, out);
        if (!st.ok()) {
            std::cerr << st.toString() << "\n";
            return static_cast<int>(st.code());
        }
        std::cout << "已合并 -> " << fsUtf8(out) << "\n";
        return 0;
    }
    if (cmd == "split" && args.size() >= 3) {
        Config cfg = loadConfig(gSelf);
        Converter converter(cfg);
        Status st = converter.pdfBackend()->split(args[0], args[1], std::stoi(args[2]));
        if (!st.ok()) {
            std::cerr << st.toString() << "\n";
            return static_cast<int>(st.code());
        }
        std::cout << "已拆分到 " << args[1] << "\n";
        return 0;
    }
    if (cmd == "extract" && args.size() >= 3) {
        Config cfg = loadConfig(gSelf);
        Converter converter(cfg);
        // 先读页数，才能把 "1-3,5" 校验到真实范围里
        auto doc = converter.pdfBackend()->open(args[0], gPassword);
        if (!doc.ok()) {
            std::cerr << doc.status().toString() << "\n";
            return static_cast<int>(doc.code());
        }
        auto pages = parsePageSpec(args[2], doc.value()->info().pageCount);
        if (!pages.ok()) {
            std::cerr << pages.status().toString() << "\n";
            return static_cast<int>(pages.code());
        }
        Status st = converter.pdfBackend()->extract(args[0], args[1], pages.value(), gPassword);
        if (!st.ok()) {
            std::cerr << st.toString() << "\n";
            return static_cast<int>(st.code());
        }
        std::cout << "已抽取 " << pages.value().size() << " 页（" << formatPageSpec(pages.value())
                  << "） -> " << args[1] << "\n";
        return 0;
    }
    if (cmd == "pages" && args.size() >= 3) return cmdPages(args[0], args[1], args[2], false);
    if (cmd == "delete" && args.size() >= 3) return cmdPages(args[0], args[1], args[2], true);
    if (cmd == "rotate" && args.size() >= 4) {
        return cmdRotateCrop(args[0], args[1], args[2], std::stoi(args[3]), false, 0, 0, 0, 0);
    }
    if (cmd == "crop" && args.size() >= 7) {
        return cmdRotateCrop(args[0], args[1], args[2], 0, true, std::stod(args[3]), std::stod(args[4]),
                             std::stod(args[5]), std::stod(args[6]));
    }
    if (cmd == "insert-blank" && args.size() >= 3) {
        const int after = std::stoi(args[2]);
        const int count = args.size() >= 4 ? std::stoi(args[3]) : 1;
        return cmdInsert(args[0], args[1], after, count, fs::path(), std::string());
    }
    if (cmd == "insert-pdf" && args.size() >= 5) {
        return cmdInsert(args[0], args[1], std::stoi(args[2]), 0, args[3], args[4]);
    }
    if (cmd == "img2pdf") return cmdImagesToPdf(args);
    if (cmd == "meta" && !args.empty()) return cmdMeta(args[0]);
    if (cmd == "setmeta") return cmdSetMeta(args);
    if (cmd == "encrypt") return cmdEncrypt(args);
    if (cmd == "decrypt") return cmdDecryptOrCompress("decrypt", args);
    if (cmd == "compress") return cmdDecryptOrCompress("compress", args);

    if (cmd == "watermark" && args.size() >= 3) {
        float opacity = 0.3f;
        std::string position = "top-left";
        for (std::size_t i = 3; i < args.size(); ++i) {
            if (args[i] == "--opacity" && i + 1 < args.size()) {
                opacity = std::stof(args[++i]);
            } else if (args[i] == "--position" && i + 1 < args.size()) {
                position = args[++i];
            } else if (args[i].rfind("--", 0) != 0) {
                opacity = std::stof(args[i]);   // 兼容旧写法：watermark in out text 0.3
            }
        }
        Config cfg = loadConfig(gSelf);
        Converter converter(cfg);
        Status st = converter.pdfBackend()->watermark(args[0], args[1], args[2], opacity, position);
        if (!st.ok()) {
            std::cerr << st.toString() << "\n";
            return static_cast<int>(st.code());
        }
        std::cout << "已加水印（" << position << "）-> " << args[1] << "\n";
        return 0;
    }

    // 图片水印：pdfword imgwatermark <in.pdf> <out.pdf> <image.png> [--scale 0.3] [--opacity 0.3] [--tile]
    // （--password 由 main 统一摘到 gPassword）
    if (cmd == "imgwatermark" && args.size() >= 3) {
        float scale = 0.3f;
        float opacity = 0.3f;
        bool tile = false;
        for (std::size_t i = 3; i < args.size(); ++i) {
            if (args[i] == "--opacity" && i + 1 < args.size()) {
                opacity = std::stof(args[++i]);
            } else if (args[i] == "--scale" && i + 1 < args.size()) {
                scale = std::stof(args[++i]);
            } else if (args[i] == "--tile") {
                tile = true;
            } else {
                scale = std::stof(args[i]);   // 兼容位置参数写法：imgwatermark in out img 0.4
            }
        }
        Config cfg = loadConfig(gSelf);
        Converter converter(cfg);
        Status st = converter.pdfBackend()->imageWatermark(fsPath(args[0]), fsPath(args[1]),
                                                           fsPath(args[2]), opacity, scale, tile,
                                                           gPassword);
        if (!st.ok()) {
            std::cerr << st.toString() << "\n";
            return static_cast<int>(st.code());
        }
        std::cout << "已加图片水印" << (tile ? "（平铺）" : "") << " -> " << args[1] << "\n";
        return 0;
    }

    if (cmd == "ocr" && args.size() >= 3) return cmdOcr(args);

    printUsage();
    return 1;
}

