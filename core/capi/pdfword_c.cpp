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
#include "pdfword/page_selection.hpp"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
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
        return fs::path(s);
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

}  // namespace

extern "C" {

pw_context* pw_context_create(const char* config_ini_path) {
    auto* ctx = new pw_context();
    fs::path ini = (config_ini_path != nullptr && *config_ini_path != '\0')
                       ? fs::path(config_ini_path)
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
    options.output = (output != nullptr && *output != '\0') ? fs::path(output) : fs::path();
    options.pdfPassword = ctx->pdfPassword;

    fs::path result;
    Status st = ctx->converter->convert(fs::path(source), options, target, &result);
    if (!st.ok()) return fail(ctx, st);

    if (produced != nullptr) *produced = dupString(result.string());
    return 0;
}

int pw_pdf_page_count(pw_context* ctx, const char* path, int* out_count) {
    if (ctx == nullptr || path == nullptr || out_count == nullptr) return 1;
    auto opened = ctx->converter->pdfBackend()->open(fs::path(path), ctx->pdfPassword);
    if (!opened.ok()) return fail(ctx, opened.status());
    *out_count = opened.value()->info().pageCount;
    return 0;
}

int pw_pdf_page_text(pw_context* ctx, const char* path, int page_index, char** out_text) {
    if (ctx == nullptr || path == nullptr || out_text == nullptr) return 1;
    auto opened = ctx->converter->pdfBackend()->open(fs::path(path), ctx->pdfPassword);
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

    auto opened = ctx->converter->pdfBackend()->open(fs::path(path), ctx->pdfPassword);
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
    Status st = ctx->converter->pdfBackend()->merge(inputs, fs::path(output));
    if (!st.ok()) return fail(ctx, st);
    return 0;
}

int pw_pdf_split(pw_context* ctx, const char* path, const char* out_dir, int pages_per_file) {
    if (ctx == nullptr || path == nullptr || out_dir == nullptr) return 1;
    Status st = ctx->converter->pdfBackend()->split(fs::path(path), fs::path(out_dir), pages_per_file);
    if (!st.ok()) return fail(ctx, st);
    return 0;
}

int pw_pdf_extract(pw_context* ctx, const char* path, const char* output, const char* page_spec) {
    if (ctx == nullptr || path == nullptr || output == nullptr || page_spec == nullptr) return 1;

    // 先拿页数，才能把 "1-3,5" 校验进真实范围
    auto doc = ctx->converter->pdfBackend()->open(fs::path(path), ctx->pdfPassword);
    if (!doc.ok()) return fail(ctx, doc.status());

    auto pages = parsePageSpec(page_spec, doc.value()->info().pageCount);
    if (!pages.ok()) return fail(ctx, pages.status());

    Status st = ctx->converter->pdfBackend()->extract(fs::path(path), fs::path(output), pages.value(),
                                                      ctx->pdfPassword);
    if (!st.ok()) return fail(ctx, st);
    return 0;
}

int pw_pdf_watermark(pw_context* ctx, const char* path, const char* output, const char* text,
                     float opacity) {
    if (ctx == nullptr || path == nullptr || output == nullptr || text == nullptr) return 1;
    Status st = ctx->converter->pdfBackend()->watermark(fs::path(path), fs::path(output), text, opacity);
    if (!st.ok()) return fail(ctx, st);
    return 0;
}

namespace {

// 元数据返回给 web 壳时用；只转义必须转义的字符，保持可读
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

}  // namespace

int pw_pdf_pages(pw_context* ctx, const char* path, const char* output, const char* page_spec) {
    if (ctx == nullptr || path == nullptr || output == nullptr || page_spec == nullptr) return 1;

    auto doc = ctx->converter->pdfBackend()->open(fs::path(path), ctx->pdfPassword);
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
    Status st = ctx->converter->pdfBackend()->assemble(refs, fs::path(output), ctx->pdfPassword);
    return st.ok() ? 0 : fail(ctx, st);
}

int pw_pdf_delete_pages(pw_context* ctx, const char* path, const char* output, const char* page_spec) {
    if (ctx == nullptr || path == nullptr || output == nullptr || page_spec == nullptr) return 1;

    auto doc = ctx->converter->pdfBackend()->open(fs::path(path), ctx->pdfPassword);
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
    Status st = ctx->converter->pdfBackend()->assemble(refs, fs::path(output), ctx->pdfPassword);
    return st.ok() ? 0 : fail(ctx, st);
}

namespace {

// 旋转 / 裁剪共用：对命中的页做变换，其余页原样
int transformPages(pw_context* ctx, const char* path, const char* output, const char* page_spec,
                   int rotation, bool crop, double l, double b, double r, double t) {
    auto doc = ctx->converter->pdfBackend()->open(fs::path(path), ctx->pdfPassword);
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
    Status st = ctx->converter->pdfBackend()->assemble(refs, fs::path(output), ctx->pdfPassword);
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

    auto doc = ctx->converter->pdfBackend()->open(fs::path(path), ctx->pdfPassword);
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
    Status st = ctx->converter->pdfBackend()->assemble(refs, fs::path(output), ctx->pdfPassword);
    return st.ok() ? 0 : fail(ctx, st);
}

int pw_pdf_insert_pages(pw_context* ctx, const char* path, const char* output, int after_page,
                        const char* other, const char* page_spec) {
    if (ctx == nullptr || path == nullptr || output == nullptr || other == nullptr ||
        page_spec == nullptr) {
        return 1;
    }

    auto doc = ctx->converter->pdfBackend()->open(fs::path(path), ctx->pdfPassword);
    if (!doc.ok()) return fail(ctx, doc.status());
    const int total = doc.value()->info().pageCount;
    if (after_page < 0 || after_page > total) {
        ctx->lastError = "插入位置超出范围";
        return 1;
    }

    auto otherDoc = ctx->converter->pdfBackend()->open(fs::path(other), ctx->pdfPassword);
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
    Status st = ctx->converter->pdfBackend()->assemble(refs, fs::path(output), ctx->pdfPassword);
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
    Status st = ctx->converter->pdfBackend()->imagesToPdf(list, fs::path(output));
    return st.ok() ? 0 : fail(ctx, st);
}

int pw_pdf_metadata(pw_context* ctx, const char* path, char** out_json) {
    if (ctx == nullptr || path == nullptr || out_json == nullptr) return 1;

    auto meta = ctx->converter->pdfBackend()->metadata(fs::path(path), ctx->pdfPassword);
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

    Status st = ctx->converter->pdfBackend()->setMetadata(fs::path(path), fs::path(output), meta,
                                                          ctx->pdfPassword);
    return st.ok() ? 0 : fail(ctx, st);
}

int pw_pdf_encrypt(pw_context* ctx, const char* path, const char* output, const char* user_password,
                   const char* owner_password, int permissions) {
    if (ctx == nullptr || path == nullptr || output == nullptr) return 1;
    Status st = ctx->converter->pdfBackend()->encrypt(fs::path(path), fs::path(output),
                                                      copyDup(user_password), copyDup(owner_password),
                                                      permissions, ctx->pdfPassword);
    return st.ok() ? 0 : fail(ctx, st);
}

int pw_pdf_decrypt(pw_context* ctx, const char* path, const char* output) {
    if (ctx == nullptr || path == nullptr || output == nullptr) return 1;
    Status st = ctx->converter->pdfBackend()->decrypt(fs::path(path), fs::path(output), ctx->pdfPassword);
    return st.ok() ? 0 : fail(ctx, st);
}

int pw_pdf_compress(pw_context* ctx, const char* path, const char* output) {
    if (ctx == nullptr || path == nullptr || output == nullptr) return 1;
    Status st = ctx->converter->pdfBackend()->compress(fs::path(path), fs::path(output), ctx->pdfPassword);
    return st.ok() ? 0 : fail(ctx, st);
}

void pw_free(void* p) {
    std::free(p);
}

}  // extern "C"
