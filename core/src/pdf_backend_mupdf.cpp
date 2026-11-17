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
#include <functional>
#include <limits>
#include <string>
#include <vector>

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

// 水印：先把原页 graft 过来（矢量、字体、透明蒙版全部原样保留），
// 再把水印画成一条独立内容流追加到该页的 /Contents，资源并入 /Resources。
// 相比"整页重绘再蒸馏"，这条路径不会丢 image mask，也不改动原页内容。
Status writeWatermarked(fz_context* ctx, const fs::path& input, const fs::path& output,
                        const std::string& text, float opacity) {
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

        // 2) 把水印画进独立内容流；pdf 设备会自己把用到的字体写进 res。
        //    topctm 传页面的 pageCtm，这样坐标按“显示方向”书写（y 向下），
        //    带 /Rotate 的页面也能横着摆。
        fz_try(ctx) {
            res = pdf_new_dict(ctx, dst, 2);
            buf = fz_new_buffer(ctx, 2048);
            dev = pdf_new_pdf_device(ctx, dst, pageCtm, res, buf);

            float w = dispbox.x1 - dispbox.x0;
            float h = dispbox.y1 - dispbox.y0;
            float size = std::max(18.0f, std::min(w, h) * 0.08f);
            // topctm 里含 y 翻转，文字矩阵的 y 要取负，否则水印会上下镜像
            fz_matrix trm = fz_concat(fz_scale(size, -size),
                                      fz_translate(dispbox.x0 + w * 0.06f,
                                                   dispbox.y0 + h * 0.08f + size));
            txt = fz_new_text(ctx);
            fz_show_string(ctx, txt, font, trm, text.c_str(), 0, 0, FZ_BIDI_LTR, FZ_LANG_UNSET);
            fz_fill_text(ctx, dev, txt, fz_identity, fz_device_rgb(ctx), color, opacity,
                         fz_default_color_params);
            fz_close_device(ctx, dev);
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
                     float opacity) override {
        if (text.empty()) return Status::error(ErrorCode::InvalidArgument, "水印文本为空");
        if (opacity < 0.0f) opacity = 0.0f;
        if (opacity > 1.0f) opacity = 1.0f;
        return withContext([&](fz_context* ctx) -> Status {
            return writeWatermarked(ctx, input, output, text, opacity);
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
    template <class Fn>
    Status withContext(Fn&& fn) {
        fz_context* ctx = fz_new_context(nullptr, nullptr, FZ_STORE_DEFAULT);
        if (ctx == nullptr) return Status::error(ErrorCode::Internal, "无法创建 MuPDF 上下文");
        bool registered = false;
        fz_try(ctx) {
            fz_register_document_handlers(ctx);
            registered = true;
        }
        fz_catch(ctx) { registered = false; }
        Status st = registered ? fn(ctx) : Status::error(ErrorCode::Internal, "MuPDF 文档处理器注册失败");
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
