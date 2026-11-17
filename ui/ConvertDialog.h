#pragma once

#include <QDialog>
#include <QString>
#include <QStringList>

class QComboBox;
class QLabel;
class QLineEdit;
class QListWidget;

// 转换参数窗口：待处理文件列表 + 目标格式 + 输出目录（可留空=源文件同目录）。
// 界面只收集参数，真正的方向判断与转换在 core 的 Converter 里。
class ConvertDialog : public QDialog {
    Q_OBJECT

public:
    explicit ConvertDialog(const QStringList& files, QWidget* parent = nullptr);

    QStringList files() const;
    QString target() const;        // "pdf" / "docx"
    QString outputDir() const;     // 空 = 与源文件同目录
    QString pdfPassword() const;

private slots:
    void pickOutputDir();
    void refreshSummary();

private:
    QListWidget* fileList_ = nullptr;
    QComboBox* target_ = nullptr;
    QLineEdit* outDir_ = nullptr;
    QLineEdit* password_ = nullptr;
    QLabel* summary_ = nullptr;
};
