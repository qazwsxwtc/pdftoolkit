#pragma once

#include <memory>

#include <QDialog>
#include <QString>

#include <pdfword/pdf_backend.hpp>

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QProgressBar;
class QPushButton;

// OCR 文字识别窗口：把扫描件 / 图片型 PDF 识别成"可检索 PDF"（不可见文字层）
// 并按需导出纯文本。识别耗时按页增长，所以放到线程池里跑，界面用忙碌进度条。
class OcrDialog : public QDialog {
    Q_OBJECT

public:
    OcrDialog(std::shared_ptr<pw::IPdfBackend> backend, const QString& source,
              QWidget* parent = nullptr);

signals:
    void logMessage(const QString& text);

protected:
    // 识别期间不允许关闭：后台线程还在跑，关了窗口结果就没法回填
    void reject() override;

private:
    void browsePdfOutput();
    void browseTxtOutput();
    void startOcr();
    void setBusy(bool busy);

    std::shared_ptr<pw::IPdfBackend> backend_;
    QString source_;
    QComboBox* langCombo_ = nullptr;
    QCheckBox* pdfCheck_ = nullptr;
    QCheckBox* txtCheck_ = nullptr;
    QLineEdit* pdfEdit_ = nullptr;
    QLineEdit* txtEdit_ = nullptr;
    QPushButton* startBtn_ = nullptr;
    QProgressBar* progress_ = nullptr;
    QLabel* statusLabel_ = nullptr;
    bool busy_ = false;
};
