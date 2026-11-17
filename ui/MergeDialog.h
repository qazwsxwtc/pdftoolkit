#pragma once

#include <QDialog>
#include <QString>
#include <QStringList>

class QListWidget;
class QLineEdit;

// 合并前的文件选择窗口：列表就是最终的拼接顺序，不需要的从列表里移除。
class MergeDialog : public QDialog {
    Q_OBJECT

public:
    // candidates 一般来自主窗口"待处理文件"里已有的 PDF；空也可以，进去再添加
    explicit MergeDialog(const QStringList& candidates, QWidget* parent = nullptr);

    QStringList files() const;      // 按拼接顺序
    QString outputPath() const;

private slots:
    void addFiles();
    void removeSelected();
    void moveUp();
    void moveDown();
    void pickOutput();

private:
    void appendFile(const QString& path);
    QString suggestOutput() const;

    QListWidget* list_ = nullptr;
    QLineEdit* output_ = nullptr;
};
