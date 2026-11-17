#include "MetadataDialog.h"

#include <filesystem>

#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QVBoxLayout>

#include "DialogCommon.h"

MetadataDialog::MetadataDialog(std::shared_ptr<pw::IPdfBackend> backend, const QString& source,
                               QWidget* parent)
    : QDialog(parent), backend_(std::move(backend)), source_(source) {
    setWindowTitle(QStringLiteral("元数据 — %1").arg(QFileInfo(source_).fileName()));
    resize(540, 300);

    auto* root = new QVBoxLayout(this);
    root->addWidget(new QLabel(QStringLiteral("改完点“写入并另存为…”，源文件不会被改动。"), this));

    auto* form = new QFormLayout();
    title_ = new QLineEdit(this);
    author_ = new QLineEdit(this);
    subject_ = new QLineEdit(this);
    keywords_ = new QLineEdit(this);
    title_->setObjectName(QStringLiteral("metaTitle"));
    author_->setObjectName(QStringLiteral("metaAuthor"));
    subject_->setObjectName(QStringLiteral("metaSubject"));
    keywords_->setObjectName(QStringLiteral("metaKeywords"));
    form->addRow(QStringLiteral("标题"), title_);
    form->addRow(QStringLiteral("作者"), author_);
    form->addRow(QStringLiteral("主题"), subject_);
    form->addRow(QStringLiteral("关键词"), keywords_);
    root->addLayout(form);

    auto* row = new QHBoxLayout();
    auto* loadBtn = new QPushButton(QStringLiteral("读取当前"), this);
    auto* saveBtn = new QPushButton(QStringLiteral("写入并另存为…"), this);
    row->addWidget(loadBtn);
    row->addWidget(saveBtn);
    row->addStretch(1);
    root->addLayout(row);
    root->addStretch(1);

    auto* box = new QDialogButtonBox(QDialogButtonBox::Close, this);
    // Qt 自带中文翻译没加载时按钮会显示英文 Close，这里统一成手写文案
    box->button(QDialogButtonBox::Close)->setText(QStringLiteral("关闭"));
    root->addWidget(box);

    connect(loadBtn, &QPushButton::clicked, this, &MetadataDialog::loadFromFile);
    connect(saveBtn, &QPushButton::clicked, this, &MetadataDialog::saveAs);
    connect(box, &QDialogButtonBox::rejected, this, &QDialog::reject);

    loadFromFile();
}

void MetadataDialog::loadFromFile() {
    auto meta = backend_->metadata(std::filesystem::path(source_.toStdString()), std::string());
    if (!meta.ok()) {
        emit logMessage(QStringLiteral("读取元数据失败: ") + QString::fromStdString(meta.status().toString()));
        return;
    }
    title_->setText(QString::fromStdString(meta.value().title));
    author_->setText(QString::fromStdString(meta.value().author));
    subject_->setText(QString::fromStdString(meta.value().subject));
    keywords_->setText(QString::fromStdString(meta.value().keywords));
}

void MetadataDialog::saveAs() {
    const QString out = QFileDialog::getSaveFileName(this, QStringLiteral("元数据输出"),
                                                     suggestPdfOutput(source_, QStringLiteral("_元数据")),
                                                     QStringLiteral("PDF (*.pdf)"));
    if (out.isEmpty()) return;

    pw::DocumentMetadata meta;
    meta.title = title_->text().toStdString();
    meta.author = author_->text().toStdString();
    meta.subject = subject_->text().toStdString();
    meta.keywords = keywords_->text().toStdString();

    const pw::Status st = backend_->setMetadata(std::filesystem::path(source_.toStdString()),
                                                std::filesystem::path(out.toStdString()), meta);
    if (!st.ok()) {
        QMessageBox::warning(this, QStringLiteral("写入失败"), QString::fromStdString(st.toString()));
        return;
    }
    emit logMessage(QStringLiteral("元数据已写入 -> ") + out);
}
