// 界面语言：中文 / English，默认跟随系统，可手动指定并记住。
//
// 约定：源码里写中文原文，靠 pw::i18n::t("中文原文") 取当前语言的译文；
// 英文表里查不到就回落中文原文（漏译不会变成空白或报错）。
// 中文原文就是 key，好处是代码里一眼能看懂这条文案，加一条新文案也不用先去登记 key。
#pragma once

#include <string>

namespace pw {
namespace i18n {

// 文案表（由 i18n_table.cpp 定义，一张 中文原文 -> English 的静态表）
namespace detail {
struct Entry {
    const char* zh;
    const char* en;
};
extern const Entry kEntries[];
extern const int kEntryCount;
}  // namespace detail

// Auto = 跟随系统（Windows 显示语言是中文就用中文，否则用英文）
enum class Lang { Auto, Zh, En };

// 读 pdfword.ini 的 language=auto|zh|en，并据系统语言算出当前生效语言。
// 程序启动最早期调用一次；重复调用无副作用。
void load();

// 用户在设置里选的值（可能仍是 Auto）
Lang setting();

// 当前实际生效的语言（Auto 已解析成 Zh / En）
Lang effective();

// "zh" / "en"，给需要拼进文件名、日志的地方用
const char* effectiveCode();

// 改动设置并立即写回 ini（只改 language= 这一行，其它内容原样保留）。
// 返回 false 表示 ini 写不了；内存里的设置仍然生效。
bool setSetting(Lang lang);

// 解析 "auto" / "zh" / "zh_CN" / "en" / "en_US"，其它一律当 Auto
Lang parseLang(const std::string& text);

// 取当前语言的文案；key 是中文原文
const char* t(const char* zh);

// 同上，便于拼 std::string
std::string t(const std::string& zh);

}  // namespace i18n
}  // namespace pw
