#pragma once

#include <string>
#include <vector>

#include "pdfword/config.hpp"
#include "pdfword/result.hpp"

namespace pw {

// 输出 PDF 的一页从哪来。
// 删除 / 重排 / 旋转 / 裁剪 / 插入空白页 / 插入其他 PDF 页 / 合并 / 拆分 / 抽取，
// 全部只是这张表的填法不同，后端只实现一次装配就够了。
struct PageRef {
    fs::path source;            // 空 = 空白页
    int pageIndex = -1;         // source 内的 0 基页号
    int rotateDelta = 0;        // 叠加在原页 /Rotate 之上，90 的倍数（可为负）
    bool hasCrop = false;       // 是否按下面四个边距裁剪
    double cropLeft = 0.0;      // 单位 pt：从左边裁掉多少
    double cropBottom = 0.0;
    double cropRight = 0.0;
    double cropTop = 0.0;
    double blankWidthPt = 595.0;    // 空白页尺寸，默认 A4
    double blankHeightPt = 842.0;
};

// 图片转 PDF 的一页
struct ImagePage {
    fs::path path;
    double dpi = 96.0;          // 像素 -> pt 的换算基准
    bool fitA4 = false;         // true：等比缩放到 A4 页面内并居中
};

struct DocumentMetadata {
    std::string title;
    std::string author;
    std::string subject;
    std::string keywords;
    std::string creator;
    std::string producer;
};

// 权限位与 PDF 规范一致（1 表示允许）。PermAll 之外的值会被后端按规范补齐保留位。
enum Permission : int {
    PermPrint = 1 << 2,
    PermModify = 1 << 3,
    PermCopy = 1 << 4,
    PermAnnotate = 1 << 5,
    PermForm = 1 << 8,
    PermAssemble = 1 << 10,
    PermPrintHq = 1 << 11,
    PermAll = PermPrint | PermModify | PermCopy | PermAnnotate | PermForm | PermAssemble | PermPrintHq,
    PermNone = 0,
};

}  // namespace pw
