#pragma once

#include <QDialog>
#include <QString>
#include <memory>

#include "pdfword/pdf_backend.hpp"

class QLineEdit;

// 「元数据…」窗口：读写 trailer /Info 里的标题、作者、主题、关键词。
class MetadataDialog : public QDialog {
    Q_OBJECT

public:
    MetadataDialog(std::shared_ptr<pw::IPdfBackend> backend, const QString& source,
                   QWidget* parent = nullptr);

signals:
    void logMessage(const QString& text);

private slots:
    void loadFromFile();
    void saveAs();

private:
    std::shared_ptr<pw::IPdfBackend> backend_;
    QString source_;

    QLineEdit* title_ = nullptr;
    QLineEdit* author_ = nullptr;
    QLineEdit* subject_ = nullptr;
    QLineEdit* keywords_ = nullptr;
};
