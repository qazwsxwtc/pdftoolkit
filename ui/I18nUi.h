// UI 侧取文案的唯一入口：TR("中文原文") -> 当前语言（中文/English）。
//
// 只在"要显示给人看的文字"上用 TR；给 core 传参、写日志格式串、拼文件后缀这类
// 不进界面的字符串保持原样。
#pragma once

#include <QString>

#include "pdfword/i18n.hpp"

#define TR(x) QString::fromStdString(::pw::i18n::t(x))
