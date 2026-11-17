#include "pdfword/pdf_backend.hpp"

namespace pw {

namespace {

// 构建时没有 MuPDF 时使用。所有操作都返回明确的 BackendMissing，
// 让 UI / web 壳能把"没装依赖"和"文件坏了"区分开。
class StubDocument : public IPdfDocument {
public:
    explicit StubDocument(const fs::path& path) {
        info_.backendName = "stub";
        info_.title = path.filename().string();
    }
    const DocumentInfo& info() const override { return info_; }
    Status pageInfo(int, PageInfo*) const override {
        return Status::error(ErrorCode::BackendMissing, "未启用 MuPDF，无法读取页面信息");
    }
    Status pageText(int, std::string*) const override {
        return Status::error(ErrorCode::BackendMissing, "未启用 MuPDF，无法提取文字");
    }
    Status renderPage(int, float, RasterImage*) const override {
        return Status::error(ErrorCode::BackendMissing, "未启用 MuPDF，无法渲染页面");
    }

private:
    DocumentInfo info_;
};

class StubBackend : public IPdfBackend {
public:
    std::string name() const override { return "stub"; }
    bool available() const override { return false; }
    std::string unavailableReason() const override {
        return "构建时未启用 MuPDF（CMake 选项 PDFWORD_WITH_MUPDF=OFF 或未找到 SDK）";
    }

    Result<std::shared_ptr<IPdfDocument>> open(const fs::path& path,
                                               const std::string& /*password*/) override {
        return Status::error(ErrorCode::BackendMissing, unavailableReason() + "；open=" + path.string());
    }
    Status merge(const std::vector<fs::path>&, const fs::path&) override {
        return Status::error(ErrorCode::BackendMissing, unavailableReason());
    }
    Status split(const fs::path&, const fs::path&, int) override {
        return Status::error(ErrorCode::BackendMissing, unavailableReason());
    }
    Status extract(const fs::path&, const fs::path&, const std::vector<int>&,
                   const std::string&) override {
        return Status::error(ErrorCode::BackendMissing, unavailableReason());
    }
    Status watermark(const fs::path&, const fs::path&, const std::string&, float) override {
        return Status::error(ErrorCode::BackendMissing, unavailableReason());
    }
    Status assemble(const std::vector<PageRef>&, const fs::path&, const std::string&) override {
        return Status::error(ErrorCode::BackendMissing, unavailableReason());
    }
    Status imagesToPdf(const std::vector<ImagePage>&, const fs::path&) override {
        return Status::error(ErrorCode::BackendMissing, unavailableReason());
    }
    Result<DocumentMetadata> metadata(const fs::path&, const std::string&) override {
        return Status::error(ErrorCode::BackendMissing, unavailableReason());
    }
    Status setMetadata(const fs::path&, const fs::path&, const DocumentMetadata&,
                       const std::string&) override {
        return Status::error(ErrorCode::BackendMissing, unavailableReason());
    }
    Status encrypt(const fs::path&, const fs::path&, const std::string&, const std::string&, int,
                   const std::string&) override {
        return Status::error(ErrorCode::BackendMissing, unavailableReason());
    }
    Status decrypt(const fs::path&, const fs::path&, const std::string&) override {
        return Status::error(ErrorCode::BackendMissing, unavailableReason());
    }
    Status compress(const fs::path&, const fs::path&, const std::string&) override {
        return Status::error(ErrorCode::BackendMissing, unavailableReason());
    }
};

}  // namespace

std::shared_ptr<IPdfBackend> makeStubBackend() {
    return std::make_shared<StubBackend>();
}

}  // namespace pw
