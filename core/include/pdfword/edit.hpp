#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "pdfword/result.hpp"

namespace pw {

// ---------------------------------------------------------------------------
// 编辑与批注类功能的数据类型。
// 所有坐标都是**页面显示空间**的 pt：原点在页面左上角、y 轴向下，与预览区
// 以及 OCR 的坐标系统一（backend 内部再用 pageCtm 的逆变换换成 PDF 用户空间），
// 于是界面里"在预览上拖出来的框"可以原样传给 core。
// ---------------------------------------------------------------------------

struct PointD {
    double x = 0.0;
    double y = 0.0;
};

struct RectD {
    double x0 = 0.0;
    double y0 = 0.0;
    double x1 = 0.0;
    double y1 = 0.0;

    bool empty() const { return x1 - x0 < 0.5 || y1 - y0 < 0.5; }
    double width() const { return x1 - x0; }
    double height() const { return y1 - y0; }
};

enum class AnnotKind {
    Highlight,   // 高亮
    Underline,   // 下划线
    StrikeOut,   // 删除线
    Squiggly,    // 波浪线
    Note,        // 便签（贴在页边的黄色小图标，内容在弹窗里）
    FreeText,    // 文本框（直接画在页面上）
    Ink,         // 手绘标记（一串折线）
};

// 一条批注。page 是 0-based 页号。
struct AnnotSpec {
    AnnotKind kind = AnnotKind::Highlight;
    int page = 0;
    RectD rect;
    std::string text;     // Note / FreeText 的内容，其它类型忽略
    std::string colorHex; // "#RRGGBB"；空 = 用默认色（高亮黄、其余红色）
    std::string author;   // 空 = "Pdf-Toolkit"
    double opacity = 1.0;
    // Ink 专用：一条或多条折线（笔迹）。每条至少两个点。
    std::vector<std::vector<PointD>> inkPaths;
};

// 超链接：uri 可以是外链（https://…、mailto:…）或文档内跳转（"#page=3"、"#page=5&zoom=1.2"）。
// 页面内跳转也支持 "第3页" 这种中文写法，由 backend 统一规整成 MuPDF 认识的 "#page=N"。
struct LinkSpec {
    int page = 0;
    RectD rect;
    std::string uri;
};

enum class FieldKind { Text, Checkbox, Choice, Signature, Unknown };

// 表单里的一个字段（读表单时返回，也可以用来做"填写"的目标）。
struct FormField {
    std::string name;
    FieldKind kind = FieldKind::Unknown;
    std::string value;
    int page = 0;
    RectD rect;
    bool readOnly = false;
    std::vector<std::string> options;  // Choice 类型的候选项
};

struct FieldValue {
    std::string name;
    std::string value;  // Checkbox 用 "1"/"0"（也认 "true"/"false"/"on"/"off"）
};

// 制作新表单字段。
struct FieldSpec {
    std::string name;
    FieldKind kind = FieldKind::Text;
    int page = 0;
    RectD rect;
    std::vector<std::string> options;  // Choice 的候选项（第一项当默认值）
    std::string label;                 // 留空用 name
};

// 文本查找的一个命中位置（rect 是整段匹配的外接框，可能跨行）。
struct TextHit {
    int page = 0;
    RectD rect;
    std::string text;
};

// 查找替换的可选项。
struct ReplaceOptions {
    bool deleteOnly = false;         // 只删不写：白底覆盖原文（见 replaceText 的说明）
    double fontSize = 0.0;           // 0 = 按命中区域高度自适应
    std::string colorHex;            // 空 = 黑色
    int limit = 0;                   // 0 = 全部替换，>0 只处理前 N 处
    bool caseSensitive = false;
};

// ---------------------------------------------------------------------------
// 书签 / 目录（PDF 里的 /Outlines，也就是阅读器左侧那棵目录树）
// 扁平化表示：按显示顺序一行一项，level 是层级（0 = 顶级，1 = 子项 …）。
//   level 只能比上一项大 1（跳级会在 backend 里被夹成"上一项 + 1"）。
// page 是 **1 基**页码，和界面上的页码框一致；0 或负数表示"没有跳转目标"。
// ---------------------------------------------------------------------------
struct OutlineItem {
    std::string title;
    int page = 1;
    int level = 0;
};

}  // namespace pw
