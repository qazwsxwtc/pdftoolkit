#include <QApplication>
#include <memory>

#include "MainWindow.h"
#include "pdfword/converter.hpp"
#include "pdfword/job.hpp"

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    // 只设 applicationName（内部标识/QSettings 用）；不要设 applicationDisplayName，
    // 否则 Qt 会把它拼到窗口标题后面，变成 "Pdf-Toolkit — … - Pdf-Toolkit"。
    QApplication::setApplicationName(QStringLiteral("Pdf-Toolkit"));

    // 配置与 core 完全一致：同目录 pdfword.ini / 环境变量 / 注册表自动探测
    pw::Config config =
        pw::loadConfig(std::filesystem::path(QApplication::applicationFilePath().toStdString()));

    auto converter = std::make_shared<pw::Converter>(config);
    auto jobs = std::make_shared<pw::JobQueue>(converter);

    MainWindow window(converter, jobs);
    window.show();

    return QApplication::exec();
}
