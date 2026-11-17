#pragma once

#include <memory>
#include <string>
#include <vector>

#include "pdfword/raster.hpp"
#include "pdfword/result.hpp"

namespace pw {

// OCR 的一个词。坐标是**页面显示空间**（原点在页面左上角，y 向下，单位 pt），
// 与 MuPDF 渲染出来的位图一一对应，写文字层时直接可换算。
struct OcrWord {
    double x0 = 0.0;
    double y0 = 0.0;
    double x1 = 0.0;
    double y1 = 0.0;
    std::string text;  // UTF-8
};

struct OcrLine {
    std::vector<OcrWord> words;  // 行内按从左到右
    std::string text;            // 已按空格拼好的一行
    double x0 = 0.0;
    double y0 = 0.0;
    double x1 = 0.0;
    double y1 = 0.0;
};

struct OcrPageResult {
    int pageIndex = 0;
    std::vector<OcrLine> lines;
    std::string text;  // 整页纯文本（行间用 \n，末尾也有 \n）
};

// OCR 引擎抽象。Windows 内置 OCR（Windows.Media.Ocr）是默认实现，离线、不额外下发模型；
// 换 Tesseract / PaddleOCR 只需再实现一个类，backend 与 UI 都不用动。
class IOcrEngine {
public:
    virtual ~IOcrEngine() = default;

    virtual std::string name() const = 0;
    virtual bool available() const = 0;
    virtual std::string unavailableReason() const { return std::string(); }
    // 引擎支持的识别语言（BCP-47，如 zh-Hans-CN / en-US）；Windows 引擎取系统已装的语言。
    virtual std::vector<std::string> languages() const = 0;

    // image 是页面栅格（RGBA8888，自上而下）；pageWidthPt/pageHeightPt 是这页显示尺寸，
    // 引擎据此把像素坐标换算成 pt 写进 out。
    virtual Status recognize(const RasterImage& image, double pageWidthPt, double pageHeightPt,
                             const std::string& language, OcrPageResult* out) = 0;
};

// 返回当前平台可用的 OCR 引擎：Windows 用内置 OCR；其它平台返回 nullptr。
std::shared_ptr<IOcrEngine> makeOcrEngine();

// 一行人类可读的探测结果，给 CLI doctor / 界面日志用。
// 例："Windows OCR（可用）：语言 zh-Hans-CN" / "OCR 不可用：未找到 ocr 引擎"
std::string ocrCapabilityReport();

}  // namespace pw
