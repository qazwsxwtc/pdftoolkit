#pragma once

#include <filesystem>
#include <string>

namespace pw {

namespace fs = std::filesystem;

// 全部外部依赖路径都从这里注入，core 里不出现任何硬编码路径。
// 解析优先级：命令行/代码显式设置 > 环境变量 > 可执行文件同目录 pdfword.ini
//            > 注册表（LibreOffice 安装信息）> 常见安装目录 > PATH
struct Config {
    fs::path executableDir;      // 可执行文件所在目录
    fs::path libreOfficePath;    // soffice.exe 的绝对路径；空 = 未配置
    fs::path mupdfRuntimeDir;    // 便携版 MuPDF 动态库目录（仅动态链接时需要）
    fs::path workDir;            // 隔离 profile / 临时文件根目录
    int officeTimeoutMs = 180000;
    int renderMaxSide = 4096;    // 单页渲染长边上限，防止超大页面炸内存

    bool libreOfficeConfigured() const { return !libreOfficePath.empty(); }
};

// exePath: 用于定位同目录 pdfword.ini 与默认 workDir。
// configFile: 显式指定的 ini；为空则按上面优先级自动查找。
Config loadConfig(const fs::path& exePath, const fs::path& configFile = {});

// 校验候选路径是不是可用的 LibreOffice 入口。
// 传入目录时会尝试拼接 program/soffice.exe。
bool probeLibreOffice(const fs::path& candidate, fs::path* resolved, std::string* why);

// 供 CLI/UI 展示的一行状态文本
std::string describeLibreOffice(const Config& config);

}  // namespace pw
