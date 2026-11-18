#include "OcrDialog.h"
#include "I18nUi.h"

#include <chrono>
#include <filesystem>

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QFutureWatcher>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QProgressBar>
#include <QPushButton>
#include <QVBoxLayout>
#include <QtConcurrent/QtConcurrentRun>

#include <pdfword/ocr.hpp>

#include "DialogCommon.h"
#include "pdfword/path_util.hpp"

OcrDialog::OcrDialog(std::shared_ptr<pw::IPdfBackend> backend, const QString& source,
                     QWidget* parent)
    : QDialog(parent), backend_(std::move(backend)), source_(source) {
    setWindowTitle(TR("OCR 文字识别 — %1").arg(QFileInfo(source_).fileName()));
    resize(700, 460);

    auto* root = new QVBoxLayout(this);

    auto* intro = new QLabel(
        TR("把扫描件 / 图片型 PDF 认出文字：默认把文字写成不可见图层，")
            + TR("页面看起来一点没变，但 Ctrl+F 能搜到、能选中复制；也可以另外导出纯文本。"),
        this);
    intro->setWordWrap(true);
    root->addWidget(intro);

    // 引擎能力：可用语言、缺什么，直接摆出来，免得用户对着"没反应"猜
    auto engine = pw::makeOcrEngine();
    auto* engineLabel = new QLabel(QString::fromStdString(pw::ocrCapabilityReport()), this);
    engineLabel->setObjectName(QStringLiteral("ocrEngineLabel"));
    engineLabel->setWordWrap(true);
    root->addWidget(engineLabel);

    auto* form = new QFormLayout();

    langCombo_ = new QComboBox(this);
    langCombo_->setObjectName(QStringLiteral("ocrLanguageCombo"));
    langCombo_->addItem(TR("自动（跟随系统默认）"), QString());
    if (engine) {
        for (const std::string& lang : engine->languages()) {
            langCombo_->addItem(QString::fromStdString(lang), QString::fromStdString(lang));
        }
    }
    // 有中文引擎就直接选中文：中文引擎同时能认英文，反过来不行
    for (int i = 0; i < langCombo_->count(); ++i) {
        if (langCombo_->itemData(i).toString().startsWith(QStringLiteral("zh"))) {
            langCombo_->setCurrentIndex(i);
            break;
        }
    }
    form->addRow(TR("识别语言："), langCombo_);

    pdfCheck_ = new QCheckBox(TR("输出可检索 PDF（不可见文字层）"), this);
    pdfCheck_->setObjectName(QStringLiteral("ocrPdfCheck"));
    pdfCheck_->setChecked(true);
    auto* pdfRow = new QHBoxLayout();
    pdfRow->addWidget(pdfCheck_);
    pdfEdit_ = new QLineEdit(suggestPdfOutput(source_, TR("_可检索")), this);
    pdfEdit_->setObjectName(QStringLiteral("ocrPdfOutput"));
    auto* pdfBrowse = new QPushButton(TR("浏览…"), this);
    pdfRow->addWidget(pdfEdit_, 1);
    pdfRow->addWidget(pdfBrowse);
    form->addRow(TR("PDF 输出："), pdfRow);

    txtCheck_ = new QCheckBox(TR("导出纯文本（.txt，UTF-8）"), this);
    txtCheck_->setObjectName(QStringLiteral("ocrTxtCheck"));
    txtCheck_->setChecked(true);
    auto* txtRow = new QHBoxLayout();
    txtRow->addWidget(txtCheck_);
    const QFileInfo fi(source_);
    txtEdit_ = new QLineEdit(
        fi.absolutePath() + QLatin1Char('/') + fi.completeBaseName() + TR("_识别.txt"),
        this);
    txtEdit_->setObjectName(QStringLiteral("ocrTxtOutput"));
    auto* txtBrowse = new QPushButton(TR("浏览…"), this);
    txtRow->addWidget(txtEdit_, 1);
    txtRow->addWidget(txtBrowse);
    form->addRow(TR("文本输出："), txtRow);

    root->addLayout(form);

    progress_ = new QProgressBar(this);
    progress_->setObjectName(QStringLiteral("ocrProgress"));
    progress_->setRange(0, 1);
    progress_->setValue(0);
    progress_->setTextVisible(false);
    root->addWidget(progress_);

    statusLabel_ = new QLabel(TR("就绪"), this);
    statusLabel_->setObjectName(QStringLiteral("ocrStatus"));
    root->addWidget(statusLabel_);
    root->addStretch(1);

    auto* btnRow = new QHBoxLayout();
    startBtn_ = new QPushButton(TR("开始识别"), this);
    startBtn_->setObjectName(QStringLiteral("ocrStartButton"));
    startBtn_->setDefault(true);
    btnRow->addStretch(1);
    btnRow->addWidget(startBtn_);
    root->addLayout(btnRow);

    auto* box = new QDialogButtonBox(QDialogButtonBox::Close, this);
    box->button(QDialogButtonBox::Close)->setText(TR("关闭"));
    root->addWidget(box);

    connect(pdfBrowse, &QPushButton::clicked, this, &OcrDialog::browsePdfOutput);
    connect(txtBrowse, &QPushButton::clicked, this, &OcrDialog::browseTxtOutput);
    connect(startBtn_, &QPushButton::clicked, this, &OcrDialog::startOcr);
    connect(box, &QDialogButtonBox::rejected, this, &OcrDialog::reject);
}

void OcrDialog::reject() {
    if (busy_) {
        statusLabel_->setText(TR("正在识别，请等这一轮结束再关闭窗口"));
        return;
    }
    QDialog::reject();
}

void OcrDialog::browsePdfOutput() {
    const QString out = QFileDialog::getSaveFileName(this, TR("可检索 PDF 输出"),
                                                     pdfEdit_->text(),
                                                     QStringLiteral("PDF (*.pdf)"));
    if (!out.isEmpty()) pdfEdit_->setText(out);
}

void OcrDialog::browseTxtOutput() {
    const QString out = QFileDialog::getSaveFileName(this, TR("纯文本输出"),
                                                     txtEdit_->text(),
                                                     TR("文本 (*.txt)"));
    if (!out.isEmpty()) txtEdit_->setText(out);
}

void OcrDialog::setBusy(bool busy) {
    busy_ = busy;
    startBtn_->setEnabled(!busy);
    langCombo_->setEnabled(!busy);
    pdfCheck_->setEnabled(!busy);
    txtCheck_->setEnabled(!busy);
    pdfEdit_->setEnabled(!busy);
    txtEdit_->setEnabled(!busy);
    progress_->setRange(0, busy ? 0 : 1);
    progress_->setValue(busy ? 0 : 0);
}

void OcrDialog::startOcr() {
    if (busy_) return;

    const bool wantPdf = pdfCheck_->isChecked();
    const bool wantTxt = txtCheck_->isChecked();
    if (!wantPdf && !wantTxt) {
        QMessageBox::information(this, QStringLiteral("OCR"),
                                 TR("至少勾选“可检索 PDF”或“纯文本”其中一项。"));
        return;
    }
    if (wantPdf && pdfEdit_->text().trimmed().isEmpty()) {
        QMessageBox::information(this, QStringLiteral("OCR"), TR("请先指定 PDF 输出路径。"));
        return;
    }
    if (wantTxt && txtEdit_->text().trimmed().isEmpty()) {
        QMessageBox::information(this, QStringLiteral("OCR"), TR("请先指定文本输出路径。"));
        return;
    }

    const QString pdfOut = pdfEdit_->text().trimmed();
    const QString txtOut = txtEdit_->text().trimmed();
    const QString lang = langCombo_->currentData().toString();

    setBusy(true);
    statusLabel_->setText(TR("识别中…（页数多时需要一会儿，期间窗口不要关）"));
    emit logMessage(TR("OCR 开始: %1").arg(QFileInfo(source_).fileName()));

    // shared_ptr 拷贝进 lambda：即使窗口先被销毁，后台任务也不会踩空指针
    auto backend = backend_;
    const std::string src = source_.toStdString();
    const std::string pdf = pdfOut.toStdString();
    const std::string txt = txtOut.toStdString();
    const std::string language = lang.toStdString();
    const auto started = std::chrono::steady_clock::now();

    auto* watcher = new QFutureWatcher<pw::Status>(this);
    connect(watcher, &QFutureWatcher<pw::Status>::finished, this,
            [this, watcher, wantPdf, wantTxt, pdfOut, txtOut, started] {
                const pw::Status st = watcher->result();
                watcher->deleteLater();
                setBusy(false);

                const double elapsed =
                    std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
                if (!st.ok()) {
                    statusLabel_->setText(TR("识别失败"));
                    QMessageBox::warning(this, TR("OCR 失败"),
                                         QString::fromStdString(st.toString()));
                    emit logMessage(TR("OCR 失败: %1").arg(QString::fromStdString(st.message())));
                    return;
                }
                statusLabel_->setText(TR("完成（用时 %1 秒）").arg(elapsed, 0, 'f', 1));
                if (wantPdf) {
                    emit logMessage(TR("可检索 PDF -> %1（%2 KB）")
                                        .arg(pdfOut)
                                        .arg(QFileInfo(pdfOut).size() / 1024));
                }
                if (wantTxt) {
                    emit logMessage(TR("纯文本 -> %1（%2 KB）")
                                        .arg(txtOut)
                                        .arg(QFileInfo(txtOut).size() / 1024));
                }
                QMessageBox::information(
                    this, TR("OCR 完成"),
                    TR("识别完成（%1 秒）。\n%2%3")
                        .arg(elapsed, 0, 'f', 1)
                        .arg(wantPdf ? TR("可检索 PDF：") + pdfOut + QStringLiteral("\n") : QString())
                        .arg(wantTxt ? TR("纯文本：") + txtOut : QString()));
            });

    auto future = QtConcurrent::run([backend, src, pdf, txt, language, wantPdf, wantTxt] {
        return backend->ocr(pw::fsPath(src),
                            wantPdf ? pw::fsPath(pdf) : std::filesystem::path(),
                            wantTxt ? pw::fsPath(txt) : std::filesystem::path(), language,
                            wantPdf, wantTxt);
    });
    watcher->setFuture(future);
}
