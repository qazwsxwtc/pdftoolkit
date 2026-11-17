// PdfWord CLI —— 证明 core 与界面解耦：同一套 core，命令行也能完整跑通。
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

namespace fs = std::filesystem;
using namespace pw;

namespace {

// 自身可执行路径：用于定位同目录 pdfword.ini（core 不硬编码任何路径）
fs::path gSelf;

// 加密 PDF 的密码，来自 --password
std::string gPassword;

void printUsage() {
    std::cout <<
        "PdfWord CLI\n"
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
        "  pdfword watermark <in.pdf> <out.pdf> <text> [opacity 0..1]\n"
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

    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) return Status::error(ErrorCode::IoError, "无法写入: " + path.string());
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
    std::cout << "产物: " << produced.string() << "\n";
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
    std::cout << "文件: " << file.string() << "\n"
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
    if (out.empty()) out = file.parent_path() / (file.stem().string() + "_p" + std::to_string(page1) + ".bmp");

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
    std::cout << "已输出 " << image.width << "x" << image.height << " -> " << out.string() << "\n";
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
    std::cerr << "无法读取页数: " << path.string() << "\n";
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
    std::cout << (deleteMode ? "已删除 " : "已重排 ") << refs.size() << " 页 -> " << out.string() << "\n";
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
    std::cout << "已处理 " << pages.value().size() << " 页 -> " << out.string() << "\n";
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
            std::cerr << "无法读取被插入文件的页数: " << other.string() << "\n";
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
    std::cout << "已插入 -> " << out.string() << "（共 " << refs.size() << " 页）\n";
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

// 前置声明：main 里调用，实现放在 main 之后
int dispatch(const std::string& cmd, std::vector<std::string>& args);

int main(int argc, char** argv) {
    gSelf = fs::absolute(fs::path(argv[0]));

    if (argc < 2) {
        printUsage();
        return 1;
    }

    std::string cmd = argv[1];
    std::vector<std::string> args;
    for (int i = 2; i < argc; ++i) args.emplace_back(argv[i]);

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

// 具体命令分派：抽出来是为了让 main 只负责参数清洗与异常兜底
int dispatch(const std::string& cmd, std::vector<std::string>& args) {
    if (cmd == "doctor") {
        Config cfg = loadConfig(gSelf);
        Converter converter(cfg);
        std::cout << converter.capabilityReport();
        return 0;
    }
    if (cmd == "convert") return cmdConvert(args);
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
        std::cout << "已合并 -> " << out.string() << "\n";
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
        float opacity = args.size() >= 4 ? std::stof(args[3]) : 0.3f;
        Config cfg = loadConfig(gSelf);
        Converter converter(cfg);
        Status st = converter.pdfBackend()->watermark(args[0], args[1], args[2], opacity);
        if (!st.ok()) {
            std::cerr << st.toString() << "\n";
            return static_cast<int>(st.code());
        }
        std::cout << "已加水印 -> " << args[1] << "\n";
        return 0;
    }

    printUsage();
    return 1;
}

