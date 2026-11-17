#ifndef PDFWORD_C_H
#define PDFWORD_C_H

// PdfWord 稳定 C ABI。
//
// 设计目的：UI（Qt）和未来的 web 壳（HTTP 服务 / WebView 桥 / FFI 到 Go、C#、Python）
// 都只依赖这一层。所有函数返回值：0 = 成功，非 0 = pw::ErrorCode 的整数值。
// 字符串一律 UTF-8；需要调用方释放的内存用 pw_free()。
//
// 线程安全：同一个 pw_context 不建议多线程并发调用（内部持有一个 Converter），
// 每个线程各自 create 一个 context 即可。

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#if defined(_WIN32)
#define PW_API __declspec(dllexport)
#else
#define PW_API __attribute__((visibility("default")))
#endif

typedef struct pw_context pw_context;

PW_API pw_context* pw_context_create(const char* config_ini_path);
PW_API void pw_context_destroy(pw_context* ctx);

// 最近一次失败的可读描述（UTF-8，生命周期随 ctx）
PW_API const char* pw_last_error(const pw_context* ctx);

// 依赖状态报告（多行文本），调用方用 pw_free 释放
PW_API const char* pw_capability_report(pw_context* ctx);

// 1 = PDF 后端真实可用
PW_API int pw_backend_available(pw_context* ctx);
PW_API const char* pw_backend_name(pw_context* ctx);

// 设置加密 PDF 的默认密码：之后 pw_pdf_page_count / page_text / render_page
// 以及 pw_convert（pdf -> docx）打开加密文档时自动使用。password 为 NULL 或空串表示清除。
PW_API int pw_set_pdf_password(pw_context* ctx, const char* password);

// 通用转换。target: "pdf" | "docx"
// produced 可为 NULL；非 NULL 时返回实际产物路径（需 pw_free）
PW_API int pw_convert(pw_context* ctx, const char* source, const char* output, const char* target,
                      char** produced);

// PDF 读取
PW_API int pw_pdf_page_count(pw_context* ctx, const char* path, int* out_count);
PW_API int pw_pdf_page_text(pw_context* ctx, const char* path, int page_index, char** out_text);
// 渲染为 RGBA8888（自上而下），调用方用 pw_free 释放 *rgba
PW_API int pw_pdf_render_page(pw_context* ctx, const char* path, int page_index, float zoom,
                              unsigned char** rgba, int* width, int* height, size_t* byte_size);

// PDF 文档级操作（MuPDF 后端）
PW_API int pw_pdf_merge(pw_context* ctx, const char* const* paths, int count, const char* output);
PW_API int pw_pdf_split(pw_context* ctx, const char* path, const char* out_dir, int pages_per_file);
// 抽取指定页另存为新 PDF。page_spec 形如 "1-3,5,8-10"（1 基、闭区间、逗号分隔）。
PW_API int pw_pdf_extract(pw_context* ctx, const char* path, const char* output, const char* page_spec);
// 按 page_spec 的顺序重排/选页（可重复同一页）
PW_API int pw_pdf_pages(pw_context* ctx, const char* path, const char* output, const char* page_spec);
// 删除 page_spec 里的页
PW_API int pw_pdf_delete_pages(pw_context* ctx, const char* path, const char* output,
                               const char* page_spec);
// 旋转 page_spec 命中的页（90 / 180 / 270，可为负）
PW_API int pw_pdf_rotate_pages(pw_context* ctx, const char* path, const char* output,
                               const char* page_spec, int rotation);
// 裁边：从四边各裁掉多少 pt
PW_API int pw_pdf_crop_pages(pw_context* ctx, const char* path, const char* output,
                             const char* page_spec, double left, double bottom, double right,
                             double top);
// 在第 after_page 页之后插入 count 个空白页（after_page=0 表示插到最前）
PW_API int pw_pdf_insert_blank(pw_context* ctx, const char* path, const char* output,
                               int after_page, int count);
// 把 other 的 page_spec 页插入到 path 的第 after_page 页之后
PW_API int pw_pdf_insert_pages(pw_context* ctx, const char* path, const char* output, int after_page,
                               const char* other, const char* page_spec);
// 多张图片合成 PDF；fit_a4 != 0 时等比缩放进 A4
PW_API int pw_images_to_pdf(pw_context* ctx, const char* const* images, int count, const char* output,
                            int fit_a4);
// 元数据以 JSON 返回（UTF-8，需 pw_free）
PW_API int pw_pdf_metadata(pw_context* ctx, const char* path, char** out_json);
PW_API int pw_pdf_set_metadata(pw_context* ctx, const char* path, const char* output, const char* title,
                               const char* author, const char* subject, const char* keywords);
PW_API int pw_pdf_watermark(pw_context* ctx, const char* path, const char* output, const char* text,
                            float opacity);

// 加密（AES-128）；permissions 用 PDFWORD_PERM_* 位组合，0 = 全部禁止
PW_API int pw_pdf_encrypt(pw_context* ctx, const char* path, const char* output,
                          const char* user_password, const char* owner_password, int permissions);
// 解密（去掉加密）；受保护文档需先 pw_set_pdf_password
PW_API int pw_pdf_decrypt(pw_context* ctx, const char* path, const char* output);
// 压缩：字体子集化 + 流压缩 + 垃圾回收
PW_API int pw_pdf_compress(pw_context* ctx, const char* path, const char* output);

// 权限位（与 PDF 规范一致，1 表示允许）
#define PDFWORD_PERM_PRINT (1 << 2)
#define PDFWORD_PERM_MODIFY (1 << 3)
#define PDFWORD_PERM_COPY (1 << 4)
#define PDFWORD_PERM_ANNOTATE (1 << 5)
#define PDFWORD_PERM_FORM (1 << 8)
#define PDFWORD_PERM_ASSEMBLE (1 << 10)
#define PDFWORD_PERM_PRINT_HQ (1 << 11)
#define PDFWORD_PERM_ALL                                                                          \
    (PDFWORD_PERM_PRINT | PDFWORD_PERM_MODIFY | PDFWORD_PERM_COPY | PDFWORD_PERM_ANNOTATE |        \
     PDFWORD_PERM_FORM | PDFWORD_PERM_ASSEMBLE | PDFWORD_PERM_PRINT_HQ)
#define PDFWORD_PERM_NONE 0

PW_API void pw_free(void* p);

#ifdef __cplusplus
}
#endif

#endif  // PDFWORD_C_H
