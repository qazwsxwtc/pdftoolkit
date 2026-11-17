#pragma once

#include <QWidget>
#include <memory>

#include "pdfword/pdf_backend.hpp"

class QLabel;
class QScrollArea;

// PDF 预览控件：只依赖 core 的 IPdfBackend 接口，不直接碰 MuPDF。
// 换成 web 壳时，这一整块被前端 canvas 替代，core 侧不需要任何改动。
class PdfViewWidget : public QWidget {
    Q_OBJECT

public:
    explicit PdfViewWidget(std::shared_ptr<pw::IPdfBackend> backend, QWidget* parent = nullptr);

    bool openDocument(const QString& path, QString* error);
    void closeDocument();
    void setZoom(double zoom);
    void showPage(int page1);

    // 当前预览的 PDF 路径（没打开时为空），供"抽取页面"等操作确定源文件
    QString currentPath() const { return path_; }
    // 已打开的文档句柄，页面编辑器直接复用，避免再解析一遍
    std::shared_ptr<pw::IPdfDocument> document() const { return document_; }

    double zoom() const { return zoom_; }
    int pageCount() const { return pageCount_; }
    int currentPage() const { return currentPage_; }

signals:
    void pageChanged(int page, int total);

private:
    void render();
    void showHint(const QString& text);

    std::shared_ptr<pw::IPdfBackend> backend_;
    std::shared_ptr<pw::IPdfDocument> document_;
    QString path_;
    QScrollArea* scroll_ = nullptr;
    QLabel* canvas_ = nullptr;
    QLabel* hint_ = nullptr;
    double zoom_ = 1.5;
    int pageCount_ = 0;
    int currentPage_ = 1;
};
