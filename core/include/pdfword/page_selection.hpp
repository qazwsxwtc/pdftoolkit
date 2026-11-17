#pragma once

#include <string>
#include <vector>

#include "pdfword/result.hpp"

namespace pw {

// 解析用户输入的页码范围："1-3,5,8-10"（1 基、闭区间、逗号分隔）。
// 也容忍中文标点（，、－—）和空格。返回 0 基索引，保持输入顺序并去重；
// pageCount > 0 时校验上界，越界直接报 InvalidArgument。
Result<std::vector<int>> parsePageSpec(const std::string& spec, int pageCount = 0);

// 0 基索引 -> "1-3,5"（连续页合并成区间），用于日志和界面回显
std::string formatPageSpec(const std::vector<int>& pages0);

}  // namespace pw
