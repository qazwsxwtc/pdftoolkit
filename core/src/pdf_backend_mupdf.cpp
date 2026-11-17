// MuPDF 后端。
// 仅在 CMake 里 PDFWORD_WITH_MUPDF=ON 且找到 SDK 时参与编译。
//
// 说明：MuPDF 的 C API 在不同发行版之间偶有签名调整（尤其 fz_show_string / 文档写入器），
// 这里用的是 1.23+ / 1.24+ 常见签名；如果你手上的 SDK 版本不同，编译器会直接指出位置，
// 按提示改参数即可，接口层（IPdfBackend）不用动。

#include "pdfword/pdf_backend.hpp"

#if defined(PDFWORD_HAVE_MUPDF)

#include <mupdf/fitz.h>
#include <mupdf/pdf.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <functional>
#include <limits>
#include <string>
#include <vector>

#include "pdfword/ocr.hpp"

namespace pw {
namespace {

constexpr float kMaxRenderPixels = 16384.0f * 16384.0f;

// MuPDF 的 stext 给的是 Unicode 码位（fz_stext_char::c），必须按 UTF-8 编码输出，
// 否则中文/日文/韩文会被压成一个 '?'。core 对外统一用 UTF-8。
void appendUtf8(std::string* out, int cp) {
    if (out == nullptr || cp < 0 || cp > 0x10FFFF) return;
    if (cp < 0x80) {
        out->push_back(static_cast<char>(cp));
    } else if (cp < 0x800) {
        out->push_back(static_cast<char>(0xC0 | (cp >> 6)));
        out->push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
        out->push_back(static_cast<char>(0xE0 | (cp >> 12)));
        out->push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out->push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
        out->push_back(static_cast<char>(0xF0 | (cp >> 18)));
        out->push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
        out->push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out->push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
}

class MuPdfDocument : public IPdfDocument {
public:
    MuPdfDocument(fz_context* ctx, fz_document* doc, int maxSide) : ctx_(ctx), doc_(doc), maxSide_(maxSide) {}

    ~MuPdfDocument() override {
        if (doc_ != nullptr) fz_drop_document(ctx_, doc_);
        if (ctx_ != nullptr) fz_drop_context(ctx_);
    }

    const DocumentInfo& info() const override { return info_; }

    // 由 backend 在 open 完成后填充（页数等信息只有 backend 侧知道）
    void setInfo(const DocumentInfo& info) {
        info_ = info;
    }

    Status pageInfo(int pageIndex, PageInfo* out) const override {
        if (out == nullptr) return Status::error(ErrorCode::InvalidArgument, "out 为空");
        char msg[512] = {0};
        bool failed = false;
        fz_page* page = nullptr;
        fz_rect bounds = fz_empty_rect;
        fz_try(ctx_) {
            page = fz_load_page(ctx_, doc_, pageIndex);
            bounds = fz_bound_page(ctx_, page);
        }
        fz_catch(ctx_) {
            std::snprintf(msg, sizeof(msg), "%s", fz_caught_message(ctx_));
            failed = true;
        }
        if (page != nullptr) fz_drop_page(ctx_, page);
        if (failed) return Status::error(ErrorCode::IoError, msg);
        if (fz_is_empty_rect(bounds)) return Status::error(ErrorCode::IoError, "页面尺寸为空");

        out->index = pageIndex;
        out->widthPt = bounds.x1 - bounds.x0;
        out->heightPt = bounds.y1 - bounds.y0;
        out->rotationDeg = 0.0;  // fz_bound_page 已包含 /Rotate 效果
        return Status::success();
    }

    Status pageText(int pageIndex, std::string* out) const override {
        if (out == nullptr) return Status::error(ErrorCode::InvalidArgument, "out 为空");
        out->clear();
        char msg[512] = {0};
        bool failed = false;

        fz_page* page = nullptr;
        fz_stext_page* stext = nullptr;
        fz_try(ctx_) {
            page = fz_load_page(ctx_, doc_, pageIndex);
            fz_stext_options opts{};
            opts.flags = FZ_STEXT_PRESERVE_LIGATURES | FZ_STEXT_PRESERVE_WHITESPACE;
            stext = fz_new_stext_page_from_page(ctx_, page, &opts);
        }
        fz_catch(ctx_) {
            std::snprintf(msg, sizeof(msg), "%s", fz_caught_message(ctx_));
            failed = true;
        }
        if (failed) {
            if (page != nullptr) fz_drop_page(ctx_, page);
            return Status::error(ErrorCode::IoError, msg);
        }

        std::string text;
        for (fz_stext_block* block = stext->first_block; block != nullptr; block = block->next) {
            if (block->type != FZ_STEXT_BLOCK_TEXT) continue;
            for (fz_stext_line* line = block->u.t.first_line; line != nullptr; line = line->next) {
                for (fz_stext_char* ch = line->first_char; ch != nullptr; ch = ch->next) {
                    if (ch->c >= 32) {
                        appendUtf8(&text, ch->c);
                    }
                }
                text.push_back('\n');
            }
            text.push_back('\n');
        }

        fz_drop_stext_page(ctx_, stext);
        fz_drop_page(ctx_, page);
        *out = std::move(text);
        return Status::success();
    }

    Status renderPage(int pageIndex, float zoom, RasterImage* out) const override {
        if (out == nullptr) return Status::error(ErrorCode::InvalidArgument, "out 为空");
        if (zoom <= 0.0f) zoom = 1.0f;

        float z = zoom;
        // 先按当前 zoom 估算像素尺寸，超限则整体降采样，避免 4K 屏 + 超大页面把内存打爆
        {
            PageInfo pi;
            if (pageInfo(pageIndex, &pi).ok()) {
                float w = static_cast<float>(pi.widthPt) * z;
                float h = static_cast<float>(pi.heightPt) * z;
                float longSide = std::max(w, h);
                if (longSide > static_cast<float>(maxSide_)) z *= static_cast<float>(maxSide_) / longSide;
                if (w * h > kMaxRenderPixels) {
                    float scale = std::sqrt(kMaxRenderPixels / (w * h));
                    z *= scale;
                }
            }
        }

        char msg[512] = {0};
        bool failed = false;
        fz_pixmap* pix = nullptr;
        fz_try(ctx_) {
            fz_matrix ctm = fz_scale(z, z);
            pix = fz_new_pixmap_from_page_number(ctx_, doc_, pageIndex, ctm, fz_device_rgb(ctx_), 0);
        }
        fz_catch(ctx_) {
            std::snprintf(msg, sizeof(msg), "%s", fz_caught_message(ctx_));
            failed = true;
        }
        if (failed || pix == nullptr) {
            if (pix != nullptr) fz_drop_pixmap(ctx_, pix);
            return Status::error(ErrorCode::IoError, failed ? msg : "渲染失败");
        }

        int w = pix->w;
        int h = pix->h;
        int n = pix->n;
        int stride = pix->stride;
        out->reset(w, h);

        for (int y = 0; y < h; ++y) {
            const unsigned char* src = pix->samples + static_cast<std::size_t>(y) * static_cast<std::size_t>(stride);
            unsigned char* dst = out->rgba.data() + static_cast<std::size_t>(y) * static_cast<std::size_t>(w) * 4u;
            for (int x = 0; x < w; ++x) {
                if (n >= 4) {
                    // RGB + alpha：合成到白底，UI 不必再处理透明
                    int a = src[3];
                    dst[0] = static_cast<unsigned char>((src[0] * a + 255 * (255 - a)) / 255);
                    dst[1] = static_cast<unsigned char>((src[1] * a + 255 * (255 - a)) / 255);
                    dst[2] = static_cast<unsigned char>((src[2] * a + 255 * (255 - a)) / 255);
                } else if (n == 3) {
                    dst[0] = src[0];
                    dst[1] = src[1];
                    dst[2] = src[2];
                } else {
                    dst[0] = dst[1] = dst[2] = src[0];
                }
                dst[3] = 255;
                src += n;
                dst += 4;
            }
        }

        fz_drop_pixmap(ctx_, pix);
        return Status::success();
    }

private:
    fz_context* ctx_ = nullptr;
    fz_document* doc_ = nullptr;
    int maxSide_ = 4096;
    DocumentInfo info_;
};

// 统一的写出参数：默认 options 全是 0（相当于不压缩），graft 之后体积会膨胀数倍，
// 所以这里打开 Flate 压缩。
pdf_write_options writeOptions() {
    pdf_write_options opts = pdf_default_write_options;
    opts.do_compress = 1;
    return opts;
}

// 把一段操作符文本变成内容流对象（流字典用 nullptr，仅带 /Length）
pdf_obj* addOpsStream(fz_context* ctx, pdf_document* doc, const std::string& ops) {
    fz_buffer* b = fz_new_buffer_from_copied_data(
        ctx, reinterpret_cast<const unsigned char*>(ops.data()), ops.size());
    pdf_obj* stream = nullptr;
    fz_try(ctx) { stream = pdf_add_stream(ctx, doc, b, nullptr, 1); }
    fz_always(ctx) { fz_drop_buffer(ctx, b); }
    fz_catch(ctx) { fz_rethrow(ctx); }
    return stream;
}

// 用 pdf_graft 直接搬运页面对象：保留矢量内容、透明蒙版和原始资源引用。
// 走 fz_document_writer("pdf") 的“重绘 + 蒸馏”路径会丢 image mask（透明通道），
// 而且慢得多——这是 MuPDF 自身 pdf 设备的限制，graft 绕开了它。
Status writeGraftedRange(fz_context* ctx, const std::vector<fs::path>& inputs, const fs::path& output,
                         int firstPageGlobal, int pageCount) {
    char msg[512] = {0};
    bool failed = false;
    int skipped = firstPageGlobal;

    pdf_document* dst = nullptr;
    fz_try(ctx) { dst = pdf_create_document(ctx); }
    fz_catch(ctx) {
        std::snprintf(msg, sizeof(msg), "%s", fz_caught_message(ctx));
        failed = true;
    }
    if (failed || dst == nullptr) {
        if (dst != nullptr) pdf_drop_document(ctx, dst);
        return Status::error(ErrorCode::IoError, failed ? msg : "无法创建输出 PDF");
    }

    for (const auto& in : inputs) {
        if (failed || pageCount <= 0) break;

        pdf_document* src = nullptr;
        fz_try(ctx) { src = pdf_open_document(ctx, in.string().c_str()); }
        fz_catch(ctx) {
            std::snprintf(msg, sizeof(msg), "%s", fz_caught_message(ctx));
            failed = true;
        }
        if (failed || src == nullptr) {
            if (src != nullptr) pdf_drop_document(ctx, src);
            break;
        }

        int total = 0;
        fz_try(ctx) { total = pdf_count_pages(ctx, src); }
        fz_catch(ctx) { failed = true; }

        // 同一个输入文件共用一个 graft map，避免每页重复拷字体/图片
        pdf_graft_map* graft_map = nullptr;
        if (!failed) {
            fz_try(ctx) { graft_map = pdf_new_graft_map(ctx, dst); }
            fz_catch(ctx) {
                std::snprintf(msg, sizeof(msg), "%s", fz_caught_message(ctx));
                failed = true;
            }
        }

        for (int i = 0; !failed && i < total && pageCount > 0; ++i) {
            if (skipped > 0) { --skipped; continue; }
            fz_try(ctx) { pdf_graft_mapped_page(ctx, graft_map, -1, src, i); }
            fz_catch(ctx) { failed = true; }
            if (!failed) --pageCount;
        }
        if (graft_map != nullptr) pdf_drop_graft_map(ctx, graft_map);
        pdf_drop_document(ctx, src);
    }

    if (!failed) {
        pdf_write_options opts = writeOptions();
        if (opts.do_garbage < 3) opts.do_garbage = 3;
        fz_try(ctx) { pdf_save_document(ctx, dst, output.string().c_str(), &opts); }
        fz_catch(ctx) {
            std::snprintf(msg, sizeof(msg), "%s", fz_caught_message(ctx));
            failed = true;
        }
    }
    pdf_drop_document(ctx, dst);

    if (failed) return Status::error(ErrorCode::IoError, msg);
    return Status::success();
}

// 抽取指定页：按传入顺序逐页 graft，所以支持重排（不只是升序截取）。
Status writeGraftedPages(fz_context* ctx, const fs::path& input, const fs::path& output,
                         const std::vector<int>& pages, const std::string& password) {
    char msg[512] = {0};
    bool failed = false;

    pdf_document* dst = nullptr;
    pdf_document* src = nullptr;
    pdf_graft_map* graft_map = nullptr;

    fz_try(ctx) { dst = pdf_create_document(ctx); }
    fz_catch(ctx) {
        std::snprintf(msg, sizeof(msg), "%s", fz_caught_message(ctx));
        failed = true;
    }
    if (failed || dst == nullptr) {
        if (dst != nullptr) pdf_drop_document(ctx, dst);
        return Status::error(ErrorCode::IoError, failed ? msg : "无法创建输出 PDF");
    }

    fz_try(ctx) { src = pdf_open_document(ctx, input.string().c_str()); }
    fz_catch(ctx) {
        std::snprintf(msg, sizeof(msg), "%s", fz_caught_message(ctx));
        failed = true;
    }
    if (failed || src == nullptr) {
        if (src != nullptr) pdf_drop_document(ctx, src);
        pdf_drop_document(ctx, dst);
        return Status::error(ErrorCode::IoError, failed ? msg : "无法打开源 PDF");
    }

    int total = 0;
    fz_try(ctx) {
        if (pdf_needs_password(ctx, src) != 0) {
            if (!pdf_authenticate_password(ctx, src, password.c_str())) {
                std::snprintf(msg, sizeof(msg), "%s",
                              password.empty() ? "文档已加密，需要密码" : "文档已加密，密码错误");
                failed = true;
            }
        }
        if (!failed) total = pdf_count_pages(ctx, src);
    }
    fz_catch(ctx) {
        std::snprintf(msg, sizeof(msg), "%s", fz_caught_message(ctx));
        failed = true;
    }

    if (!failed) {
        for (int p : pages) {
            if (p < 0 || p >= total) {
                std::snprintf(msg, sizeof(msg), "页码 %d 超出范围（共 %d 页）", p + 1, total);
                failed = true;
                break;
            }
        }
    }

    if (!failed) {
        fz_try(ctx) { graft_map = pdf_new_graft_map(ctx, dst); }
        fz_catch(ctx) {
            std::snprintf(msg, sizeof(msg), "%s", fz_caught_message(ctx));
            failed = true;
        }
    }

    for (int p : pages) {
        if (failed) break;
        fz_try(ctx) {
            // 同一个源文件共用一个 graft map：字体/图片只拷一次，页间复用
            pdf_graft_mapped_page(ctx, graft_map, -1, src, p);
        }
        fz_catch(ctx) { failed = true; }
    }

    if (!failed) {
        pdf_write_options opts = writeOptions();
        if (opts.do_garbage < 3) opts.do_garbage = 3;
        fz_try(ctx) { pdf_save_document(ctx, dst, output.string().c_str(), &opts); }
        fz_catch(ctx) {
            std::snprintf(msg, sizeof(msg), "%s", fz_caught_message(ctx));
            failed = true;
        }
    }

    if (graft_map != nullptr) pdf_drop_graft_map(ctx, graft_map);
    pdf_drop_document(ctx, src);
    pdf_drop_document(ctx, dst);

    if (failed) return Status::error(ErrorCode::IoError, msg);
    return Status::success();
}

// ---------------------------------------------------------------------------
// 页面装配引擎：删除 / 重排 / 旋转 / 裁剪 / 插入空白页 / 插入其他 PDF 页
// 都只是"输出页从哪来"这张表的不同填法，所以只在 graft 之上加一层。
// ---------------------------------------------------------------------------

int normalizeRotate(int deg) {
    int r = deg % 360;
    if (r < 0) r += 360;
    return r;
}

// 打开源文档并按需解密；失败时返回 nullptr 并给出可读原因
pdf_document* openSourceDocument(fz_context* ctx, const fs::path& path, const std::string& password,
                                 std::string* errMsg) {
    pdf_document* doc = nullptr;
    std::string err;
    bool failed = false;

    fz_try(ctx) { doc = pdf_open_document(ctx, path.string().c_str()); }
    fz_catch(ctx) {
        err = fz_caught_message(ctx);
        failed = true;
    }
    if (failed || doc == nullptr) {
        if (doc != nullptr) pdf_drop_document(ctx, doc);
        *errMsg = failed ? err : ("无法打开 " + path.filename().string());
        return nullptr;
    }

    bool needPassword = false;
    fz_try(ctx) { needPassword = pdf_needs_password(ctx, doc) != 0; }
    fz_catch(ctx) {
        err = fz_caught_message(ctx);
        failed = true;
    }
    if (!failed && needPassword) {
        bool authenticated = false;
        fz_try(ctx) { authenticated = pdf_authenticate_password(ctx, doc, password.c_str()) != 0; }
        fz_catch(ctx) {
            err = fz_caught_message(ctx);
            failed = true;
        }
        if (!failed && !authenticated) {
            *errMsg = password.empty() ? "文档已加密，需要密码" : "文档已加密，密码错误";
            pdf_drop_document(ctx, doc);
            return nullptr;
        }
    }
    if (failed) {
        *errMsg = err;
        pdf_drop_document(ctx, doc);
        return nullptr;
    }
    return doc;
}

// 在 graft 过来的页上叠加旋转 / 裁剪
void applyPageEdits(fz_context* ctx, pdf_document* doc, pdf_obj* pageobj, const PageRef& ref) {
    if (pageobj == nullptr) return;

    const int delta = normalizeRotate(ref.rotateDelta);
    if (delta != 0) {
        int base = 0;
        fz_try(ctx) { base = pdf_dict_get_inheritable_int(ctx, pageobj, PDF_NAME(Rotate)); }
        fz_catch(ctx) { base = 0; }
        pdf_dict_put_int(ctx, pageobj, PDF_NAME(Rotate), normalizeRotate(base + delta));
    }

    if (ref.hasCrop) {
        fz_rect media = fz_make_rect(0, 0, 595, 842);
        fz_try(ctx) { media = pdf_dict_get_inheritable_rect(ctx, pageobj, PDF_NAME(MediaBox)); }
        fz_catch(ctx) {}

        double x0 = media.x0 + ref.cropLeft;
        double y0 = media.y0 + ref.cropBottom;
        double x1 = media.x1 - ref.cropRight;
        double y1 = media.y1 - ref.cropTop;
        // 裁到负尺寸会直接生成坏页，退回不裁
        if (x1 - x0 < 1.0) { x0 = media.x0; x1 = media.x1; }
        if (y1 - y0 < 1.0) { y0 = media.y0; y1 = media.y1; }

        pdf_obj* arr = pdf_new_array(ctx, doc, 4);
        fz_try(ctx) {
            pdf_array_push_real(ctx, arr, x0);
            pdf_array_push_real(ctx, arr, y0);
            pdf_array_push_real(ctx, arr, x1);
            pdf_array_push_real(ctx, arr, y1);
            pdf_dict_put(ctx, pageobj, PDF_NAME(CropBox), arr);
        }
        fz_always(ctx) { pdf_drop_obj(ctx, arr); }
        fz_catch(ctx) { fz_rethrow(ctx); }
    }
}

Status writeAssembled(fz_context* ctx, const std::vector<PageRef>& pages, const fs::path& output,
                      const std::string& password) {
    std::string err;
    pdf_document* dst = nullptr;
    fz_try(ctx) { dst = pdf_create_document(ctx); }
    fz_catch(ctx) { return Status::error(ErrorCode::IoError, fz_caught_message(ctx)); }
    if (dst == nullptr) return Status::error(ErrorCode::IoError, "无法创建输出 PDF");

    bool failed = false;
    // 同一个源文件只打开一次；它同时对应一个 graft map，保证对象只拷一次
    struct SrcEntry {
        std::string key;
        pdf_document* doc = nullptr;
        pdf_graft_map* map = nullptr;
    };
    std::vector<SrcEntry> srcCache;

    for (const PageRef& ref : pages) {
        if (failed) break;

        if (ref.source.empty()) {
            fz_buffer* contents = nullptr;
            pdf_obj* res = nullptr;
            pdf_obj* page = nullptr;
            fz_try(ctx) {
                res = pdf_new_dict(ctx, dst, 2);
                contents = fz_new_buffer(ctx, 8);
                page = pdf_add_page(ctx, dst,
                                    fz_make_rect(0, 0, ref.blankWidthPt, ref.blankHeightPt),
                                    normalizeRotate(ref.rotateDelta), res, contents);
                pdf_insert_page(ctx, dst, -1, page);
            }
            fz_always(ctx) {
                fz_drop_buffer(ctx, contents);
                pdf_drop_obj(ctx, res);
                pdf_drop_obj(ctx, page);
            }
            fz_catch(ctx) {
                err = fz_caught_message(ctx);
                failed = true;
            }
            continue;
        }

        pdf_document* src = nullptr;
        pdf_graft_map* map = nullptr;
        for (auto& item : srcCache) {
            if (item.key == ref.source.string()) {
                src = item.doc;
                map = item.map;
                break;
            }
        }
        if (src == nullptr) {
            src = openSourceDocument(ctx, ref.source, password, &err);
            if (src == nullptr) {
                failed = true;
                break;
            }
            fz_try(ctx) { map = pdf_new_graft_map(ctx, dst); }
            fz_catch(ctx) {
                err = fz_caught_message(ctx);
                pdf_drop_document(ctx, src);
                failed = true;
                break;
            }
            srcCache.push_back({ref.source.string(), src, map});
        }

        int total = 0;
        fz_try(ctx) { total = pdf_count_pages(ctx, src); }
        fz_catch(ctx) {
            err = fz_caught_message(ctx);
            failed = true;
        }
        if (failed) break;
        if (ref.pageIndex < 0 || ref.pageIndex >= total) {
            err = "页码 " + std::to_string(ref.pageIndex + 1) + " 超出范围（" +
                  ref.source.filename().string() + " 共 " + std::to_string(total) + " 页）";
            failed = true;
            break;
        }

        fz_try(ctx) {
            // 关键：同一个源文件共用一个 graft map，对象只拷一次并在页间复用。
            // 用 pdf_graft_page 逐页 graft 会每页各拷一份字体/图片，82 页能膨胀到 1.6 倍。
            pdf_graft_mapped_page(ctx, map, -1, src, ref.pageIndex);
            const int count = pdf_count_pages(ctx, dst);
            pdf_obj* pageobj = pdf_lookup_page_obj(ctx, dst, count - 1);
            applyPageEdits(ctx, dst, pageobj, ref);
        }
        fz_catch(ctx) {
            err = fz_caught_message(ctx);
            failed = true;
        }
    }

    if (!failed) {
        pdf_write_options opts = writeOptions();
        // graft 会给每页资源各留一份副本，不去重时 82 页能膨胀近一倍。
        // do_garbage: 1=回收, 2=重编号, 3=去重复（MuPDF 的语义，取最大值把三件事都做上）
        if (opts.do_garbage < 3) opts.do_garbage = 3;
        fz_try(ctx) { pdf_save_document(ctx, dst, output.string().c_str(), &opts); }
        fz_catch(ctx) {
            err = fz_caught_message(ctx);
            failed = true;
        }
    }

    for (auto& item : srcCache) {
        pdf_drop_graft_map(ctx, item.map);
        pdf_drop_document(ctx, item.doc);
    }
    pdf_drop_document(ctx, dst);

    if (failed) return Status::error(ErrorCode::IoError, err);
    return Status::success();
}

// 图片 -> PDF：图片作为 XObject 直接嵌入，不走重绘，像素无损
Status writeImagesAsPdf(fz_context* ctx, const std::vector<ImagePage>& images, const fs::path& output) {
    std::string err;
    pdf_document* dst = nullptr;
    fz_try(ctx) { dst = pdf_create_document(ctx); }
    fz_catch(ctx) { return Status::error(ErrorCode::IoError, fz_caught_message(ctx)); }

    bool failed = false;
    for (const ImagePage& item : images) {
        if (failed) break;

        fz_image* image = nullptr;
        fz_buffer* contents = nullptr;
        pdf_obj* imgobj = nullptr;
        pdf_obj* imName = nullptr;
        pdf_obj* xobjects = nullptr;
        pdf_obj* res = nullptr;
        pdf_obj* page = nullptr;

        fz_try(ctx) {
            image = fz_new_image_from_file(ctx, item.path.string().c_str());
            const double dpi = (item.dpi > 1.0) ? item.dpi : 96.0;
            const double wpt = image->w * 72.0 / dpi;
            const double hpt = image->h * 72.0 / dpi;

            double pageW = wpt;
            double pageH = hpt;
            double drawW = wpt;
            double drawH = hpt;
            if (item.fitA4) {
                const double kA4W = 595.0;
                const double kA4H = 842.0;
                pageW = kA4W;
                pageH = kA4H;
                const double s = std::min(kA4W / wpt, kA4H / hpt);
                drawW = wpt * s;
                drawH = hpt * s;
            }
            const double tx = (pageW - drawW) / 2.0;
            const double ty = (pageH - drawH) / 2.0;

            imgobj = pdf_add_image(ctx, dst, image);
            imName = pdf_new_name(ctx, "Im0");
            xobjects = pdf_new_dict(ctx, dst, 1);
            pdf_dict_put(ctx, xobjects, imName, imgobj);
            res = pdf_new_dict(ctx, dst, 1);
            pdf_dict_put(ctx, res, PDF_NAME(XObject), xobjects);

            contents = fz_new_buffer(ctx, 64);
            fz_append_printf(ctx, contents, "q %g 0 0 %g %g %g cm /Im0 Do Q", drawW, drawH, tx, ty);

            page = pdf_add_page(ctx, dst, fz_make_rect(0, 0, pageW, pageH), 0, res, contents);
            pdf_insert_page(ctx, dst, -1, page);
        }
        fz_always(ctx) {
            fz_drop_image(ctx, image);
            fz_drop_buffer(ctx, contents);
            pdf_drop_obj(ctx, page);
            pdf_drop_obj(ctx, res);
            pdf_drop_obj(ctx, xobjects);
            pdf_drop_obj(ctx, imName);
            pdf_drop_obj(ctx, imgobj);
        }
        fz_catch(ctx) {
            err = fz_caught_message(ctx);
            failed = true;
        }
    }

    if (!failed) {
        pdf_write_options opts = writeOptions();
        fz_try(ctx) { pdf_save_document(ctx, dst, output.string().c_str(), &opts); }
        fz_catch(ctx) {
            err = fz_caught_message(ctx);
            failed = true;
        }
    }
    pdf_drop_document(ctx, dst);

    if (failed) return Status::error(ErrorCode::IoError, err);
    return Status::success();
}

Result<DocumentMetadata> readMetadata(fz_context* ctx, const fs::path& path,
                                      const std::string& password) {
    std::string err;
    pdf_document* doc = openSourceDocument(ctx, path, password, &err);
    if (doc == nullptr) return Status::error(ErrorCode::IoError, err);

    DocumentMetadata meta;
    fz_try(ctx) {
        pdf_obj* info = pdf_dict_get(ctx, pdf_trailer(ctx, doc), PDF_NAME(Info));
        if (info != nullptr) {
            auto text = [ctx, info](pdf_obj* key) -> std::string {
                const char* v = pdf_dict_get_text_string(ctx, info, key);
                return (v != nullptr) ? std::string(v) : std::string();
            };
            meta.title = text(PDF_NAME(Title));
            meta.author = text(PDF_NAME(Author));
            meta.subject = text(PDF_NAME(Subject));
            meta.keywords = text(PDF_NAME(Keywords));
            meta.creator = text(PDF_NAME(Creator));
            meta.producer = text(PDF_NAME(Producer));
        }
    }
    fz_catch(ctx) {
        err = fz_caught_message(ctx);
        pdf_drop_document(ctx, doc);
        return Status::error(ErrorCode::IoError, err);
    }
    pdf_drop_document(ctx, doc);
    return meta;
}

Status writeMetadata(fz_context* ctx, const fs::path& input, const fs::path& output,
                     const DocumentMetadata& meta, const std::string& password) {
    std::string err;
    pdf_document* doc = openSourceDocument(ctx, input, password, &err);
    if (doc == nullptr) return Status::error(ErrorCode::IoError, err);

    bool failed = false;
    fz_try(ctx) {
        pdf_obj* trailer = pdf_trailer(ctx, doc);
        pdf_obj* info = pdf_dict_get(ctx, trailer, PDF_NAME(Info));
        if (info == nullptr) {
            info = pdf_new_dict(ctx, doc, 8);
            pdf_dict_put(ctx, trailer, PDF_NAME(Info), info);
            pdf_drop_obj(ctx, info);
        }
        auto setText = [ctx, info](pdf_obj* key, const std::string& value) {
            if (value.empty()) {
                pdf_dict_del(ctx, info, key);
            } else {
                pdf_dict_put_text_string(ctx, info, key, value.c_str());
            }
        };
        setText(PDF_NAME(Title), meta.title);
        setText(PDF_NAME(Author), meta.author);
        setText(PDF_NAME(Subject), meta.subject);
        setText(PDF_NAME(Keywords), meta.keywords);
        setText(PDF_NAME(Creator), meta.creator);
        setText(PDF_NAME(Producer), meta.producer);

        pdf_write_options opts = writeOptions();
        pdf_save_document(ctx, doc, output.string().c_str(), &opts);
    }
    fz_catch(ctx) {
        err = fz_caught_message(ctx);
        failed = true;
    }
    pdf_drop_document(ctx, doc);
    if (failed) return Status::error(ErrorCode::IoError, err);
    return Status::success();
}

// 重新保存一份：加密 / 解密 / 压缩都走这里，差别只在 write options
Status resaveWithOptions(fz_context* ctx, const fs::path& input, const fs::path& output,
                         const std::string& password,
                         const std::function<void(pdf_write_options*)>& configure,
                         bool subsetFonts = false) {
    std::string err;
    pdf_document* doc = openSourceDocument(ctx, input, password, &err);
    if (doc == nullptr) return Status::error(ErrorCode::IoError, err);

    bool failed = false;
    fz_try(ctx) {
        if (subsetFonts) pdf_subset_fonts(ctx, doc, 0, nullptr);
        pdf_write_options opts = writeOptions();
        if (configure) configure(&opts);
        pdf_save_document(ctx, doc, output.string().c_str(), &opts);
    }
    fz_catch(ctx) {
        err = fz_caught_message(ctx);
        failed = true;
    }
    pdf_drop_document(ctx, doc);
    if (failed) return Status::error(ErrorCode::IoError, err);
    return Status::success();
}

// ---- 书签 / 目录（/Outlines）------------------------------------------
// 读：fz_load_outline 拿到整棵目录树，深度优先拍平成 (title, page, level) 的扁平列表。
//     内部跳转的 fz_location 换成 0 基页号再 +1 变成界面的 1 基页码；外链没有页码，记 0。
Result<std::vector<OutlineItem>> readOutline(fz_context* ctx, const fs::path& input,
                                             const std::string& password) {
    std::string err;
    pdf_document* doc = openSourceDocument(ctx, input, password, &err);
    if (doc == nullptr) return Status::error(ErrorCode::IoError, err);

    std::vector<OutlineItem> items;
    fz_outline* root = nullptr;
    bool failed = false;
    fz_try(ctx) {
        root = fz_load_outline(ctx, &doc->super);
        if (root != nullptr) {
            // 递归 lambda：同一层用 next 串起来，子项挂在 down 上
            std::function<void(fz_outline*, int)> walk = [&](fz_outline* node, int level) {
                for (fz_outline* cur = node; cur != nullptr; cur = cur->next) {
                    OutlineItem item;
                    item.title = cur->title != nullptr ? std::string(cur->title) : std::string();
                    item.level = level;
                    // fz_load_outline 已经把目标解析进 page（外链 / 无目标是 -1），
                    // 这里换成 0 基页号再 +1 变成界面的 1 基页码；解析不到就记 0
                    const int pageNo = fz_page_number_from_location(ctx, &doc->super, cur->page);
                    item.page = pageNo >= 0 ? pageNo + 1 : 0;
                    items.push_back(item);
                    if (cur->down != nullptr) walk(cur->down, level + 1);
                }
            };
            walk(root, 0);
        }
    }
    fz_catch(ctx) {
        err = fz_caught_message(ctx);
        failed = true;
    }
    if (root != nullptr) fz_drop_outline(ctx, root);
    pdf_drop_document(ctx, doc);
    if (failed) return Status::error(ErrorCode::IoError, err);
    return items;
}

// 写：outline iterator 先删光原有项，再按扁平列表逐项插入。
// 层级只做 ±1 的移动（跳级会被夹掉），因为迭代器只能 down 一层 / up 一层地走。
// insert 之后迭代器进入"追加"模式（下一条自动接在后面），所以往下钻之前要先 prev() 复位。
Status writeOutline(fz_context* ctx, const fs::path& input, const fs::path& output,
                    const std::vector<OutlineItem>& items, const std::string& password) {
    std::string err;
    pdf_document* doc = openSourceDocument(ctx, input, password, &err);
    if (doc == nullptr) return Status::error(ErrorCode::IoError, err);
    const int pageCount = pdf_count_pages(ctx, doc);

    fz_outline_iterator* iter = nullptr;
    bool failed = false;
    fz_try(ctx) {
        iter = pdf_new_outline_iterator(ctx, doc);

        // 1) 清掉原有目录。删完迭代器会停在"可以插入"的位置（空目录或 Outlines 根）
        while (fz_outline_iterator_item(ctx, iter) != nullptr) {
            if (fz_outline_iterator_delete(ctx, iter) < 0) break;
        }

        // 2) 逐项插入
        int level = 0;
        for (size_t i = 0; i < items.size(); ++i) {
            const OutlineItem& src = items[i];
            int want = src.level < 0 ? 0 : src.level;
            if (i == 0) want = 0;                        // 第一项只能是顶级
            else if (want > level + 1) want = level + 1;  // 不跳级

            while (level < want) {
                fz_outline_iterator_prev(ctx, iter); // 复位成"当前项"，否则 down 不生效
                if (fz_outline_iterator_down(ctx, iter) < 0) break;
                ++level;
            }
            while (level > want) {
                if (fz_outline_iterator_up(ctx, iter) < 0) break;
                fz_outline_iterator_next(ctx, iter); // 停到"父项之后可以继续追加"的位置
                --level;
            }

            std::string uri;
            if (src.page >= 1 && src.page <= pageCount) {
                uri = "#page=" + std::to_string(src.page);
            }
            fz_outline_item it{};
            it.title = const_cast<char*>(src.title.c_str());
            it.uri = uri.empty() ? nullptr : const_cast<char*>(uri.c_str());
            it.is_open = 1;
            fz_outline_iterator_insert(ctx, iter, &it);
        }

        pdf_write_options opts = writeOptions();
        pdf_save_document(ctx, doc, output.string().c_str(), &opts);
    }
    fz_catch(ctx) {
        err = fz_caught_message(ctx);
        failed = true;
    }
    if (iter != nullptr) fz_drop_outline_iterator(ctx, iter);
    pdf_drop_document(ctx, doc);
    if (failed) return Status::error(ErrorCode::IoError, err);
    return Status::success();
}

// ---- 文字水印的位置计算 ---------------------------------------------
// 全部在"显示空间"里算：x 向右、y 向下（pageCtm 已经把 /Rotate、CropBox 折进来），单位 pt。
// 每条水印就是一个基线起点 + 旋转角，最后统一塞进一个 fz_text 一次画完。
struct TextSpot {
    float x;
    float y;
    float angle;   // 度，0 = 水平
};

// 给定字号测量文字宽度（pt）：先按该字号摆一条，再用 fz_bound_text 取包围盒。
// 不用 fz_measure_string——它返回的矩阵在 CJK 字体上取不到可靠 advance。
float measureTextWidth(fz_context* ctx, fz_font* font, const std::string& text, float size) {
    float width = 0;
    fz_text* probe = nullptr;
    fz_try(ctx) {
        probe = fz_new_text(ctx);
        fz_show_string(ctx, probe, font, fz_scale(size, -size), text.c_str(), 0, 0, FZ_BIDI_LTR,
                       FZ_LANG_UNSET);
        fz_rect b = fz_bound_text(ctx, probe, nullptr, fz_identity);
        width = b.x1 - b.x0;
    }
    fz_catch(ctx) {
        if (probe != nullptr) fz_drop_text(ctx, probe);
        fz_rethrow(ctx);
    }
    fz_drop_text(ctx, probe);
    return width;
}

// 一条水印文字栅格化后的位图：字号相同的页共用一张，整份文档只嵌一份像素数据。
struct TextStampSpec {
    int key;          // 量化后的字号，用来判定能否复用
    pdf_obj* image;   // image XObject
    float w;          // 位图在页面上的宽度（pt）
    float h;          // 位图在页面上的高度（pt）
    float baseU;      // 文字基线起点在位图里的归一化坐标（0..1，y 向上）
    float baseV;
};

// 把一条水印文字渲染成透明底位图，再包成 image XObject。
// 斜向水印必须走这条路：MuPDF 渲染“文字矩阵里带旋转”的内容流时会把字号放大 2~3 倍
//（45° 时整页糊成一片色块），而图片的 cm 变换在所有阅读器（含 MuPDF 自己）结果一致。
pdf_obj* renderTextStamp(fz_context* ctx, pdf_document* doc, fz_font* font,
                         const std::string& text, float size, float textWidth, const float rgb[3],
                         float* outW, float* outH, float* outBaseU, float* outBaseV) {
    const float kSuper = 4.0f;                 // 超采样，放大看也不糊
    const float padX = size * 0.35f;
    const float padY = size * 0.28f;
    const float wPts = textWidth + padX * 2;
    const float hPts = size * 1.28f + padY * 2;

    pdf_obj* result = nullptr;
    fz_pixmap* pix = nullptr;
    fz_device* dev = nullptr;
    fz_text* txt = nullptr;
    fz_image* img = nullptr;

    fz_try(ctx) {
        fz_irect bb = fz_irect_from_rect(fz_make_rect(0, 0, wPts * kSuper, hPts * kSuper));
        pix = fz_new_pixmap_with_bbox(ctx, fz_device_rgb(ctx), bb, nullptr, 1);
        // 全透明底：alpha=0，颜色铺白，缩放时边缘不会发黑
        for (int y = 0; y < pix->h; ++y) {
            unsigned char* p = pix->samples + static_cast<std::size_t>(y) * pix->stride;
            for (int x = 0; x < pix->w; ++x, p += pix->n) {
                p[0] = 255;
                p[1] = 255;
                p[2] = 255;
                if (pix->n > 3) p[3] = 0;
            }
        }
        dev = fz_new_draw_device(ctx, fz_scale(kSuper, kSuper), pix);
        txt = fz_new_text(ctx);
        // 基线落在位图里，左右各留 padX、字顶留 padY
        fz_matrix trm = fz_concat(fz_scale(size, -size), fz_translate(padX, size + padY));
        fz_show_string(ctx, txt, font, trm, text.c_str(), 0, 0, FZ_BIDI_LTR, FZ_LANG_UNSET);
        fz_fill_text(ctx, dev, txt, fz_identity, fz_device_rgb(ctx), rgb, 1.0f,
                     fz_default_color_params);
        fz_close_device(ctx, dev);
        fz_drop_device(ctx, dev);
        dev = nullptr;
        fz_drop_text(ctx, txt);
        txt = nullptr;
        img = fz_new_image_from_pixmap(ctx, pix, nullptr);
        result = pdf_add_image(ctx, doc, img);
    }
    fz_always(ctx) {
        if (txt != nullptr) fz_drop_text(ctx, txt);
        if (dev != nullptr) fz_drop_device(ctx, dev);
        if (img != nullptr) fz_drop_image(ctx, img);
        if (pix != nullptr) fz_drop_pixmap(ctx, pix);
    }
    fz_catch(ctx) { result = nullptr; }

    if (outW != nullptr) *outW = wPts;
    if (outH != nullptr) *outH = hPts;
    // 位图坐标系 y 向下，PDF 图片的单位正方形 y 向上，所以算归一化时把 y 翻过来
    if (outBaseU != nullptr) *outBaseU = (wPts > 0) ? (padX / wPts) : 0.0f;
    if (outBaseV != nullptr) *outBaseV = (hPts > 0) ? (1.0f - (padY + size) / hPts) : 0.0f;
    return result;
}

// 12 种摆放方式：九宫格单条 / 居中斜放 / 水平铺满 / 45° 斜向铺满。
std::vector<TextSpot> computeTextSpots(const std::string& position, fz_rect box, float textWidth,
                                       float size) {
    std::vector<TextSpot> spots;
    const float kSlope = 0.70710678f;   // sin/cos 45°

    const float x0 = box.x0;
    const float y0 = box.y0;
    const float w = box.x1 - box.x0;
    const float h = box.y1 - box.y0;
    const float padX = w * 0.06f;
    const float padY = h * 0.06f;

    const float left = x0 + padX;
    const float right = x0 + w - padX - textWidth;
    const float hcenter = x0 + (w - textWidth) * 0.5f;
    const float top = y0 + padY + size;              // 基线：顶边往下 padY，再加一个字高
    const float vmiddle = y0 + (h + size) * 0.5f;
    const float bottom = y0 + h - padY;

    auto put = [&spots](float x, float y, float angle) { spots.push_back({x, y, angle}); };

    if (position == "top-center") {
        put(hcenter, top, 0);
    } else if (position == "top-right") {
        put(right, top, 0);
    } else if (position == "middle-left") {
        put(left, vmiddle, 0);
    } else if (position == "center") {
        put(hcenter, vmiddle, 0);
    } else if (position == "middle-right") {
        put(right, vmiddle, 0);
    } else if (position == "bottom-left") {
        put(left, bottom, 0);
    } else if (position == "bottom-center") {
        put(hcenter, bottom, 0);
    } else if (position == "bottom-right") {
        put(right, bottom, 0);
    } else if (position == "center-diagonal") {
        // 让文字的中点落在页面中心：基线起点沿文字方向回退半个宽度
        put(x0 + w * 0.5f - textWidth * 0.5f * kSlope, y0 + h * 0.5f + textWidth * 0.5f * kSlope,
            -45.0f);
    } else if (position == "tile-horizontal") {
        const float stepX = textWidth * 1.5f + size * 0.5f;
        const float stepY = size * 3.0f;
        int guard = 0;
        for (float y = top; y <= y0 + h - padY * 0.4f && guard < 400; y += stepY) {
            for (float x = left; x <= x0 + w - padX * 0.4f && guard < 400; x += stepX) {
                put(x, y, 0);
                ++guard;
            }
        }
    } else if (position == "tile-diagonal") {
        // 文字方向 (kSlope, -kSlope)（往右上），行方向 (kSlope, kSlope)（垂直往右下）。
        // 斜排文字的"带厚"在垂直方向上是 (宽 + 字号) * sin45，行距取它的 1.45 倍才不会叠字。
        const float stepU = textWidth * 1.5f + size * 0.5f;
        const float stepV = (textWidth + size) * kSlope * 1.45f;
        const float cx = x0 + w * 0.5f;
        const float cy = y0 + h * 0.5f;
        const float reach = std::sqrt(w * w + h * h) * 0.5f + stepU * 2;
        const int nU = static_cast<int>(reach / stepU) + 1;
        const int nV = static_cast<int>(reach / stepV) + 1;
        int guard = 0;
        for (int iv = -nV; iv <= nV && guard < 400; ++iv) {
            for (int iu = -nU; iu <= nU && guard < 400; ++iu) {
                const float sx = cx + iu * stepU * kSlope + iv * stepV * kSlope;
                const float sy = cy - iu * stepU * kSlope + iv * stepV * kSlope;
                // 文字中点落在页面内才画，页外的大片格点直接跳过
                const float mx = sx + textWidth * 0.5f * kSlope;
                const float my = sy - textWidth * 0.5f * kSlope;
                if (mx < x0 - 2 || mx > x0 + w + 2 || my < y0 - 2 || my > y0 + h + 2) continue;
                put(sx, sy, -45.0f);
                ++guard;
            }
        }
    } else {
        // top-left，以及任何不认识的取值
        put(left, top, 0);
    }
    return spots;
}

// 水印：先把原页 graft 过来（矢量、字体、透明蒙版全部原样保留），
// 再把水印画成一条独立内容流追加到该页的 /Contents，资源并入 /Resources。
// 相比"整页重绘再蒸馏"，这条路径不会丢 image mask，也不改动原页内容。
Status writeWatermarked(fz_context* ctx, const fs::path& input, const fs::path& output,
                        const std::string& text, float opacity, const std::string& position) {
    char msg[512] = {0};
    bool failed = false;

    pdf_document* dst = nullptr;
    pdf_document* src = nullptr;
    pdf_graft_map* graft_map = nullptr;
    fz_font* font = nullptr;

    fz_try(ctx) { dst = pdf_create_document(ctx); }
    fz_catch(ctx) {
        std::snprintf(msg, sizeof(msg), "%s", fz_caught_message(ctx));
        failed = true;
    }
    if (failed || dst == nullptr) {
        if (dst != nullptr) pdf_drop_document(ctx, dst);
        return Status::error(ErrorCode::IoError, failed ? msg : "无法创建输出 PDF");
    }

    fz_try(ctx) { src = pdf_open_document(ctx, input.string().c_str()); }
    fz_catch(ctx) {
        std::snprintf(msg, sizeof(msg), "%s", fz_caught_message(ctx));
        failed = true;
    }
    if (failed || src == nullptr) {
        if (src != nullptr) pdf_drop_document(ctx, src);
        pdf_drop_document(ctx, dst);
        return Status::error(ErrorCode::IoError, failed ? msg : "无法打开源 PDF");
    }

    int total = 0;
    fz_try(ctx) {
        total = pdf_count_pages(ctx, src);
        // 共用一个 graft map：源页字体/图片只拷一次，避免每页各留一份（否则体积翻倍）
        graft_map = pdf_new_graft_map(ctx, dst);
    }
    fz_catch(ctx) {
        std::snprintf(msg, sizeof(msg), "%s", fz_caught_message(ctx));
        failed = true;
    }

    if (!failed) {
        bool needsCjk = false;
        for (std::size_t i = 0; i < text.size(); ++i) {
            if (static_cast<unsigned char>(text[i]) >= 0x80) { needsCjk = true; break; }
        }
        if (needsCjk) {
            // 中文水印：base14（Helvetica）只覆盖 ASCII，直接画会变成空白/乱码。
            // 先试系统微软雅黑，再退回 MuPDF 内置 CJK 字体。
            fz_try(ctx) { font = fz_new_font_from_file(ctx, "PwCjk", "C:/Windows/Fonts/msyh.ttc", 0, 0); }
            fz_catch(ctx) { font = nullptr; }
            if (font == nullptr) {
                fz_try(ctx) { font = fz_new_cjk_font(ctx, FZ_ADOBE_GB); }
                fz_catch(ctx) { font = nullptr; }
            }
        }
        if (font == nullptr) {
            fz_try(ctx) { font = fz_new_base14_font(ctx, "Helvetica"); }
            fz_catch(ctx) { font = nullptr; }
        }
        if (font == nullptr) failed = true;
    }

    float color[3] = {0.85f, 0.15f, 0.15f};
    // 45° 水印的文字位图缓存：字号相同的页共用一张，整份文档只嵌一份像素数据
    std::vector<TextStampSpec> stamps;

    for (int i = 0; !failed && i < total; ++i) {
        pdf_obj* pageobj = nullptr;
        pdf_obj* res = nullptr;
        fz_buffer* buf = nullptr;
        fz_device* dev = nullptr;
        fz_text* txt = nullptr;
        fz_rect mediabox = fz_empty_rect;
        fz_rect dispbox = fz_empty_rect;   // 显示空间（含 /Rotate、CropBox，y 向下）
        fz_matrix pageCtm = fz_identity;

        // 1) graft 原页：内容、字体、透明蒙版原样搬过来
        fz_try(ctx) {
            pdf_graft_mapped_page(ctx, graft_map, -1, src, i);
            pageobj = pdf_lookup_page_obj(ctx, dst, i);
            pdf_page_obj_transform(ctx, pageobj, &mediabox, &pageCtm);
            dispbox = fz_transform_rect(mediabox, pageCtm);
        }
        fz_catch(ctx) {
            std::snprintf(msg, sizeof(msg), "%s", fz_caught_message(ctx));
            failed = true;
        }
        if (failed) break;

        if (fz_is_empty_rect(dispbox) || fz_is_infinite_rect(dispbox) ||
            dispbox.x1 - dispbox.x0 < 1 || dispbox.y1 - dispbox.y0 < 1) {
            mediabox = fz_make_rect(0, 0, 595, 842);
            dispbox = fz_make_rect(0, 0, 595, 842);
            pageCtm = fz_identity;
        }

        // 2) 把水印画进独立内容流。
        //    水平摆放用 pdf 设备写出真正的文字对象；45° 摆放改用位图（见 renderTextStamp）。
        fz_try(ctx) {
            float w = dispbox.x1 - dispbox.x0;
            float h = dispbox.y1 - dispbox.y0;
            float size = std::max(18.0f, std::min(w, h) * 0.08f);
            if (position == "tile-horizontal" || position == "tile-diagonal") {
                // 铺满时用更小的字号：字号若和单条一样大，一页要放几十条就叠成一片
                size = std::max(16.0f, std::min(w, h) * 0.055f);
            }
            const float tw = measureTextWidth(ctx, font, text, size);
            const std::vector<TextSpot> spots = computeTextSpots(position, dispbox, tw, size);

            bool allDiagonal = !spots.empty();
            for (const TextSpot& spot : spots) {
                if (std::fabs(spot.angle) <= 0.01f) {
                    allDiagonal = false;
                    break;
                }
            }

            if (allDiagonal) {
                // —— 45° 摆放：文字先栅格化成位图，再按图片水印的 cm 旋转贴上去 ——
                const int key = static_cast<int>(std::lround(size * 10.0f));
                const TextStampSpec* spec = nullptr;
                for (const TextStampSpec& s : stamps) {
                    if (s.key == key) {
                        spec = &s;
                        break;
                    }
                }
                if (spec == nullptr) {
                    TextStampSpec created{};
                    created.key = key;
                    created.image = renderTextStamp(ctx, dst, font, text, size, tw, color, &created.w,
                                                    &created.h, &created.baseU, &created.baseV);
                    if (created.image == nullptr) {
                        failed = true;
                    } else {
                        stamps.push_back(created);
                        spec = &stamps.back();
                    }
                }
                if (!failed && spec != nullptr) {
                    res = pdf_new_dict(ctx, dst, 2);
                    pdf_obj* xo = pdf_new_dict(ctx, dst, 1);                    pdf_dict_puts(ctx, xo, "PwTxt", spec->image);
                    pdf_dict_put_drop(ctx, res, PDF_NAME(XObject), xo);

                    pdf_obj* gstate = pdf_new_dict(ctx, dst, 1);
                    pdf_dict_put_real(ctx, gstate, PDF_NAME(ca), opacity);
                    pdf_obj* eg = pdf_new_dict(ctx, dst, 1);
                    pdf_dict_puts(ctx, eg, "GS0", gstate);
                    pdf_dict_put_drop(ctx, res, PDF_NAME(ExtGState), eg);
                    pdf_drop_obj(ctx, gstate);

                    buf = fz_new_buffer(ctx, 512);
                    const fz_matrix inv = fz_invert_matrix(pageCtm);
                    for (const TextSpot& spot : spots) {
                        const float rad = spot.angle * 3.14159265f / 180.0f;
                        const float c = std::cos(rad);
                        const float s = std::sin(rad);
                        // 显示空间 y 向下：图片 x 轴沿文字方向，图片 y 轴沿文字“向下”方向
                        fz_matrix cmDisp = fz_make_matrix(spec->w * c, spec->w * s, -spec->h * s,
                                                          spec->h * c, 0, 0);
                        // 让位图里的文字基线起点正好落在这个 spot 上
                        const float ox = spec->baseU * spec->w * c - spec->baseV * spec->h * s;
                        const float oy = spec->baseU * spec->w * s + spec->baseV * spec->h * c;
                        cmDisp.e = spot.x - ox;
                        cmDisp.f = spot.y - oy;
                        // 折回页面用户空间（y 向上）后再写 cm，/Rotate 页面同样成立
                        const fz_matrix cmUser = fz_concat(cmDisp, inv);
                        fz_append_printf(ctx, buf,
                                         "q /GS0 gs %.4f %.4f %.4f %.4f %.4f %.4f cm /PwTxt Do Q\n",
                                         cmUser.a, cmUser.b, cmUser.c, cmUser.d, cmUser.e, cmUser.f);
                    }
                }
            } else {
                // 水平摆放：直接写 PDF 文字，可选中、可搜索，体积也小
                res = pdf_new_dict(ctx, dst, 2);
                buf = fz_new_buffer(ctx, 2048);
                dev = pdf_new_pdf_device(ctx, dst, pageCtm, res, buf);
                for (const TextSpot& spot : spots) {
                    // topctm 里含 y 翻转，文字矩阵的 y 要取负，否则水印会上下镜像
                    fz_matrix trm = fz_concat(fz_scale(size, -size), fz_translate(spot.x, spot.y));
                    // 一条文字一次 fill：pdf device 不再把多条合并成一条 TJ，
                    // 省得字距调整把相邻水印挤到一起
                    txt = fz_new_text(ctx);
                    fz_show_string(ctx, txt, font, trm, text.c_str(), 0, 0, FZ_BIDI_LTR,
                                   FZ_LANG_UNSET);
                    fz_fill_text(ctx, dev, txt, fz_identity, fz_device_rgb(ctx), color, opacity,
                                 fz_default_color_params);
                    fz_drop_text(ctx, txt);
                    txt = nullptr;
                }
                fz_close_device(ctx, dev);
            }
        }
        fz_catch(ctx) {
            std::snprintf(msg, sizeof(msg), "%s", fz_caught_message(ctx));
            failed = true;
        }
        if (txt != nullptr) { fz_drop_text(ctx, txt); txt = nullptr; }
        if (dev != nullptr) { fz_drop_device(ctx, dev); dev = nullptr; }

        // 3) 把上面的内容流封成 Form XObject：它自带 /Resources，
        //    页面只需多一个 /XObject 引用，不会覆盖页面原有的 /Font、/ExtGState。
        if (!failed) {
            fz_try(ctx) {
                pdf_obj* form = pdf_new_dict(ctx, dst, 6);
                pdf_dict_put(ctx, form, PDF_NAME(Type), PDF_NAME(XObject));
                pdf_dict_put(ctx, form, PDF_NAME(Subtype), PDF_NAME(Form));
                pdf_obj* bbox = pdf_new_array(ctx, dst, 4);
                pdf_array_push_real(ctx, bbox, mediabox.x0);
                pdf_array_push_real(ctx, bbox, mediabox.y0);
                pdf_array_push_real(ctx, bbox, mediabox.x1);
                pdf_array_push_real(ctx, bbox, mediabox.y1);
                pdf_dict_put_drop(ctx, form, PDF_NAME(BBox), bbox);
                pdf_dict_put(ctx, form, PDF_NAME(Resources), res);

                // pdf_add_stream 的 obj 是流字典模板；form 的所有权仍在我们手里
                pdf_obj* xobj = pdf_add_stream(ctx, dst, buf, form, 1);
                pdf_drop_obj(ctx, form);

                // 页面资源里给 Form 取一个不冲突的名字
                pdf_obj* pres = pdf_dict_get(ctx, pageobj, PDF_NAME(Resources));
                if (!pdf_is_dict(ctx, pres)) {
                    pres = pdf_new_dict(ctx, dst, 2);
                    pdf_dict_put_drop(ctx, pageobj, PDF_NAME(Resources), pres);
                    pres = pdf_dict_get(ctx, pageobj, PDF_NAME(Resources));
                }
                pdf_obj* pxo = pdf_dict_gets(ctx, pres, "XObject");
                if (!pdf_is_dict(ctx, pxo)) {
                    pdf_dict_put_drop(ctx, pres, PDF_NAME(XObject), pdf_new_dict(ctx, dst, 2));
                    pxo = pdf_dict_gets(ctx, pres, "XObject");
                }
                char name[32] = "PwWm";
                int suffix = 0;
                while (pdf_dict_gets(ctx, pxo, name) != nullptr) {
                    std::snprintf(name, sizeof(name), "PwWm%d", ++suffix);
                }
                pdf_dict_puts(ctx, pxo, name, xobj);
                pdf_drop_obj(ctx, xobj);

                // 原页内容常常留下未闭合的图形状态（PPT 导出的 PDF 很常见），
                // 水印若直接接在后面会继承残留的 CTM（表现为旋转/跑位）。
                // 用 q 把原内容包起来，Q 之后回到页面初始状态再画水印。
                std::string post = "Q\nq /" + std::string(name) + " Do Q\n";
                pdf_obj* preStream = addOpsStream(ctx, dst, "q\n");
                pdf_obj* wmStream = addOpsStream(ctx, dst, post);

                pdf_obj* contents = pdf_dict_get(ctx, pageobj, PDF_NAME(Contents));
                if (pdf_is_array(ctx, contents)) {
                    pdf_array_insert_drop(ctx, contents, preStream, 0);
                    pdf_array_push_drop(ctx, contents, wmStream);
                } else {
                    pdf_obj* arr = pdf_new_array(ctx, dst, 3);
                    pdf_array_push_drop(ctx, arr, preStream);
                    if (contents != nullptr) pdf_array_push(ctx, arr, contents);
                    pdf_array_push_drop(ctx, arr, wmStream);
                    pdf_dict_put_drop(ctx, pageobj, PDF_NAME(Contents), arr);
                }
            }
            fz_catch(ctx) {
                std::snprintf(msg, sizeof(msg), "%s", fz_caught_message(ctx));
                failed = true;
            }
        }

        if (buf != nullptr) fz_drop_buffer(ctx, buf);
        if (res != nullptr) pdf_drop_obj(ctx, res);
    }

    if (font != nullptr) fz_drop_font(ctx, font);

    if (!failed) {
        // 水印用系统 CJK 字体（微软雅黑 ≈ 19 MB），MuPDF 默认整包嵌入。
        // 按文档里实际用到的字形做子集化，否则输出会比源文件大好几倍。
        fz_try(ctx) { pdf_subset_fonts(ctx, dst, 0, nullptr); }
        fz_catch(ctx) {
            // 子集化失败不影响正确性，只是体积偏大，这里不当作错误
        }
        pdf_write_options opts = writeOptions();
        fz_try(ctx) { pdf_save_document(ctx, dst, output.string().c_str(), &opts); }
        fz_catch(ctx) {
            std::snprintf(msg, sizeof(msg), "%s", fz_caught_message(ctx));
            failed = true;
        }
    }

    if (graft_map != nullptr) pdf_drop_graft_map(ctx, graft_map);
    pdf_drop_document(ctx, src);
    pdf_drop_document(ctx, dst);

    if (failed) return Status::error(ErrorCode::IoError, msg[0] != '\0' ? msg : "水印写入失败");
    return Status::success();
}

// ---------------------------------------------------------------------------
// OCR：把识别出来的词写成"不可见文字层"（文本渲染模式 3 Tr）
// ---------------------------------------------------------------------------

// UTF-8 → Unicode 码位。<oc 只服务 OCR 输出，遇到非法字节跳过即可。
std::vector<std::uint32_t> utf8Codepoints(const std::string& s) {
    std::vector<std::uint32_t> out;
    std::size_t i = 0;
    while (i < s.size()) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        std::uint32_t cp = 0;
        std::size_t extra = 0;
        if (c < 0x80) {
            cp = c;
            extra = 0;
        } else if ((c & 0xE0) == 0xC0) {
            cp = c & 0x1F;
            extra = 1;
        } else if ((c & 0xF0) == 0xE0) {
            cp = c & 0x0F;
            extra = 2;
        } else if ((c & 0xF8) == 0xF0) {
            cp = c & 0x07;
            extra = 3;
        } else {
            ++i;
            continue;
        }
        if (i + extra >= s.size()) break;
        for (std::size_t k = 1; k <= extra; ++k) {
            const unsigned char cc = static_cast<unsigned char>(s[i + k]);
            if ((cc & 0xC0) != 0x80) {
                extra = 0;  // 截断的序列，按单字节处理
                break;
            }
            cp = (cp << 6) | (cc & 0x3F);
        }
        out.push_back(cp);
        i += extra + 1;
    }
    return out;
}

// fz_pixmap → RGBA8888（和 renderPage 同一套合成规则：有 alpha 的按白底合成）
void pixmapToRgba(fz_pixmap* pix, RasterImage* out) {
    if (pix == nullptr || out == nullptr) return;
    int w = pix->w;
    int h = pix->h;
    int n = pix->n;
    int stride = pix->stride;
    out->reset(w, h);
    for (int y = 0; y < h; ++y) {
        const unsigned char* src =
            pix->samples + static_cast<std::size_t>(y) * static_cast<std::size_t>(stride);
        unsigned char* dst =
            out->rgba.data() + static_cast<std::size_t>(y) * static_cast<std::size_t>(w) * 4u;
        for (int x = 0; x < w; ++x) {
            if (n >= 4) {
                int a = src[3];
                dst[0] = static_cast<unsigned char>((src[0] * a + 255 * (255 - a)) / 255);
                dst[1] = static_cast<unsigned char>((src[1] * a + 255 * (255 - a)) / 255);
                dst[2] = static_cast<unsigned char>((src[2] * a + 255 * (255 - a)) / 255);
            } else if (n == 3) {
                dst[0] = src[0];
                dst[1] = src[1];
                dst[2] = src[2];
            } else {
                dst[0] = dst[1] = dst[2] = src[0];
            }
            dst[3] = 255;
            src += n;
            dst += 4;
        }
    }
}

// 给 dst 的第 pageIndex 页追加一个只含文字层的 Form XObject。
// 关键点：Form 的 /Matrix 取 inv(pageCtm)∘[1 0 0 -1 0 H]，于是 form 内部可以直接用
// "显示空间"坐标（原点左上、y 向下），页面 /Rotate 90/180 也能落到正确位置。
// 返回写入的词数，0 表示这页没有可写的内容。
int writeOcrPageLayer(fz_context* ctx, pdf_document* dst, int pageIndex, const OcrPageResult& page,
                      fz_font* font, pdf_obj* fontRef) {
    char msg[512] = {0};
    bool failed = false;
    int written = 0;

    pdf_obj* pageobj = nullptr;
    pdf_obj* res = nullptr;
    pdf_obj* form = nullptr;
    fz_buffer* buf = nullptr;
    fz_rect mediabox = fz_empty_rect;
    fz_rect dispbox = fz_empty_rect;
    fz_matrix pageCtm = fz_identity;

    fz_try(ctx) {
        pageobj = pdf_lookup_page_obj(ctx, dst, pageIndex);
        pdf_page_obj_transform(ctx, pageobj, &mediabox, &pageCtm);
        dispbox = fz_transform_rect(mediabox, pageCtm);
        if (fz_is_empty_rect(dispbox) || fz_is_infinite_rect(dispbox)) {
            mediabox = fz_make_rect(0, 0, 595, 842);
            dispbox = mediabox;
            pageCtm = fz_identity;
        }

        const float dispW = float(dispbox.x1 - dispbox.x0);
        const float dispH = float(dispbox.y1 - dispbox.y0);

        res = pdf_new_dict(ctx, dst, 2);
        pdf_obj* fonts = pdf_new_dict(ctx, dst, 1);
        pdf_dict_puts(ctx, fonts, "PwOcr", fontRef);
        pdf_dict_put_drop(ctx, res, PDF_NAME(Font), fonts);

        buf = fz_new_buffer(ctx, 4096);
        for (const OcrLine& line : page.lines) {
            for (const OcrWord& word : line.words) {
                if (word.text.empty()) continue;
                const double size = std::max(1.0, double(word.y1 - word.y0));

                std::vector<int> gids;
                double naturalWidth = 0.0;
                for (std::uint32_t cp : utf8Codepoints(word.text)) {
                    int gid = 0;
                    fz_try(ctx) { gid = fz_encode_character(ctx, font, cp); }
                    fz_catch(ctx) { gid = 0; }
                    if (gid <= 0 || gid > 0xFFFF) continue;
                    gids.push_back(gid);
                    double adv = 0.0;
                    fz_try(ctx) { adv = fz_advance_glyph(ctx, font, gid, 0); }
                    fz_catch(ctx) { adv = 0.0; }
                    naturalWidth += adv * size;
                }
                if (gids.empty()) continue;

                // 用 Tz 把文字横向拉到 OCR 给的词框宽度，选中高亮才会落在字上
                const double bboxW = word.x1 - word.x0;
                double tz = (naturalWidth > 1.0 && bboxW > 1.0) ? (bboxW / naturalWidth) * 100.0 : 100.0;
                if (tz < 20.0) tz = 20.0;
                if (tz > 500.0) tz = 500.0;

                std::string hex;
                hex.reserve(gids.size() * 4 + 2);
                for (int gid : gids) {
                    char code[8];
                    std::snprintf(code, sizeof(code), "%04X", static_cast<unsigned>(gid));
                    hex += code;
                }

                // form 空间 y 向上，而显示空间 y 向下：基线 = H - (词底 - 0.25 字高)
                const double baseline = double(dispH) - (word.y1 - size * 0.25);
                fz_append_printf(ctx, buf,
                                 "BT /PwOcr %.3f Tf %.3f Tz 3 Tr 1 0 0 1 %.2f %.2f Tm <%s> Tj ET\n",
                                 size, tz, word.x0, baseline, hex.c_str());
                ++written;
            }
        }
        if (written == 0) fz_throw(ctx, FZ_ERROR_GENERIC, "no words");
    }
    fz_catch(ctx) {
        std::snprintf(msg, sizeof(msg), "%s", fz_caught_message(ctx));
        failed = true;
    }

    if (!failed && written > 0) {
        fz_try(ctx) {
            form = pdf_new_dict(ctx, dst, 8);
            pdf_dict_put(ctx, form, PDF_NAME(Type), PDF_NAME(XObject));
            pdf_dict_put(ctx, form, PDF_NAME(Subtype), PDF_NAME(Form));
            pdf_obj* bbox = pdf_new_array(ctx, dst, 4);
            pdf_array_push_real(ctx, bbox, 0);
            pdf_array_push_real(ctx, bbox, 0);
            pdf_array_push_real(ctx, bbox, float(dispbox.x1 - dispbox.x0));
            pdf_array_push_real(ctx, bbox, float(dispbox.y1 - dispbox.y0));
            pdf_dict_put_drop(ctx, form, PDF_NAME(BBox), bbox);
            pdf_dict_put(ctx, form, PDF_NAME(Resources), res);

            // 显示空间 → 用户空间：先翻 y（form 里 y 向上），再套 inv(pageCtm)
            fz_matrix flip = fz_make_matrix(1, 0, 0, -1, 0, float(dispbox.y1 - dispbox.y0));
            fz_matrix inv = fz_invert_matrix(pageCtm);
            fz_matrix matrix = fz_concat(inv, flip);
            pdf_obj* mtx = pdf_new_array(ctx, dst, 6);
            pdf_array_push_real(ctx, mtx, matrix.a);
            pdf_array_push_real(ctx, mtx, matrix.b);
            pdf_array_push_real(ctx, mtx, matrix.c);
            pdf_array_push_real(ctx, mtx, matrix.d);
            pdf_array_push_real(ctx, mtx, matrix.e);
            pdf_array_push_real(ctx, mtx, matrix.f);
            pdf_dict_put_drop(ctx, form, PDF_NAME(Matrix), mtx);

            pdf_obj* xobj = pdf_add_stream(ctx, dst, buf, form, 1);

            // 页面资源里挂上文字层，再把调用追加到 /Contents 末尾
            pdf_obj* pres = pdf_dict_get(ctx, pageobj, PDF_NAME(Resources));
            if (!pdf_is_dict(ctx, pres)) {
                pdf_dict_put_drop(ctx, pageobj, PDF_NAME(Resources), pdf_new_dict(ctx, dst, 2));
                pres = pdf_dict_get(ctx, pageobj, PDF_NAME(Resources));
            }
            pdf_obj* pxo = pdf_dict_gets(ctx, pres, "XObject");
            if (!pdf_is_dict(ctx, pxo)) {
                pdf_dict_put_drop(ctx, pres, PDF_NAME(XObject), pdf_new_dict(ctx, dst, 2));
                pxo = pdf_dict_gets(ctx, pres, "XObject");
            }
            char name[32] = "PwOcrLayer";
            int suffix = 0;
            while (pdf_dict_gets(ctx, pxo, name) != nullptr) {
                std::snprintf(name, sizeof(name), "PwOcrLayer%d", ++suffix);
            }
            pdf_dict_puts(ctx, pxo, name, xobj);
            pdf_drop_obj(ctx, xobj);

            // 原页内容可能留了未闭合的 q，先 Q 收尾再画我们的图层
            std::string post = "Q\nq /" + std::string(name) + " Do Q\n";
            pdf_obj* preStream = addOpsStream(ctx, dst, "q\n");
            pdf_obj* layerStream = addOpsStream(ctx, dst, post);
            pdf_obj* contents = pdf_dict_get(ctx, pageobj, PDF_NAME(Contents));
            if (pdf_is_array(ctx, contents)) {
                pdf_array_insert_drop(ctx, contents, preStream, 0);
                pdf_array_push_drop(ctx, contents, layerStream);
            } else {
                pdf_obj* arr = pdf_new_array(ctx, dst, 3);
                pdf_array_push_drop(ctx, arr, preStream);
                if (contents != nullptr) pdf_array_push(ctx, arr, contents);
                pdf_array_push_drop(ctx, arr, layerStream);
                pdf_dict_put_drop(ctx, pageobj, PDF_NAME(Contents), arr);
            }
        }
        fz_catch(ctx) {
            std::snprintf(msg, sizeof(msg), "%s", fz_caught_message(ctx));
            failed = true;
        }
    }

    if (form != nullptr) pdf_drop_obj(ctx, form);
    if (res != nullptr) pdf_drop_obj(ctx, res);
    if (buf != nullptr) fz_drop_buffer(ctx, buf);
    return failed ? -1 : written;
}

// OCR 主流程：逐页渲染 → 识别 → （可选）graft 原页并把文字层写进去 → （可选）导出纯文本。
Status writeOcr(fz_context* ctx, const fs::path& input, const fs::path& outPdf,
                const fs::path& outTxt, const std::string& language, bool generatePdf,
                bool generateTxt, const std::string& password) {
    auto engine = makeOcrEngine();
    if (!engine) {
        return Status::error(ErrorCode::BackendMissing, ocrCapabilityReport());
    }

    char msg[512] = {0};
    bool failed = false;
    std::string allText;

    fz_document* srcDoc = nullptr;
    pdf_document* src = nullptr;
    pdf_document* dst = nullptr;
    pdf_graft_map* graftMap = nullptr;
    fz_font* font = nullptr;
    pdf_obj* fontRef = nullptr;
    int total = 0;

    fz_try(ctx) {
        srcDoc = fz_open_document(ctx, input.string().c_str());
        if (srcDoc == nullptr) fz_throw(ctx, FZ_ERROR_GENERIC, "无法打开源 PDF");
        if (fz_needs_password(ctx, srcDoc)) {
            if (password.empty()) fz_throw(ctx, FZ_ERROR_GENERIC, "文档已加密，需要密码");
            if (!fz_authenticate_password(ctx, srcDoc, password.c_str())) {
                fz_throw(ctx, FZ_ERROR_GENERIC, "文档已加密，密码错误");
            }
        }
        src = pdf_specifics(ctx, srcDoc);
        if (src == nullptr) fz_throw(ctx, FZ_ERROR_GENERIC, "源文件不是 PDF");
        total = pdf_count_pages(ctx, src);

        if (generatePdf) {
            dst = pdf_create_document(ctx);
            graftMap = pdf_new_graft_map(ctx, dst);
            // 文字层要能装下中文：优先系统微软雅黑，失败退回 MuPDF 内置 CJK 字体
            fz_try(ctx) {
                font = fz_new_font_from_file(ctx, "PwOcrFont", "C:/Windows/Fonts/msyh.ttc", 0, 0);
            }
            fz_catch(ctx) {
                font = nullptr;
            }
            if (font == nullptr) {
                fz_try(ctx) { font = fz_new_cjk_font(ctx, FZ_ADOBE_GB); }
                fz_catch(ctx) { font = nullptr; }
            }
            if (font == nullptr) fz_throw(ctx, FZ_ERROR_GENERIC, "找不到可用的中文字体（msyh.ttc / 内置 CJK）");
            fontRef = pdf_add_cid_font(ctx, dst, font);
        }
    }
    fz_catch(ctx) {
        std::snprintf(msg, sizeof(msg), "%s", fz_caught_message(ctx));
        failed = true;
    }

    for (int i = 0; !failed && i < total; ++i) {
        fz_page* pg = nullptr;
        fz_pixmap* pix = nullptr;
        float zoom = 1.0f;
        double dispWidthPt = 0.0;
        double dispHeightPt = 0.0;
        RasterImage raster;

        fz_try(ctx) {
            pg = fz_load_page(ctx, srcDoc, i);
            fz_rect bounds = fz_bound_page(ctx, pg);
            double wPt = double(bounds.x1 - bounds.x0);
            double hPt = double(bounds.y1 - bounds.y0);
            if (wPt < 1.0) wPt = 1.0;
            if (hPt < 1.0) hPt = 1.0;
            // 长边落到 2200px：扫描件 300dpi 大约就是这个量级，中文小字也够认
            const double longSide = std::max(wPt, hPt);
            zoom = float(std::clamp(2200.0 / longSide, 0.5, 8.0));
            fz_matrix ctm = fz_scale(zoom, zoom);
            pix = fz_new_pixmap_from_page(ctx, pg, ctm, fz_device_rgb(ctx), 0);
            if (pix == nullptr) fz_throw(ctx, FZ_ERROR_GENERIC, "页面渲染失败");
            pixmapToRgba(pix, &raster);
            dispWidthPt = double(pix->w) / double(zoom);
            dispHeightPt = double(pix->h) / double(zoom);
        }
        fz_catch(ctx) {
            std::snprintf(msg, sizeof(msg), "%s", fz_caught_message(ctx));
            failed = true;
        }
        if (pix != nullptr) fz_drop_pixmap(ctx, pix);
        if (pg != nullptr) fz_drop_page(ctx, pg);
        if (failed) break;

        OcrPageResult pageResult;
        pageResult.pageIndex = i;
        Status st = engine->recognize(raster, dispWidthPt, dispHeightPt, language, &pageResult);
        if (!st.ok()) {
            return st;
        }
        allText += pageResult.text;
        if (pageResult.text.empty()) allText.push_back('\n');

        if (generatePdf) {
            fz_try(ctx) {
                pdf_graft_mapped_page(ctx, graftMap, -1, src, i);
                if (writeOcrPageLayer(ctx, dst, i, pageResult, font, fontRef) < 0) {
                    fz_throw(ctx, FZ_ERROR_GENERIC, "文字层写入失败");
                }
            }
            fz_catch(ctx) {
                std::snprintf(msg, sizeof(msg), "%s", fz_caught_message(ctx));
                failed = true;
            }
        }
    }

    if (!failed && generatePdf && dst != nullptr) {
        fz_try(ctx) {
            // 文字层只用到少量字形，做一下子集化，否则会多背一个 19MB 字体
            fz_try(ctx) { pdf_subset_fonts(ctx, dst, 0, nullptr); }
            fz_catch(ctx) { /* 子集化失败只影响体积，不影响正确性 */ }
            pdf_write_options opts = writeOptions();
            pdf_save_document(ctx, dst, outPdf.string().c_str(), &opts);
        }
        fz_catch(ctx) {
            std::snprintf(msg, sizeof(msg), "%s", fz_caught_message(ctx));
            failed = true;
        }
    }

    if (!failed && generateTxt && !outTxt.empty()) {
        std::ofstream ofs(outTxt, std::ios::binary | std::ios::trunc);
        if (!ofs) {
            failed = true;
            std::snprintf(msg, sizeof(msg), "无法写入文本文件: %s", outTxt.string().c_str());
        } else {
            ofs.write(allText.data(), static_cast<std::streamsize>(allText.size()));
        }
    }

    if (fontRef != nullptr) pdf_drop_obj(ctx, fontRef);
    if (font != nullptr) fz_drop_font(ctx, font);
    if (graftMap != nullptr) pdf_drop_graft_map(ctx, graftMap);
    if (dst != nullptr) pdf_drop_document(ctx, dst);
    if (srcDoc != nullptr) fz_drop_document(ctx, srcDoc);

    if (failed) return Status::error(ErrorCode::IoError, msg[0] != '\0' ? msg : "OCR 失败");
    return Status::success();
}

// ---------------------------------------------------------------------------
// 编辑与批注：坐标换算 + 批注 / 超链接
// ---------------------------------------------------------------------------

// 页面 用户空间 ↔ 显示空间 的一对矩阵。显示空间原点在页面左上、y 向下，
// 界面上拖出来的框就是显示空间坐标，写进 PDF 前必须乘 inv。
struct PageTransform {
    fz_matrix ctm = fz_identity;   // 用户 → 显示
    fz_matrix inv = fz_identity;   // 显示 → 用户
    fz_rect mediabox = fz_empty_rect;
    fz_rect dispbox = fz_empty_rect;
};

PageTransform pageTransform(fz_context* ctx, pdf_obj* pageobj) {
    PageTransform t;
    pdf_page_obj_transform(ctx, pageobj, &t.mediabox, &t.ctm);
    t.dispbox = fz_transform_rect(t.mediabox, t.ctm);
    if (fz_is_empty_rect(t.dispbox) || fz_is_infinite_rect(t.dispbox)) {
        t.mediabox = fz_make_rect(0, 0, 595, 842);
        t.ctm = fz_identity;
        t.dispbox = t.mediabox;
    }
    t.inv = fz_invert_matrix(t.ctm);
    return t;
}

fz_rect rectToUser(const PageTransform& t, const RectD& r) {
    fz_point a = fz_transform_point(fz_make_point(r.x0, r.y0), t.inv);
    fz_point b = fz_transform_point(fz_make_point(r.x1, r.y1), t.inv);
    return fz_rect_from_quad(
        fz_make_quad(a.x, a.y, b.x, a.y, a.x, b.y, b.x, b.y));
}

fz_quad quadFromDisp(const PageTransform& t, const RectD& r) {
    fz_point ul = fz_transform_point(fz_make_point(r.x0, r.y0), t.inv);
    fz_point ur = fz_transform_point(fz_make_point(r.x1, r.y0), t.inv);
    fz_point ll = fz_transform_point(fz_make_point(r.x0, r.y1), t.inv);
    fz_point lr = fz_transform_point(fz_make_point(r.x1, r.y1), t.inv);
    return fz_make_quad(ul.x, ul.y, ur.x, ur.y, ll.x, ll.y, lr.x, lr.y);
}

// "#RRGGBB" → 0..1 的三个分量；空串返回 false（调用方用默认色）
bool parseColorHex(const std::string& hex, float out[3]) {
    std::string s = hex;
    if (!s.empty() && s[0] == '#') s.erase(s.begin());
    if (s.size() != 6) return false;
    auto hexVal = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    int v[6];
    for (int i = 0; i < 6; ++i) {
        v[i] = hexVal(s[static_cast<std::size_t>(i)]);
        if (v[i] < 0) return false;
    }
    out[0] = float((v[0] << 4) | v[1]) / 255.0f;
    out[1] = float((v[2] << 4) | v[3]) / 255.0f;
    out[2] = float((v[4] << 4) | v[5]) / 255.0f;
    return true;
}

// 把文字转成 UTF-16BE 的 hex 串。批注外观与替换文字层都用 pdf_add_cjk_font 建的
// “非嵌入 UTF16 CID 字体”，编码就是 UTF-16 码点，不再需要查 glyph id：
// 好处是不往 PDF 里塞 19MB 的字体程序，体积零增量；代价是显示依赖阅读器自带 CJK 字体
// （MuPDF 内置、Acrobat 通常也有）。
std::string utf16HexOf(const std::string& text) {
    std::string hex;
    hex.reserve(text.size() * 4);
    for (std::uint32_t cp : utf8Codepoints(text)) {
        if (cp >= 0x10000) {
            const std::uint32_t v = cp - 0x10000;
            const std::uint32_t hi = 0xD800 + (v >> 10);
            const std::uint32_t lo = 0xDC00 + (v & 0x3FF);
            char code[16];
            std::snprintf(code, sizeof(code), "%04X%04X", static_cast<unsigned>(hi),
                          static_cast<unsigned>(lo));
            hex += code;
        } else {
            char code[8];
            std::snprintf(code, sizeof(code), "%04X", static_cast<unsigned>(cp));
            hex += code;
        }
    }
    return hex;
}

// 粗略估宽（CJK 一个字 = size，ASCII 约 0.55*size）：只用于折行与 Tz 拉伸
double estTextWidth(const std::string& text, double size) {
    double w = 0.0;
    for (std::uint32_t cp : utf8Codepoints(text)) {
        // 0.62 是实测值：MuPDF 内置 CJK 字体里 ASCII 字宽约 0.62em，
        // 用 0.55 会低估宽度，压新文字时溢出到后面的字上。
        w += (cp >= 0x2E80 || cp == 0x3000) ? size : size * 0.62;
    }
    return w;
}

// 给批注建一个"显示空间坐标"的外观（Form XObject），Matrix 用 inv∘flip，
// 于是内容流里可以放心用左上原点、y 向下的坐标（跟 OCR 文字层同一套）。
// 返回 true 表示成功；失败时调用方退回默认外观。
bool setAnnotAppearanceFromDisp(fz_context* ctx, pdf_document* doc, pdf_annot* annot,
                                const PageTransform& t, const RectD& r, fz_buffer* content,
                                pdf_obj* fontRef, const char* fontName, double size) {
    bool ok = true;
    pdf_obj* res = nullptr;
    pdf_obj* appearance = nullptr;
    fz_try(ctx) {
        res = pdf_new_dict(ctx, doc, 2);
        pdf_obj* fonts = pdf_new_dict(ctx, doc, 1);
        pdf_dict_puts(ctx, fonts, fontName, fontRef);
        pdf_dict_put_drop(ctx, res, PDF_NAME(Font), fonts);

        // 本外观的 BBox 是 [0 0 w h] 的局部坐标系（内容里用左上原点、y 向下的局部坐标）。
        // 外观矩阵的坐标系就是 annot 所在页面的用户空间，所以直接：局部(0,0) → 矩形左上角
        // (x0, y1)，并把 y 翻过来。再叠一次页面逆变换会跑位（踩过一次）。
        fz_matrix matrix = fz_make_matrix(1, 0, 0, -1, float(r.x0), float(r.y1));
        fz_rect bbox = fz_make_rect(0, 0, float(r.x1 - r.x0), float(r.y1 - r.y0));
        pdf_set_annot_appearance(ctx, annot, "N", nullptr, matrix, bbox, res, content);
        (void)t;
        (void)size;
        (void)appearance;
    }
    fz_catch(ctx) {
        ok = false;
    }
    if (res != nullptr) pdf_drop_obj(ctx, res);
    return ok;
}

// 把文本批注的内容渲染成一张透明底位图，作为它的外观 /XObject。
// 为什么用图像而不是 PDF 文字：非嵌入 CJK 字体在部分阅读器里画不出来，
// 而嵌入字体又会让文件背 19MB。渲染成图后中文一定能显示，体积也只有几十 KB。
// 批注的 /Contents 里仍然保留原文，搜索与编辑照旧可用。
pdf_obj* makeTextAnnotImage(fz_context* ctx, pdf_document* doc, const RectD& r,
                            const std::vector<std::string>& lines, double size,
                            const float color[3]) {
    const double wPt = std::max(1.0, r.x1 - r.x0);
    const double hPt = std::max(1.0, r.y1 - r.y0);
    // 每 pt 4 像素；再给个总量上限，避免超大批注框渲染出巨图
    double scale = 4.0;
    if (wPt * scale > 2400.0) scale = 2400.0 / wPt;
    if (hPt * scale > 2400.0) scale = std::min(scale, 2400.0 / hPt);
    if (scale < 1.0) scale = 1.0;

    const int pw = std::max(1, static_cast<int>(wPt * scale + 0.5));
    const int ph = std::max(1, static_cast<int>(hPt * scale + 0.5));

    pdf_obj* imgRef = nullptr;
    fz_pixmap* pm = nullptr;
    fz_device* dev = nullptr;
    fz_text* text = nullptr;
    fz_image* img = nullptr;
    fz_font* font = nullptr;

    fz_try(ctx) {
        font = fz_new_cjk_font(ctx, FZ_ADOBE_GB);
        pm = fz_new_pixmap_with_bbox(ctx, fz_device_rgb(ctx), fz_make_irect(0, 0, pw, ph),
                                     nullptr, 0);
        fz_clear_pixmap_with_value(ctx, pm, 255);  // 白底：位图外观不带透明通道，稳妥可读

        dev = fz_new_draw_device(ctx, fz_identity, pm);
        text = fz_new_text(ctx);

        // 用"左上原点、y 向下"的直观坐标摆放；trm 必须把字号也乘进去（MuPDF 的
        // text rendering matrix 是完整的 em 变换，只给缩放会让字缩成 1pt 高）。
        const double padX = size * 0.25;
        const double padY = size * 0.2;
        const double lineHeight = size * 1.25;
        for (std::size_t i = 0; i < lines.size(); ++i) {
            const double baseline = padY + size * 0.85 + i * lineHeight;
            if (baseline > hPt - padY * 0.5) break;
            // draw device 的设备空间本身就是 y 向下，所以 d 取正，字形才是正立的
            const float em = static_cast<float>(scale * size);
            fz_matrix trm = fz_make_matrix(em, 0, 0, em, static_cast<float>(padX * scale),
                                           static_cast<float>(baseline * scale));
            fz_show_string(ctx, text, font, trm, lines[i].c_str(), 0, 0, FZ_BIDI_LTR,
                           FZ_LANG_UNSET);
        }
        fz_fill_text(ctx, dev, text, fz_identity, fz_device_rgb(ctx), color, 1.0f,
                     fz_default_color_params);
        fz_close_device(ctx, dev);

        img = fz_new_image_from_pixmap(ctx, pm, nullptr);
        imgRef = pdf_add_image(ctx, doc, img);
    }
    fz_always(ctx) {
        fz_drop_image(ctx, img);
        fz_drop_text(ctx, text);
        fz_drop_device(ctx, dev);
        fz_drop_pixmap(ctx, pm);
        fz_drop_font(ctx, font);
    }
    fz_catch(ctx) { imgRef = nullptr; }
    return imgRef;
}

// 生成外观：一张铺满批注框的图像。Matrix 同 setAnnotAppearanceFromDisp。
bool setAnnotAppearanceWithImage(fz_context* ctx, pdf_document* doc, pdf_annot* annot,
                                 const RectD& r, pdf_obj* imgRef) {
    if (imgRef == nullptr) return false;
    bool ok = true;
    pdf_obj* res = nullptr;
    fz_buffer* content = nullptr;
    fz_try(ctx) {
        res = pdf_new_dict(ctx, doc, 2);
        pdf_obj* xobjs = pdf_new_dict(ctx, doc, 1);
        pdf_dict_puts(ctx, xobjs, "PwImg", imgRef);
        pdf_dict_put_drop(ctx, res, PDF_NAME(XObject), xobjs);

        const double w = std::max(1.0, r.x1 - r.x0);
        const double h = std::max(1.0, r.y1 - r.y0);
        content = fz_new_buffer(ctx, 128);
        fz_append_printf(ctx, content, "q %.2f 0 0 %.2f 0 0 cm /PwImg Do Q\n", w, h);

        fz_matrix matrix = fz_make_matrix(1, 0, 0, -1, float(r.x0), float(r.y1));
        fz_rect bbox = fz_make_rect(0, 0, float(w), float(h));
        pdf_set_annot_appearance(ctx, annot, "N", nullptr, matrix, bbox, res, content);
    }
    fz_always(ctx) { fz_drop_buffer(ctx, content); }
    fz_catch(ctx) { ok = false; }
    if (res != nullptr) pdf_drop_obj(ctx, res);
    return ok;
}

// 把字体挂进 AcroForm 的 /DR /Font，键名就是 /DA 里引用的名字。
// 文本批注（FreeText）靠它让 MuPDF 合成中文外观，而不必嵌入字体程序。
bool registerAnnotFont(fz_context* ctx, pdf_document* doc, pdf_obj* fontRef, const char* name) {
    bool ok = true;
    fz_try(ctx) {
        pdf_obj* root = pdf_dict_get(ctx, pdf_trailer(ctx, doc), PDF_NAME(Root));
        if (root == nullptr) fz_throw(ctx, FZ_ERROR_GENERIC, "PDF 缺少 /Root");
        pdf_obj* acro = pdf_dict_get(ctx, root, PDF_NAME(AcroForm));
        if (acro == nullptr) {
            acro = pdf_new_dict(ctx, doc, 3);
            pdf_dict_put_drop(ctx, root, PDF_NAME(AcroForm), acro);
        }
        pdf_obj* dr = pdf_dict_get(ctx, acro, PDF_NAME(DR));
        if (dr == nullptr) {
            dr = pdf_new_dict(ctx, doc, 2);
            pdf_dict_put_drop(ctx, acro, PDF_NAME(DR), dr);
        }
        pdf_obj* fonts = pdf_dict_get(ctx, dr, PDF_NAME(Font));
        if (fonts == nullptr) {
            fonts = pdf_new_dict(ctx, doc, 2);
            pdf_dict_put_drop(ctx, dr, PDF_NAME(Font), fonts);
        }
        if (pdf_dict_get(ctx, fonts, pdf_new_name(ctx, name)) == nullptr) {
            pdf_dict_puts_drop(ctx, fonts, name, pdf_keep_obj(ctx, fontRef));
        }
    }
    fz_catch(ctx) { ok = false; }
    return ok;
}

// 批注主流程：打开源 → 逐条加批注 → 另存。原页内容不改，批注是叠加对象。
Status writeAnnots(fz_context* ctx, const fs::path& input, const fs::path& output,
                   const std::vector<AnnotSpec>& annots, const std::string& password) {
    if (annots.empty()) return Status::error(ErrorCode::InvalidArgument, "没有要添加的批注");

    char msg[512] = {0};
    bool failed = false;
    pdf_document* doc = nullptr;
    fz_font* font = nullptr;       // 只在需要自定义外观（文本框）时才建
    pdf_obj* fontRef = nullptr;

    fz_try(ctx) {
        doc = pdf_open_document(ctx, input.string().c_str());
        if (doc == nullptr) fz_throw(ctx, FZ_ERROR_GENERIC, "无法打开源 PDF");
        if (fz_needs_password(ctx, reinterpret_cast<fz_document*>(doc))) {
            if (password.empty()) fz_throw(ctx, FZ_ERROR_GENERIC, "文档已加密，需要密码");
            if (!fz_authenticate_password(ctx, reinterpret_cast<fz_document*>(doc),
                                          password.c_str())) {
                fz_throw(ctx, FZ_ERROR_GENERIC, "文档已加密，密码错误");
            }
        }
    }
    fz_catch(ctx) {
        std::snprintf(msg, sizeof(msg), "%s", fz_caught_message(ctx));
        failed = true;
    }
    if (failed) {
        if (doc != nullptr) pdf_drop_document(ctx, doc);
        return Status::error(ErrorCode::IoError, msg);
    }

    const int pageCount = pdf_count_pages(ctx, doc);
    for (const AnnotSpec& spec : annots) {
        if (failed) break;
        if (spec.page < 0 || spec.page >= pageCount) {
            std::snprintf(msg, sizeof(msg), "第 %d 页越界（共 %d 页）", spec.page + 1, pageCount);
            failed = true;
            break;
        }
        if (spec.kind != AnnotKind::Ink && spec.kind != AnnotKind::Note && spec.rect.empty()) {
            std::snprintf(msg, sizeof(msg), "第 %d 页批注区域为空", spec.page + 1);
            failed = true;
            break;
        }

        pdf_page* page = nullptr;
        pdf_annot* annot = nullptr;
        bool skipUpdate = false;
        fz_try(ctx) {
            page = pdf_load_page(ctx, doc, spec.page);
            pdf_obj* pageobj = pdf_lookup_page_obj(ctx, doc, spec.page);
            PageTransform t = pageTransform(ctx, pageobj);

            enum pdf_annot_type type = PDF_ANNOT_HIGHLIGHT;
            switch (spec.kind) {
                case AnnotKind::Highlight: type = PDF_ANNOT_HIGHLIGHT; break;
                case AnnotKind::Underline: type = PDF_ANNOT_UNDERLINE; break;
                case AnnotKind::StrikeOut: type = PDF_ANNOT_STRIKE_OUT; break;
                case AnnotKind::Squiggly: type = PDF_ANNOT_SQUIGGLY; break;
                case AnnotKind::Note: type = PDF_ANNOT_TEXT; break;
                case AnnotKind::FreeText: type = PDF_ANNOT_FREE_TEXT; break;
                case AnnotKind::Ink: type = PDF_ANNOT_INK; break;
            }
            annot = pdf_create_annot(ctx, page, type);

            // 高亮/下划线/删除线/波浪线只有 QuadPoints，Rect 由 quad 自己推导，
            // 此时调 pdf_set_annot_rect 会被 MuPDF 拒绝（"have no Rect property"）。
            const bool quadKind = spec.kind == AnnotKind::Highlight ||
                                  spec.kind == AnnotKind::Underline ||
                                  spec.kind == AnnotKind::StrikeOut ||
                                  spec.kind == AnnotKind::Squiggly ||
                                  spec.kind == AnnotKind::Ink;  // Ink 只有 InkList，同样没有 Rect
            if (!quadKind) {
                pdf_set_annot_rect(ctx, annot, rectToUser(t, spec.rect));
            }

            switch (spec.kind) {
                case AnnotKind::Highlight:
                case AnnotKind::Underline:
                case AnnotKind::StrikeOut:
                case AnnotKind::Squiggly: {
                    fz_quad q = quadFromDisp(t, spec.rect);
                    pdf_set_annot_quad_points(ctx, annot, 1, &q);
                } break;
                case AnnotKind::Note: {
                    pdf_set_annot_contents(ctx, annot, spec.text.c_str());
                    pdf_set_annot_icon_name(ctx, annot, "Comment");
                } break;
                case AnnotKind::Ink: {
                    // 手绘：所有折线首尾拼成一整串，用 count 数组切段
                    std::vector<int> counts;
                    std::vector<fz_point> pts;
                    for (const auto& path : spec.inkPaths) {
                        if (path.size() < 2) continue;
                        counts.push_back(static_cast<int>(path.size()));
                        for (const PointD& p : path) {
                            pts.push_back(fz_transform_point(fz_make_point(p.x, p.y), t.inv));
                        }
                    }
                    if (!counts.empty()) {
                        pdf_set_annot_ink_list(ctx, annot, static_cast<int>(counts.size()),
                                               counts.data(), pts.data());
                    }
                    if (!spec.colorHex.empty()) {
                        float c[3];
                        if (parseColorHex(spec.colorHex, c)) {
                            pdf_set_annot_color(ctx, annot, 3, c);
                        } else {
                            float def[3] = {1.0f, 0.0f, 0.0f};
                            pdf_set_annot_color(ctx, annot, 3, def);
                        }
                    }
                } break;
                case AnnotKind::FreeText: {
                    // 文本框：默认外观的字体是 Helv，画不出中文；嵌入 CJK 字体又会让文件暴涨。
                    // 所以把文字渲染成透明底位图挂成外观（见 makeTextAnnotImage），
                    // 批注的 /Contents 仍保存原文，搜索与再编辑照旧。
                    const double size =
                        std::max(6.0, std::min(24.0, (spec.rect.y1 - spec.rect.y0) * 0.9));
                    float ftColor[3] = {0.0f, 0.0f, 0.0f};
                    if (!spec.colorHex.empty()) parseColorHex(spec.colorHex, ftColor);

                    const double padX = size * 0.25;
                    const double maxWidth =
                        std::max(size, spec.rect.x1 - spec.rect.x0 - 2 * padX * 2);
                    std::vector<std::string> lines;
                    std::string line;
                    double lineWidth = 0.0;
                    for (std::uint32_t cp : utf8Codepoints(spec.text)) {
                        if (cp == '\n' || cp == '\r') {
                            lines.push_back(line);
                            line.clear();
                            lineWidth = 0.0;
                            continue;
                        }
                        const double adv = (cp >= 0x2E80) ? size : size * 0.55;
                        if (lineWidth + adv > maxWidth && !line.empty()) {
                            lines.push_back(line);
                            line.clear();
                            lineWidth = 0.0;
                        }
                        appendUtf8(&line, static_cast<int>(cp));
                        lineWidth += adv;
                    }
                    if (!line.empty()) lines.push_back(line);

                    pdf_obj* imgRef = nullptr;
                    fz_try(ctx) {
                        imgRef = makeTextAnnotImage(ctx, doc, spec.rect, lines, size, ftColor);
                    }
                    fz_catch(ctx) { imgRef = nullptr; }
                    bool placed = false;
                    if (imgRef != nullptr) {
                        fz_try(ctx) {
                            placed = setAnnotAppearanceWithImage(ctx, doc, annot, spec.rect, imgRef);
                        }
                        fz_catch(ctx) { placed = false; }
                        pdf_drop_obj(ctx, imgRef);
                    }
                    if (placed) {
                        // 外观是我们自己写的，别让 pdf_update_annot 再合成：
                        // 它会把 /DA 字体整块嵌进 PDF（内置 CJK 约 19MB）。
                        skipUpdate = true;
                    } else {
                        pdf_set_annot_default_appearance(ctx, annot, "Helv", 12.0f, 3, ftColor);
                    }
                    pdf_set_annot_contents(ctx, annot, spec.text.c_str());
                } break;
            }

            if (spec.kind != AnnotKind::Ink && !spec.colorHex.empty()) {
                float c[3];
                if (parseColorHex(spec.colorHex, c)) pdf_set_annot_color(ctx, annot, 3, c);
            }
            if (spec.opacity >= 0.0 && spec.opacity <= 1.0) {
                pdf_set_annot_opacity(ctx, annot, static_cast<float>(spec.opacity));
            }
            pdf_set_annot_author(ctx, annot, spec.author.empty() ? "Pdf-Toolkit" : spec.author.c_str());
            if (!skipUpdate) pdf_update_annot(ctx, annot);
        }
        fz_catch(ctx) {
            std::snprintf(msg, sizeof(msg), "%s", fz_caught_message(ctx));
            failed = true;
        }
        if (page != nullptr) pdf_drop_page(ctx, page);
    }

    if (!failed) {
        fz_try(ctx) {
            // 不要在这里调 pdf_subset_fonts：批注用的是“非嵌入 CJK 字体”，
            // 子集化反而会把它（约 19MB 的内置字体程序）整块嵌进产物里。
            pdf_write_options opts = writeOptions();
            pdf_save_document(ctx, doc, output.string().c_str(), &opts);
        }
        fz_catch(ctx) {
            std::snprintf(msg, sizeof(msg), "%s", fz_caught_message(ctx));
            failed = true;
        }
    }

    if (fontRef != nullptr) pdf_drop_obj(ctx, fontRef);
    if (font != nullptr) fz_drop_font(ctx, font);
    pdf_drop_document(ctx, doc);

    if (failed) return Status::error(ErrorCode::IoError, msg[0] != '\0' ? msg : "添加批注失败");
    return Status::success();
}

// 超链接：外链直接给 URL；文档内跳转给 "#page=N"（也认 "第3页" / "P3" / "page 3" / "3"）。
std::string normalizeLinkUri(const std::string& uri) {
    std::string s = uri;  // 只去首尾空白，URL 内部空格原样保留
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.erase(s.begin());
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r' ||
                          s.back() == '\n')) {
        s.pop_back();
    }
    if (s.empty()) return s;
    if (s[0] == '#') return s;  // 已经是 MuPDF 认的写法

    std::string compact;  // 判定页码时允许 "第 3 页" 这种带空格的写法
    for (char c : s) {
        if (c == ' ' || c == '\t') continue;
        compact.push_back(c);
    }

    auto allDigits = [](const std::string& t) {
        if (t.empty()) return false;
        for (char c : t) {
            if (!std::isdigit(static_cast<unsigned char>(c))) return false;
        }
        return true;
    };

    // 纯数字
    if (allDigits(compact)) return "#page=" + compact;

    // "第N页" / "第N"
    const std::string kDi = "\xE7\xAC\xAC";  // 第
    const std::string kYe = "\xE9\xA1\xB5";  // 页
    if (compact.size() > kDi.size() && compact.compare(0, kDi.size(), kDi) == 0) {
        std::string rest = compact.substr(kDi.size());
        if (rest.size() >= kYe.size() &&
            rest.compare(rest.size() - kYe.size(), kYe.size(), kYe) == 0) {
            rest = rest.substr(0, rest.size() - kYe.size());
        }
        if (allDigits(rest)) return "#page=" + rest;
    }

    // "P3" / "Page3" / "P3页"
    if (compact[0] == 'p' || compact[0] == 'P') {
        std::size_t k = 1;
        if (compact.size() >= 4) {
            const std::string head = compact.substr(0, 4);
            if (head == "page" || head == "Page" || head == "PAGE") k = 4;
        }
        std::string rest = compact.substr(k);
        if (rest.size() >= kYe.size() &&
            rest.compare(rest.size() - kYe.size(), kYe.size(), kYe) == 0) {
            rest = rest.substr(0, rest.size() - kYe.size());
        }
        if (allDigits(rest)) return "#page=" + rest;
    }
    return s;
}

Status writeLink(fz_context* ctx, const fs::path& input, const fs::path& output,
                 const LinkSpec& link, const std::string& password) {
    const std::string uri = normalizeLinkUri(link.uri);
    if (uri.empty()) return Status::error(ErrorCode::InvalidArgument, "链接地址为空");
    if (link.rect.empty()) return Status::error(ErrorCode::InvalidArgument, "链接区域为空");

    char msg[512] = {0};
    bool failed = false;
    pdf_document* doc = nullptr;
    fz_try(ctx) {
        doc = pdf_open_document(ctx, input.string().c_str());
        if (doc == nullptr) fz_throw(ctx, FZ_ERROR_GENERIC, "无法打开源 PDF");
        if (fz_needs_password(ctx, reinterpret_cast<fz_document*>(doc))) {
            if (password.empty()) fz_throw(ctx, FZ_ERROR_GENERIC, "文档已加密，需要密码");
            if (!fz_authenticate_password(ctx, reinterpret_cast<fz_document*>(doc),
                                          password.c_str())) {
                fz_throw(ctx, FZ_ERROR_GENERIC, "文档已加密，密码错误");
            }
        }
        const int pageCount = pdf_count_pages(ctx, doc);
        if (link.page < 0 || link.page >= pageCount) {
            fz_throw(ctx, FZ_ERROR_GENERIC, "页号越界");
        }
    }
    fz_catch(ctx) {
        std::snprintf(msg, sizeof(msg), "%s", fz_caught_message(ctx));
        failed = true;
    }
    if (failed) {
        if (doc != nullptr) pdf_drop_document(ctx, doc);
        return Status::error(ErrorCode::InvalidArgument, msg);
    }

    pdf_page* page = nullptr;
    fz_try(ctx) {
        page = pdf_load_page(ctx, doc, link.page);
        PageTransform t = pageTransform(ctx, pdf_lookup_page_obj(ctx, doc, link.page));
        pdf_create_link(ctx, page, rectToUser(t, link.rect), uri.c_str());
        pdf_write_options opts = writeOptions();
        pdf_save_document(ctx, doc, output.string().c_str(), &opts);
    }
    fz_catch(ctx) {
        std::snprintf(msg, sizeof(msg), "%s", fz_caught_message(ctx));
        failed = true;
    }
    if (page != nullptr) pdf_drop_page(ctx, page);
    pdf_drop_document(ctx, doc);

    if (failed) return Status::error(ErrorCode::IoError, msg[0] != '\0' ? msg : "添加链接失败");
    return Status::success();
}

// ---------------------------------------------------------------------------
// 表单：读 / 填 / 建
// ---------------------------------------------------------------------------

// 统一的打开（含密码校验）。失败时抛 MuPDF 异常，调用方用 fz_catch 接。
pdf_document* openPdfForEdit(fz_context* ctx, const fs::path& input, const std::string& password) {
    pdf_document* doc = pdf_open_document(ctx, input.string().c_str());
    if (doc == nullptr) fz_throw(ctx, FZ_ERROR_GENERIC, "无法打开源 PDF");
    fz_document* fzdoc = reinterpret_cast<fz_document*>(doc);
    if (fz_needs_password(ctx, fzdoc)) {
        if (password.empty()) {
            fz_drop_document(ctx, fzdoc);
            fz_throw(ctx, FZ_ERROR_GENERIC, "文档已加密，需要密码");
        }
        if (!fz_authenticate_password(ctx, fzdoc, password.c_str())) {
            fz_drop_document(ctx, fzdoc);
            fz_throw(ctx, FZ_ERROR_GENERIC, "文档已加密，密码错误");
        }
    }
    return doc;
}

FieldKind fieldKindFromWidget(enum pdf_widget_type type) {
    switch (type) {
        case PDF_WIDGET_TYPE_TEXT: return FieldKind::Text;
        case PDF_WIDGET_TYPE_CHECKBOX:
        case PDF_WIDGET_TYPE_RADIOBUTTON: return FieldKind::Checkbox;
        case PDF_WIDGET_TYPE_COMBOBOX:
        case PDF_WIDGET_TYPE_LISTBOX: return FieldKind::Choice;
        case PDF_WIDGET_TYPE_SIGNATURE: return FieldKind::Signature;
        default: return FieldKind::Unknown;
    }
}

fz_rect dispRectFromUser(const PageTransform& t, fz_rect r) {
    return fz_transform_rect(r, t.ctm);
}

RectD rectFromFz(fz_rect r) {
    RectD out;
    out.x0 = r.x0;
    out.y0 = r.y0;
    out.x1 = r.x1;
    out.y1 = r.y1;
    return out;
}

// 逐页遍历 widget，把字段信息收出来
Result<std::vector<FormField>> readFormFields(fz_context* ctx, const fs::path& input,
                                              const std::string& password) {
    char msg[512] = {0};
    bool failed = false;
    pdf_document* doc = nullptr;
    std::vector<FormField> out;

    fz_try(ctx) { doc = openPdfForEdit(ctx, input, password); }
    fz_catch(ctx) {
        std::snprintf(msg, sizeof(msg), "%s", fz_caught_message(ctx));
        failed = true;
    }
    if (failed) return Status::error(ErrorCode::IoError, msg);

    const int pageCount = pdf_count_pages(ctx, doc);
    for (int i = 0; i < pageCount && !failed; ++i) {
        pdf_page* page = nullptr;
        fz_try(ctx) {
            page = pdf_load_page(ctx, doc, i);
            PageTransform t = pageTransform(ctx, pdf_lookup_page_obj(ctx, doc, i));
            pdf_annot* widget = pdf_first_widget(ctx, page);
            while (widget != nullptr) {
                pdf_obj* obj = pdf_annot_obj(ctx, widget);
                FormField f;
                f.page = i;
                f.kind = fieldKindFromWidget(pdf_widget_type(ctx, widget));

                char* name = pdf_load_field_name(ctx, obj);
                if (name != nullptr) {
                    f.name = name;
                    fz_free(ctx, name);
                }
                if (f.name.empty()) f.name = "(未命名字段)";

                const char* value = pdf_field_value(ctx, obj);
                f.value = value != nullptr ? value : "";
                f.readOnly = pdf_widget_is_readonly(ctx, widget) != 0;
                f.rect = rectFromFz(dispRectFromUser(t, pdf_bound_widget(ctx, widget)));

                if (f.kind == FieldKind::Choice) {
                    int n = pdf_choice_field_option_count(ctx, obj);
                    for (int k = 0; k < n; ++k) {
                        const char* opt = pdf_choice_field_option(ctx, obj, 0, k);
                        if (opt != nullptr) f.options.push_back(opt);
                    }
                }
                out.push_back(f);
                widget = pdf_next_widget(ctx, widget);
            }
        }
        fz_catch(ctx) {
            std::snprintf(msg, sizeof(msg), "%s", fz_caught_message(ctx));
            failed = true;
        }
        if (page != nullptr) pdf_drop_page(ctx, page);
    }

    pdf_drop_document(ctx, doc);
    if (failed) return Status::error(ErrorCode::IoError, msg);
    return out;
}

// 勾选框的 "开 / 关" 文本规整
bool isOnValue(const std::string& v) {
    std::string s;
    for (char c : v) s.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    return s == "1" || s == "yes" || s == "on" || s == "true" || s == "checked";
}

Status writeFormFill(fz_context* ctx, const fs::path& input, const fs::path& output,
                     const std::vector<FieldValue>& values, const std::string& password) {
    if (values.empty()) return Status::error(ErrorCode::InvalidArgument, "没有填写任何字段");

    char msg[512] = {0};
    bool failed = false;
    pdf_document* doc = nullptr;
    int matched = 0;

    fz_try(ctx) { doc = openPdfForEdit(ctx, input, password); }
    fz_catch(ctx) {
        std::snprintf(msg, sizeof(msg), "%s", fz_caught_message(ctx));
        failed = true;
    }
    if (failed) return Status::error(ErrorCode::IoError, msg);

    const int pageCount = pdf_count_pages(ctx, doc);
    for (int i = 0; i < pageCount && !failed; ++i) {
        pdf_page* page = nullptr;
        fz_try(ctx) {
            page = pdf_load_page(ctx, doc, i);
            pdf_annot* widget = pdf_first_widget(ctx, page);
            while (widget != nullptr) {
                pdf_annot* next = pdf_next_widget(ctx, widget);
                pdf_obj* obj = pdf_annot_obj(ctx, widget);

                char* name = pdf_load_field_name(ctx, obj);
                std::string fieldName = name != nullptr ? name : "";
                if (name != nullptr) fz_free(ctx, name);

                for (const FieldValue& item : values) {
                    if (fieldName != item.name) continue;
                    const enum pdf_widget_type wt = pdf_widget_type(ctx, widget);
                    int changed = 0;
                    if (wt == PDF_WIDGET_TYPE_CHECKBOX || wt == PDF_WIDGET_TYPE_RADIOBUTTON) {
                        changed = pdf_set_annot_field_value(ctx, doc, widget,
                                                            isOnValue(item.value) ? "1" : "0", 1);
                    } else if (wt == PDF_WIDGET_TYPE_COMBOBOX || wt == PDF_WIDGET_TYPE_LISTBOX) {
                        changed = pdf_set_choice_field_value(ctx, widget, item.value.c_str());
                    } else if (wt == PDF_WIDGET_TYPE_TEXT) {
                        changed = pdf_set_text_field_value(ctx, widget, item.value.c_str());
                    } else if (wt == PDF_WIDGET_TYPE_SIGNATURE) {
                        // 签名域不在这里填
                        changed = 0;
                    } else {
                        changed = pdf_set_annot_field_value(ctx, doc, widget, item.value.c_str(), 1);
                    }
                    (void)changed;
                    pdf_update_widget(ctx, widget);
                    ++matched;
                    break;
                }
                widget = next;
            }
        }
        fz_catch(ctx) {
            std::snprintf(msg, sizeof(msg), "%s", fz_caught_message(ctx));
            failed = true;
        }
        if (page != nullptr) pdf_drop_page(ctx, page);
    }

    if (!failed && matched == 0) {
        failed = true;
        std::snprintf(msg, sizeof(msg), "文档里没有这些字段名，一个都没填上");
    }

    if (!failed) {
        fz_try(ctx) {
            pdf_write_options opts = writeOptions();
            pdf_save_document(ctx, doc, output.string().c_str(), &opts);
        }
        fz_catch(ctx) {
            std::snprintf(msg, sizeof(msg), "%s", fz_caught_message(ctx));
            failed = true;
        }
    }

    pdf_drop_document(ctx, doc);
    if (failed) return Status::error(ErrorCode::IoError, msg);
    return Status::success();
}

// 一个 widget 的"空白外观"：白底 + 灰框，可选把默认值画进去（用 CJK 字体，中文才显示）
pdf_obj* makeWidgetAppearance(fz_context* ctx, pdf_document* doc, fz_rect urect, const char* fontName,
                              pdf_obj* fontRef, const std::string& text) {
    pdf_obj* form = nullptr;
    fz_buffer* buf = nullptr;
    fz_try(ctx) {
        const double w = std::max(1.0, double(urect.x1 - urect.x0));
        const double h = std::max(1.0, double(urect.y1 - urect.y0));
        buf = fz_new_buffer(ctx, 256);
        fz_append_printf(ctx, buf,
                         "q 1 1 1 rg 0 0 %.2f %.2f re f 0.5 0.5 0.5 RG 0.8 w 0.4 0.4 %.2f %.2f re S Q\n",
                         w, h, std::max(0.0, w - 0.8), std::max(0.0, h - 0.8));
        const std::string hex = utf16HexOf(text);
        if (!hex.empty()) {
            const double size = std::max(4.0, std::min(12.0, h * 0.7));
            // 字体是非嵌入 CJK（UTF16 编码），所以文字必须写成 <hex> 而不是 (literal)
            fz_append_printf(ctx, buf,
                             "q 0 0 %.2f %.2f re W n BT /%s %.2f Tf 0 g 2 %.2f Td <%s> Tj ET Q\n",
                             w, h, fontName, size, size * 0.3, hex.c_str());
        }
        form = pdf_new_dict(ctx, doc, 8);
        pdf_dict_put(ctx, form, PDF_NAME(Type), PDF_NAME(XObject));
        pdf_dict_put(ctx, form, PDF_NAME(Subtype), PDF_NAME(Form));
        pdf_obj* bbox = pdf_new_array(ctx, doc, 4);
        pdf_array_push_real(ctx, bbox, 0);
        pdf_array_push_real(ctx, bbox, 0);
        pdf_array_push_real(ctx, bbox, double(urect.x1 - urect.x0));
        pdf_array_push_real(ctx, bbox, double(urect.y1 - urect.y0));
        pdf_dict_put_drop(ctx, form, PDF_NAME(BBox), bbox);
        pdf_obj* res = pdf_new_dict(ctx, doc, 2);
        if (fontRef != nullptr) {
            pdf_obj* fonts = pdf_new_dict(ctx, doc, 1);
            pdf_dict_puts(ctx, fonts, fontName, fontRef);
            pdf_dict_put_drop(ctx, res, PDF_NAME(Font), fonts);
        }
        pdf_dict_put_drop(ctx, form, PDF_NAME(Resources), res);
        return pdf_add_stream(ctx, doc, buf, form, 0);
    }
    fz_catch(ctx) { form = nullptr; }
    if (buf != nullptr) fz_drop_buffer(ctx, buf);
    return form;
}

Status writeFormCreate(fz_context* ctx, const fs::path& input, const fs::path& output,
                       const std::vector<FieldSpec>& fields, const std::string& password) {
    if (fields.empty()) return Status::error(ErrorCode::InvalidArgument, "没有要创建的字段");

    char msg[512] = {0};
    bool failed = false;
    pdf_document* doc = nullptr;
    fz_font* font = nullptr;
    pdf_obj* fontRef = nullptr;
    int created = 0;

    fz_try(ctx) {
        doc = openPdfForEdit(ctx, input, password);
        // 字段的 /DA 要能写中文，但嵌入字体动辄 20MB：用 MuPDF 的非嵌入 UTF16 CJK 字体。
        fz_try(ctx) { font = fz_new_cjk_font(ctx, FZ_ADOBE_GB); }
        fz_catch(ctx) { font = nullptr; }
        if (font != nullptr) {
            fz_try(ctx) { fontRef = pdf_add_cjk_font(ctx, doc, font, FZ_ADOBE_GB, 0, 0); }
            fz_catch(ctx) { fontRef = nullptr; }
        }

        pdf_obj* root = pdf_dict_get(ctx, pdf_trailer(ctx, doc), PDF_NAME(Root));
        pdf_obj* acro = pdf_dict_get(ctx, root, PDF_NAME(AcroForm));
        if (!pdf_is_dict(ctx, acro)) {
            acro = pdf_new_dict(ctx, doc, 6);
            pdf_dict_put_drop(ctx, root, PDF_NAME(AcroForm), acro);
            acro = pdf_dict_get(ctx, root, PDF_NAME(AcroForm));
        }
        pdf_obj* acroFields = pdf_dict_get(ctx, acro, PDF_NAME(Fields));
        if (!pdf_is_array(ctx, acroFields)) {
            acroFields = pdf_new_array(ctx, doc, 4);
            pdf_dict_put_drop(ctx, acro, PDF_NAME(Fields), acroFields);
            acroFields = pdf_dict_get(ctx, acro, PDF_NAME(Fields));
        }
        const char* daName = "PwForm";
        pdf_obj* dr = pdf_dict_get(ctx, acro, PDF_NAME(DR));
        if (!pdf_is_dict(ctx, dr)) {
            dr = pdf_new_dict(ctx, doc, 4);
            pdf_dict_put_drop(ctx, acro, PDF_NAME(DR), dr);
            dr = pdf_dict_get(ctx, acro, PDF_NAME(DR));
        }
        pdf_obj* drFonts = pdf_dict_get(ctx, dr, PDF_NAME(Font));
        if (!pdf_is_dict(ctx, drFonts)) {
            drFonts = pdf_new_dict(ctx, doc, 4);
            pdf_dict_put_drop(ctx, dr, PDF_NAME(Font), drFonts);
            drFonts = pdf_dict_get(ctx, dr, PDF_NAME(Font));
        }
        if (fontRef != nullptr) pdf_dict_puts(ctx, drFonts, daName, fontRef);
        char da[64];
        std::snprintf(da, sizeof(da), "/%s 10 Tf 0 g", daName);
        pdf_dict_put_text_string(ctx, acro, PDF_NAME(DA), da);
        pdf_dict_put_int(ctx, acro, PDF_NAME(SigFlags), 3);

        const int pageCount = pdf_count_pages(ctx, doc);
        for (const FieldSpec& spec : fields) {
            if (spec.page < 0 || spec.page >= pageCount) {
                fz_throw(ctx, FZ_ERROR_GENERIC, "新建字段的页号越界");
            }
            if (spec.rect.empty()) fz_throw(ctx, FZ_ERROR_GENERIC, "新建字段的区域为空");
            if (spec.name.empty()) fz_throw(ctx, FZ_ERROR_GENERIC, "字段名不能为空");

            pdf_obj* pageobj = pdf_lookup_page_obj(ctx, doc, spec.page);
            PageTransform t = pageTransform(ctx, pageobj);
            fz_rect urect = rectToUser(t, spec.rect);

            pdf_obj* w = pdf_new_dict(ctx, doc, 14);
            pdf_dict_put(ctx, w, PDF_NAME(Type), PDF_NAME(Annot));
            pdf_dict_put(ctx, w, PDF_NAME(Subtype), PDF_NAME(Widget));
            switch (spec.kind) {
                case FieldKind::Checkbox:
                    pdf_dict_put(ctx, w, PDF_NAME(FT), PDF_NAME(Btn));
                    {
                        pdf_obj* offName = pdf_new_name(ctx, "Off");
                        pdf_dict_put(ctx, w, PDF_NAME(V), offName);
                        pdf_drop_obj(ctx, offName);
                    }
                    break;
                case FieldKind::Choice: {
                    pdf_dict_put(ctx, w, PDF_NAME(FT), PDF_NAME(Ch));
                    pdf_obj* opt = pdf_new_array(ctx, doc, static_cast<int>(
                        spec.options.size() > 1 ? spec.options.size() : 1));
                    if (!spec.options.empty()) {
                        for (const std::string& o : spec.options) {
                            pdf_array_push_text_string(ctx, opt, o.c_str());
                        }
                        pdf_dict_put_text_string(ctx, w, PDF_NAME(V),
                                                 spec.options.front().c_str());
                    }
                    pdf_dict_put_drop(ctx, w, PDF_NAME(Opt), opt);
                } break;
                case FieldKind::Signature:
                    pdf_dict_put(ctx, w, PDF_NAME(FT), PDF_NAME(Sig));
                    break;
                default:
                    pdf_dict_put(ctx, w, PDF_NAME(FT), PDF_NAME(Tx));
                    break;
            }
            pdf_dict_put_text_string(ctx, w, PDF_NAME(T), spec.name.c_str());
            pdf_dict_put_int(ctx, w, PDF_NAME(F), 4);  // Print
            pdf_dict_put_text_string(ctx, w, PDF_NAME(DA), da);

            pdf_obj* rectArr = pdf_new_array(ctx, doc, 4);
            pdf_array_push_real(ctx, rectArr, double(urect.x0));
            pdf_array_push_real(ctx, rectArr, double(urect.y0));
            pdf_array_push_real(ctx, rectArr, double(urect.x1));
            pdf_array_push_real(ctx, rectArr, double(urect.y1));
            pdf_dict_put_drop(ctx, w, PDF_NAME(Rect), rectArr);
            pdf_dict_put(ctx, w, PDF_NAME(P), pageobj);

            // 外观：勾选框给两态，其余给一张白底框
            pdf_obj* ap = pdf_new_dict(ctx, doc, 2);
            if (spec.kind == FieldKind::Checkbox) {
                pdf_obj* states = pdf_new_dict(ctx, doc, 2);
                pdf_obj* on = makeWidgetAppearance(ctx, doc, urect, daName, fontRef, "X");
                pdf_obj* off = makeWidgetAppearance(ctx, doc, urect, daName, fontRef, "");
                if (off != nullptr) pdf_dict_put_drop(ctx, states, PDF_NAME(Off), off);
                if (on != nullptr) pdf_dict_put_drop(ctx, states, PDF_NAME(Yes), on);
                pdf_dict_put_drop(ctx, ap, PDF_NAME(N), states);
                pdf_obj* as = pdf_new_name(ctx, "Off");
                pdf_dict_put(ctx, w, PDF_NAME(AS), as);
                pdf_drop_obj(ctx, as);
                pdf_obj* v = pdf_new_name(ctx, "Off");
                pdf_dict_put(ctx, w, PDF_NAME(V), v);
                pdf_drop_obj(ctx, v);
            } else {
                pdf_obj* on = makeWidgetAppearance(ctx, doc, urect, daName, fontRef,
                                                   spec.kind == FieldKind::Choice && !spec.options.empty()
                                                       ? spec.options.front()
                                                       : std::string());
                if (on != nullptr) pdf_dict_put_drop(ctx, ap, PDF_NAME(N), on);
            }
            pdf_dict_put_drop(ctx, w, PDF_NAME(AP), ap);

            pdf_obj* wref = pdf_add_object_drop(ctx, doc, w);

            pdf_obj* annots = pdf_dict_get(ctx, pageobj, PDF_NAME(Annots));
            if (!pdf_is_array(ctx, annots)) {
                annots = pdf_new_array(ctx, doc, 2);
                pdf_dict_put_drop(ctx, pageobj, PDF_NAME(Annots), annots);
                annots = pdf_dict_get(ctx, pageobj, PDF_NAME(Annots));
            }
            pdf_array_push(ctx, annots, wref);
            pdf_array_push(ctx, acroFields, wref);
            ++created;
        }

        // 让 MuPDF 逐页重建 widget，把刚建的字段认进来
        for (int i = 0; i < pageCount; ++i) {
            pdf_page* page = nullptr;
            fz_try(ctx) {
                page = pdf_load_page(ctx, doc, i);
                pdf_annot* widget = pdf_first_widget(ctx, page);
                while (widget != nullptr) {
                    pdf_update_widget(ctx, widget);
                    widget = pdf_next_widget(ctx, widget);
                }
            }
            fz_catch(ctx) { /* 重建外观失败不致命 */ }
            if (page != nullptr) pdf_drop_page(ctx, page);
        }

        pdf_write_options opts = writeOptions();
        pdf_save_document(ctx, doc, output.string().c_str(), &opts);
    }
    fz_catch(ctx) {
        std::snprintf(msg, sizeof(msg), "%s", fz_caught_message(ctx));
        failed = true;
    }

    if (fontRef != nullptr) pdf_drop_obj(ctx, fontRef);
    if (font != nullptr) fz_drop_font(ctx, font);
    if (doc != nullptr) pdf_drop_document(ctx, doc);

    if (failed) return Status::error(ErrorCode::IoError, msg);
    if (created == 0) return Status::error(ErrorCode::InvalidArgument, "没有创建任何字段");
    return Status::success();
}

// ---------------------------------------------------------------------------
// 文本查找：把每页的字符按行拼成一串，再用码点序列匹配，命中框取并集
// -------------------------------------------------------------------

struct SearchCell {
    std::uint32_t cp = 0;
    fz_rect rect = fz_empty_rect;
};

std::uint32_t foldAscii(std::uint32_t cp) {
    if (cp >= 'A' && cp <= 'Z') return cp + ('a' - 'A');
    return cp;
}

std::vector<TextHit> searchTextInDoc(fz_context* ctx, pdf_document* doc, const std::string& needle,
                                     int onlyPage, bool caseSensitive) {
    std::vector<TextHit> hits;
    if (needle.empty()) return hits;

    std::vector<std::uint32_t> need = utf8Codepoints(needle);
    if (!caseSensitive) {
        for (std::uint32_t& cp : need) cp = foldAscii(cp);
    }
    if (need.empty()) return hits;

    const int pageCount = pdf_count_pages(ctx, doc);
    const int first = onlyPage >= 0 ? onlyPage : 0;
    const int last = onlyPage >= 0 ? std::min(onlyPage, pageCount - 1) : pageCount - 1;

    for (int i = first; i <= last; ++i) {
        fz_page* pg = nullptr;
        fz_stext_page* stext = nullptr;
        fz_try(ctx) {
            pg = fz_load_page(ctx, reinterpret_cast<fz_document*>(doc), i);
            stext = fz_new_stext_page_from_page(ctx, pg, nullptr);
        }
        fz_catch(ctx) { stext = nullptr; }
        if (pg != nullptr) fz_drop_page(ctx, pg);
        if (stext == nullptr) continue;

        PageTransform t = pageTransform(ctx, pdf_lookup_page_obj(ctx, doc, i));
        std::vector<SearchCell> cells;

        fz_try(ctx) {
            for (fz_stext_block* block = stext->first_block; block != nullptr; block = block->next) {
                if (block->type != FZ_STEXT_BLOCK_TEXT) continue;
                for (fz_stext_line* line = block->u.t.first_line; line != nullptr; line = line->next) {
                    // 行与行之间插一个换行，避免跨行误命中
                    if (!cells.empty()) {
                        SearchCell nl;
                        nl.cp = '\n';
                        nl.rect = fz_empty_rect;
                        cells.push_back(nl);
                    }
                    fz_rect prev = fz_empty_rect;
                    for (fz_stext_char* ch = line->first_char; ch != nullptr; ch = ch->next) {
                        if (ch->c <= 0) continue;
                        // 注意：stext 的坐标已经是"显示空间"（fz_run_page_contents 用的 page ctm
                        // 已把 y 翻过来），这里不能再乘 t.ctm，否则会二次翻转、位置跑到对侧。
                        fz_rect dr = fz_rect_from_quad(ch->quad);
                        if (!fz_is_empty_rect(prev)) {
                            const double gap = double(dr.x0) - double(prev.x1);
                            if (gap > double(ch->size) * 0.28 && gap < double(ch->size) * 3.0) {
                                SearchCell sp;
                                sp.cp = ' ';
                                sp.rect = fz_make_rect(prev.x1, dr.y0, dr.x0, dr.y1);
                                cells.push_back(sp);
                            }
                        }
                        SearchCell c;
                        c.cp = caseSensitive
                                   ? static_cast<std::uint32_t>(ch->c)
                                   : foldAscii(static_cast<std::uint32_t>(ch->c));
                        c.rect = dr;
                        cells.push_back(c);
                        prev = dr;
                    }
                }
            }
        }
        fz_catch(ctx) {
            fz_drop_stext_page(ctx, stext);
            continue;
        }
        fz_drop_stext_page(ctx, stext);

        if (cells.empty() || cells.size() < need.size()) continue;
        for (std::size_t at = 0; at + need.size() <= cells.size(); ++at) {
            bool match = true;
            for (std::size_t k = 0; k < need.size(); ++k) {
                if (cells[at + k].cp != need[k]) {
                    match = false;
                    break;
                }
            }
            if (!match) continue;
            fz_rect box = fz_empty_rect;
            std::string matched;
            for (std::size_t k = 0; k < need.size(); ++k) {
                const fz_rect& r = cells[at + k].rect;
                if (!fz_is_empty_rect(r)) box = fz_is_empty_rect(box) ? r : fz_union_rect(box, r);
                appendUtf8(&matched, static_cast<int>(cells[at + k].cp));
            }
            if (fz_is_empty_rect(box)) continue;
            TextHit hit;
            hit.page = i;
            hit.rect = rectFromFz(box);
            hit.text = matched;
            hits.push_back(hit);
            at += need.size() - 1;
        }
    }
    return hits;
}

Result<std::vector<TextHit>> readFindText(fz_context* ctx, const fs::path& input,
                                          const std::string& needle, int page,
                                          const std::string& password) {
    if (needle.empty()) return Status::error(ErrorCode::InvalidArgument, "查找内容为空");

    char msg[512] = {0};
    bool failed = false;
    pdf_document* doc = nullptr;
    fz_try(ctx) { doc = openPdfForEdit(ctx, input, password); }
    fz_catch(ctx) {
        std::snprintf(msg, sizeof(msg), "%s", fz_caught_message(ctx));
        failed = true;
    }
    if (failed) return Status::error(ErrorCode::IoError, msg);

    std::vector<TextHit> hits;
    fz_try(ctx) { hits = searchTextInDoc(ctx, doc, needle, page, false); }
    fz_catch(ctx) {
        std::snprintf(msg, sizeof(msg), "%s", fz_caught_message(ctx));
        failed = true;
    }
    pdf_drop_document(ctx, doc);
    if (failed) return Status::error(ErrorCode::IoError, msg);
    return hits;
}

// ---------------------------------------------------------------------------
// 取"字符中心落在矩形里"的文字：预览框一段文字，用它把原文带出来
// -------------------------------------------------------------------

std::string textInRectOfDoc(fz_context* ctx, pdf_document* doc, int pageIndex, const RectD& rect) {
    std::string text;
    const int pageCount = pdf_count_pages(ctx, doc);
    if (pageIndex < 0 || pageIndex >= pageCount) return text;

    fz_page* pg = nullptr;
    fz_stext_page* stext = nullptr;
    fz_try(ctx) {
        pg = fz_load_page(ctx, reinterpret_cast<fz_document*>(doc), pageIndex);
        stext = fz_new_stext_page_from_page(ctx, pg, nullptr);
    }
    fz_catch(ctx) { stext = nullptr; }
    if (pg != nullptr) fz_drop_page(ctx, pg);
    if (stext == nullptr) return text;

    fz_try(ctx) {
        for (fz_stext_block* block = stext->first_block; block != nullptr; block = block->next) {
            if (block->type != FZ_STEXT_BLOCK_TEXT) continue;
            for (fz_stext_line* line = block->u.t.first_line; line != nullptr; line = line->next) {
                std::string lineText;
                fz_rect prev = fz_empty_rect;
                for (fz_stext_char* ch = line->first_char; ch != nullptr; ch = ch->next) {
                    if (ch->c <= 0) continue;
                    // stext 的 quad 已经是显示空间（和 searchTextInDoc 同理，不能再乘 ctm）
                    const fz_rect dr = fz_rect_from_quad(ch->quad);
                    const double cx = (double(dr.x0) + double(dr.x1)) * 0.5;
                    const double cy = (double(dr.y0) + double(dr.y1)) * 0.5;
                    if (cx < rect.x0 || cx > rect.x1 || cy < rect.y0 || cy > rect.y1) continue;
                    if (!fz_is_empty_rect(prev)) {
                        const double gap = double(dr.x0) - double(prev.x1);
                        if (gap > double(ch->size) * 0.28) lineText.push_back(' ');
                    }
                    appendUtf8(&lineText, ch->c);
                    prev = dr;
                }
                if (lineText.empty()) continue;  // 只有部分文字落进框，空行不往结果里塞
                if (!text.empty()) text.push_back('\n');
                text += lineText;
            }
        }
    }
    fz_catch(ctx) {
        fz_drop_stext_page(ctx, stext);
        return std::string();
    }
    fz_drop_stext_page(ctx, stext);
    return text;
}

Result<std::string> readTextInRect(fz_context* ctx, const fs::path& input, int page,
                                   const RectD& rect, const std::string& password) {
    if (rect.empty()) return Status::error(ErrorCode::InvalidArgument, "矩形太小");

    char msg[512] = {0};
    bool failed = false;
    pdf_document* doc = nullptr;
    fz_try(ctx) { doc = openPdfForEdit(ctx, input, password); }
    fz_catch(ctx) {
        std::snprintf(msg, sizeof(msg), "%s", fz_caught_message(ctx));
        failed = true;
    }
    if (failed) return Status::error(ErrorCode::IoError, msg);

    std::string text;
    fz_try(ctx) { text = textInRectOfDoc(ctx, doc, page, rect); }
    fz_catch(ctx) {
        std::snprintf(msg, sizeof(msg), "%s", fz_caught_message(ctx));
        failed = true;
    }
    pdf_drop_document(ctx, doc);
    if (failed) return Status::error(ErrorCode::IoError, msg);
    return text;
}

// ---------------------------------------------------------------------------
// 在已有文档某页追加一层“看得见的文字”（显示空间坐标，复用 OCR 的 Form XObject 套路）
// -------------------------------------------------------------------

struct TextPlacement {
    RectD rect;
    std::string text;
    double fontSize = 0.0;   // 0 用矩形高度
    std::string colorHex;    // 空 = 黑
};

// 写文字时的 run 拆分：可打印 ASCII / Latin-1 走 base14 Helvetica（字宽真实，
// 不会像非嵌入 CJK 字体那样每个字母都占满一个全角），汉字与全角标点走 CJK 字体。
struct TextRun {
    bool latin = true;
    std::vector<std::uint32_t> cps;
    std::string hex;
};

std::vector<TextRun> splitTextRuns(const std::string& text) {
    std::vector<TextRun> runs;
    for (std::uint32_t cp : utf8Codepoints(text)) {
        const bool latin = (cp >= 0x20 && cp <= 0xFF && cp != 0x7F);
        if (runs.empty() || runs.back().latin != latin) {
            TextRun r;
            r.latin = latin;
            runs.push_back(r);
        }
        TextRun& r = runs.back();
        r.cps.push_back(cp);
        char code[16];
        if (latin) {
            std::snprintf(code, sizeof(code), "%02X", static_cast<unsigned>(cp));
            r.hex += code;
        } else {
            // CJK 字体走 unicode 编码（UniGB-UCS2-H），写 UTF-16BE 码值
            if (cp >= 0x10000) {
                const std::uint32_t v = cp - 0x10000;
                std::snprintf(code, sizeof(code), "%04X%04X",
                              static_cast<unsigned>(0xD800 + (v >> 10)),
                              static_cast<unsigned>(0xDC00 + (v & 0x3FF)));
            } else {
                std::snprintf(code, sizeof(code), "%04X", static_cast<unsigned>(cp));
            }
            r.hex += code;
        }
    }
    return runs;
}

double runWidth(fz_context* ctx, fz_font* helv, const TextRun& r, double size) {
    if (!r.latin) return double(r.cps.size()) * size;  // CJK 字体：一个字一个 em
    if (helv == nullptr) return double(r.cps.size()) * size * 0.5;
    double w = 0.0;
    for (std::uint32_t cp : r.cps) {
        int gid = 0;
        fz_try(ctx) { gid = fz_encode_character(ctx, helv, int(cp)); }
        fz_catch(ctx) { gid = 0; }
        double adv = 0.5;
        if (gid > 0) {
            fz_try(ctx) { adv = double(fz_advance_glyph(ctx, helv, gid, 0)); }
            fz_catch(ctx) { adv = 0.5; }
        }
        w += adv * size;
    }
    return w;
}

int writeTextLayer(fz_context* ctx, pdf_document* doc, int pageIndex,
                   const std::vector<TextPlacement>& items, fz_font*& font, pdf_obj*& fontRef,
                   bool backdrop) {
    if (items.empty()) return 0;
    if (font == nullptr || fontRef == nullptr) {
        fz_try(ctx) {
            font = fz_new_cjk_font(ctx, FZ_ADOBE_GB);
            fontRef = pdf_add_cjk_font(ctx, doc, font, FZ_ADOBE_GB, 0, 0);
        }
        fz_catch(ctx) { fontRef = nullptr; }
    }
    if (font == nullptr || fontRef == nullptr) return -1;

    pdf_obj* pageobj = pdf_lookup_page_obj(ctx, doc, pageIndex);
    PageTransform t = pageTransform(ctx, pageobj);
    const double dispW = double(t.dispbox.x1 - t.dispbox.x0);
    const double dispH = double(t.dispbox.y1 - t.dispbox.y0);

    // base14 Helvetica：拉丁字母用真实字宽，不用 CJK 字体的全角推进
    fz_font* helv = nullptr;
    pdf_obj* helvRef = nullptr;
    fz_try(ctx) {
        helv = fz_new_base14_font(ctx, "Helvetica");
        helvRef = pdf_add_simple_font(ctx, doc, helv, PDF_SIMPLE_ENCODING_LATIN);
    }
    fz_catch(ctx) { helvRef = nullptr; }

    int written = 0;
    fz_buffer* buf = nullptr;
    pdf_obj* res = nullptr;
    fz_try(ctx) {
        res = pdf_new_dict(ctx, doc, 2);
        pdf_obj* fonts = pdf_new_dict(ctx, doc, 2);
        pdf_dict_puts(ctx, fonts, "PwText", fontRef);
        if (helvRef != nullptr) pdf_dict_puts(ctx, fonts, "Helv", helvRef);
        pdf_dict_put_drop(ctx, res, PDF_NAME(Font), fonts);
        if (helvRef != nullptr) {
            pdf_drop_obj(ctx, helvRef);
            helvRef = nullptr;
        }

        buf = fz_new_buffer(ctx, 4096);
        for (const TextPlacement& it : items) {
            if (it.rect.empty()) continue;

            // 先铺一块白底盖掉原来的内容。MuPDF 的 redact 在部分文档上删不掉文字
            // （文字可能藏在嵌套 Form 里），覆盖法更可靠；代价是底层旧文字仍在，
            // 需要"彻底抹除"场景请用脱敏（redact）功能。
            if (backdrop) {
                // 只向下外扩（降部、下划线常伸到命中框下方），上边界不越过 y0，
                // 否则行距紧的文档会把上一行文字的下半截切掉。
                // 注意 y 方向：显示空间 y 向下、用户空间 y 向上，所以下扩 = 起点减、高度加。
                fz_append_printf(ctx, buf, "q 1 1 1 rg %.2f %.2f %.2f %.2f re f Q\n",
                                 it.rect.x0 - 1.0, dispH - it.rect.y1 - 2.0,
                                 (it.rect.x1 - it.rect.x0) + 2.0, (it.rect.y1 - it.rect.y0) + 2.0);
            }
            if (it.text.empty()) {
                ++written;
                continue;
            }

            double size = it.fontSize > 0 ? it.fontSize : (it.rect.y1 - it.rect.y0) * 0.82;
            if (size < 1.0) size = 1.0;

            const std::vector<TextRun> runs = splitTextRuns(it.text);
            if (runs.empty()) continue;

            // 新文字比原框长时缩字号，而不是把字横向压扁（压扁会糊成一条线）
            const double boxW = it.rect.x1 - it.rect.x0;
            auto widthOf = [&](double sz) {
                double w = 0.0;
                for (const TextRun& r : runs) w += runWidth(ctx, helv, r, sz);
                return w;
            };
            if (boxW > 1.0) {
                const double natural = widthOf(size);
                if (natural > boxW && natural > 0.1) {
                    size = size * boxW / natural * 0.98;
                    if (size < 4.0) size = 4.0;
                }
            }
            float color[3] = {0.0f, 0.0f, 0.0f};
            if (!it.colorHex.empty()) parseColorHex(it.colorHex, color);
            const double baseline = dispH - (it.rect.y1 - size * 0.25);

            double x = it.rect.x0;
            for (const TextRun& r : runs) {
                const bool useHelv = r.latin && helv != nullptr;
                fz_append_printf(ctx, buf,
                                 "BT /%s %.3f Tf %.3f %.3f %.3f rg 100 Tz 1 0 0 1 %.2f %.2f Tm <%s> Tj ET\n",
                                 useHelv ? "Helv" : "PwText", size, color[0], color[1], color[2], x,
                                 baseline, r.hex.c_str());
                x += runWidth(ctx, helv, r, size);
            }
            ++written;
        }
        if (written == 0) fz_throw(ctx, FZ_ERROR_GENERIC, "没有可写入的文字");

        pdf_obj* form = pdf_new_dict(ctx, doc, 8);
        pdf_dict_put(ctx, form, PDF_NAME(Type), PDF_NAME(XObject));
        pdf_dict_put(ctx, form, PDF_NAME(Subtype), PDF_NAME(Form));
        pdf_obj* bbox = pdf_new_array(ctx, doc, 4);
        pdf_array_push_real(ctx, bbox, 0);
        pdf_array_push_real(ctx, bbox, 0);
        pdf_array_push_real(ctx, bbox, dispW);
        pdf_array_push_real(ctx, bbox, dispH);
        pdf_dict_put_drop(ctx, form, PDF_NAME(BBox), bbox);
        pdf_dict_put(ctx, form, PDF_NAME(Resources), res);

        fz_matrix flip = fz_make_matrix(1, 0, 0, -1, 0, float(dispH));
        fz_matrix matrix = fz_concat(t.inv, flip);
        pdf_obj* mtx = pdf_new_array(ctx, doc, 6);
        pdf_array_push_real(ctx, mtx, matrix.a);
        pdf_array_push_real(ctx, mtx, matrix.b);
        pdf_array_push_real(ctx, mtx, matrix.c);
        pdf_array_push_real(ctx, mtx, matrix.d);
        pdf_array_push_real(ctx, mtx, matrix.e);
        pdf_array_push_real(ctx, mtx, matrix.f);
        pdf_dict_put_drop(ctx, form, PDF_NAME(Matrix), mtx);

        pdf_obj* xobj = pdf_add_stream(ctx, doc, buf, form, 1);
        pdf_drop_obj(ctx, form);

        pdf_obj* pres = pdf_dict_get(ctx, pageobj, PDF_NAME(Resources));
        if (!pdf_is_dict(ctx, pres)) {
            pdf_dict_put_drop(ctx, pageobj, PDF_NAME(Resources), pdf_new_dict(ctx, doc, 2));
            pres = pdf_dict_get(ctx, pageobj, PDF_NAME(Resources));
        }
        pdf_obj* pxo = pdf_dict_gets(ctx, pres, "XObject");
        if (!pdf_is_dict(ctx, pxo)) {
            pdf_dict_put_drop(ctx, pres, PDF_NAME(XObject), pdf_new_dict(ctx, doc, 2));
            pxo = pdf_dict_gets(ctx, pres, "XObject");
        }
        char name[32] = "PwEditText";
        int suffix = 0;
        while (pdf_dict_gets(ctx, pxo, name) != nullptr) {
            std::snprintf(name, sizeof(name), "PwEditText%d", ++suffix);
        }
        pdf_dict_puts(ctx, pxo, name, xobj);
        pdf_drop_obj(ctx, xobj);

        std::string post = "Q\nq /" + std::string(name) + " Do Q\n";
        pdf_obj* preStream = addOpsStream(ctx, doc, "q\n");
        pdf_obj* layerStream = addOpsStream(ctx, doc, post);
        pdf_obj* contents = pdf_dict_get(ctx, pageobj, PDF_NAME(Contents));
        if (pdf_is_array(ctx, contents)) {
            pdf_array_insert_drop(ctx, contents, preStream, 0);
            pdf_array_push_drop(ctx, contents, layerStream);
        } else {
            pdf_obj* arr = pdf_new_array(ctx, doc, 3);
            pdf_array_push_drop(ctx, arr, preStream);
            if (contents != nullptr) pdf_array_push(ctx, arr, contents);
            pdf_array_push_drop(ctx, arr, layerStream);
            pdf_dict_put_drop(ctx, pageobj, PDF_NAME(Contents), arr);
        }
    }
    fz_catch(ctx) { written = -1; }

    if (buf != nullptr) fz_drop_buffer(ctx, buf);
    if (res != nullptr) pdf_drop_obj(ctx, res);
    if (helv != nullptr) fz_drop_font(ctx, helv);
    return written;
}

// ---------------------------------------------------------------------------
// 查找替换：命中处 redaction 真删原文字，再在原框里写新文字
// -------------------------------------------------------------------

Status writeReplaceText(fz_context* ctx, const fs::path& input, const fs::path& output,
                        const std::string& find, const std::string& replace,
                        const ReplaceOptions& opts, const std::string& password) {
    if (find.empty()) return Status::error(ErrorCode::InvalidArgument, "查找内容为空");
    if (!opts.deleteOnly && replace.empty()) {
        return Status::error(ErrorCode::InvalidArgument, "替换内容为空（若只想删除，请勾选“只删不写”）");
    }

    char msg[512] = {0};
    bool failed = false;
    pdf_document* doc = nullptr;
    fz_font* font = nullptr;
    pdf_obj* fontRef = nullptr;

    fz_try(ctx) { doc = openPdfForEdit(ctx, input, password); }
    fz_catch(ctx) {
        std::snprintf(msg, sizeof(msg), "%s", fz_caught_message(ctx));
        failed = true;
    }
    if (failed) return Status::error(ErrorCode::IoError, msg);

    std::vector<TextHit> hits;
    fz_try(ctx) { hits = searchTextInDoc(ctx, doc, find, -1, opts.caseSensitive); }
    fz_catch(ctx) {
        std::snprintf(msg, sizeof(msg), "%s", fz_caught_message(ctx));
        failed = true;
    }
    if (failed) {
        pdf_drop_document(ctx, doc);
        return Status::error(ErrorCode::IoError, msg);
    }
    if (hits.empty()) {
        pdf_drop_document(ctx, doc);
        return Status::error(ErrorCode::InvalidArgument, "没有找到要替换的文字");
    }
    if (opts.limit > 0 && static_cast<int>(hits.size()) > opts.limit) {
        hits.resize(static_cast<std::size_t>(opts.limit));
    }

    // 逐页：白底覆盖原文字（deleteOnly 就到此为止）→ 需要时再压上新文字
    const int pageCount = pdf_count_pages(ctx, doc);
    for (int i = 0; i < pageCount && !failed; ++i) {
        std::vector<TextHit> pageHits;
        for (const TextHit& h : hits) {
            if (h.page == i) pageHits.push_back(h);
        }
        if (pageHits.empty()) continue;

        std::vector<TextPlacement> items;
        for (const TextHit& h : pageHits) {
            TextPlacement p;
            p.rect = h.rect;
            p.text = opts.deleteOnly ? std::string() : replace;
            p.fontSize = opts.fontSize;
            p.colorHex = opts.colorHex;
            // 新文字比原框短时右边界收一点，避免被横向拉伸
            if (!opts.deleteOnly) {
                const double size = p.fontSize > 0 ? p.fontSize : (h.rect.y1 - h.rect.y0);
                const double want = estTextWidth(replace, size);
                if (want > 1.0 && want < h.rect.width()) p.rect.x1 = p.rect.x0 + want;
            }
            items.push_back(p);
        }
        fz_try(ctx) {
            if (writeTextLayer(ctx, doc, i, items, font, fontRef, true) < 0) {
                fz_throw(ctx, FZ_ERROR_GENERIC, "写回新文字失败");
            }
        }
        fz_catch(ctx) {
            std::snprintf(msg, sizeof(msg), "%s", fz_caught_message(ctx));
            failed = true;
        }
    }

    if (!failed) {
        fz_try(ctx) {
            // 同上：写回的新文字用的是非嵌入 CJK 字体，不做子集化（否则体积暴涨）
            pdf_write_options wo = writeOptions();
            pdf_save_document(ctx, doc, output.string().c_str(), &wo);
        }
        fz_catch(ctx) {
            std::snprintf(msg, sizeof(msg), "%s", fz_caught_message(ctx));
            failed = true;
        }
    }

    if (fontRef != nullptr) pdf_drop_obj(ctx, fontRef);
    if (font != nullptr) fz_drop_font(ctx, font);
    pdf_drop_document(ctx, doc);

    if (failed) return Status::error(ErrorCode::IoError, msg);
    return Status::success();
}

// ---------------------------------------------------------------------------
// 按矩形写字：预览里"框住这块 → 改字 / 加字"走这一条
// -------------------------------------------------------------------
//
// 为什么不复用 replaceText：那条路是"按文本全篇查找替换"——
//   * 框里的文字在文档里出现多次时，会把别处的同名文字一起改掉；
//   * textInRect 取出来的文本经常带换行/断词，拿去 searchTextInDoc 未必命中，
//     表现就是"框住了、点了确定、什么也没变"。
// 这里只认矩形：白底盖掉这一块（deleteOnly 就到此为止），再按框宽写新字。
// backdrop=false 时不铺白底，用于"加文字"——原内容一个字都不动。

Status writeTextAtRect(fz_context* ctx, const fs::path& input, const fs::path& output, int pageIndex,
                       const RectD& rect, const std::string& text, const ReplaceOptions& opts,
                       bool backdrop, const std::string& password) {
    if (rect.empty()) return Status::error(ErrorCode::InvalidArgument, "矩形为空");
    if (!opts.deleteOnly && text.empty()) {
        return Status::error(ErrorCode::InvalidArgument, "新文字为空（只想删就把 deleteOnly 打开）");
    }

    char msg[512] = {0};
    bool failed = false;
    pdf_document* doc = nullptr;
    fz_font* font = nullptr;
    pdf_obj* fontRef = nullptr;

    fz_try(ctx) { doc = openPdfForEdit(ctx, input, password); }
    fz_catch(ctx) {
        std::snprintf(msg, sizeof(msg), "%s", fz_caught_message(ctx));
        failed = true;
    }
    if (failed) return Status::error(ErrorCode::IoError, msg);

    const int pageCount = pdf_count_pages(ctx, doc);
    if (pageIndex < 0 || pageIndex >= pageCount) {
        pdf_drop_document(ctx, doc);
        return Status::error(ErrorCode::InvalidArgument,
                             std::string("页码超出范围（这里要 0 基，文档共 ") + std::to_string(pageCount) +
                                 " 页）");
    }

    TextPlacement p;
    p.rect = rect;
    p.text = opts.deleteOnly ? std::string() : text;
    p.fontSize = opts.fontSize;
    p.colorHex = opts.colorHex;
    // 新文字比原框短时右边界收一点，避免被横向拉伸（与 writeReplaceText 同一套算法）
    if (!opts.deleteOnly) {
        const double size = p.fontSize > 0 ? p.fontSize : (rect.y1 - rect.y0);
        const double want = estTextWidth(text, size);
        if (want > 1.0 && want < rect.width()) p.rect.x1 = p.rect.x0 + want;
    }

    std::vector<TextPlacement> items{p};
    fz_try(ctx) {
        if (writeTextLayer(ctx, doc, pageIndex, items, font, fontRef, backdrop) < 0) {
            fz_throw(ctx, FZ_ERROR_GENERIC, "写字失败（字体或页面资源没准备好）");
        }
    }
    fz_catch(ctx) {
        std::snprintf(msg, sizeof(msg), "%s", fz_caught_message(ctx));
        failed = true;
    }

    if (!failed) {
        fz_try(ctx) {
            pdf_write_options wo = writeOptions();
            pdf_save_document(ctx, doc, output.string().c_str(), &wo);
        }
        fz_catch(ctx) {
            std::snprintf(msg, sizeof(msg), "%s", fz_caught_message(ctx));
            failed = true;
        }
    }

    if (fontRef != nullptr) pdf_drop_obj(ctx, fontRef);
    if (font != nullptr) fz_drop_font(ctx, font);
    pdf_drop_document(ctx, doc);

    if (failed) return Status::error(ErrorCode::IoError, msg);
    return Status::success();
}

// ---------------------------------------------------------------------------
// 替换图片：redaction 抹掉矩形内的图像，再把新图按矩形等比铺进去
// -------------------------------------------------------------------

Status writeReplaceImage(fz_context* ctx, const fs::path& input, const fs::path& output, int pageIndex,
                         const RectD& rect, const fs::path& imagePath, const std::string& password) {
    if (rect.empty()) return Status::error(ErrorCode::InvalidArgument, "替换区域为空");
    if (imagePath.empty()) return Status::error(ErrorCode::InvalidArgument, "没有指定图片");

    char msg[512] = {0};
    bool failed = false;
    pdf_document* doc = nullptr;
    fz_image* image = nullptr;

    fz_try(ctx) {
        doc = openPdfForEdit(ctx, input, password);
        const int pageCount = pdf_count_pages(ctx, doc);
        if (pageIndex < 0 || pageIndex >= pageCount) {
            fz_throw(ctx, FZ_ERROR_GENERIC, "页号越界");
        }
        image = fz_new_image_from_file(ctx, imagePath.string().c_str());
    }
    fz_catch(ctx) {
        std::snprintf(msg, sizeof(msg), "%s", fz_caught_message(ctx));
        failed = true;
    }
    if (failed) {
        if (image != nullptr) fz_drop_image(ctx, image);
        if (doc != nullptr) pdf_drop_document(ctx, doc);
        return Status::error(ErrorCode::InvalidArgument, msg);
    }

    // 用一块白底盖住原图，再在上面铺新图（MuPDF 的 redact 在部分文档上不可靠）
    // 铺新图（等比装进矩形，居中）
    if (!failed) {
        fz_try(ctx) {
            pdf_obj* imgRef = pdf_add_image(ctx, doc, image);
            pdf_obj* pageobj = pdf_lookup_page_obj(ctx, doc, pageIndex);
            PageTransform t = pageTransform(ctx, pageobj);
            const double dispW = double(t.dispbox.x1 - t.dispbox.x0);
            const double dispH = double(t.dispbox.y1 - t.dispbox.y0);

            const double boxW = rect.x1 - rect.x0;
            const double boxH = rect.y1 - rect.y0;
            const double imgW = std::max(1.0, double(image->w));
            const double imgH = std::max(1.0, double(image->h));
            const double scale = std::min(boxW / imgW, boxH / imgH);
            const double drawW = imgW * scale;
            const double drawH = imgH * scale;
            const double x = rect.x0 + (boxW - drawW) / 2.0;
            const double top = rect.y0 + (boxH - drawH) / 2.0;

            fz_buffer* buf = fz_new_buffer(ctx, 512);
            // 白底：form 内用的是页面用户空间（y 向上）
            fz_append_printf(ctx, buf, "q 1 1 1 rg %.2f %.2f %.2f %.2f re f Q\n",
                             rect.x0 - 0.6, std::max(0.0, dispH - rect.y1 - 0.6),
                             boxW + 1.2, boxH + 1.2);
            // form 空间 y 向上：图片左下角 = dispH - (top + drawH)
            fz_append_printf(ctx, buf, "q %.2f 0 0 %.2f %.2f %.2f cm /PwImg Do Q\n", drawW, drawH,
                             x, std::max(0.0, dispH - (top + drawH)));

            pdf_obj* form = pdf_new_dict(ctx, doc, 10);
            pdf_dict_put(ctx, form, PDF_NAME(Type), PDF_NAME(XObject));
            pdf_dict_put(ctx, form, PDF_NAME(Subtype), PDF_NAME(Form));
            pdf_obj* bbox = pdf_new_array(ctx, doc, 4);
            pdf_array_push_real(ctx, bbox, 0);
            pdf_array_push_real(ctx, bbox, 0);
            pdf_array_push_real(ctx, bbox, dispW);
            pdf_array_push_real(ctx, bbox, dispH);
            pdf_dict_put_drop(ctx, form, PDF_NAME(BBox), bbox);

            pdf_obj* res = pdf_new_dict(ctx, doc, 2);
            pdf_obj* xobjs = pdf_new_dict(ctx, doc, 2);
            pdf_dict_puts(ctx, xobjs, "PwImg", imgRef);
            pdf_dict_put_drop(ctx, res, PDF_NAME(XObject), xobjs);
            pdf_dict_put(ctx, form, PDF_NAME(Resources), res);

            // 上面的 cm / re 已经按用户空间写好，Matrix 保持恒等即可
            pdf_obj* mtx = pdf_new_array(ctx, doc, 6);
            pdf_array_push_real(ctx, mtx, 1);
            pdf_array_push_real(ctx, mtx, 0);
            pdf_array_push_real(ctx, mtx, 0);
            pdf_array_push_real(ctx, mtx, 1);
            pdf_array_push_real(ctx, mtx, 0);
            pdf_array_push_real(ctx, mtx, 0);
            pdf_dict_put_drop(ctx, form, PDF_NAME(Matrix), mtx);
            (void)t;

            pdf_obj* xobj = pdf_add_stream(ctx, doc, buf, form, 1);
            pdf_drop_obj(ctx, form);
            fz_drop_buffer(ctx, buf);
            pdf_drop_obj(ctx, res);
            pdf_drop_obj(ctx, imgRef);

            pdf_obj* pres = pdf_dict_get(ctx, pageobj, PDF_NAME(Resources));
            if (!pdf_is_dict(ctx, pres)) {
                pdf_dict_put_drop(ctx, pageobj, PDF_NAME(Resources), pdf_new_dict(ctx, doc, 2));
                pres = pdf_dict_get(ctx, pageobj, PDF_NAME(Resources));
            }
            pdf_obj* pxo = pdf_dict_gets(ctx, pres, "XObject");
            if (!pdf_is_dict(ctx, pxo)) {
                pdf_dict_put_drop(ctx, pres, PDF_NAME(XObject), pdf_new_dict(ctx, doc, 2));
                pxo = pdf_dict_gets(ctx, pres, "XObject");
            }
            char name[32] = "PwReplaceImg";
            int suffix = 0;
            while (pdf_dict_gets(ctx, pxo, name) != nullptr) {
                std::snprintf(name, sizeof(name), "PwReplaceImg%d", ++suffix);
            }
            pdf_dict_puts(ctx, pxo, name, xobj);
            pdf_drop_obj(ctx, xobj);

            std::string post = "Q\nq /" + std::string(name) + " Do Q\n";
            pdf_obj* preStream = addOpsStream(ctx, doc, "q\n");
            pdf_obj* layerStream = addOpsStream(ctx, doc, post);
            pdf_obj* contents = pdf_dict_get(ctx, pageobj, PDF_NAME(Contents));
            if (pdf_is_array(ctx, contents)) {
                pdf_array_insert_drop(ctx, contents, preStream, 0);
                pdf_array_push_drop(ctx, contents, layerStream);
            } else {
                pdf_obj* arr = pdf_new_array(ctx, doc, 3);
                pdf_array_push_drop(ctx, arr, preStream);
                if (contents != nullptr) pdf_array_push(ctx, arr, contents);
                pdf_array_push_drop(ctx, arr, layerStream);
                pdf_dict_put_drop(ctx, pageobj, PDF_NAME(Contents), arr);
            }

            pdf_write_options wo = writeOptions();
            pdf_save_document(ctx, doc, output.string().c_str(), &wo);
        }
        fz_catch(ctx) {
            std::snprintf(msg, sizeof(msg), "%s", fz_caught_message(ctx));
            failed = true;
        }
    }

    if (image != nullptr) fz_drop_image(ctx, image);
    pdf_drop_document(ctx, doc);
    if (failed) return Status::error(ErrorCode::IoError, msg);
    return Status::success();
}

// 图片水印：骨架与 writeWatermarked 完全一致（graft 原页 → 独立内容流 → 封成 Form XObject
// 追加到 /Contents），差别只在内容流里画的是图片（fz_fill_image）而不是文字。
// scale 是水印宽度占页面宽度的比例；tile=true 时按网格平铺。
Status writeImageWatermarked(fz_context* ctx, const fs::path& input, const fs::path& output,
                             const fs::path& imagePath, float opacity, float scale, bool tile,
                             const std::string& password) {
    char msg[512] = {0};
    bool failed = false;

    pdf_document* dst = nullptr;
    pdf_document* src = nullptr;
    pdf_graft_map* graft_map = nullptr;
    fz_image* image = nullptr;
    pdf_obj* imgObj = nullptr;

    fz_try(ctx) { dst = pdf_create_document(ctx); }
    fz_catch(ctx) {
        std::snprintf(msg, sizeof(msg), "%s", fz_caught_message(ctx));
        failed = true;
    }
    if (failed || dst == nullptr) {
        if (dst != nullptr) pdf_drop_document(ctx, dst);
        return Status::error(ErrorCode::IoError, failed ? msg : "无法创建输出 PDF");
    }

    fz_try(ctx) { src = pdf_open_document(ctx, input.string().c_str()); }
    fz_catch(ctx) {
        std::snprintf(msg, sizeof(msg), "%s", fz_caught_message(ctx));
        failed = true;
    }
    if (failed || src == nullptr) {
        if (src != nullptr) pdf_drop_document(ctx, src);
        pdf_drop_document(ctx, dst);
        return Status::error(ErrorCode::IoError, failed ? msg : "无法打开源 PDF");
    }

    int total = 0;
    fz_try(ctx) {
        if (pdf_needs_password(ctx, src) != 0) {
            if (!pdf_authenticate_password(ctx, src, password.c_str())) {
                std::snprintf(msg, sizeof(msg), "%s",
                              password.empty() ? "文档已加密，需要密码" : "文档已加密，密码错误");
                failed = true;
            }
        }
        if (!failed) {
            total = pdf_count_pages(ctx, src);
            // 与文字版相同：整份文档共用一个 graft map，源页资源只拷一次
            graft_map = pdf_new_graft_map(ctx, dst);
            image = fz_new_image_from_file(ctx, imagePath.string().c_str());
            // 图片只嵌入一次（整份文档共用一个 image XObject），
            // 否则平铺时每一格都会复制一份像素数据，输出能涨到几十 MB。
            if (image != nullptr) imgObj = pdf_add_image(ctx, dst, image);
        }
    }
    fz_catch(ctx) {
        std::snprintf(msg, sizeof(msg), "%s", fz_caught_message(ctx));
        failed = true;
    }

    if (!failed && image == nullptr) {
        failed = true;
        std::snprintf(msg, sizeof(msg), "无法读取水印图片: %s", imagePath.string().c_str());
    }

    // 图片宽高比（fz_image 的 w/h 是像素尺寸，与 DPI 无关，只用比例）
    const float aspect = (image != nullptr && image->w > 0)
                             ? static_cast<float>(image->h) / static_cast<float>(image->w)
                             : 1.0f;

    for (int i = 0; !failed && i < total; ++i) {
        pdf_obj* pageobj = nullptr;
        pdf_obj* res = nullptr;
        fz_buffer* buf = nullptr;
        fz_rect mediabox = fz_empty_rect;
        fz_rect dispbox = fz_empty_rect;
        fz_matrix pageCtm = fz_identity;

        fz_try(ctx) {
            pdf_graft_mapped_page(ctx, graft_map, -1, src, i);
            pageobj = pdf_lookup_page_obj(ctx, dst, i);
            pdf_page_obj_transform(ctx, pageobj, &mediabox, &pageCtm);
            dispbox = fz_transform_rect(mediabox, pageCtm);
        }
        fz_catch(ctx) {
            std::snprintf(msg, sizeof(msg), "%s", fz_caught_message(ctx));
            failed = true;
        }
        if (failed) break;

        if (fz_is_empty_rect(dispbox) || fz_is_infinite_rect(dispbox) ||
            dispbox.x1 - dispbox.x0 < 1 || dispbox.y1 - dispbox.y0 < 1) {
            mediabox = fz_make_rect(0, 0, 595, 842);
            dispbox = fz_make_rect(0, 0, 595, 842);
            pageCtm = fz_identity;
        }

        // 用页面用户空间（y 向上）手写内容流：每格一条 cm + Do，
        // 整份文档只嵌一份 image XObject，平铺再多也不复制像素数据。
        const fz_matrix inv = fz_invert_matrix(pageCtm);
        const float pw = dispbox.x1 - dispbox.x0;
        const float ph = dispbox.y1 - dispbox.y0;
        float wmW = pw * scale;
        float wmH = wmW * aspect;
        // 长条图兜底：高度不许超过页高的 90%，超了按高度反算宽度
        if (ph > 0 && wmH > ph * 0.9f) {
            wmH = ph * 0.9f;
            wmW = (aspect > 0) ? wmH / aspect : wmH;
        }

        fz_try(ctx) {
            res = pdf_new_dict(ctx, dst, 2);
            pdf_obj* xo = pdf_new_dict(ctx, dst, 1);
            pdf_dict_puts(ctx, xo, "PwImg", imgObj);
            pdf_dict_put_drop(ctx, res, PDF_NAME(XObject), xo);
            buf = fz_new_buffer(ctx, 512);

            // 显示空间的矩形折回 PDF 用户空间后再写 cm（/Rotate 页面同样成立）
            auto stamp = [&](float x, float y) {
                fz_rect u = fz_transform_rect(fz_make_rect(x, y, x + wmW, y + wmH), inv);
                const float ux0 = std::min(u.x0, u.x1);
                const float ux1 = std::max(u.x0, u.x1);
                const float uy0 = std::min(u.y0, u.y1);
                const float uy1 = std::max(u.y0, u.y1);
                fz_append_printf(ctx, buf, "q /GS0 gs %.4f 0 0 %.4f %.4f %.4f cm /PwImg Do Q\n",
                                 ux1 - ux0, uy1 - uy0, ux0, uy0);
            };

            if (tile) {
                const float stepX = std::max(wmW * 1.6f, 8.0f);
                const float stepY = std::max(wmH * 1.6f, 8.0f);
                for (float y = dispbox.y0; y + wmH <= dispbox.y1 + 0.01f; y += stepY) {
                    for (float x = dispbox.x0; x + wmW <= dispbox.x1 + 0.01f; x += stepX) {
                        stamp(x, y);
                    }
                }
            } else {
                stamp(dispbox.x0 + (pw - wmW) * 0.5f, dispbox.y0 + (ph - wmH) * 0.5f);
            }

            // 透明度靠 ExtGState 的 /ca：每个 Do 前挂一次 /GS0 gs，PNG 自带 alpha 也保留
            pdf_obj* gstate = pdf_new_dict(ctx, dst, 1);
            pdf_dict_put_real(ctx, gstate, PDF_NAME(ca), opacity);
            pdf_obj* eg = pdf_new_dict(ctx, dst, 1);
            pdf_dict_puts(ctx, eg, "GS0", gstate);
            pdf_dict_put_drop(ctx, res, PDF_NAME(ExtGState), eg);
            pdf_drop_obj(ctx, gstate);
        }
        fz_catch(ctx) {
            std::snprintf(msg, sizeof(msg), "%s", fz_caught_message(ctx));
            failed = true;
        }

        // 与文字版同样封成自带 /Resources 的 Form XObject，页面只多一个 /XObject 项
        if (!failed) {
            fz_try(ctx) {
                pdf_obj* form = pdf_new_dict(ctx, dst, 6);
                pdf_dict_put(ctx, form, PDF_NAME(Type), PDF_NAME(XObject));
                pdf_dict_put(ctx, form, PDF_NAME(Subtype), PDF_NAME(Form));
                pdf_obj* bbox = pdf_new_array(ctx, dst, 4);
                pdf_array_push_real(ctx, bbox, mediabox.x0);
                pdf_array_push_real(ctx, bbox, mediabox.y0);
                pdf_array_push_real(ctx, bbox, mediabox.x1);
                pdf_array_push_real(ctx, bbox, mediabox.y1);
                pdf_dict_put_drop(ctx, form, PDF_NAME(BBox), bbox);
                pdf_dict_put(ctx, form, PDF_NAME(Resources), res);

                pdf_obj* xobj = pdf_add_stream(ctx, dst, buf, form, 1);
                pdf_drop_obj(ctx, form);

                pdf_obj* pres = pdf_dict_get(ctx, pageobj, PDF_NAME(Resources));
                if (!pdf_is_dict(ctx, pres)) {
                    pres = pdf_new_dict(ctx, dst, 2);
                    pdf_dict_put_drop(ctx, pageobj, PDF_NAME(Resources), pres);
                    pres = pdf_dict_get(ctx, pageobj, PDF_NAME(Resources));
                }
                pdf_obj* pxo = pdf_dict_gets(ctx, pres, "XObject");
                if (!pdf_is_dict(ctx, pxo)) {
                    pdf_dict_put_drop(ctx, pres, PDF_NAME(XObject), pdf_new_dict(ctx, dst, 2));
                    pxo = pdf_dict_gets(ctx, pres, "XObject");
                }
                char name[32] = "PwImgWm";
                int suffix = 0;
                while (pdf_dict_gets(ctx, pxo, name) != nullptr) {
                    std::snprintf(name, sizeof(name), "PwImgWm%d", ++suffix);
                }
                pdf_dict_puts(ctx, pxo, name, xobj);
                pdf_drop_obj(ctx, xobj);

                std::string post = "Q\nq /" + std::string(name) + " Do Q\n";
                pdf_obj* preStream = addOpsStream(ctx, dst, "q\n");
                pdf_obj* wmStream = addOpsStream(ctx, dst, post);

                pdf_obj* contents = pdf_dict_get(ctx, pageobj, PDF_NAME(Contents));
                if (pdf_is_array(ctx, contents)) {
                    pdf_array_insert_drop(ctx, contents, preStream, 0);
                    pdf_array_push_drop(ctx, contents, wmStream);
                } else {
                    pdf_obj* arr = pdf_new_array(ctx, dst, 3);
                    pdf_array_push_drop(ctx, arr, preStream);
                    if (contents != nullptr) pdf_array_push(ctx, arr, contents);
                    pdf_array_push_drop(ctx, arr, wmStream);
                    pdf_dict_put_drop(ctx, pageobj, PDF_NAME(Contents), arr);
                }
            }
            fz_catch(ctx) {
                std::snprintf(msg, sizeof(msg), "%s", fz_caught_message(ctx));
                failed = true;
            }
        }

        if (buf != nullptr) fz_drop_buffer(ctx, buf);
        if (res != nullptr) pdf_drop_obj(ctx, res);
    }

    if (imgObj != nullptr) pdf_drop_obj(ctx, imgObj);

    if (!failed) {
        pdf_write_options opts = writeOptions();
        fz_try(ctx) { pdf_save_document(ctx, dst, output.string().c_str(), &opts); }
        fz_catch(ctx) {
            std::snprintf(msg, sizeof(msg), "%s", fz_caught_message(ctx));
            failed = true;
        }
    }

    if (image != nullptr) fz_drop_image(ctx, image);

    if (graft_map != nullptr) pdf_drop_graft_map(ctx, graft_map);
    pdf_drop_document(ctx, src);
    pdf_drop_document(ctx, dst);

    if (failed) return Status::error(ErrorCode::IoError, msg[0] != '\0' ? msg : "图片水印写入失败");
    return Status::success();
}

class MuPdfBackend : public IPdfBackend {
public:
    explicit MuPdfBackend(const Config& config) : maxSide_(config.renderMaxSide) {}

    std::string name() const override { return "mupdf"; }
    bool available() const override { return true; }

    Result<std::shared_ptr<IPdfDocument>> open(const fs::path& path,
                                               const std::string& password) override {
        std::error_code ec;
        if (!fs::exists(path, ec)) {
            return Status::error(ErrorCode::IoError, "文件不存在: " + path.string());
        }

        fz_context* ctx = fz_new_context(nullptr, nullptr, FZ_STORE_DEFAULT);
        if (ctx == nullptr) return Status::error(ErrorCode::Internal, "无法创建 MuPDF 上下文");

        fz_document* doc = nullptr;
        char msg[512] = {0};
        bool failed = false;
        bool encrypted = false;
        fz_try(ctx) {
            fz_register_document_handlers(ctx);
            doc = fz_open_document(ctx, path.string().c_str());
            if (doc != nullptr && fz_needs_password(ctx, doc)) {
                encrypted = true;
                if (password.empty()) {
                    failed = true;
                    std::snprintf(msg, sizeof(msg),
                                  "文档已加密，需要密码（open() 的 password 参数 / CLI --password / C ABI pw_set_pdf_password）");
                } else if (fz_authenticate_password(ctx, doc, password.c_str()) == 0) {
                    failed = true;
                    std::snprintf(msg, sizeof(msg), "文档已加密，密码错误");
                }
            }
        }
        fz_catch(ctx) {
            std::snprintf(msg, sizeof(msg), "%s", fz_caught_message(ctx));
            failed = true;
        }
        if (failed || doc == nullptr) {
            if (doc != nullptr) fz_drop_document(ctx, doc);
            fz_drop_context(ctx);
            return Status::error(ErrorCode::IoError, msg);
        }

        auto document = std::make_shared<MuPdfDocument>(ctx, doc, maxSide_);
        DocumentInfo info;
        info.pageCount = 0;
        info.encrypted = encrypted;
        fz_try(ctx) { info.pageCount = fz_count_pages(ctx, doc); }
        fz_catch(ctx) { info.pageCount = 0; }
        info.title = path.filename().string();
        info.backendName = "mupdf";
        document->setInfo(info);
        return std::shared_ptr<IPdfDocument>(document);
    }

    Status merge(const std::vector<fs::path>& inputs, const fs::path& output) override {
        if (inputs.empty()) return Status::error(ErrorCode::InvalidArgument, "输入列表为空");
        return withContext([&](fz_context* ctx) -> Status {
            // 页数上限给足，等于“把 inputs 里所有页全部 graft 过来”
            return writeGraftedRange(ctx, inputs, output, 0, std::numeric_limits<int>::max());
        });
    }

    Status split(const fs::path& input, const fs::path& outDir, int pagesPerFile) override {
        if (pagesPerFile <= 0) return Status::error(ErrorCode::InvalidArgument, "pagesPerFile 必须 > 0");
        std::error_code ec;
        fs::create_directories(outDir, ec);
        return withContext([&](fz_context* ctx) -> Status {
            fz_document* doc = nullptr;
            char msg[512] = {0};
            bool failed = false;
            fz_try(ctx) { doc = fz_open_document(ctx, input.string().c_str()); }
            fz_catch(ctx) {
                std::snprintf(msg, sizeof(msg), "%s", fz_caught_message(ctx));
                failed = true;
            }
            if (failed || doc == nullptr) {
                if (doc != nullptr) fz_drop_document(ctx, doc);
                return Status::error(ErrorCode::IoError, msg);
            }
            int total = 0;
            fz_try(ctx) { total = fz_count_pages(ctx, doc); }
            fz_catch(ctx) { failed = true; }
            fz_drop_document(ctx, doc);
            if (failed) return Status::error(ErrorCode::IoError, msg);

            std::string stem = input.stem().string();
            for (int start = 0; start < total; start += pagesPerFile) {
                int count = std::min(pagesPerFile, total - start);
                fs::path out = outDir / (stem + "_p" + std::to_string(start + 1) + "-" +
                                         std::to_string(start + count) + ".pdf");
                Status st = writeGraftedRange(ctx, {input}, out, start, count);
                if (!st.ok()) return st;
            }
            return Status::success();
        });
    }

    Status extract(const fs::path& input, const fs::path& output, const std::vector<int>& pages,
                   const std::string& password) override {
        if (pages.empty()) return Status::error(ErrorCode::InvalidArgument, "没有选中任何页面");
        return withContext([&](fz_context* ctx) -> Status {
            return writeGraftedPages(ctx, input, output, pages, password);
        });
    }

    Status watermark(const fs::path& input, const fs::path& output, const std::string& text,
                     float opacity, const std::string& position) override {
        if (text.empty()) return Status::error(ErrorCode::InvalidArgument, "水印文本为空");
        if (opacity < 0.0f) opacity = 0.0f;
        if (opacity > 1.0f) opacity = 1.0f;
        return withContext([&](fz_context* ctx) -> Status {
            return writeWatermarked(ctx, input, output, text, opacity, position);
        });
    }

    Status imageWatermark(const fs::path& input, const fs::path& output, const fs::path& image,
                          float opacity, float scale, bool tile,
                          const std::string& password) override {
        std::error_code ec;
        if (!fs::exists(image, ec)) {
            return Status::error(ErrorCode::InvalidArgument, "水印图片不存在: " + image.string());
        }
        if (opacity < 0.0f) opacity = 0.0f;
        if (opacity > 1.0f) opacity = 1.0f;
        if (scale <= 0.0f) scale = 0.3f;
        if (scale > 1.0f) scale = 1.0f;
        return withContext([&](fz_context* ctx) -> Status {
            return writeImageWatermarked(ctx, input, output, image, opacity, scale, tile, password);
        });
    }

    Status assemble(const std::vector<PageRef>& pages, const fs::path& output,
                    const std::string& password) override {
        if (pages.empty()) return Status::error(ErrorCode::InvalidArgument, "输出没有任何页面");
        return withContext([&](fz_context* ctx) -> Status {
            return writeAssembled(ctx, pages, output, password);
        });
    }

    Status imagesToPdf(const std::vector<ImagePage>& images, const fs::path& output) override {
        if (images.empty()) return Status::error(ErrorCode::InvalidArgument, "没有输入图片");
        return withContext([&](fz_context* ctx) -> Status {
            return writeImagesAsPdf(ctx, images, output);
        });
    }

    Status ocr(const fs::path& input, const fs::path& outputPdf, const fs::path& outputTxt,
               const std::string& language, bool generatePdf, bool generateTxt,
               const std::string& password) override {
        if (!generatePdf && !generateTxt) {
            return Status::error(ErrorCode::InvalidArgument, "至少选择“可检索 PDF”或“纯文本”其中一项");
        }
        if (generatePdf && outputPdf.empty()) {
            return Status::error(ErrorCode::InvalidArgument, "没有指定可检索 PDF 的输出路径");
        }
        if (generateTxt && outputTxt.empty()) {
            return Status::error(ErrorCode::InvalidArgument, "没有指定纯文本的输出路径");
        }
        return withContext([&](fz_context* ctx) -> Status {
            return writeOcr(ctx, input, outputPdf, outputTxt, language, generatePdf, generateTxt,
                            password);
        });
    }

    // ---- 编辑与批注类 ----

    Status annotate(const fs::path& input, const fs::path& output,
                    const std::vector<AnnotSpec>& annots, const std::string& password) override {
        if (input.empty() || output.empty()) {
            return Status::error(ErrorCode::InvalidArgument, "输入或输出路径为空");
        }
        return withContext([&](fz_context* ctx) -> Status {
            return writeAnnots(ctx, input, output, annots, password);
        });
    }

    Status addLink(const fs::path& input, const fs::path& output, const LinkSpec& link,
                   const std::string& password) override {
        if (input.empty() || output.empty()) {
            return Status::error(ErrorCode::InvalidArgument, "输入或输出路径为空");
        }
        return withContext([&](fz_context* ctx) -> Status {
            return writeLink(ctx, input, output, link, password);
        });
    }

    // ---- 编辑与批注类：表单 ----

    Result<std::vector<FormField>> formFields(const fs::path& input,
                                             const std::string& password) override {
        if (input.empty()) return Status::error(ErrorCode::InvalidArgument, "输入路径为空");
        return withContext([&](fz_context* ctx) -> Result<std::vector<FormField>> {
            return readFormFields(ctx, input, password);
        });
    }

    Status fillForm(const fs::path& input, const fs::path& output,
                    const std::vector<FieldValue>& values, const std::string& password) override {
        if (input.empty() || output.empty()) {
            return Status::error(ErrorCode::InvalidArgument, "输入或输出路径为空");
        }
        return withContext([&](fz_context* ctx) -> Status {
            return writeFormFill(ctx, input, output, values, password);
        });
    }

    Status createForm(const fs::path& input, const fs::path& output,
                      const std::vector<FieldSpec>& fields, const std::string& password) override {
        if (input.empty() || output.empty()) {
            return Status::error(ErrorCode::InvalidArgument, "输入或输出路径为空");
        }
        return withContext([&](fz_context* ctx) -> Status {
            return writeFormCreate(ctx, input, output, fields, password);
        });
    }

    Result<std::vector<TextHit>> findText(const fs::path& input, const std::string& needle, int page,
                                          const std::string& password) override {
        if (input.empty()) return Status::error(ErrorCode::InvalidArgument, "输入路径为空");
        return withContext([&](fz_context* ctx) -> Result<std::vector<TextHit>> {
            return readFindText(ctx, input, needle, page, password);
        });
    }

    Status replaceText(const fs::path& input, const fs::path& output, const std::string& find,
                       const std::string& replace, const ReplaceOptions& opts,
                       const std::string& password) override {
        if (input.empty() || output.empty()) {
            return Status::error(ErrorCode::InvalidArgument, "输入或输出路径为空");
        }
        return withContext([&](fz_context* ctx) -> Status {
            return writeReplaceText(ctx, input, output, find, replace, opts, password);
        });
    }

    Status replaceImage(const fs::path& input, const fs::path& output, int page, const RectD& rect,
                        const fs::path& imagePath, const std::string& password) override {
        if (input.empty() || output.empty()) {
            return Status::error(ErrorCode::InvalidArgument, "输入或输出路径为空");
        }
        return withContext([&](fz_context* ctx) -> Status {
            return writeReplaceImage(ctx, input, output, page, rect, imagePath, password);
        });
    }

    Status replaceTextAt(const fs::path& input, const fs::path& output, int page, const RectD& rect,
                         const std::string& text, const ReplaceOptions& opts,
                         const std::string& password) override {
        if (input.empty() || output.empty()) {
            return Status::error(ErrorCode::InvalidArgument, "输入或输出路径为空");
        }
        return withContext([&](fz_context* ctx) -> Status {
            return writeTextAtRect(ctx, input, output, page, rect, text, opts, true, password);
        });
    }

    Status addTextAt(const fs::path& input, const fs::path& output, int page, const RectD& rect,
                     const std::string& text, const ReplaceOptions& opts,
                     const std::string& password) override {
        if (input.empty() || output.empty()) {
            return Status::error(ErrorCode::InvalidArgument, "输入或输出路径为空");
        }
        return withContext([&](fz_context* ctx) -> Status {
            return writeTextAtRect(ctx, input, output, page, rect, text, opts, false, password);
        });
    }

    Result<std::vector<OutlineItem>> outline(const fs::path& input,
                                             const std::string& password) override {
        if (input.empty()) return Status::error(ErrorCode::InvalidArgument, "输入路径为空");
        return withContext([&](fz_context* ctx) -> Result<std::vector<OutlineItem>> {
            return readOutline(ctx, input, password);
        });
    }

    Status setOutline(const fs::path& input, const fs::path& output,
                      const std::vector<OutlineItem>& items,
                      const std::string& password) override {
        if (input.empty() || output.empty()) {
            return Status::error(ErrorCode::InvalidArgument, "输入或输出路径为空");
        }
        return withContext([&](fz_context* ctx) -> Status {
            return writeOutline(ctx, input, output, items, password);
        });
    }

    Result<std::string> textInRect(const fs::path& input, int page, const RectD& rect,
                                   const std::string& password) override {
        if (input.empty()) return Status::error(ErrorCode::InvalidArgument, "输入路径为空");
        return withContext([&](fz_context* ctx) -> Result<std::string> {
            return readTextInRect(ctx, input, page, rect, password);
        });
    }

    Result<DocumentMetadata> metadata(const fs::path& path, const std::string& password) override {
        fz_context* ctx = fz_new_context(nullptr, nullptr, FZ_STORE_DEFAULT);
        if (ctx == nullptr) return Status::error(ErrorCode::Internal, "无法创建 MuPDF 上下文");
        bool registered = false;
        fz_try(ctx) {
            fz_register_document_handlers(ctx);
            registered = true;
        }
        fz_catch(ctx) { registered = false; }

        Result<DocumentMetadata> result =
            registered ? readMetadata(ctx, path, password)
                       : Result<DocumentMetadata>(
                             Status::error(ErrorCode::Internal, "MuPDF 文档处理器注册失败"));
        fz_drop_context(ctx);
        return result;
    }

    Status setMetadata(const fs::path& input, const fs::path& output, const DocumentMetadata& meta,
                       const std::string& password) override {
        return withContext([&](fz_context* ctx) -> Status {
            return writeMetadata(ctx, input, output, meta, password);
        });
    }

    Status encrypt(const fs::path& input, const fs::path& output, const std::string& userPassword,
                   const std::string& ownerPassword, int permissions,
                   const std::string& readPassword) override {
        if (userPassword.empty() && ownerPassword.empty()) {
            return Status::error(ErrorCode::InvalidArgument, "加密至少需要用户密码或所有者密码");
        }
        // 所有者密码为空时用用户密码兜底：PDF 规范里两种密码不能同时为空
        const std::string owner = ownerPassword.empty() ? userPassword : ownerPassword;
        return withContext([&](fz_context* ctx) -> Status {
            return resaveWithOptions(ctx, input, output, readPassword, [&](pdf_write_options* opts) {
                opts->do_encrypt = PDF_ENCRYPT_AES_128;
                opts->permissions = permissions;
                std::snprintf(opts->upwd_utf8, sizeof(opts->upwd_utf8), "%s", userPassword.c_str());
                std::snprintf(opts->opwd_utf8, sizeof(opts->opwd_utf8), "%s", owner.c_str());
            });
        });
    }

    Status decrypt(const fs::path& input, const fs::path& output,
                   const std::string& password) override {
        return withContext([&](fz_context* ctx) -> Status {
            return resaveWithOptions(ctx, input, output, password, [&](pdf_write_options* opts) {
                opts->do_encrypt = PDF_ENCRYPT_NONE;
            });
        });
    }

    Status compress(const fs::path& input, const fs::path& output,
                    const std::string& password) override {
        return withContext([&](fz_context* ctx) -> Status {
            return resaveWithOptions(ctx, input, output, password, [&](pdf_write_options* opts) {
                opts->do_compress = 1;
                opts->do_compress_images = 1;
                opts->do_compress_fonts = 1;
                opts->do_garbage = 3;   // 去重 + 重新编号
                opts->do_pretty = 0;
            }, /*subsetFonts=*/true);
        });
    }

private:
    // 建上下文 → 跑 fn → 无论结果如何都回收 ctx。
    // 返回类型跟着 fn 走，于是 Status 和 Result<T> 两种都能用。
    template <class Fn>
    auto withContext(Fn&& fn) -> decltype(fn(std::declval<fz_context*>())) {
        using Ret = decltype(fn(std::declval<fz_context*>()));
        fz_context* ctx = fz_new_context(nullptr, nullptr, FZ_STORE_DEFAULT);
        if (ctx == nullptr) {
            return Ret(Status::error(ErrorCode::Internal, "无法创建 MuPDF 上下文"));
        }
        bool registered = false;
        fz_try(ctx) {
            fz_register_document_handlers(ctx);
            registered = true;
        }
        fz_catch(ctx) { registered = false; }
        Ret st = registered ? Ret(fn(ctx))
                            : Ret(Status::error(ErrorCode::Internal, "MuPDF 文档处理器注册失败"));
        fz_drop_context(ctx);
        return st;
    }

    int maxSide_ = 4096;
};

}  // namespace

std::shared_ptr<IPdfBackend> makeMuPdfBackend(const Config& config) {
    return std::make_shared<MuPdfBackend>(config);
}

}  // namespace pw

#endif  // PDFWORD_HAVE_MUPDF
