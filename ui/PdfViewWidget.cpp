#include "PdfViewWidget.h"

#include <QImage>
#include <QLabel>
#include <QPixmap>
#include <QScrollArea>
#include <QVBoxLayout>

PdfViewWidget::PdfViewWidget(std::shared_ptr<pw::IPdfBackend> backend, QWidget* parent)
    : QWidget(parent), backend_(std::move(backend)) {
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);

    canvas_ = new QLabel(this);
    canvas_->setAlignment(Qt::AlignCenter);
    canvas_->setBackgroundRole(QPalette::Base);

    scroll_ = new QScrollArea(this);
    scroll_->setWidget(canvas_);
    scroll_->setWidgetResizable(false);
    scroll_->setAlignment(Qt::AlignCenter);
    layout->addWidget(scroll_);

    hint_ = new QLabel(this);
    hint_->setAlignment(Qt::AlignCenter);
    hint_->setWordWrap(true);
    hint_->setStyleSheet("color:#666; padding:32px;");
    layout->addWidget(hint_);

    showHint(backend_ && backend_->available()
                 ? QStringLiteral("打开一个 PDF 开始预览")
                 : QStringLiteral("PDF 后端不可用：%1\n\n构建时启用 -DPDFWORD_WITH_MUPDF=ON 并指定 MuPDF SDK 路径。")
                       .arg(backend_ ? QString::fromStdString(backend_->unavailableReason())
                                     : QStringLiteral("backend 为空")));
}

void PdfViewWidget::showHint(const QString& text) {
    hint_->setText(text);
    hint_->show();
    scroll_->hide();
}

bool PdfViewWidget::openDocument(const QString& path, QString* error) {
    if (!backend_ || !backend_->available()) {
        if (error != nullptr) {
            *error = QStringLiteral("PDF 后端不可用：%1")
                         .arg(backend_ ? QString::fromStdString(backend_->unavailableReason())
                                       : QStringLiteral("backend 为空"));
        }
        return false;
    }

    auto opened = backend_->open(std::filesystem::path(path.toStdString()));
    if (!opened.ok()) {
        if (error != nullptr) *error = QString::fromStdString(opened.status().toString());
        return false;
    }

    document_ = opened.value();
    path_ = path;
    pageCount_ = document_->info().pageCount;
    currentPage_ = 1;
    scroll_->show();
    hint_->hide();
    render();
    emit pageChanged(currentPage_, pageCount_);
    return true;
}

void PdfViewWidget::closeDocument() {
    document_.reset();
    path_.clear();
    pageCount_ = 0;
    currentPage_ = 1;
    canvas_->clear();
    showHint(QStringLiteral("打开一个 PDF 开始预览"));
}

void PdfViewWidget::setZoom(double zoom) {
    if (zoom < 0.2) zoom = 0.2;
    if (zoom > 8.0) zoom = 8.0;
    zoom_ = zoom;
    if (document_) render();
}

void PdfViewWidget::showPage(int page1) {
    if (!document_) return;
    if (page1 < 1) page1 = 1;
    if (page1 > pageCount_) page1 = pageCount_;
    currentPage_ = page1;
    render();
    emit pageChanged(currentPage_, pageCount_);
}

void PdfViewWidget::render() {
    if (!document_) return;

    pw::RasterImage image;
    pw::Status st = document_->renderPage(currentPage_ - 1, static_cast<float>(zoom_), &image);
    if (!st.ok()) {
        canvas_->setText(QString::fromStdString(st.toString()));
        canvas_->adjustSize();
        return;
    }

    QImage qimage(image.rgba.data(), image.width, image.height,
                  image.width * 4, QImage::Format_RGBA8888);
    canvas_->setPixmap(QPixmap::fromImage(qimage));
    canvas_->resize(qimage.size());
}
