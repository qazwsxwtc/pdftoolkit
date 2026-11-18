#include "WatermarkDialog.h"
#include "I18nUi.h"

#include <filesystem>

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QSizePolicy>
#include <QTabWidget>
#include <QVBoxLayout>

#include "DialogCommon.h"
#include "pdfword/path_util.hpp"

WatermarkDialog::WatermarkDialog(std::shared_ptr<pw::IPdfBackend> backend, const QString& source,
                                 QWidget* parent)
    : QDialog(parent), backend_(std::move(backend)), source_(source) {
    setWindowTitle(TR("水印 — %1").arg(QFileInfo(source_).fileName()));
    resize(560, 400);

    auto* root = new QVBoxLayout(this);
    root->addWidget(new QLabel(TR("两种水印都内嵌到每一页，源页内容不改动。"), this));

    tabs_ = new QTabWidget(this);
    tabs_->setObjectName(QStringLiteral("watermarkTabs"));

    // ---- 页签一：文字水印 ----
    auto* textPage = new QWidget(tabs_);
    auto* textForm = new QFormLayout(textPage);
    text_ = new QLineEdit(textPage);
    text_->setObjectName(QStringLiteral("watermarkText"));
    text_->setPlaceholderText(TR("如：内部资料 / 草稿 / 机密"));
    text_->setText(TR("内部资料"));
    textForm->addRow(TR("文字"), text_);

    position_ = new QComboBox(textPage);
    position_->setObjectName(QStringLiteral("watermarkPosition"));
    position_->addItem(TR("左上角"), QStringLiteral("top-left"));
    position_->addItem(TR("顶部居中"), QStringLiteral("top-center"));
    position_->addItem(TR("右上角"), QStringLiteral("top-right"));
    position_->addItem(TR("左侧居中"), QStringLiteral("middle-left"));
    position_->addItem(TR("页面正中"), QStringLiteral("center"));
    position_->addItem(TR("右侧居中"), QStringLiteral("middle-right"));
    position_->addItem(TR("左下角"), QStringLiteral("bottom-left"));
    position_->addItem(TR("底部居中（页脚）"), QStringLiteral("bottom-center"));
    position_->addItem(TR("右下角"), QStringLiteral("bottom-right"));
    position_->addItem(TR("居中斜放 45°"), QStringLiteral("center-diagonal"));
    position_->addItem(TR("水平平铺（铺满整页）"), QStringLiteral("tile-horizontal"));
    position_->addItem(TR("斜向 45° 铺满（经典机密）"), QStringLiteral("tile-diagonal"));
    position_->setCurrentIndex(0);
    textForm->addRow(TR("位置"), position_);

    auto* textHint = new QLabel(
        TR("单条水印字号约为页宽短边的 8%，铺满时自动缩到 5.5%；超出页边的部分会被裁掉。"),
        textPage);
    textHint->setStyleSheet(QStringLiteral("color:#666;"));
    textForm->addRow(textHint);
    tabs_->addTab(textPage, TR("文字水印"));

    // ---- 页签二：图片水印 ----
    auto* imagePage = new QWidget(tabs_);
    auto* imageForm = new QFormLayout(imagePage);
    auto* imageRow = new QHBoxLayout();
    imagePath_ = new QLineEdit(imagePage);
    imagePath_->setObjectName(QStringLiteral("watermarkImage"));
    imagePath_->setPlaceholderText(TR("PNG / JPG，透明底 PNG 效果最好"));
    auto* browseBtn = new QPushButton(TR("浏览…"), imagePage);
    imageRow->addWidget(imagePath_, 1);
    imageRow->addWidget(browseBtn);
    imageForm->addRow(TR("图片"), imageRow);

    imageScale_ = new QDoubleSpinBox(imagePage);
    imageScale_->setObjectName(QStringLiteral("watermarkScale"));
    imageScale_->setRange(0.05, 1.0);
    imageScale_->setSingleStep(0.05);
    imageScale_->setDecimals(2);
    imageScale_->setValue(0.30);
    imageForm->addRow(TR("宽度占页宽"), imageScale_);

    tile_ = new QCheckBox(TR("网格平铺（间距 = 水印宽的 1.6 倍）"), imagePage);
    tile_->setObjectName(QStringLiteral("watermarkTile"));
    imageForm->addRow(QString(), tile_);

    imageHint_ = new QLabel(TR("不勾平铺时图片居中放一张；竖长图会被压到页高的 90% 以内。"), imagePage);
    imageHint_->setStyleSheet(QStringLiteral("color:#666;"));
    imageForm->addRow(imageHint_);
    tabs_->addTab(imagePage, TR("图片水印"));
    // 页签按内容取高，别把窗口剩余的竖向空间都吃掉
    tabs_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Maximum);
    root->addWidget(tabs_);

    // ---- 共用：不透明度 ----
    auto* opacityRow = new QHBoxLayout();
    opacityRow->addWidget(new QLabel(TR("不透明度"), this));
    opacity_ = new QDoubleSpinBox(this);
    opacity_->setObjectName(QStringLiteral("watermarkOpacity"));
    opacity_->setRange(0.02, 1.0);
    opacity_->setSingleStep(0.05);
    opacity_->setDecimals(2);
    opacity_->setValue(0.15);
    opacityRow->addWidget(opacity_);
    opacityRow->addStretch(1);
    root->addLayout(opacityRow);
    root->addStretch(1);

    auto* btnRow = new QHBoxLayout();
    auto* btn = new QPushButton(TR("加水印并另存为…"), this);
    btnRow->addStretch(1);
    btnRow->addWidget(btn);
    root->addLayout(btnRow);

    auto* box = new QDialogButtonBox(QDialogButtonBox::Close, this);
    box->button(QDialogButtonBox::Close)->setText(TR("关闭"));
    root->addWidget(box);

    connect(browseBtn, &QPushButton::clicked, this, &WatermarkDialog::pickImage);
    connect(btn, &QPushButton::clicked, this, &WatermarkDialog::addWatermark);
    connect(box, &QDialogButtonBox::rejected, this, &QDialog::reject);
}

void WatermarkDialog::pickImage() {
    const QString startDir = imagePath_->text().trimmed().isEmpty()
                                 ? QFileInfo(source_).absolutePath()
                                 : QFileInfo(imagePath_->text().trimmed()).absolutePath();
    const QString file = QFileDialog::getOpenFileName(
        this, TR("选择水印图片"), startDir,
        TR("图片 (*.png *.jpg *.jpeg *.bmp *.tif *.tiff);;所有文件 (*)"));
    if (file.isEmpty()) return;
    imagePath_->setText(file);
    imageHint_->setText(TR("已选择：%1").arg(QFileInfo(file).fileName()));
}

void WatermarkDialog::addWatermark() {
    const bool imageMode = (tabs_->currentIndex() == 1);

    QString out;
    if (imageMode) {
        const QString imagePath = imagePath_->text().trimmed();
        if (imagePath.isEmpty()) {
            QMessageBox::information(this, TR("需要图片"),
                                     TR("先选一张水印图片（PNG 透明底效果最好）"));
            return;
        }
        if (!QFileInfo::exists(imagePath)) {
            QMessageBox::warning(this, TR("图片不存在"),
                                 TR("找不到这张图片：\n%1").arg(imagePath));
            return;
        }
        out = QFileDialog::getSaveFileName(this, TR("图片水印输出"),
                                           suggestPdfOutput(source_, TR("_水印")),
                                           QStringLiteral("PDF (*.pdf)"));
        if (out.isEmpty()) return;

        QApplication::setOverrideCursor(Qt::WaitCursor);
        const pw::Status st = backend_->imageWatermark(
            pw::fsPath(source_.toUtf8().toStdString()), pw::fsPath(out.toUtf8().toStdString()),
            pw::fsPath(imagePath.toUtf8().toStdString()),
            static_cast<float>(opacity_->value()), static_cast<float>(imageScale_->value()),
            tile_->isChecked());
        QApplication::restoreOverrideCursor();

        if (!st.ok()) {
            QMessageBox::warning(this, TR("加水印失败"),
                                 QString::fromStdString(st.toString()));
            return;
        }
        emit logMessage(TR("已加图片水印%1 -> %2")
                            .arg(tile_->isChecked() ? TR("（平铺）") : QString())
                            .arg(out));
        return;
    }

    const QString text = text_->text().trimmed();
    if (text.isEmpty()) {
        QMessageBox::information(this, TR("需要文字"), TR("先填要打的水印文字"));
        return;
    }
    out = QFileDialog::getSaveFileName(this, TR("水印输出"),
                                       suggestPdfOutput(source_, TR("_水印")),
                                       QStringLiteral("PDF (*.pdf)"));
    if (out.isEmpty()) return;

    QApplication::setOverrideCursor(Qt::WaitCursor);
    const pw::Status st = backend_->watermark(
        pw::fsPath(source_.toUtf8().toStdString()), pw::fsPath(out.toUtf8().toStdString()),
        text.toStdString(), static_cast<float>(opacity_->value()),
        position_->currentData().toString().toStdString());
    QApplication::restoreOverrideCursor();

    if (!st.ok()) {
        QMessageBox::warning(this, TR("加水印失败"), QString::fromStdString(st.toString()));
        return;
    }
    emit logMessage(TR("已加水印（%1） -> %2")
                        .arg(position_->currentText())
                        .arg(out));
}
