#include "AppIcon.h"

QIcon appIcon() {
    // 优先用 png（Qt 内置解码，稳定）；万一资源没打进来，退到 ico。
    QIcon icon(QStringLiteral(":/icons/pdftoolkit.png"));
    if (icon.isNull()) icon = QIcon(QStringLiteral(":/icons/pdftoolkit.ico"));
    return icon;
}
