#pragma once

#include <QDialog>
#include <QString>
#include <memory>

#include "pdfword/pdf_backend.hpp"

class QCheckBox;
class QLineEdit;

// 「加密 / 解密…」窗口：AES-128 加密（可勾权限）与移除打开密码。
class SecurityDialog : public QDialog {
    Q_OBJECT

public:
    SecurityDialog(std::shared_ptr<pw::IPdfBackend> backend, const QString& source,
                   QWidget* parent = nullptr);

signals:
    void logMessage(const QString& text);

private slots:
    void encryptPdf();
    void decryptPdf();

private:
    std::shared_ptr<pw::IPdfBackend> backend_;
    QString source_;

    QLineEdit* userPassword_ = nullptr;
    QLineEdit* ownerPassword_ = nullptr;
    QCheckBox* allowPrint_ = nullptr;
    QCheckBox* allowCopy_ = nullptr;
    QCheckBox* allowModify_ = nullptr;
    QCheckBox* allowAnnotate_ = nullptr;
};
