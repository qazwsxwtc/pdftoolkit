#include <QApplication>
#include <memory>

#include "AppIcon.h"
#include "I18nUi.h"
#include "MainWindow.h"
#include "pdfword/converter.hpp"
#include "pdfword/i18n.hpp"
#include "pdfword/job.hpp"
#include "pdfword/path_util.hpp"

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

    return QApplication::exec();
}
