#pragma once

#include <memory>
#include <string>
#include <vector>

#include "pdfword/config.hpp"
#include "pdfword/edit.hpp"
#include "pdfword/page_ops.hpp"
#include "pdfword/raster.hpp"
#include "pdfword/result.hpp"

namespace pw {

struct PageInfo {
    int index = 0;              // 0 基
    double widthPt = 0.0;
    double heightPt = 0.0;
    double rotationDeg = 0.0;
};

struct DocumentInfo {
    int pageCount = 0;
    bool encrypted = false;
    std::string title;
    std::string author;
    std::string backendName;    // "mupdf" / "stub"
};

// 单份已打开的 PDF。所有方法都是 const/无状态，方便 UI 多线程渲染同一文档。
class IPdfDocument {
public:
    virtual ~IPdfDocument() = default;

    virtual const DocumentInfo& info() const = 0;
    virtual Status pageInfo(int pageIndex, PageInfo* out) const = 0;
    // 逐页纯文本；页码越界返回 InvalidArgument
    virtual Status pageText(int pageIndex, std::string* out) const = 0;
    // zoom=1.0 表示 72dpi; 长边超过 Config::renderMaxSide 时后端自行降采样
    virtual Status renderPage(int pageIndex, float zoom, RasterImage* out) const = 0;
};

// PDF 后端抽象。当前只实现 MuPDF 一家，但接口留了扩展位（比如以后接 pdfium 做预览）。
class IPdfBackend {
public:
    virtual ~IPdfBackend() = default;

    virtual std::string name() const = 0;
    virtual bool available() const = 0;
    virtual std::string unavailableReason() const { return std::string(); }

    // password 为空表示不做解密尝试。加密文档在密码缺失或错误时返回 IoError，
    // 消息里会区分"需要密码"和"密码错误"。已成功解密的文档 info().encrypted 仍为 true。
    virtual Result<std::shared_ptr<IPdfDocument>> open(const fs::path& path,
                                                       const std::string& password = std::string()) = 0;

    // 文档级操作：合并 / 拆分 / 抽取页面 / 水印。都通过「重写到新 PDF」实现，不改源文件。
    virtual Status merge(const std::vector<fs::path>& inputs, const fs::path& output) = 0;
    virtual Status split(const fs::path& input, const fs::path& outDir, int pagesPerFile) = 0;
    // 抽取指定页另存为新 PDF。pages 是 0 基索引，按传入顺序排版（不是升序也行，用于重排）。
    virtual Status extract(const fs::path& input, const fs::path& output, const std::vector<int>& pages,
                           const std::string& password = std::string()) = 0;
    // 文字水印。position 决定摆放方式：
    //   九宫格单条：top-left / top-center / top-right / middle-left / center /
    //               middle-right / bottom-left / bottom-center / bottom-right
    //   单条斜放：center-diagonal（45°）
    //   铺满整页：tile-horizontal（水平网格） / tile-diagonal（45° 斜向网格，经典"机密"）
    //   opacity 0..1。未知值按 top-left 处理。
    virtual Status watermark(const fs::path& input, const fs::path& output, const std::string& text,
                             float opacity, const std::string& position = "top-left") = 0;

    // 图片水印（PNG/JPG/BMP/TIFF，PNG 的透明通道会保留）。
    //   opacity 0..1；scale 是水印宽度相对页面宽度的比例（0.05..1.0）；
    //   tile=true 时在整页按网格平铺（间距 = 水印宽的 1.6 倍），否则居中单张。
    virtual Status imageWatermark(const fs::path& input, const fs::path& output,
                                  const fs::path& image, float opacity, float scale, bool tile,
                                  const std::string& password = std::string()) = 0;

    // ---- 页面装配：删除 / 重排 / 旋转 / 裁剪 / 插入 全靠这一张表 ----
    // pages 里每一项决定输出的一页；空白页 source 为空。
    virtual Status assemble(const std::vector<PageRef>& pages, const fs::path& output,
                            const std::string& password = std::string()) = 0;

    // 图片转 PDF：每张图一页
    virtual Status imagesToPdf(const std::vector<ImagePage>& images, const fs::path& output) = 0;

    // 元数据读 / 写
    virtual Result<DocumentMetadata> metadata(const fs::path& path,
                                              const std::string& password = std::string()) = 0;
    virtual Status setMetadata(const fs::path& input, const fs::path& output,
                               const DocumentMetadata& meta,
                               const std::string& password = std::string()) = 0;

    // 加密（AES-128）/ 解密
    virtual Status encrypt(const fs::path& input, const fs::path& output,
                           const std::string& userPassword, const std::string& ownerPassword,
                           int permissions, const std::string& readPassword = std::string()) = 0;
    virtual Status decrypt(const fs::path& input, const fs::path& output,
                           const std::string& password = std::string()) = 0;

    // 压缩：字体子集化 + 流压缩 + 垃圾回收
    virtual Status compress(const fs::path& input, const fs::path& output,
                            const std::string& password = std::string()) = 0;

    // OCR：逐页渲染成位图 → 交给 OCR 引擎识别 → 把识别结果写成**不可见文字层**（/Tr 3）
    // 免得图上多出可见文字，同时 Ctrl+F 能搜到、能选中复制；另外可导出整份纯文本。
    //   language      BCP-47 语言标签（"zh-Hans-CN" / "en-US" / 空 = 跟随系统）
    //   generatePdf   是否写回一份可检索 PDF（outputPdf 为空则跳过）
    //   generateTxt   是否导出纯文本（outputTxt 为空则跳过）
    // 引擎不可用时返回 BackendMissing，消息里说明缺什么。
    virtual Status ocr(const fs::path& input, const fs::path& outputPdf, const fs::path& outputTxt,
                       const std::string& language, bool generatePdf, bool generateTxt,
                       const std::string& password = std::string()) = 0;

    // ---- 编辑与批注类（数据类型见 pdfword/edit.hpp） ----
    // 坐标统一是“页面显示空间 pt”（原点在页面左上、y 向下），与预览区和 OCR 一致。

    // 加批注：高亮 / 下划线 / 删除线 / 波浪线 / 便签 / 文本框 / 手绘。
    // 批注只是叠加对象，原页内容一个字节都不改；产物写到 output，不动 input。
    virtual Status annotate(const fs::path& input, const fs::path& output,
                            const std::vector<AnnotSpec>& annots,
                            const std::string& password = std::string()) = 0;

    // 在指定区域加超链接：外链（https://… / mailto:…）或文档内跳转（"#page=3"、"第3页"）。
    virtual Status addLink(const fs::path& input, const fs::path& output, const LinkSpec& link,
                           const std::string& password = std::string()) = 0;

    // 列出文档里的可填写字段（AcroForm）；没有表单就返回空表。
    virtual Result<std::vector<FormField>> formFields(const fs::path& input,
                                                     const std::string& password = std::string()) = 0;

    // 按字段名填值（文本 / 下拉 / 勾选），并把外观刷新成静态内容。
    virtual Status fillForm(const fs::path& input, const fs::path& output,
                            const std::vector<FieldValue>& values,
                            const std::string& password = std::string()) = 0;

    // 在页面上新建可交互表单字段（文本 / 勾选 / 下拉）。
    virtual Status createForm(const fs::path& input, const fs::path& output,
                              const std::vector<FieldSpec>& fields,
                              const std::string& password = std::string()) = 0;

    // 搜文本，返回所有命中的页号与外接框（page = -1 表示整个文档）。
    virtual Result<std::vector<TextHit>> findText(const fs::path& input, const std::string& needle,
                                                  int page = -1,
                                                  const std::string& password = std::string()) = 0;

    // 批量替换：命中处先 redaction（真删原文字），再在原框里写新文字。
    virtual Status replaceText(const fs::path& input, const fs::path& output,
                               const std::string& find, const std::string& replace,
                               const ReplaceOptions& opts = ReplaceOptions(),
                               const std::string& password = std::string()) = 0;

    // 替换图片：抹掉指定矩形里的图像，再把新图按矩形等比铺进去。
    virtual Status replaceImage(const fs::path& input, const fs::path& output, int page,
                                const RectD& rect, const fs::path& imagePath,
                                const std::string& password = std::string()) = 0;

    // 按矩形就地改写：只处理 page（0 基）上 rect 里那一块——先铺白底盖掉原文再写新字，
    // 不动这一块以外的东西，也不去整篇搜同一个词（与 replaceText 的分工就在这里）。
    // opts.deleteOnly 时只盖不写，等于"删掉这块文字"。
    virtual Status replaceTextAt(const fs::path& input, const fs::path& output, int page,
                                 const RectD& rect, const std::string& text,
                                 const ReplaceOptions& opts = ReplaceOptions(),
                                 const std::string& password = std::string()) = 0;

    // 在矩形里插入新文字：不铺白底、不碰原文，用于"预览里拖个框直接打字"。
    // opts.fontSize = 0 时按框高自适应；新文字比框宽时缩字号，而不是横向压扁。
    virtual Status addTextAt(const fs::path& input, const fs::path& output, int page,
                             const RectD& rect, const std::string& text,
                             const ReplaceOptions& opts = ReplaceOptions(),
                             const std::string& password = std::string()) = 0;

    // 书签 / 目录（/Outlines）。读出来是扁平列表（level 表示层级，page 1 基）。
    // 没有目录的文档返回空表，不是错误。
    virtual Result<std::vector<OutlineItem>> outline(const fs::path& input,
                                                     const std::string& password = std::string()) = 0;

    // 整棵目录树按 items 重建（先删掉原有全部项再按 level 逐项插入），写到 output。
    // items 为空等于"清空目录"。
    virtual Status setOutline(const fs::path& input, const fs::path& output,
                              const std::vector<OutlineItem>& items,
                              const std::string& password = std::string()) = 0;

    // 取某页"落在给定矩形内"的文字（字符中心落在框里才算）。
    // 预览里在页面上框一段文字，用它把原文带出来，改字时不用再手打一遍。
    // 框里没有文字返回空串，不算错误。
    virtual Result<std::string> textInRect(const fs::path& input, int page, const RectD& rect,
                                           const std::string& password = std::string()) = 0;
};

// 工厂：链接了 MuPDF 时返回 MuPDF 后端，否则返回 stub 后端（所有操作返回 BackendMissing）。
std::shared_ptr<IPdfBackend> makePdfBackend(const Config& config);

// 便于 CLI/UI 判断当前构建是否真的带上了 MuPDF
bool pdfBackendIsReal();

}  // namespace pw
