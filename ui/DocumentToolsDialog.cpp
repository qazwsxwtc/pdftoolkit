#include "DocumentToolsDialog.h"

#include <algorithm>
#include <filesystem>

#include <QApplication>
#include <QCheckBox>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPushButton>
#include <QVBoxLayout>

namespace fs = std::filesystem;

namespace {

// 权限位与 core 的 pw::Permission 一致
constexpr int kPermPrint = 1 << 2;
constexpr int kPermModify = 1 << 3;
constexpr int kPermCopy = 1 << 4;
constexpr int kPermAnnotate = 1 << 5;

}  // namespace

DocumentToolsDialog::DocumentToolsDialog(std::shared_ptr<pw::IPdfBackend> backend,
                                         const QString& source, QWidget* parent)
    : QDialog(parent), backend_(std::move(backend)), source_(source) {
    setWindowTitle(QStringLiteral("文档工具 — %1").arg(QFileInfo(source_).fileName()));
    resize(560, 640);

    auto* root = new QVBoxLayout(this);

    // ---- 元数据 ----
    auto* metaBox = new QGroupBox(QStringLiteral("元数据（标题 / 作者 / 主题 / 关键词）"), this);
    auto* metaForm = new QFormLayout(metaBox);
    title_ = new QLineEdit(metaBox);
    author_ = new QLineEdit(metaBox);
    subject_ = new QLineEdit(metaBox);
    keywords_ = new QLineEdit(metaBox);
    metaForm->addRow(QStringLiteral("标题"), title_);
    metaForm->addRow(QStringLiteral("作者"), author_);
    metaForm->addRow(QStringLiteral("主题"), subject_);
    metaForm->addRow(QStringLiteral("关键词"), keywords_);
    auto* metaButtons = new QHBoxLayout();
    auto* loadBtn = new QPushButton(QStringLiteral("读取当前"), metaBox);
    auto* saveMetaBtn = new QPushButton(QStringLiteral("写入并另存为…"), metaBox);
    metaButtons->addWidget(loadBtn);
    metaButtons->addWidget(saveMetaBtn);
    metaForm->addRow(metaButtons);
    root->addWidget(metaBox);

    // ---- 加密 / 解密 ----
    auto* encBox = new QGroupBox(QStringLiteral("加密 / 解密"), this);
    auto* encForm = new QFormLayout(encBox);
    userPassword_ = new QLineEdit(encBox);
    ownerPassword_ = new QLineEdit(encBox);
    userPassword_->setEchoMode(QLineEdit::Password);
    ownerPassword_->setEchoMode(QLineEdit::Password);
    userPassword_->setPlaceholderText(QStringLiteral("打开文档时要输入的密码"));
    ownerPassword_->setPlaceholderText(QStringLiteral("留空则用打开密码代替"));
    encForm->addRow(QStringLiteral("打开密码"), userPassword_);
    encForm->addRow(QStringLiteral("所有者密码"), ownerPassword_);
    allowPrint_ = new QCheckBox(QStringLiteral("允许打印"), encBox);
    allowCopy_ = new QCheckBox(QStringLiteral("允许复制文本"), encBox);
    allowModify_ = new QCheckBox(QStringLiteral("允许修改"), encBox);
    allowAnnotate_ = new QCheckBox(QStringLiteral("允许批注"), encBox);
    allowPrint_->setChecked(true);
    allowCopy_->setChecked(true);
    auto* permRow = new QHBoxLayout();
    permRow->addWidget(allowPrint_);
    permRow->addWidget(allowCopy_);
    permRow->addWidget(allowModify_);
    permRow->addWidget(allowAnnotate_);
    encForm->addRow(QStringLiteral("权限"), permRow);
    auto* encButtons = new QHBoxLayout();
    auto* encryptBtn = new QPushButton(QStringLiteral("加密另存为…"), encBox);
    auto* decryptBtn = new QPushButton(QStringLiteral("解密另存为…"), encBox);
    encButtons->addWidget(encryptBtn);
    encButtons->addWidget(decryptBtn);
    encForm->addRow(encButtons);
    root->addWidget(encBox);

    // ---- 压缩 ----
    auto* cmpBox = new QGroupBox(QStringLiteral("压缩"), this);
    auto* cmpRow = new QHBoxLayout(cmpBox);
    auto* cmpBtn = new QPushButton(QStringLiteral("字体子集化 + 流压缩，另存为…"), cmpBox);
    cmpRow->addWidget(cmpBtn);
    root->addWidget(cmpBox);

    // ---- 水印 ----
    auto* wmBox = new QGroupBox(QStringLiteral("水印（文字水印，斜向内嵌到每一页）"), this);
    auto* wmForm = new QFormLayout(wmBox);
    watermarkText_ = new QLineEdit(wmBox);
    watermarkText_->setPlaceholderText(QStringLiteral("如：内部资料 / 草稿 / 机密"));
    watermarkText_->setText(QStringLiteral("内部资料"));
    watermarkOpacity_ = new QDoubleSpinBox(wmBox);
    watermarkOpacity_->setRange(0.02, 1.0);
    watermarkOpacity_->setSingleStep(0.05);
    watermarkOpacity_->setDecimals(2);
    watermarkOpacity_->setValue(0.15);
    wmForm->addRow(QStringLiteral("文字"), watermarkText_);
    wmForm->addRow(QStringLiteral("不透明度"), watermarkOpacity_);
    auto* wmBtn = new QPushButton(QStringLiteral("加水印并另存为…"), wmBox);
    wmForm->addRow(wmBtn);
    root->addWidget(wmBox);

    // ---- 图片转 PDF ----
    auto* imgBox = new QGroupBox(QStringLiteral("图片转 PDF（多张图片合成一个 PDF）"), this);
    auto* imgLayout = new QVBoxLayout(imgBox);
    imageList_ = new QListWidget(imgBox);
    imageList_->setSelectionMode(QAbstractItemView::ExtendedSelection);
    imgLayout->addWidget(imageList_, 1);
    auto* imgButtons = new QHBoxLayout();
    auto* addImgBtn = new QPushButton(QStringLiteral("添加图片…"), imgBox);
    auto* delImgBtn = new QPushButton(QStringLiteral("移除选中"), imgBox);
    fitA4_ = new QCheckBox(QStringLiteral("等比缩放放进 A4"), imgBox);
    imgButtons->addWidget(addImgBtn);
    imgButtons->addWidget(delImgBtn);
    imgButtons->addWidget(fitA4_);
    imgLayout->addLayout(imgButtons);
    auto* imgSaveBtn = new QPushButton(QStringLiteral("生成 PDF…"), imgBox);
    imgLayout->addWidget(imgSaveBtn);
    root->addWidget(imgBox, 1);

    auto* closeBtn = new QPushButton(QStringLiteral("关闭"), this);
    auto* bottomRow = new QHBoxLayout();
    bottomRow->addStretch(1);
    bottomRow->addWidget(closeBtn);
    root->addLayout(bottomRow);

    connect(loadBtn, &QPushButton::clicked, this, &DocumentToolsDialog::loadMetadata);
    connect(saveMetaBtn, &QPushButton::clicked, this, &DocumentToolsDialog::saveMetadata);
    connect(encryptBtn, &QPushButton::clicked, this, &DocumentToolsDialog::encryptPdf);
    connect(decryptBtn, &QPushButton::clicked, this, &DocumentToolsDialog::decryptPdf);
    connect(cmpBtn, &QPushButton::clicked, this, &DocumentToolsDialog::compressPdf);
    connect(wmBtn, &QPushButton::clicked, this, &DocumentToolsDialog::addWatermark);
    connect(addImgBtn, &QPushButton::clicked, this, &DocumentToolsDialog::addImages);
    connect(delImgBtn, &QPushButton::clicked, this, &DocumentToolsDialog::removeImages);
    connect(imgSaveBtn, &QPushButton::clicked, this, &DocumentToolsDialog::imagesToPdf);
    connect(closeBtn, &QPushButton::clicked, this, &QDialog::reject);

    loadMetadata();
}

QString DocumentToolsDialog::suggest(const QString& suffix) const {
    const QFileInfo fi(source_);
    return fi.absolutePath() + QLatin1Char('/') + fi.completeBaseName() + suffix;
}

void DocumentToolsDialog::loadMetadata() {
    auto meta = backend_->metadata(fs::path(source_.toStdString()), std::string());
    if (!meta.ok()) {
        emit logMessage(QStringLiteral("读取元数据失败: ") + QString::fromStdString(meta.status().toString()));
        return;
    }
    title_->setText(QString::fromStdString(meta.value().title));
    author_->setText(QString::fromStdString(meta.value().author));
    subject_->setText(QString::fromStdString(meta.value().subject));
    keywords_->setText(QString::fromStdString(meta.value().keywords));
}

void DocumentToolsDialog::saveMetadata() {
    const QString out = QFileDialog::getSaveFileName(this, QStringLiteral("元数据输出"),
                                                     suggest(QStringLiteral("_元数据.pdf")),
                                                     QStringLiteral("PDF (*.pdf)"));
    if (out.isEmpty()) return;

    pw::DocumentMetadata meta;
    meta.title = title_->text().toStdString();
    meta.author = author_->text().toStdString();
    meta.subject = subject_->text().toStdString();
    meta.keywords = keywords_->text().toStdString();

    const pw::Status st = backend_->setMetadata(fs::path(source_.toStdString()),
                                                fs::path(out.toStdString()), meta);
    if (!st.ok()) {
        QMessageBox::warning(this, QStringLiteral("写入失败"), QString::fromStdString(st.toString()));
        return;
    }
    emit logMessage(QStringLiteral("元数据已写入 -> ") + out);
}

void DocumentToolsDialog::encryptPdf() {
    const QString user = userPassword_->text();
    if (user.isEmpty() && ownerPassword_->text().isEmpty()) {
        QMessageBox::information(this, QStringLiteral("需要密码"),
                                 QStringLiteral("至少填一个打开密码或所有者密码"));
        return;
    }
    const QString out = QFileDialog::getSaveFileName(this, QStringLiteral("加密输出"),
                                                     suggest(QStringLiteral("_加密.pdf")),
                                                     QStringLiteral("PDF (*.pdf)"));
    if (out.isEmpty()) return;

    int perms = 0;
    if (allowPrint_->isChecked()) perms |= kPermPrint;
    if (allowCopy_->isChecked()) perms |= kPermCopy;
    if (allowModify_->isChecked()) perms |= kPermModify;
    if (allowAnnotate_->isChecked()) perms |= kPermAnnotate;
    // 其余位（表单/装配/高清打印）按"与打印一致"给出，避免出现半开半闭的组合
    if (allowPrint_->isChecked()) perms |= (1 << 11);
    if (allowModify_->isChecked()) perms |= (1 << 8) | (1 << 10);

    const pw::Status st =
        backend_->encrypt(fs::path(source_.toStdString()), fs::path(out.toStdString()),
                          user.toStdString(), ownerPassword_->text().toStdString(), perms);
    if (!st.ok()) {
        QMessageBox::warning(this, QStringLiteral("加密失败"), QString::fromStdString(st.toString()));
        return;
    }
    emit logMessage(QStringLiteral("已加密（AES-128）-> ") + out);
}

void DocumentToolsDialog::decryptPdf() {
    bool ok = false;
    const QString pwd = QInputDialog::getText(this, QStringLiteral("解密"),
                                              QStringLiteral("输入该 PDF 的打开密码："),
                                              QLineEdit::Password, QString(), &ok);
    if (!ok) return;

    const QString out = QFileDialog::getSaveFileName(this, QStringLiteral("解密输出"),
                                                     suggest(QStringLiteral("_解密.pdf")),
                                                     QStringLiteral("PDF (*.pdf)"));
    if (out.isEmpty()) return;

    const pw::Status st = backend_->decrypt(fs::path(source_.toStdString()),
                                            fs::path(out.toStdString()), pwd.toStdString());
    if (!st.ok()) {
        QMessageBox::warning(this, QStringLiteral("解密失败"), QString::fromStdString(st.toString()));
        return;
    }
    emit logMessage(QStringLiteral("已解密 -> ") + out);
}

void DocumentToolsDialog::compressPdf() {
    const QString out = QFileDialog::getSaveFileName(this, QStringLiteral("压缩输出"),
                                                     suggest(QStringLiteral("_压缩.pdf")),
                                                     QStringLiteral("PDF (*.pdf)"));
    if (out.isEmpty()) return;

    QApplication::setOverrideCursor(Qt::WaitCursor);
    const pw::Status st = backend_->compress(fs::path(source_.toStdString()),
                                             fs::path(out.toStdString()));
    QApplication::restoreOverrideCursor();

    if (!st.ok()) {
        QMessageBox::warning(this, QStringLiteral("压缩失败"), QString::fromStdString(st.toString()));
        return;
    }
    const qint64 before = QFileInfo(source_).size();
    const qint64 after = QFileInfo(out).size();
    emit logMessage(QStringLiteral("已压缩 -> %1（%2 KB → %3 KB）")
                        .arg(out)
                        .arg(before / 1024)
                        .arg(after / 1024));
}

void DocumentToolsDialog::addWatermark() {
    const QString text = watermarkText_->text().trimmed();
    if (text.isEmpty()) {
        QMessageBox::information(this, QStringLiteral("需要文字"), QStringLiteral("先填要打的水印文字"));
        return;
    }
    const QString out = QFileDialog::getSaveFileName(this, QStringLiteral("水印输出"),
                                                     suggest(QStringLiteral("_水印.pdf")),
                                                     QStringLiteral("PDF (*.pdf)"));
    if (out.isEmpty()) return;

    QApplication::setOverrideCursor(Qt::WaitCursor);
    const pw::Status st = backend_->watermark(fs::path(source_.toStdString()),
                                              fs::path(out.toStdString()),
                                              text.toStdString(),
                                              static_cast<float>(watermarkOpacity_->value()));
    QApplication::restoreOverrideCursor();

    if (!st.ok()) {
        QMessageBox::warning(this, QStringLiteral("加水印失败"), QString::fromStdString(st.toString()));
        return;
    }
    emit logMessage(QStringLiteral("已加水印 -> ") + out);
}

void DocumentToolsDialog::addImages() {
    const QStringList files = QFileDialog::getOpenFileNames(
        this, QStringLiteral("选择图片"), QFileInfo(source_).absolutePath(),
        QStringLiteral("图片 (*.png *.jpg *.jpeg *.bmp *.tif *.tiff *.gif);;所有文件 (*)"));
    for (const auto& f : files) imageList_->addItem(f);
}

void DocumentToolsDialog::removeImages() {
    for (auto* item : imageList_->selectedItems()) delete item;
}

void DocumentToolsDialog::imagesToPdf() {
    if (imageList_->count() == 0) {
        QMessageBox::information(this, QStringLiteral("没有图片"),
                                 QStringLiteral("先添加至少一张图片"));
        return;
    }
    const QString out = QFileDialog::getSaveFileName(this, QStringLiteral("图片转 PDF 输出"),
                                                     suggest(QStringLiteral("_图片.pdf")),
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
    const pw::Status st = backend_->imagesToPdf(images, fs::path(out.toStdString()));
    QApplication::restoreOverrideCursor();

    if (!st.ok()) {
        QMessageBox::warning(this, QStringLiteral("生成失败"), QString::fromStdString(st.toString()));
        return;
    }
    emit logMessage(QStringLiteral("图片转 PDF 完成：%1 张 -> %2").arg(images.size()).arg(out));
}
