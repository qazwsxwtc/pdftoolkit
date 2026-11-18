#include "pdfword_c.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "pdfword/converter.hpp"
#include "pdfword/job.hpp"
#include "pdfword/ocr.hpp"
#include "pdfword/page_selection.hpp"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include "pdfword/path_util.hpp"
#endif

using namespace pw;

// 注意：必须定义在全局作用域，否则会和头文件里的 `typedef struct pw_context pw_context;`
// 在匿名命名空间内形成两个不同的 pw_context 类型。
struct pw_context {
    Config config;
    std::shared_ptr<Converter> converter;
    std::string pdfPassword;  // 加密 PDF 的默认密码，由 pw_set_pdf_password 设置
    mutable std::string lastError;
};

namespace {

// 通过模块自身位置定位 pdfword.ini，避免 C API 调用方还得自己算路径
fs::path modulePath() {
#ifdef _WIN32
    wchar_t buf[MAX_PATH * 2];
    if (::GetModuleFileNameW(nullptr, buf, static_cast<DWORD>(sizeof(buf) / sizeof(wchar_t))) > 0) {
        std::wstring w(buf);
        int n = ::WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), nullptr, 0,
                                      nullptr, nullptr);
        std::string s(static_cast<std::size_t>(n), '\0');
        ::WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), s.data(), n, nullptr,
                              nullptr);
        return fsPath(s);
    }
#endif
    return fs::current_path() / "pdfword";
}

int fail(pw_context* ctx, const Status& status) {
    if (ctx != nullptr) ctx->lastError = status.toString();
    return static_cast<int>(status.code());
}

char* dupString(const std::string& s) {
    char* p = static_cast<char*>(std::malloc(s.size() + 1));
    if (p == nullptr) return nullptr;
    std::memcpy(p, s.c_str(), s.size() + 1);
    return p;
}

unsigned char* dupBytes(const std::vector<std::uint8_t>& v, size_t* size) {
    if (v.empty()) {
        *size = 0;
        return nullptr;
    }
    unsigned char* p = static_cast<unsigned char*>(std::malloc(v.size()));
    if (p == nullptr) return nullptr;
    std::memcpy(p, v.data(), v.size());
    *size = v.size();
    return p;
}

// ---- 编辑与批注类的参数解析 -------------------------------------------------


// 元数据/表单返回给 web 壳时用；只转义必须转义的字符，保持可读
std::string jsonEscape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned char>(c));
                    out += buf;
                } else {
                    out.push_back(c);
                }
        }
    }
    return out;
}

std::string copyDup(const char* s) { return (s != nullptr) ? std::string(s) : std::string(); }

AnnotKind annotKindOf(int kind) {
    switch (kind) {
        case 1: return AnnotKind::Underline;
        case 2: return AnnotKind::StrikeOut;
        case 3: return AnnotKind::Squiggly;
        case 4: return AnnotKind::Note;
        case 5: return AnnotKind::FreeText;
        case 6: return AnnotKind::Ink;
        default: return AnnotKind::Highlight;
    }
}

FieldKind fieldKindOf(const char* name) {
    const std::string s = (name != nullptr) ? name : "";
    if (s == "checkbox" || s == "check") return FieldKind::Checkbox;
    if (s == "choice" || s == "combo" || s == "list") return FieldKind::Choice;
    if (s == "signature" || s == "sign") return FieldKind::Signature;
    return FieldKind::Text;
}

const char* fieldKindText(FieldKind kind) {
    switch (kind) {
        case FieldKind::Text: return "text";
        case FieldKind::Checkbox: return "checkbox";
        case FieldKind::Choice: return "choice";
        case FieldKind::Signature: return "signature";
        default: return "unknown";
    }
}

// "x1,y1;x2,y2;x3,y3|a,b;c,d" —— 分号分隔点，竖线分隔多笔
std::vector<std::vector<PointD>> parseInkPaths(const char* spec) {
    std::vector<std::vector<PointD>> paths;
    if (spec == nullptr || *spec == '\0') return paths;
    std::string s = spec;
    std::vector<std::string> strokes;
    std::size_t start = 0;
    while (true) {
        const std::size_t bar = s.find('|', start);
        strokes.push_back(s.substr(start, bar == std::string::npos ? std::string::npos : bar - start));
        if (bar == std::string::npos) break;
        start = bar + 1;
    }
    for (const std::string& stroke : strokes) {
        std::vector<PointD> pts;
        std::size_t p = 0;
        while (p < stroke.size()) {
            const std::size_t sep = stroke.find(';', p);
            std::string item = stroke.substr(p, sep == std::string::npos ? std::string::npos : sep - p);
            const std::size_t comma = item.find(',');
            if (comma != std::string::npos) {
                PointD pt;
                pt.x = std::atof(item.substr(0, comma).c_str());
                pt.y = std::atof(item.substr(comma + 1).c_str());
                pts.push_back(pt);
            }
            if (sep == std::string::npos) break;
            p = sep + 1;
        }
        if (pts.size() >= 2) paths.push_back(pts);
    }
    return paths;
}

std::string num(double v) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.4g", v);
    return std::string(buf);
}

std::string formFieldsToJson(const std::vector<FormField>& fields) {
    std::string json = "[";
    for (std::size_t i = 0; i < fields.size(); ++i) {
        const FormField& f = fields[i];
        if (i != 0) json += ",";
        json += "{\"name\":\"" + jsonEscape(f.name) + "\",\"kind\":\"" + fieldKindText(f.kind) +
                "\",\"value\":\"" + jsonEscape(f.value) + "\",\"page\":" + std::to_string(f.page) +
                ",\"rect\":[" + num(f.rect.x0) + "," + num(f.rect.y0) + "," + num(f.rect.x1) + "," +
                num(f.rect.y1) + "],\"readOnly\":" + (f.readOnly ? "true" : "false") + ",\"options\":[";
        for (std::size_t k = 0; k < f.options.size(); ++k) {
            if (k != 0) json += ",";
            json += "\"" + jsonEscape(f.options[k]) + "\"";
        }
        json += "]}";
    }
    json += "]";
    return json;
}

std::string outlineToJson(const std::vector<OutlineItem>& items) {
    std::string json = "[";
    for (std::size_t i = 0; i < items.size(); ++i) {
        const OutlineItem& item = items[i];
        if (i != 0) json += ",";
        json += "{\"title\":\"" + jsonEscape(item.title) + "\",\"page\":" +
                std::to_string(item.page) + ",\"level\":" + std::to_string(item.level) + "}";
    }
    json += "]";
    return json;
}

// "level:page:title" —— 只按前两个冒号切，标题里可以再有冒号
bool parseOutlineSpec(const std::string& text, OutlineItem* out) {
    const std::size_t c1 = text.find(':');
    if (c1 == std::string::npos) return false;
    const std::size_t c2 = text.find(':', c1 + 1);
    if (c2 == std::string::npos) return false;
    try {
        out->level = std::stoi(text.substr(0, c1));
        out->page = std::stoi(text.substr(c1 + 1, c2 - c1 - 1));
    } catch (...) {
        return false;
    }
    out->title = text.substr(c2 + 1);
    return !out->title.empty();
}

// "name:type:page:x0,y0,x1,y1[:opt1|opt2]" —— 与 CLI 的 --form-create 同一套写法
bool parseFieldSpec(const std::string& text, FieldSpec* out) {
    std::vector<std::string> parts;
    std::size_t start = 0;
    // 用第一个 ':' 切字段名、第二个切类型、第三个切页号，剩下的整体当矩形+选项，
    // 这样字段名里允许出现冒号。
    int colons = 0;
    std::string name, kind, pageStr, rest;
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] != ':') continue;
        if (colons == 0) {
            name = text.substr(0, i);
        } else if (colons == 1) {
            kind = text.substr(start, i - start);
        } else if (colons == 2) {
            pageStr = text.substr(start, i - start);
        }
        start = i + 1;
        ++colons;
        if (colons == 3) {
            rest = text.substr(start);
            break;
        }
    }
    if (colons < 3 || name.empty() || rest.empty()) return false;

    // rest = "x0,y0,x1,y1" 或 "x0,y0,x1,y1:opt1|opt2"
    std::string rectStr = rest;
    std::string optsStr;
    const std::size_t colon = rest.find(':');
    if (colon != std::string::npos) {
        rectStr = rest.substr(0, colon);
        optsStr = rest.substr(colon + 1);
    }

    double v[4] = {0, 0, 0, 0};
    int got = 0;
    std::size_t p = 0;
    while (p <= rectStr.size() && got < 4) {
        const std::size_t sep = rectStr.find_first_of(", ;", p);
        std::string item =
            rectStr.substr(p, sep == std::string::npos ? std::string::npos : sep - p);
        if (!item.empty()) v[got++] = std::atof(item.c_str());
        if (sep == std::string::npos) break;
        p = sep + 1;
    }
    if (got < 4) return false;

    FieldSpec spec;
    spec.name = name;
    spec.kind = fieldKindOf(kind.c_str());
    spec.page = std::atoi(pageStr.c_str()) - 1;  // 外部一律 1 基
    if (spec.page < 0) spec.page = 0;
    spec.rect.x0 = v[0];
    spec.rect.y0 = v[1];
    spec.rect.x1 = v[2];
    spec.rect.y1 = v[3];
    if (!optsStr.empty()) {
        std::size_t s = 0;
        while (s <= optsStr.size()) {
            const std::size_t bar = optsStr.find('|', s);
            std::string opt = optsStr.substr(s, bar == std::string::npos ? std::string::npos : bar - s);
            if (!opt.empty()) spec.options.push_back(opt);
            if (bar == std::string::npos) break;
            s = bar + 1;
        }
    }
    *out = spec;
    return true;
}

}  // namespace

extern "C" {

pw_context* pw_context_create(const char* config_ini_path) {
    auto* ctx = new pw_context();
    fs::path ini = (config_ini_path != nullptr && *config_ini_path != '\0')
                       ? fsPath(config_ini_path)
                       : fs::path();
    ctx->config = loadConfig(modulePath(), ini);
    ctx->converter = std::make_shared<Converter>(ctx->config);
    return ctx;
}

void pw_context_destroy(pw_context* ctx) {
    delete ctx;
}

const char* pw_last_error(const pw_context* ctx) {
    if (ctx == nullptr) return "";
    return ctx->lastError.c_str();
}

const char* pw_capability_report(pw_context* ctx) {
    if (ctx == nullptr) return nullptr;
    return dupString(ctx->converter->capabilityReport());
}

int pw_backend_available(pw_context* ctx) {
    if (ctx == nullptr) return 0;
    return ctx->converter->pdfBackend()->available() ? 1 : 0;
}

const char* pw_backend_name(pw_context* ctx) {
    if (ctx == nullptr) return nullptr;
    return dupString(ctx->converter->pdfBackend()->name());
}

int pw_set_pdf_password(pw_context* ctx, const char* password) {
    if (ctx == nullptr) return 1;
    ctx->pdfPassword = (password != nullptr) ? std::string(password) : std::string();
    return 0;
}

int pw_convert(pw_context* ctx, const char* source, const char* output, const char* target,
               char** produced) {
    if (ctx == nullptr || source == nullptr || target == nullptr) return 1;

    ConvertOptions options;
    options.output = (output != nullptr && *output != '\0') ? fsPath(output) : fs::path();
    options.pdfPassword = ctx->pdfPassword;

    fs::path result;
    Status st = ctx->converter->convert(fsPath(source), options, target, &result);
    if (!st.ok()) return fail(ctx, st);

    if (produced != nullptr) *produced = dupString(fsUtf8(result));
    return 0;
}

int pw_pdf_page_count(pw_context* ctx, const char* path, int* out_count) {
    if (ctx == nullptr || path == nullptr || out_count == nullptr) return 1;
    auto opened = ctx->converter->pdfBackend()->open(fsPath(path), ctx->pdfPassword);
    if (!opened.ok()) return fail(ctx, opened.status());
    *out_count = opened.value()->info().pageCount;
    return 0;
}

int pw_pdf_page_text(pw_context* ctx, const char* path, int page_index, char** out_text) {
    if (ctx == nullptr || path == nullptr || out_text == nullptr) return 1;
    auto opened = ctx->converter->pdfBackend()->open(fsPath(path), ctx->pdfPassword);
    if (!opened.ok()) return fail(ctx, opened.status());

    std::string text;
    Status st = opened.value()->pageText(page_index, &text);
    if (!st.ok()) return fail(ctx, st);

    *out_text = dupString(text);
    return 0;
}

int pw_pdf_render_page(pw_context* ctx, const char* path, int page_index, float zoom,
                       unsigned char** rgba, int* width, int* height, size_t* byte_size) {
    if (ctx == nullptr || path == nullptr || rgba == nullptr || width == nullptr || height == nullptr ||
        byte_size == nullptr) {
        return 1;
    }
    *rgba = nullptr;
    *width = 0;
    *height = 0;
    *byte_size = 0;

    auto opened = ctx->converter->pdfBackend()->open(fsPath(path), ctx->pdfPassword);
    if (!opened.ok()) return fail(ctx, opened.status());

    RasterImage image;
    Status st = opened.value()->renderPage(page_index, zoom, &image);
    if (!st.ok()) return fail(ctx, st);

    *width = image.width;
    *height = image.height;
    *rgba = dupBytes(image.rgba, byte_size);
    if (*rgba == nullptr && *byte_size != 0) {
        return fail(ctx, Status::error(ErrorCode::Internal, "内存分配失败"));
    }
    return 0;
}

int pw_pdf_merge(pw_context* ctx, const char* const* paths, int count, const char* output) {
    if (ctx == nullptr || paths == nullptr || count <= 0 || output == nullptr) return 1;
    std::vector<fs::path> inputs;
    inputs.reserve(static_cast<std::size_t>(count));
    for (int i = 0; i < count; ++i) {
        if (paths[i] == nullptr) return 1;
        inputs.emplace_back(paths[i]);
    }
    Status st = ctx->converter->pdfBackend()->merge(inputs, fsPath(output));
    if (!st.ok()) return fail(ctx, st);
    return 0;
}

int pw_pdf_split(pw_context* ctx, const char* path, const char* out_dir, int pages_per_file) {
    if (ctx == nullptr || path == nullptr || out_dir == nullptr) return 1;
    Status st = ctx->converter->pdfBackend()->split(fsPath(path), fsPath(out_dir), pages_per_file);
    if (!st.ok()) return fail(ctx, st);
    return 0;
}

int pw_pdf_extract(pw_context* ctx, const char* path, const char* output, const char* page_spec) {
    if (ctx == nullptr || path == nullptr || output == nullptr || page_spec == nullptr) return 1;

    // 先拿页数，才能把 "1-3,5" 校验进真实范围
    auto doc = ctx->converter->pdfBackend()->open(fsPath(path), ctx->pdfPassword);
    if (!doc.ok()) return fail(ctx, doc.status());

    auto pages = parsePageSpec(page_spec, doc.value()->info().pageCount);
    if (!pages.ok()) return fail(ctx, pages.status());

    Status st = ctx->converter->pdfBackend()->extract(fsPath(path), fsPath(output), pages.value(),
                                                      ctx->pdfPassword);
    if (!st.ok()) return fail(ctx, st);
    return 0;
}

int pw_pdf_watermark(pw_context* ctx, const char* path, const char* output, const char* text,
                     float opacity) {
    return pw_pdf_watermark_position(ctx, path, output, text, opacity, "top-left");
}

int pw_pdf_watermark_position(pw_context* ctx, const char* path, const char* output,
                              const char* text, float opacity, const char* position) {
    if (ctx == nullptr || path == nullptr || output == nullptr || text == nullptr) return 1;
    const std::string pos = (position != nullptr && position[0] != '\0') ? position : "top-left";
    Status st = ctx->converter->pdfBackend()->watermark(fsPath(path), fsPath(output), text,
                                                        opacity, pos);
    if (!st.ok()) return fail(ctx, st);
    return 0;
}

int pw_pdf_image_watermark(pw_context* ctx, const char* path, const char* output, const char* image,
                           float opacity, float scale, int tile) {
    if (ctx == nullptr || path == nullptr || output == nullptr || image == nullptr) return 1;
    Status st = ctx->converter->pdfBackend()->imageWatermark(fsPath(path), fsPath(output),
                                                             fsPath(image), opacity, scale,
                                                             tile != 0);
    if (!st.ok()) return fail(ctx, st);
    return 0;
}

int pw_pdf_pages(pw_context* ctx, const char* path, const char* output, const char* page_spec) {
    if (ctx == nullptr || path == nullptr || output == nullptr || page_spec == nullptr) return 1;

    auto doc = ctx->converter->pdfBackend()->open(fsPath(path), ctx->pdfPassword);
    if (!doc.ok()) return fail(ctx, doc.status());

    auto pages = parsePageSpec(page_spec, doc.value()->info().pageCount);
    if (!pages.ok()) return fail(ctx, pages.status());

    std::vector<PageRef> refs;
    for (int idx : pages.value()) {
        PageRef ref;
        ref.source = path;
        ref.pageIndex = idx;
        refs.push_back(ref);
    }
    Status st = ctx->converter->pdfBackend()->assemble(refs, fsPath(output), ctx->pdfPassword);
    return st.ok() ? 0 : fail(ctx, st);
}

int pw_pdf_delete_pages(pw_context* ctx, const char* path, const char* output, const char* page_spec) {
    if (ctx == nullptr || path == nullptr || output == nullptr || page_spec == nullptr) return 1;

    auto doc = ctx->converter->pdfBackend()->open(fsPath(path), ctx->pdfPassword);
    if (!doc.ok()) return fail(ctx, doc.status());

    const int total = doc.value()->info().pageCount;
    auto pages = parsePageSpec(page_spec, total);
    if (!pages.ok()) return fail(ctx, pages.status());

    std::vector<PageRef> refs;
    for (int i = 0; i < total; ++i) {
        if (std::find(pages.value().begin(), pages.value().end(), i) != pages.value().end()) continue;
        PageRef ref;
        ref.source = path;
        ref.pageIndex = i;
        refs.push_back(ref);
    }
    if (refs.empty()) {
        ctx->lastError = "删完之后没有任何页面";
        return 1;
    }
    Status st = ctx->converter->pdfBackend()->assemble(refs, fsPath(output), ctx->pdfPassword);
    return st.ok() ? 0 : fail(ctx, st);
}

namespace {

// 旋转 / 裁剪共用：对命中的页做变换，其余页原样
int transformPages(pw_context* ctx, const char* path, const char* output, const char* page_spec,
                   int rotation, bool crop, double l, double b, double r, double t) {
    auto doc = ctx->converter->pdfBackend()->open(fsPath(path), ctx->pdfPassword);
    if (!doc.ok()) return fail(ctx, doc.status());

    const int total = doc.value()->info().pageCount;
    auto pages = parsePageSpec(page_spec, total);
    if (!pages.ok()) return fail(ctx, pages.status());

    std::vector<PageRef> refs;
    for (int i = 0; i < total; ++i) {
        PageRef ref;
        ref.source = path;
        ref.pageIndex = i;
        if (std::find(pages.value().begin(), pages.value().end(), i) != pages.value().end()) {
            ref.rotateDelta = rotation;
            if (crop) {
                ref.hasCrop = true;
                ref.cropLeft = l;
                ref.cropBottom = b;
                ref.cropRight = r;
                ref.cropTop = t;
            }
        }
        refs.push_back(ref);
    }
    Status st = ctx->converter->pdfBackend()->assemble(refs, fsPath(output), ctx->pdfPassword);
    return st.ok() ? 0 : fail(ctx, st);
}

}  // namespace

int pw_pdf_rotate_pages(pw_context* ctx, const char* path, const char* output, const char* page_spec,
                        int rotation) {
    if (ctx == nullptr || path == nullptr || output == nullptr || page_spec == nullptr) return 1;
    return transformPages(ctx, path, output, page_spec, rotation, false, 0, 0, 0, 0);
}

int pw_pdf_crop_pages(pw_context* ctx, const char* path, const char* output, const char* page_spec,
                      double left, double bottom, double right, double top) {
    if (ctx == nullptr || path == nullptr || output == nullptr || page_spec == nullptr) return 1;
    return transformPages(ctx, path, output, page_spec, 0, true, left, bottom, right, top);
}

int pw_pdf_insert_blank(pw_context* ctx, const char* path, const char* output, int after_page,
                        int count) {
    if (ctx == nullptr || path == nullptr || output == nullptr) return 1;

    auto doc = ctx->converter->pdfBackend()->open(fsPath(path), ctx->pdfPassword);
    if (!doc.ok()) return fail(ctx, doc.status());
    const int total = doc.value()->info().pageCount;
    if (after_page < 0 || after_page > total) {
        ctx->lastError = "插入位置超出范围";
        return 1;
    }

    std::vector<PageRef> refs;
    for (int i = 0; i < after_page; ++i) {
        PageRef ref;
        ref.source = path;
        ref.pageIndex = i;
        refs.push_back(ref);
    }
    for (int i = 0; i < std::max(count, 0); ++i) refs.emplace_back();   // 空白页
    for (int i = after_page; i < total; ++i) {
        PageRef ref;
        ref.source = path;
        ref.pageIndex = i;
        refs.push_back(ref);
    }
    Status st = ctx->converter->pdfBackend()->assemble(refs, fsPath(output), ctx->pdfPassword);
    return st.ok() ? 0 : fail(ctx, st);
}

int pw_pdf_insert_pages(pw_context* ctx, const char* path, const char* output, int after_page,
                        const char* other, const char* page_spec) {
    if (ctx == nullptr || path == nullptr || output == nullptr || other == nullptr ||
        page_spec == nullptr) {
        return 1;
    }

    auto doc = ctx->converter->pdfBackend()->open(fsPath(path), ctx->pdfPassword);
    if (!doc.ok()) return fail(ctx, doc.status());
    const int total = doc.value()->info().pageCount;
    if (after_page < 0 || after_page > total) {
        ctx->lastError = "插入位置超出范围";
        return 1;
    }

    auto otherDoc = ctx->converter->pdfBackend()->open(fsPath(other), ctx->pdfPassword);
    if (!otherDoc.ok()) return fail(ctx, otherDoc.status());
    auto pages = parsePageSpec(page_spec, otherDoc.value()->info().pageCount);
    if (!pages.ok()) return fail(ctx, pages.status());

    std::vector<PageRef> refs;
    for (int i = 0; i < after_page; ++i) {
        PageRef ref;
        ref.source = path;
        ref.pageIndex = i;
        refs.push_back(ref);
    }
    for (int idx : pages.value()) {
        PageRef ref;
        ref.source = other;
        ref.pageIndex = idx;
        refs.push_back(ref);
    }
    for (int i = after_page; i < total; ++i) {
        PageRef ref;
        ref.source = path;
        ref.pageIndex = i;
        refs.push_back(ref);
    }
    Status st = ctx->converter->pdfBackend()->assemble(refs, fsPath(output), ctx->pdfPassword);
    return st.ok() ? 0 : fail(ctx, st);
}

int pw_images_to_pdf(pw_context* ctx, const char* const* images, int count, const char* output,
                     int fit_a4) {
    if (ctx == nullptr || images == nullptr || output == nullptr || count <= 0) return 1;

    std::vector<ImagePage> list;
    for (int i = 0; i < count; ++i) {
        if (images[i] == nullptr) continue;
        ImagePage page;
        page.path = images[i];
        page.fitA4 = (fit_a4 != 0);
        list.push_back(page);
    }
    if (list.empty()) {
        ctx->lastError = "没有有效图片";
        return 1;
    }
    Status st = ctx->converter->pdfBackend()->imagesToPdf(list, fsPath(output));
    return st.ok() ? 0 : fail(ctx, st);
}

int pw_pdf_metadata(pw_context* ctx, const char* path, char** out_json) {
    if (ctx == nullptr || path == nullptr || out_json == nullptr) return 1;

    auto meta = ctx->converter->pdfBackend()->metadata(fsPath(path), ctx->pdfPassword);
    if (!meta.ok()) return fail(ctx, meta.status());

    const DocumentMetadata& m = meta.value();
    std::string json = "{\"title\":\"" + jsonEscape(m.title) + "\",\"author\":\"" +
                       jsonEscape(m.author) + "\",\"subject\":\"" + jsonEscape(m.subject) +
                       "\",\"keywords\":\"" + jsonEscape(m.keywords) + "\",\"creator\":\"" +
                       jsonEscape(m.creator) + "\",\"producer\":\"" + jsonEscape(m.producer) + "\"}";

    char* copy = static_cast<char*>(std::malloc(json.size() + 1));
    if (copy == nullptr) {
        ctx->lastError = "内存不足";
        return 1;
    }
    std::memcpy(copy, json.c_str(), json.size() + 1);
    *out_json = copy;
    return 0;
}

int pw_pdf_set_metadata(pw_context* ctx, const char* path, const char* output, const char* title,
                        const char* author, const char* subject, const char* keywords) {
    if (ctx == nullptr || path == nullptr || output == nullptr) return 1;

    DocumentMetadata meta;
    meta.title = copyDup(title);
    meta.author = copyDup(author);
    meta.subject = copyDup(subject);
    meta.keywords = copyDup(keywords);

    Status st = ctx->converter->pdfBackend()->setMetadata(fsPath(path), fsPath(output), meta,
                                                          ctx->pdfPassword);
    return st.ok() ? 0 : fail(ctx, st);
}

int pw_pdf_encrypt(pw_context* ctx, const char* path, const char* output, const char* user_password,
                   const char* owner_password, int permissions) {
    if (ctx == nullptr || path == nullptr || output == nullptr) return 1;
    Status st = ctx->converter->pdfBackend()->encrypt(fsPath(path), fsPath(output),
                                                      copyDup(user_password), copyDup(owner_password),
                                                      permissions, ctx->pdfPassword);
    return st.ok() ? 0 : fail(ctx, st);
}

int pw_pdf_decrypt(pw_context* ctx, const char* path, const char* output) {
    if (ctx == nullptr || path == nullptr || output == nullptr) return 1;
    Status st = ctx->converter->pdfBackend()->decrypt(fsPath(path), fsPath(output), ctx->pdfPassword);
    return st.ok() ? 0 : fail(ctx, st);
}

int pw_pdf_compress(pw_context* ctx, const char* path, const char* output) {
    if (ctx == nullptr || path == nullptr || output == nullptr) return 1;
    Status st = ctx->converter->pdfBackend()->compress(fsPath(path), fsPath(output), ctx->pdfPassword);
    return st.ok() ? 0 : fail(ctx, st);
}

int pw_pdf_ocr(pw_context* ctx, const char* path, const char* output_pdf, const char* output_txt,
               const char* language) {
    if (ctx == nullptr || path == nullptr) return 1;
    if (output_pdf == nullptr && output_txt == nullptr) {
        ctx->lastError = "至少需要 output_pdf 或 output_txt 其中一项";
        return 1;
    }
    const std::string lang = (language != nullptr && *language != '\0') ? language : std::string();
    Status st = ctx->converter->pdfBackend()->ocr(
        fsPath(path), output_pdf != nullptr ? fsPath(output_pdf) : fs::path(),
        output_txt != nullptr ? fsPath(output_txt) : fs::path(), lang, output_pdf != nullptr,
        output_txt != nullptr, ctx->pdfPassword);
    return st.ok() ? 0 : fail(ctx, st);
}

const char* pw_ocr_capability(pw_context*) {
    // 静态局部变量：生命周期贯穿进程，返回的指针可以直接给调用方读
    static const std::string report = ocrCapabilityReport();
    return report.c_str();
}

// ---------------------------------------------------------------------------
// 编辑与批注类
// ---------------------------------------------------------------------------

int pw_pdf_annotate(pw_context* ctx, const char* path, const char* output, int page_index, int kind,
                    double x0, double y0, double x1, double y1, const char* text, const char* color,
                    const char* author, double opacity, const char* ink) {
    if (ctx == nullptr || path == nullptr || output == nullptr) return 1;

    AnnotSpec spec;
    spec.kind = annotKindOf(kind);
    spec.page = page_index;
    spec.rect.x0 = x0;
    spec.rect.y0 = y0;
    spec.rect.x1 = x1;
    spec.rect.y1 = y1;
    if (text != nullptr) spec.text = text;
    if (color != nullptr) spec.colorHex = color;
    if (author != nullptr) spec.author = author;
    spec.opacity = (opacity > 0.0 && opacity <= 1.0) ? opacity : 1.0;
    spec.inkPaths = parseInkPaths(ink);

    std::vector<AnnotSpec> specs{spec};
    Status st = ctx->converter->pdfBackend()->annotate(fsPath(path), fsPath(output), specs,
                                                       ctx->pdfPassword);
    return st.ok() ? 0 : fail(ctx, st);
}

int pw_pdf_add_link(pw_context* ctx, const char* path, const char* output, int page_index, double x0,
                    double y0, double x1, double y1, const char* uri) {
    if (ctx == nullptr || path == nullptr || output == nullptr || uri == nullptr) return 1;

    LinkSpec spec;
    spec.page = page_index;
    spec.rect.x0 = x0;
    spec.rect.y0 = y0;
    spec.rect.x1 = x1;
    spec.rect.y1 = y1;
    spec.uri = uri;

    Status st = ctx->converter->pdfBackend()->addLink(fsPath(path), fsPath(output), spec,
                                                      ctx->pdfPassword);
    return st.ok() ? 0 : fail(ctx, st);
}

int pw_pdf_find_text(pw_context* ctx, const char* path, const char* needle, int page_index,
                     char** out_text) {
    if (ctx == nullptr || path == nullptr || needle == nullptr || out_text == nullptr) return -1;

    auto hits = ctx->converter->pdfBackend()->findText(fsPath(path), needle, page_index,
                                                       ctx->pdfPassword);
    if (!hits.ok()) {
        fail(ctx, hits.status());
        return -static_cast<int>(hits.status().code());
    }

    std::string text;
    for (const TextHit& h : hits.value()) {
        text += std::to_string(h.page) + "\t" + num(h.rect.x0) + "\t" + num(h.rect.y0) + "\t" +
                num(h.rect.x1) + "\t" + num(h.rect.y1) + "\t" + h.text + "\n";
    }
    *out_text = dupString(text);
    return static_cast<int>(hits.value().size());
}

int pw_pdf_replace_text(pw_context* ctx, const char* path, const char* output, const char* find,
                        const char* replace, int delete_only, double font_size, const char* color,
                        int limit, int case_sensitive) {
    if (ctx == nullptr || path == nullptr || output == nullptr || find == nullptr) return 1;

    ReplaceOptions opts;
    opts.deleteOnly = (delete_only != 0);
    opts.fontSize = (font_size > 0.0) ? font_size : 0.0;
    if (color != nullptr) opts.colorHex = color;
    opts.limit = limit;
    opts.caseSensitive = (case_sensitive != 0);

    Status st = ctx->converter->pdfBackend()->replaceText(
        fsPath(path), fsPath(output), find, (replace != nullptr) ? replace : "", opts,
        ctx->pdfPassword);
    return st.ok() ? 0 : fail(ctx, st);
}

int pw_pdf_replace_image(pw_context* ctx, const char* path, const char* output, int page_index,
                         double x0, double y0, double x1, double y1, const char* image) {
    if (ctx == nullptr || path == nullptr || output == nullptr || image == nullptr) return 1;

    RectD rect;
    rect.x0 = x0;
    rect.y0 = y0;
    rect.x1 = x1;
    rect.y1 = y1;
    Status st = ctx->converter->pdfBackend()->replaceImage(
        fsPath(path), fsPath(output), page_index, rect, fsPath(image), ctx->pdfPassword);
    return st.ok() ? 0 : fail(ctx, st);
}

int pw_pdf_replace_text_at(pw_context* ctx, const char* path, const char* output, int page_index,
                           double x0, double y0, double x1, double y1, const char* text,
                           int delete_only) {
    if (ctx == nullptr || path == nullptr || output == nullptr || text == nullptr) return 1;

    RectD rect;
    rect.x0 = x0;
    rect.y0 = y0;
    rect.x1 = x1;
    rect.y1 = y1;
    ReplaceOptions opts;
    opts.deleteOnly = delete_only != 0;
    Status st = ctx->converter->pdfBackend()->replaceTextAt(
        fsPath(path), fsPath(output), page_index, rect, text, opts, ctx->pdfPassword);
    return st.ok() ? 0 : fail(ctx, st);
}

int pw_pdf_add_text_at(pw_context* ctx, const char* path, const char* output, int page_index,
                       double x0, double y0, double x1, double y1, const char* text,
                       double font_size) {
    if (ctx == nullptr || path == nullptr || output == nullptr || text == nullptr) return 1;

    RectD rect;
    rect.x0 = x0;
    rect.y0 = y0;
    rect.x1 = x1;
    rect.y1 = y1;
    ReplaceOptions opts;
    opts.fontSize = font_size;
    Status st = ctx->converter->pdfBackend()->addTextAt(
        fsPath(path), fsPath(output), page_index, rect, text, opts, ctx->pdfPassword);
    return st.ok() ? 0 : fail(ctx, st);
}

int pw_pdf_form_fields(pw_context* ctx, const char* path, char** out_json) {
    if (ctx == nullptr || path == nullptr || out_json == nullptr) return -1;

    auto fields = ctx->converter->pdfBackend()->formFields(fsPath(path), ctx->pdfPassword);
    if (!fields.ok()) {
        fail(ctx, fields.status());
        return -static_cast<int>(fields.status().code());
    }
    const std::string json = formFieldsToJson(fields.value());
    *out_json = dupString(json);
    return static_cast<int>(fields.value().size());
}

int pw_pdf_form_fill(pw_context* ctx, const char* path, const char* output, const char* const* names,
                     const char* const* values, int count) {
    if (ctx == nullptr || path == nullptr || output == nullptr || names == nullptr ||
        values == nullptr || count <= 0) {
        return 1;
    }

    std::vector<FieldValue> list;
    for (int i = 0; i < count; ++i) {
        if (names[i] == nullptr) continue;
        FieldValue fv;
        fv.name = names[i];
        fv.value = (values[i] != nullptr) ? values[i] : "";
        list.push_back(fv);
    }
    if (list.empty()) {
        ctx->lastError = "没有有效字段";
        return 1;
    }
    Status st = ctx->converter->pdfBackend()->fillForm(fsPath(path), fsPath(output), list,
                                                       ctx->pdfPassword);
    return st.ok() ? 0 : fail(ctx, st);
}

int pw_pdf_form_create(pw_context* ctx, const char* path, const char* output, const char* const* specs,
                       int count) {
    if (ctx == nullptr || path == nullptr || output == nullptr || specs == nullptr || count <= 0) {
        return 1;
    }

    std::vector<FieldSpec> list;
    for (int i = 0; i < count; ++i) {
        if (specs[i] == nullptr) continue;
        FieldSpec spec;
        if (!parseFieldSpec(specs[i], &spec)) {
            ctx->lastError = std::string("字段定义无法解析: ") + specs[i];
            return 1;
        }
        list.push_back(spec);
    }
    if (list.empty()) {
        ctx->lastError = "没有有效字段";
        return 1;
    }
    Status st = ctx->converter->pdfBackend()->createForm(fsPath(path), fsPath(output), list,
                                                         ctx->pdfPassword);
    return st.ok() ? 0 : fail(ctx, st);
}

int pw_pdf_outline(pw_context* ctx, const char* path, char** out_json) {
    if (ctx == nullptr || path == nullptr || out_json == nullptr) return -1;

    auto items = ctx->converter->pdfBackend()->outline(fsPath(path), ctx->pdfPassword);
    if (!items.ok()) {
        fail(ctx, items.status());
        return -static_cast<int>(items.status().code());
    }
    *out_json = dupString(outlineToJson(items.value()));
    return static_cast<int>(items.value().size());
}

int pw_pdf_set_outline(pw_context* ctx, const char* path, const char* output,
                       const char* const* specs, int count) {
    if (ctx == nullptr || path == nullptr || output == nullptr) return 1;
    if (count < 0) count = 0;

    std::vector<OutlineItem> list;
    for (int i = 0; i < count; ++i) {
        if (specs == nullptr || specs[i] == nullptr) continue;
        OutlineItem item;
        if (!parseOutlineSpec(specs[i], &item)) {
            ctx->lastError = std::string("书签定义无法解析: ") + specs[i];
            return 1;
        }
        list.push_back(item);
    }
    // list 为空 = 清空目录，这是合法用法
    Status st = ctx->converter->pdfBackend()->setOutline(fsPath(path), fsPath(output), list,
                                                        ctx->pdfPassword);
    return st.ok() ? 0 : fail(ctx, st);
}

void pw_free(void* p) {
    std::free(p);
}

}  // extern "C"
