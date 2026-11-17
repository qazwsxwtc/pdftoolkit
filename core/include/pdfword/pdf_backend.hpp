#pragma once

#include <memory>
#include <string>
#include <vector>

#include "pdfword/config.hpp"
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
    virtual Status watermark(const fs::path& input, const fs::path& output, const std::string& text,
                             float opacity) = 0;

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
};

// 工厂：链接了 MuPDF 时返回 MuPDF 后端，否则返回 stub 后端（所有操作返回 BackendMissing）。
std::shared_ptr<IPdfBackend> makePdfBackend(const Config& config);

// 便于 CLI/UI 判断当前构建是否真的带上了 MuPDF
bool pdfBackendIsReal();

}  // namespace pw
