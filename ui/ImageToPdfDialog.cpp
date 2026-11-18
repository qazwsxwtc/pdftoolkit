#include "ImageToPdfDialog.h"
#include "I18nUi.h"

#include <filesystem>
#include <vector>

#include <QApplication>
#include <QAbstractItemView>
#include <QCheckBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QMessageBox>
#include <QPushButton>
#include <QVBoxLayout>

#include "DropListWidget.h"
#include "pdfword/path_util.hpp"

ImageToPdfDialog::ImageToPdfDialog(std::shared_ptr<pw::IPdfBackend> backend, const QString& suggestedDir,
                                   QWidget* parent)
    : QDialog(parent), backend_(std::move(backend)), dir_(suggestedDir) {
    setWindowTitle(TR("图片转 PDF"));
    resize(640, 460);

    auto* root = new QVBoxLayout(this);
    root->addWidget(new QLabel(TR("列表顺序就是页面顺序；支持 PNG/JPG/BMP/TIFF/GIF，可拖入图片。"), this));

    auto* dropList = new DropListWidget(this);
    dropList->setExtensions({QStringLiteral("png"), QStringLiteral("jpg"), QStringLiteral("jpeg"),
                             QStringLiteral("bmp"), QStringLiteral("tif"), QStringLiteral("tiff"),
                             QStringLiteral("gif")});
    imageList_ = dropList;
    imageList_->setObjectName(QStringLiteral("imageList"));
    imageList_->setSelectionMode(QAbstractItemView::ExtendedSelection);
    root->addWidget(imageList_, 1);
    connect(dropList, &DropListWidget::filesDropped, this,
            [this](const QStringList& paths) { addImagePaths(paths); });

    auto* row = new QHBoxLayout();
    auto* addBtn = new QPushButton(TR("添加图片…"), this);
    auto* delBtn = new QPushButton(TR("移除选中"), this);
    fitA4_ = new QCheckBox(TR("等比缩放放进 A4"), this);
    fitA4_->setObjectName(QStringLiteral("fitA4"));
    row->addWidget(addBtn);
    row->addWidget(delBtn);
    row->addWidget(fitA4_);
    row->addStretch(1);
    root->addLayout(row);

    auto* genRow = new QHBoxLayout();
    auto* genBtn = new QPushButton(TR("生成 PDF…"), this);
    genRow->addStretch(1);
    genRow->addWidget(genBtn);
    root->addLayout(genRow);

    auto* box = new QDialogButtonBox(QDialogButtonBox::Close, this);
    box->button(QDialogButtonBox::Close)->setText(TR("关闭"));
    root->addWidget(box);

    connect(addBtn, &QPushButton::clicked, this, &ImageToPdfDialog::addImages);
    connect(delBtn, &QPushButton::clicked, this, &ImageToPdfDialog::removeImages);
    connect(genBtn, &QPushButton::clicked, this, &ImageToPdfDialog::imagesToPdf);
    connect(box, &QDialogButtonBox::rejected, this, &QDialog::reject);
}

void ImageToPdfDialog::addImagePaths(const QStringList& paths) {
    for (const QString& raw : paths) {
        if (raw.trimmed().isEmpty()) continue;
        const QString absolute = QFileInfo(raw).absoluteFilePath();
        bool exists = false;
        for (int i = 0; i < imageList_->count(); ++i) {
            if (imageList_->item(i)->text() == absolute) {
                exists = true;
                break;
            }
        }
        if (!exists) imageList_->addItem(absolute);
    }
}

void ImageToPdfDialog::addImages() {
    const QString startDir = dir_.isEmpty() ? QDir::currentPath() : dir_;
    const QStringList files = QFileDialog::getOpenFileNames(
        this, TR("选择图片"), startDir,
        TR("图片 (*.png *.jpg *.jpeg *.bmp *.tif *.tiff *.gif);;所有文件 (*)"));
    addImagePaths(files);
}

void ImageToPdfDialog::removeImages() {
    for (auto* item : imageList_->selectedItems()) delete item;
}

void ImageToPdfDialog::imagesToPdf() {
    if (imageList_->count() == 0) {
        QMessageBox::information(this, TR("没有图片"), TR("先添加至少一张图片"));
        return;
    }
    const QString defaultDir = dir_.isEmpty() ? QDir::currentPath() : dir_;
    const QString out = QFileDialog::getSaveFileName(this, TR("图片转 PDF 输出"),
                                                     defaultDir + TR("/图片合成.pdf"),
                                                     QStringLiteral("PDF (*.pdf)"));
    if (out.isEmpty()) return;

    std::vector<pw::ImagePage> images;
    for (int i = 0; i < imageList_->count(); ++i) {
        pw::ImagePage page;
        page.path = imageList_->item(i)->text().toStdString();
        page.fitA4 = fitA4_->isChecked();
        images.push_back(page);
    }

    QApplication::setOverrideCursor(Qt::WaitCursor);
    const pw::Status st = backend_->imagesToPdf(images, pw::fsPath(out.toUtf8().toStdString()));
    QApplication::restoreOverrideCursor();

    if (!st.ok()) {
        QMessageBox::warning(this, TR("生成失败"), QString::fromStdString(st.toString()));
        return;
    }
    emit logMessage(TR("图片转 PDF 完成：%1 张 -> %2").arg(images.size()).arg(out));
}
