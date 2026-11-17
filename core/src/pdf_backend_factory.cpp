#include "pdfword/pdf_backend.hpp"

namespace pw {

// 在 stub 后端实现文件中定义（同一命名空间下的内部符号）
std::shared_ptr<IPdfBackend> makeStubBackend();

#if defined(PDFWORD_HAVE_MUPDF)
std::shared_ptr<IPdfBackend> makeMuPdfBackend(const Config& config);
#endif

std::shared_ptr<IPdfBackend> makePdfBackend(const Config& config) {
#if defined(PDFWORD_HAVE_MUPDF)
    auto real = makeMuPdfBackend(config);
    if (real && real->available()) return real;
#endif
    (void)config;
    return makeStubBackend();
}

bool pdfBackendIsReal() {
#if defined(PDFWORD_HAVE_MUPDF)
    return true;
#else
    return false;
#endif
}

}  // namespace pw
