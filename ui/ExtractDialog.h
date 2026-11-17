#pragma once

#include <QDialog>
#include <QString>
#include <QStringList>
#include <memory>
#include <vector>

#include "pdfword/pdf_backend.hpp"

class QComboBox;
class QLabel;
class QLineEdit;

// 抽取页面的参数窗口：选源 PDF、填页码范围、看实时校验结果。
// 页码解析复用 core 的 parsePageSpec，界面不重复实现一遍规则。
class ExtractDialog : public QDialog {
    Q_OBJECT

public:
    ExtractDialog(std::shared_ptr<pw::IPdfBackend> backend, const QStringList& candidates,
                  const QString& current, QWidget* parent = nullptr);

    QString sourcePath() const;
    QString spec() const;
    QString outputPath() const;
    // 对话框里已经按真实页数校验过的 0 基页码，主窗口直接拿去用
    std::vector<int> pages() const { return pages_; }

private slots:
    void browseSource();
    void browseOutput();
    void selectAllPages();
    void revalidate();

private:
    void reloadPageCount();
    QString suggestOutput() const;

    std::shared_ptr<pw::IPdfBackend> backend_;
    QComboBox* source_ = nullptr;
    QLineEdit* spec_ = nullptr;
    QLabel* hint_ = nullptr;
    QLineEdit* output_ = nullptr;
    int pageCount_ = 0;
    bool specValid_ = false;
    std::vector<int> pages_;
};
