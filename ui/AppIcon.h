#pragma once

#include <QIcon>

// 应用图标（logo）：打包在 resources/icons.qrc 里，路径 ":/icons/pdftoolkit.png"。
// 主程序调用 QApplication::setWindowIcon(appIcon())；MainWindow 也会自己设一次，
// 这样冒烟测试（不经过 main）同样能拿到图标、关于窗口里也能显示同一个 logo。
QIcon appIcon();
