// Windows 内置 OCR（Windows.Media.Ocr）实现。
// 优点：离线、不需要下发模型文件、识别质量对印刷体够好，并且直接给出词级矩形，
// 正好用来把文字写回 PDF 做不可见文字层。
// 编译依赖：Windows 10 SDK 自带的 cppwinrt 头文件 + WindowsApp.lib（CMake 里已配）。
#include "pdfword/ocr.hpp"

#include <algorithm>
#include <string>

#if defined(_WIN32) && defined(PDFWORD_HAVE_WINRT_OCR)

#include <winrt/base.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Globalization.h>
#include <winrt/Windows.Graphics.Imaging.h>
#include <winrt/Windows.Media.Ocr.h>
#include <winrt/Windows.Security.Cryptography.h>
#include <winrt/Windows.Storage.Streams.h>

namespace wocr = winrt::Windows::Media::Ocr;
namespace wgi = winrt::Windows::Graphics::Imaging;
namespace wgl = winrt::Windows::Globalization;
namespace wsc = winrt::Windows::Security::Cryptography;

namespace {

// C++/WinRT 要求每个使用 WinRT 的线程先 init_apartment。同一线程重复初始化同类公寓是允许的，
// 这里用 thread_local 挡一下，避免每页都调一次。
void ensureApartment() {
    static thread_local bool inited = false;
    if (inited) return;
    try {
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
        inited = true;
    } catch (winrt::hresult_error const&) {
        // COM 已经被别人初始化成别的公寓（例如 Qt 的 STA），直接用即可
        inited = true;
    }
}

std::string hstringToUtf8(winrt::hstring const& value) {
    return winrt::to_string(value);
}

}  // namespace

namespace pw {

class WindowsOcrEngine : public IOcrEngine {
public:
    std::string name() const override { return "windows-ocr"; }

    bool available() const override {
        ensureApartment();
        try {
            return wocr::OcrEngine::AvailableRecognizerLanguages().Size() > 0;
        } catch (winrt::hresult_error const&) {
            return false;
        }
    }

    std::string unavailableReason() const override {
        return "系统没有可用的 OCR 语言包（设置 → 时间和语言 → 语言 → 中文(简体) 的“光学字符识别”组件）";
    }

    std::vector<std::string> languages() const override {
        std::vector<std::string> tags;
        ensureApartment();
        try {
            for (auto const& lang : wocr::OcrEngine::AvailableRecognizerLanguages()) {
                tags.push_back(hstringToUtf8(lang.LanguageTag()));
            }
        } catch (winrt::hresult_error const&) {
        }
        return tags;
    }

    Status recognize(const RasterImage& image, double pageWidthPt, double pageHeightPt,
                     const std::string& language, OcrPageResult* out) override {
        if (out == nullptr) return Status::error(ErrorCode::InvalidArgument, "out 为空");
        if (image.empty()) return Status::error(ErrorCode::InvalidArgument, "页面位图为空");
        ensureApartment();

        try {
            winrt::Windows::Media::Ocr::OcrEngine engine{nullptr};
            if (!language.empty()) {
                engine = wocr::OcrEngine::TryCreateFromLanguage(wgl::Language(winrt::to_hstring(language)));
                if (!engine) {
                    return Status::error(ErrorCode::Unsupported,
                                         "系统没有安装 " + language + " 的 OCR 语言包");
                }
            }
            if (!engine) engine = wocr::OcrEngine::TryCreateFromUserProfileLanguages();
            if (!engine) {
                return Status::error(ErrorCode::Unsupported,
                                     "系统没有可用的 OCR 语言包（设置里给中文(简体)装上“光学字符识别”）");
            }

            // Windows OCR 要 BGRA8；我们的栅格是 RGBA8888，逐像素换一下通道
            std::vector<std::uint8_t> bgra(image.rgba.size());
            for (std::size_t i = 0; i + 3 < image.rgba.size(); i += 4) {
                bgra[i + 0] = image.rgba[i + 2];
                bgra[i + 1] = image.rgba[i + 1];
                bgra[i + 2] = image.rgba[i + 0];
                bgra[i + 3] = image.rgba[i + 3];
            }
            winrt::array_view<const std::uint8_t> view(&bgra[0], &bgra[0] + bgra.size());
            auto buffer = wsc::CryptographicBuffer::CreateFromByteArray(view);
            auto bitmap = wgi::SoftwareBitmap::CreateCopyFromBuffer(buffer, wgi::BitmapPixelFormat::Bgra8,
                                                                   image.width, image.height);

            auto result = engine.RecognizeAsync(bitmap).get();

            // 像素 → pt。渲染是等比缩放，两个方向比例一致，但分别算更保险。
            const double sx = (image.width > 0) ? pageWidthPt / double(image.width) : 1.0;
            const double sy = (image.height > 0) ? pageHeightPt / double(image.height) : 1.0;

            out->lines.clear();
            out->text.clear();
            for (auto const& line : result.Lines()) {
                OcrLine outLine;
                outLine.x0 = 1e9;
                outLine.y0 = 1e9;
                outLine.x1 = -1e9;
                outLine.y1 = -1e9;

                std::string joined;
                for (auto const& word : line.Words()) {
                    const auto rect = word.BoundingRect();
                    if (rect.Width <= 0 || rect.Height <= 0) continue;
                    OcrWord w;
                    w.x0 = double(rect.X) * sx;
                    w.y0 = double(rect.Y) * sy;
                    w.x1 = double(rect.X + rect.Width) * sx;
                    w.y1 = double(rect.Y + rect.Height) * sy;
                    w.text = hstringToUtf8(word.Text());
                    if (w.text.empty()) continue;

                    outLine.words.push_back(w);
                    outLine.x0 = std::min(outLine.x0, w.x0);
                    outLine.y0 = std::min(outLine.y0, w.y0);
                    outLine.x1 = std::max(outLine.x1, w.x1);
                    outLine.y1 = std::max(outLine.y1, w.y1);

                    // 中文行内不加空格，拉丁词之间补一个空格（Windows OCR 给的词已经切好）
                    if (!joined.empty() && !needsNoSpace(joined.back(), w.text.front())) joined.push_back(' ');
                    joined += w.text;
                }
                if (outLine.words.empty()) continue;

                outLine.text = joined;
                out->text += joined;
                out->text.push_back('\n');
                out->lines.push_back(std::move(outLine));
            }
            return Status::success();
        } catch (winrt::hresult_error const& e) {
            return Status::error(ErrorCode::ProcessFailed, "Windows OCR 失败: " + hstringToUtf8(e.message()));
        } catch (std::exception const& e) {
            return Status::error(ErrorCode::Internal, std::string("Windows OCR 失败: ") + e.what());
        }
    }

private:
    // 前后都是 CJK 时不插空格，否则复制出来的中文会多出空格
    static bool needsNoSpace(char prev, char next) {
        const bool prevCjk = static_cast<unsigned char>(prev) >= 0x80;
        const bool nextCjk = static_cast<unsigned char>(next) >= 0x80;
        return prevCjk || nextCjk;
    }
};

}  // namespace pw

namespace pw {

std::shared_ptr<IOcrEngine> makeOcrEngine() {
    auto engine = std::make_shared<WindowsOcrEngine>();
    if (engine->available()) return engine;
    return nullptr;
}

std::string ocrCapabilityReport() {
    auto engine = std::make_shared<WindowsOcrEngine>();
    if (!engine->available()) return "OCR 不可用：" + engine->unavailableReason();
    std::string langs;
    for (const std::string& tag : engine->languages()) {
        if (!langs.empty()) langs += ", ";
        langs += tag;
    }
    return "Windows OCR（可用）：语言 " + langs;
}

}  // namespace pw

#else  // 非 Windows，或没打开 PDFWORD_HAVE_WINRT_OCR

namespace pw {

std::shared_ptr<IOcrEngine> makeOcrEngine() { return nullptr; }

std::string ocrCapabilityReport() { return "OCR 不可用：当前平台没有接入 OCR 引擎"; }

}  // namespace pw

#endif
