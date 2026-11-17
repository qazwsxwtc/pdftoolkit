#ifndef PDFWORD_C_H
#define PDFWORD_C_H

// Pdf-Toolkit 稳定 C ABI（库文件名沿用 pdfword.dll）。
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
// 文字水印（可指定摆放位置）：position 取
//   top-left / top-center / top-right / middle-left / center / middle-right /
//   bottom-left / bottom-center / bottom-right / center-diagonal /
//   tile-horizontal / tile-diagonal
// 传 nullptr 或空串等于 top-left；不认识的值也按 top-left
PW_API int pw_pdf_watermark_position(pw_context* ctx, const char* path, const char* output,
                                     const char* text, float opacity, const char* position);
// 图片水印：scale 是水印宽度占页宽的比例（0.05..1），tile 非 0 时网格平铺
PW_API int pw_pdf_image_watermark(pw_context* ctx, const char* path, const char* output,
                                  const char* image, float opacity, float scale, int tile);

// 加密（AES-128）；permissions 用 PDFWORD_PERM_* 位组合，0 = 全部禁止
PW_API int pw_pdf_encrypt(pw_context* ctx, const char* path, const char* output,
                          const char* user_password, const char* owner_password, int permissions);
// 解密（去掉加密）；受保护文档需先 pw_set_pdf_password
PW_API int pw_pdf_decrypt(pw_context* ctx, const char* path, const char* output);
// 压缩：字体子集化 + 流压缩 + 垃圾回收
PW_API int pw_pdf_compress(pw_context* ctx, const char* path, const char* output);

// OCR 文字识别：扫描件/图片型 PDF → 可检索 PDF（写入不可见文字层，能搜索和复制）+ 纯文本。
//   output_pdf / output_txt 传 NULL 表示这一类不要（两者不能同时为 NULL）
//   language 传 NULL 或 "" 表示跟随引擎默认；Windows 内置 OCR 一般用 "zh-Hans-CN"
PW_API int pw_pdf_ocr(pw_context* ctx, const char* path, const char* output_pdf,
                      const char* output_txt, const char* language);
// 当前 OCR 引擎能力报告（引擎名、可用语言、不可用原因），UTF-8，指针由库持有，不要 free
PW_API const char* pw_ocr_capability(pw_context* ctx);

// ---------------------------------------------------------------------------
// 编辑与批注类（5 组功能）
// 坐标一律是**页面显示空间**的 pt：原点在页面左上角、y 轴向下，page_index 是 0 基页号。
// ---------------------------------------------------------------------------

// 批注类型（pw_pdf_annotate 的 kind 参数）
#define PDFWORD_ANNOT_HIGHLIGHT 0
#define PDFWORD_ANNOT_UNDERLINE 1
#define PDFWORD_ANNOT_STRIKEOUT 2
#define PDFWORD_ANNOT_SQUIGGLY 3
#define PDFWORD_ANNOT_NOTE 4
#define PDFWORD_ANNOT_FREETEXT 5
#define PDFWORD_ANNOT_INK 6

// 加一条批注。text 用于便签/文本框；color 形如 "#FF0000"（NULL = 默认色）；
// author 可为 NULL；opacity 取 0..1；ink 仅手绘用，格式 "x1,y1;x2,y2;x3,y3|..."（
// 分号分隔点、竖线分隔多笔），其它类型传 NULL。
PW_API int pw_pdf_annotate(pw_context* ctx, const char* path, const char* output, int page_index,
                           int kind, double x0, double y0, double x1, double y1, const char* text,
                           const char* color, const char* author, double opacity, const char* ink);

// 加一个超链接区域。uri 形如 "https://example.com"、"mailto:a@b.c" 或页内跳转
// "#page=3"（也认 "第3页" / "P3" / 纯数字）。
PW_API int pw_pdf_add_link(pw_context* ctx, const char* path, const char* output, int page_index,
                           double x0, double y0, double x1, double y1, const char* uri);

// 查找文字。page_index < 0 表示全文。命中结果以文本返回（需 pw_free），每行一条：
//   "<0基页号>\t<x0>\t<y0>\t<x1>\t<y1>\t<命中文字>"
// 返回值为命中条数；负数表示失败（与 pw_last_error 对应）。
PW_API int pw_pdf_find_text(pw_context* ctx, const char* path, const char* needle, int page_index,
                            char** out_text);

// 批量替换文字（覆盖式：白底盖掉原文后压上新文字，原文字仍在底层，需要彻底抹除请用脱敏）。
// delete_only 非 0 时只盖不写。font_size <= 0 表示自适应。limit <= 0 表示不限条数。
PW_API int pw_pdf_replace_text(pw_context* ctx, const char* path, const char* output, const char* find,
                               const char* replace, int delete_only, double font_size,
                               const char* color, int limit, int case_sensitive);

// 替换某页某块区域里的图片（白底盖掉原内容后铺新图，等比居中）
PW_API int pw_pdf_replace_image(pw_context* ctx, const char* path, const char* output, int page_index,
                                double x0, double y0, double x1, double y1, const char* image);

// 读取表单字段，JSON 数组（需 pw_free）：
//   [{"name":"","kind":"text|checkbox|choice|signature|unknown","value":"","page":0,
//     "rect":[x0,y0,x1,y1],"readOnly":false,"options":["..."]}]
// 无表单字段时返回空数组。失败返回负值。
PW_API int pw_pdf_form_fields(pw_context* ctx, const char* path, char** out_json);

// 填写表单：names/values 为等长数组，按字段名逐个赋值
PW_API int pw_pdf_form_fill(pw_context* ctx, const char* path, const char* output,
                            const char* const* names, const char* const* values, int count);

// 制作可交互表单。specs 每个元素形如
//   "name:type:page:x0,y0,x1,y1[:opt1|opt2]"
// （type 取 text/checkbox/choice；page 为 1 基页号；choice 的选项用 | 分隔）
PW_API int pw_pdf_form_create(pw_context* ctx, const char* path, const char* output,
                              const char* const* specs, int count);

// 读取书签 / 目录，返回条数；out_json（需 pw_free）形如
//   [{"title":"","page":1,"level":0}]
// page 为 1 基页码，0 表示没有跳转目标；level 0 为顶级。没有书签时返回空数组。
PW_API int pw_pdf_outline(pw_context* ctx, const char* path, char** out_json);

// 写入 / 重建书签目录。specs 每个元素形如 "level:page:title"
// （只按前两个冒号切分，标题里可以含冒号；page 传 0 表示没有跳转目标）
// count <= 0 等于清空目录。
PW_API int pw_pdf_set_outline(pw_context* ctx, const char* path, const char* output,
                              const char* const* specs, int count);

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
