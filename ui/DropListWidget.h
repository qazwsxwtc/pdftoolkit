#pragma once

#include <QListWidget>
#include <QStringList>

class QDragEnterEvent;
class QDragMoveEvent;
class QDropEvent;
class QMimeData;

// 支持"把文件从资源管理器直接拖进来"的列表。
// 它只负责把拖入的本地文件转成 filesDropped 信号，加不加、加到哪里由宿主决定，
// 所以主窗口的文件面板、合并窗口、图片转 PDF 窗口可以共用这一个实现。
class DropListWidget : public QListWidget {
    Q_OBJECT

public:
    explicit DropListWidget(QWidget* parent = nullptr);

    // 允许的扩展名（小写、不带点）；传空表示任何文件都接受
    void setExtensions(const QStringList& exts);
    QStringList extensions() const { return exts_; }

signals:
    void filesDropped(const QStringList& paths);

protected:
    void dragEnterEvent(QDragEnterEvent* e) override;
    void dragMoveEvent(QDragMoveEvent* e) override;
    void dropEvent(QDropEvent* e) override;

private:
    // 从 mime 里挑出本地文件路径（按扩展名过滤）
    QStringList collectPaths(const QMimeData* mime) const;

    QStringList exts_;
};
