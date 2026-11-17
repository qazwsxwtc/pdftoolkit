#include "ExtractDialog.h"

#include <QComboBox>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QVBoxLayout>

#include "pdfword/page_selection.hpp"

#include <filesystem>

ExtractDialog::ExtractDialog(std::shared_ptr<pw::IPdfBackend> backend, const QStringList& candidates,
                             const QString& current, QWidget* parent)
    : QDialog(parent), backend_(std::move(backend)) {
    setWindowTitle(QStringLiteral("抽取页面 — 选源文件与页码范围"));
    resize(560, 320);

    auto* root = new QVBoxLayout(this);

    auto* srcRow = new QHBoxLayout();
    srcRow->addWidget(new QLabel(QStringLiteral("源 PDF"), this));
    source_ = new QComboBox(this);
    source_->setMinimumWidth(320);
    for (const auto& path : candidates) {
        if (path.isEmpty()) continue;
        source_->addItem(QFileInfo(path).fileName(), path);
    }
    if (!current.isEmpty() && source_->findData(current) < 0) {
        source_->addItem(QFileInfo(current).fileName(), current);
    }
    if (source_->findData(current) >= 0) source_->setCurrentIndex(source_->findData(current));
    auto* browseSrc = new QPushButton(QStringLiteral("浏览…"), this);
    srcRow->addWidget(source_, 1);
    srcRow->addWidget(browseSrc);
    root->addLayout(srcRow);

    auto* specRow = new QHBoxLayout();
    specRow->addWidget(new QLabel(QStringLiteral("页码范围"), this));
    spec_ = new QLineEdit(this);
    spec_->setPlaceholderText(QStringLiteral("如 1-3,5 或 1,3,5-8；中文逗号也认"));
    auto* allBtn = new QPushButton(QStringLiteral("全部页"), this);
    specRow->addWidget(spec_, 1);
    specRow->addWidget(allBtn);
    root->addLayout(specRow);

    hint_ = new QLabel(QStringLiteral("正在读取页数…"), this);
    hint_->setWordWrap(true);
    root->addWidget(hint_);

    auto* outRow = new QHBoxLayout();
    outRow->addWidget(new QLabel(QStringLiteral("输出文件"), this));
    output_ = new QLineEdit(this);
    auto* browseOut = new QPushButton(QStringLiteral("浏览…"), this);
    outRow->addWidget(output_, 1);
    outRow->addWidget(browseOut);
    root->addLayout(outRow);

    root->addStretch(1);

    auto* box = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    box->button(QDialogButtonBox::Ok)->setText(QStringLiteral("抽取"));
    root->addWidget(box);

    connect(browseSrc, &QPushButton::clicked, this, &ExtractDialog::browseSource);
    connect(browseOut, &QPushButton::clicked, this, &ExtractDialog::browseOutput);
    connect(allBtn, &QPushButton::clicked, this, &ExtractDialog::selectAllPages);
    connect(spec_, &QLineEdit::textChanged, this, &ExtractDialog::revalidate);
    connect(source_, &QComboBox::currentIndexChanged, this, [this] {
        output_->setText(suggestOutput());
        reloadPageCount();
    });
    connect(box, &QDialogButtonBox::accepted, this, [this] {
        if (!specValid_) {
            hint_->setText(QStringLiteral("页码范围无效，先改对再抽取"));
            return;
        }
        if (output_->text().trimmed().isEmpty()) {
            hint_->setText(QStringLiteral("请先指定输出文件"));
            return;
        }
        accept();
    });
    connect(box, &QDialogButtonBox::rejected, this, &QDialog::reject);

    output_->setText(suggestOutput());
    reloadPageCount();
}

void ExtractDialog::reloadPageCount() {
    pageCount_ = 0;
    const QString path = sourcePath();
    if (path.isEmpty()) {
        hint_->setText(QStringLiteral("没有可用的源 PDF"));
        specValid_ = false;
        return;
    }
    auto opened = backend_->open(std::filesystem::path(path.toStdString()), std::string());
    if (!opened.ok()) {
        hint_->setText(QStringLiteral("打不开该文件: ") + QString::fromStdString(opened.message()));
        specValid_ = false;
        revalidate();
        return;
    }
    pageCount_ = opened.value()->info().pageCount;
    revalidate();
}

void ExtractDialog::revalidate() {
    const QString text = spec_->text().trimmed();
    if (text.isEmpty()) {
        pages_.clear();
        hint_->setText(pageCount_ > 0 ? QStringLiteral("共 %1 页，填页码范围后抽取").arg(pageCount_)
                                      : QStringLiteral("填页码范围"));
        specValid_ = false;
        return;
    }
    auto pages = pw::parsePageSpec(text.toStdString(), pageCount_);
    if (!pages.ok()) {
        pages_.clear();
        hint_->setText(QStringLiteral("页码范围无效：") + QString::fromStdString(pages.message()));
        specValid_ = false;
        return;
    }
    pages_ = pages.value();
    specValid_ = true;
    hint_->setText(QStringLiteral("共 %1 页，将抽取 %2 页（%3）")
                       .arg(pageCount_)
                       .arg(pages_.size())
                       .arg(QString::fromStdString(pw::formatPageSpec(pages_))));
}

void ExtractDialog::selectAllPages() {
    if (pageCount_ <= 0) {
        hint_->setText(QStringLiteral("还没读到页数，稍等或换个文件"));
        return;
    }
    spec_->setText(QStringLiteral("1-%1").arg(pageCount_));
}

void ExtractDialog::browseSource() {
    const QString file = QFileDialog::getOpenFileName(this, QStringLiteral("选择源 PDF"), QString(),
                                                      QStringLiteral("PDF (*.pdf)"));
    if (file.isEmpty()) return;
    const QString abs = QFileInfo(file).absoluteFilePath();
    if (source_->findData(abs) < 0) source_->addItem(QFileInfo(abs).fileName(), abs);
    source_->setCurrentIndex(source_->findData(abs));
    output_->setText(suggestOutput());
    reloadPageCount();
}

void ExtractDialog::browseOutput() {
    const QString suggested = output_->text().trimmed().isEmpty() ? suggestOutput() : output_->text();
    const QString out = QFileDialog::getSaveFileName(this, QStringLiteral("抽取输出文件"), suggested,
                                                     QStringLiteral("PDF (*.pdf)"));
    if (!out.isEmpty()) output_->setText(out);
}

QString ExtractDialog::suggestOutput() const {
    const QString path = sourcePath();
    if (path.isEmpty()) return QString();
    const QFileInfo fi(path);
    return fi.absolutePath() + QLatin1Char('/') + fi.completeBaseName() + QStringLiteral("_抽页.pdf");
}

QString ExtractDialog::sourcePath() const {
    return source_->currentData().toString();
}

QString ExtractDialog::spec() const {
    return spec_->text().trimmed();
}

QString ExtractDialog::outputPath() const {
    return output_->text().trimmed();
}
