#include <QApplication>
#include <QFileInfo>
#include <QStringList>
#include <memory>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>
#endif

#include "AppIcon.h"
#include "I18nUi.h"
#include "MainWindow.h"
#include "pdfword/converter.hpp"
#include "pdfword/i18n.hpp"
#include "pdfword/job.hpp"
#include "pdfword/path_util.hpp"

namespace {

// 从资源管理器"打开方式"、双击 PDF、拖到 exe 图标上启动时，Windows 会把文件路径塞进命令行。
// Windows 上优先取宽字符版命令行：ANSI main() 的 argv 受系统代码页影响，
// 中文/日文/阿拉伯文路径在非对应代码页的机器上会被读坏。
QStringList commandLineArgs(int argc, char** argv) {
    QStringList args;
#ifdef _WIN32
    int wideArgc = 0;
    LPWSTR* wideArgv = ::CommandLineToArgvW(::GetCommandLineW(), &wideArgc);
    if (wideArgv != nullptr) {
        for (int i = 1; i < wideArgc; ++i) {
            args << QString::fromWCharArray(wideArgv[i]);
        }
        ::LocalFree(reinterpret_cast<HLOCAL>(wideArgv));
        return args;
    }
#endif
    for (int i = 1; i < argc; ++i) {
        args << QString::fromLocal8Bit(argv[i]);
    }
    return args;
}

// 只留下真实存在的文件，顺手跳过 -style 之类的程序开关（Windows 绝对路径不会以 "-" 开头）
QStringList existingPaths(const QStringList& args) {
    QStringList files;
    for (const QString& arg : args) {
        if (arg.isEmpty() || arg.startsWith(QLatin1Char('-'))) continue;
        if (QFileInfo::exists(arg)) files << arg;
    }
    return files;
}

}  // namespace

int main(int argc, char** argv) {
    // 界面语言要在建任何窗口之前定下来：ini 里 language=auto|zh|en，
    // auto 就看系统显示语言（中文系统用中文，其它一律用 English）。
    pw::i18n::load();

    QApplication app(argc, argv);
    // 只设 applicationName（内部标识/QSettings 用）；不要设 applicationDisplayName，
    // 否则 Qt 会把它拼到窗口标题后面，变成 "Pdf-Toolkit — … - Pdf-Toolkit"。
    QApplication::setApplicationName(QStringLiteral("Pdf-Toolkit"));
    // 任务栏 / 标题栏 / Alt-Tab 用的图标（logo 已编进 :/icons 资源）
    QApplication::setWindowIcon(appIcon());

    // 配置与 core 完全一致：同目录 pdfword.ini / 环境变量 / 注册表自动探测
    pw::Config config =
        pw::loadConfig(pw::fsPath(QApplication::applicationFilePath().toUtf8().toStdString()));

    auto converter = std::make_shared<pw::Converter>(config);
    auto jobs = std::make_shared<pw::JobQueue>(converter);

    // 窗口放在堆上（关窗即销毁）：换界面语言时主窗口会重建一个，自己把自己关掉，
    // 所以要能随 close() 自动 delete。
    auto* window = new MainWindow(converter, jobs);
    window->setAttribute(Qt::WA_DeleteOnClose);
    window->show();

    // 命令行带来的文件（"打开方式"/双击 PDF）：入列并预览第一个 PDF
    const QStringList startupFiles = existingPaths(commandLineArgs(argc, argv));
    if (!startupFiles.isEmpty()) {
        window->openFilesFromCommandLine(startupFiles);
    }

    return QApplication::exec();
}
