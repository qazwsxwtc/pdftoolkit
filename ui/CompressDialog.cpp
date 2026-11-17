#include "CompressDialog.h"

#include <filesystem>

#include <QApplication>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QVBoxLayout>

#include "DialogCommon.h"

CompressDialog::CompressDialog(std::shared_ptr<pw::IPdfBackend> backend, const QString& source,
                               QWidget* parent)
    : QDialog(parent), backend_(std::move(backend)), source_(source) {
    setWindowTitle(QStringLiteral("压缩 — %1").arg(QFileInfo(source_).fileName()));
    resize(520, 220);

    auto* root = new QVBoxLayout(this);
    root->addWidget(new QLabel(QStringLiteral("做三件事：字体子集化、流压缩、垃圾对象回收。"), this));
    root->addWidget(new QLabel(QStringLiteral("页面内容与像素不变，只去掉没人引用的对象与重复资源。"), this));

    const qint64 bytes = QFileInfo(source_).size();
    sizeLabel_ = new QLabel(QStringLiteral("源文件大小：%1 KB").arg(bytes / 1024), this);
    root->addWidget(sizeLabel_);
    root->addStretch(1);

    auto* btnRow = new QHBoxLayout();
    auto* compressBtn = new QPushButton(QStringLiteral("压缩并另存为…"), this);
    btnRow->addStretch(1);
    btnRow->addWidget(compressBtn);
    root->addLayout(btnRow);

    auto* box = new QDialogButtonBox(QDialogButtonBox::Close, this);
    box->button(QDialogButtonBox::Close)->setText(QStringLiteral("关闭"));
    root->addWidget(box);

    connect(compressBtn, &QPushButton::clicked, this, &CompressDialog::compressPdf);
    connect(box, &QDialogButtonBox::rejected, this, &QDialog::reject);
}

void CompressDialog::compressPdf() {
    const QString out = QFileDialog::getSaveFileName(this, QStringLiteral("压缩输出"),
                                                     suggestPdfOutput(source_, QStringLiteral("_压缩")),
                                                     QStringLiteral("PDF (*.pdf)"));
    if (out.isEmpty()) return;

    QApplication::setOverrideCursor(Qt::WaitCursor);
    const pw::Status st = backend_->compress(std::filesystem::path(source_.toStdString()),
                                             std::filesystem::path(out.toStdString()));
    QApplication::restoreOverrideCursor();

    if (!st.ok()) {
        QMessageBox::warning(this, QStringLiteral("压缩失败"), QString::fromStdString(st.toString()));
        return;
    }
    const qint64 before = QFileInfo(source_).size();
    const qint64 after = QFileInfo(out).size();
    sizeLabel_->setText(QStringLiteral("源文件 %1 KB → 输出 %2 KB").arg(before / 1024).arg(after / 1024));
    emit logMessage(QStringLiteral("已压缩 -> %1（%2 KB → %3 KB）")
                        .arg(out)
                        .arg(before / 1024)
                        .arg(after / 1024));
}
