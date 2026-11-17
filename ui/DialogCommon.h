#pragma once

#include <QFileInfo>
#include <QString>

// 五个文档级工具窗口共用的"默认输出名"规则：<源目录>/<源主名><后缀>.pdf
// 放在头里 inline，避免每个对话框各写一份。
inline QString suggestPdfOutput(const QString& source, const QString& suffix) {
    const QFileInfo fi(source);
    if (fi.absolutePath().isEmpty()) return QString();
    return fi.absolutePath() + QLatin1Char('/') + fi.completeBaseName() + suffix + QStringLiteral(".pdf");
}
