#pragma once

#include <QDialog>
#include <QString>
#include <memory>

#include "pdfword/pdf_backend.hpp"

class QLabel;

// 「压缩…」窗口：字体子集化 + 流压缩 + 垃圾回收，另存为体积更小的副本。
class CompressDialog : public QDialog {
    Q_OBJECT

public:
    CompressDialog(std::shared_ptr<pw::IPdfBackend> backend, const QString& source,
                   QWidget* parent = nullptr);

signals:
    void logMessage(const QString& text);

private slots:
    void compressPdf();

private:
    std::shared_ptr<pw::IPdfBackend> backend_;
    QString source_;
    QLabel* sizeLabel_ = nullptr;
};
