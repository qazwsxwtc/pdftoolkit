#pragma once

#include <QDialog>
#include <QString>
#include <memory>

#include "pdfword/pdf_backend.hpp"

class QLineEdit;
class QCheckBox;
class QDoubleSpinBox;
class QListWidget;

// 文档级工具：元数据 / 加密 / 解密 / 压缩 / 水印 / 图片转 PDF。
// 全部落到 core 的 IPdfBackend，界面只负责收集参数与选输出路径。
class DocumentToolsDialog : public QDialog {
    Q_OBJECT

public:
    DocumentToolsDialog(std::shared_ptr<pw::IPdfBackend> backend, const QString& source,
                        QWidget* parent = nullptr);

signals:
    void logMessage(const QString& text);

private slots:
    void loadMetadata();
    void saveMetadata();
    void encryptPdf();
    void decryptPdf();
    void compressPdf();
    void addWatermark();
    void addImages();
    void removeImages();
    void imagesToPdf();

private:
    QString suggest(const QString& suffix) const;

    std::shared_ptr<pw::IPdfBackend> backend_;
    QString source_;

    QLineEdit* title_ = nullptr;
    QLineEdit* author_ = nullptr;
    QLineEdit* subject_ = nullptr;
    QLineEdit* keywords_ = nullptr;

    QLineEdit* userPassword_ = nullptr;
    QLineEdit* ownerPassword_ = nullptr;
    QCheckBox* allowPrint_ = nullptr;
    QCheckBox* allowCopy_ = nullptr;
    QCheckBox* allowModify_ = nullptr;
    QCheckBox* allowAnnotate_ = nullptr;

    QLineEdit* watermarkText_ = nullptr;
    QDoubleSpinBox* watermarkOpacity_ = nullptr;

    QListWidget* imageList_ = nullptr;
    QCheckBox* fitA4_ = nullptr;
};
