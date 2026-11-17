#include "DropListWidget.h"

#include <QApplication>
#include <QDragEnterEvent>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QFileInfo>
#include <QMimeData>
#include <QUrl>

namespace {

// 允许的扩展名为空时表示"什么都收"
bool extAllowed(const QStringList& exts, const QString& path) {
    if (exts.isEmpty()) return true;
    const QString suffix = QFileInfo(path).suffix().toLower();
    return !suffix.isEmpty() && exts.contains(suffix);
}

}  // namespace

DropListWidget::DropListWidget(QWidget* parent) : QListWidget(parent) {
    setAcceptDrops(true);
    // 只接受从外面拖进来的文件；列表内部不提供拖动排序（合并窗口用上移/下移按钮）
    setDragDropMode(QAbstractItemView::DropOnly);
    setDropIndicatorShown(true);
}

void DropListWidget::setExtensions(const QStringList& exts) {
    exts_.clear();
    for (const auto& e : exts) {
        QString s = e.trimmed().toLower();
        if (s.startsWith(QLatin1Char('.'))) s.remove(0, 1);
        if (!s.isEmpty()) exts_ << s;
    }
}

QStringList DropListWidget::collectPaths(const QMimeData* mime) const {
    QStringList out;
    if (mime == nullptr || !mime->hasUrls()) return out;
    for (const QUrl& url : mime->urls()) {
        if (!url.isLocalFile()) continue;
        const QString path = QFileInfo(url.toLocalFile()).absoluteFilePath();
        if (extAllowed(exts_, path)) out << path;
    }
    return out;
}

void DropListWidget::dragEnterEvent(QDragEnterEvent* e) {
    if (!collectPaths(e->mimeData()).isEmpty()) {
        e->acceptProposedAction();
        return;
    }
    QListWidget::dragEnterEvent(e);
}

void DropListWidget::dragMoveEvent(QDragMoveEvent* e) {
    if (!collectPaths(e->mimeData()).isEmpty()) {
        e->acceptProposedAction();
        return;
    }
    QListWidget::dragMoveEvent(e);
}

void DropListWidget::dropEvent(QDropEvent* e) {
    const QStringList paths = collectPaths(e->mimeData());
    if (paths.isEmpty()) {
        QListWidget::dropEvent(e);
        return;
    }
    // 这里不自己插 item：宿主知道该加哪一列、该怎么去重
    e->acceptProposedAction();
    e->setDropAction(Qt::CopyAction);
    emit filesDropped(paths);
}
