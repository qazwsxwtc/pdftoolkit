#pragma once

#include <QDialog>
#include <QString>
#include <memory>

#include "pdfword/pdf_backend.hpp"

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QLineEdit;
class QTabWidget;

// 「水印…」窗口：文字水印 / 图片水印两个页签，共用不透明度。
// 文字水印可选 12 种摆放位置（九宫格单条 / 居中斜放 / 水平铺满 / 45° 斜向铺满）；
// 图片水印默认居中单张，可勾选平铺。
class WatermarkDialog : public QDialog {
    Q_OBJECT

public:
    WatermarkDialog(std::shared_ptr<pw::IPdfBackend> backend, const QString& source,
                    QWidget* parent = nullptr);

signals:
    void logMessage(const QString& text);

private slots:
    void addWatermark();
    void pickImage();

private:
    std::shared_ptr<pw::IPdfBackend> backend_;
    QString source_;

    QTabWidget* tabs_ = nullptr;

    QLineEdit* text_ = nullptr;
    QComboBox* position_ = nullptr;
    QDoubleSpinBox* opacity_ = nullptr;

    QLineEdit* imagePath_ = nullptr;
    QDoubleSpinBox* imageScale_ = nullptr;
    QCheckBox* tile_ = nullptr;
    QLabel* imageHint_ = nullptr;
};
