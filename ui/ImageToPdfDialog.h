#pragma once

#include <QDialog>
#include <QString>
#include <memory>

#include "pdfword/pdf_backend.hpp"

class QCheckBox;
class QListWidget;

// 「图片转 PDF…」窗口：多张图片按列表顺序合成一个 PDF。
// 不依赖源 PDF，构造时只给一个默认输出目录。
class ImageToPdfDialog : public QDialog {
    Q_OBJECT

public:
    ImageToPdfDialog(std::shared_ptr<pw::IPdfBackend> backend, const QString& suggestedDir,
                     QWidget* parent = nullptr);

signals:
    void logMessage(const QString& text);

private slots:
    void addImages();
    void removeImages();
    void imagesToPdf();

private:
    // 把一批图片路径加进列表（按绝对路径去重），文件对话框与拖放共用
    void addImagePaths(const QStringList& paths);

private:
    std::shared_ptr<pw::IPdfBackend> backend_;
    QString dir_;

    QListWidget* imageList_ = nullptr;
    QCheckBox* fitA4_ = nullptr;
};
