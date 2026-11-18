#include "ConvertDialog.h"
#include "I18nUi.h"

#include <QComboBox>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QVBoxLayout>

ConvertDialog::ConvertDialog(const QStringList& files, QWidget* parent) : QDialog(parent) {
    setWindowTitle(TR("开始转换 — 选择目标格式"));
    resize(560, 420);

    auto* root = new QVBoxLayout(this);
    root->addWidget(new QLabel(TR("待转换文件（%1 个）：").arg(files.size()), this));

    fileList_ = new QListWidget(this);
    fileList_->setSelectionMode(QAbstractItemView::NoSelection);
    for (const auto& f : files) {
        auto* item = new QListWidgetItem(QFileInfo(f).fileName(), fileList_);
        item->setData(Qt::UserRole, f);
        item->setToolTip(f);
    }
    root->addWidget(fileList_, 1);

    auto* fmtRow = new QHBoxLayout();
    fmtRow->addWidget(new QLabel(TR("目标格式"), this));
    target_ = new QComboBox(this);
    target_->setObjectName(QStringLiteral("formatCombo"));
    target_->addItem(TR("pdf — Word/文档 转 PDF（LibreOffice，保版式）"), QStringLiteral("pdf"));
    target_->addItem(TR("docx — PDF 转 Word（提取文字重排，版式有损）"), QStringLiteral("docx"));
    fmtRow->addWidget(target_, 1);
    root->addLayout(fmtRow);

    auto* outRow = new QHBoxLayout();
    outRow->addWidget(new QLabel(TR("输出目录"), this));
    outDir_ = new QLineEdit(this);
    outDir_->setPlaceholderText(TR("留空 = 与源文件同目录"));
    auto* browseBtn = new QPushButton(TR("浏览…"), this);
    outRow->addWidget(outDir_, 1);
    outRow->addWidget(browseBtn);
    root->addLayout(outRow);

    auto* pwdRow = new QHBoxLayout();
    pwdRow->addWidget(new QLabel(TR("PDF 密码"), this));
    password_ = new QLineEdit(this);
    password_->setObjectName(QStringLiteral("passwordEdit"));
    password_->setEchoMode(QLineEdit::Password);
    password_->setPlaceholderText(TR("加密 PDF 才需要填"));
    pwdRow->addWidget(password_, 1);
    root->addLayout(pwdRow);

    summary_ = new QLabel(this);
    summary_->setObjectName(QStringLiteral("summaryLabel"));
    summary_->setWordWrap(true);
    root->addWidget(summary_);

    auto* box = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    box->button(QDialogButtonBox::Ok)->setText(TR("开始转换"));
    root->addWidget(box);

    connect(browseBtn, &QPushButton::clicked, this, &ConvertDialog::pickOutputDir);
    connect(target_, &QComboBox::currentIndexChanged, this, &ConvertDialog::refreshSummary);
    connect(box, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(box, &QDialogButtonBox::rejected, this, &QDialog::reject);

    refreshSummary();
}

void ConvertDialog::refreshSummary() {
    int pdf = 0;
    int other = 0;
    for (const auto& f : files()) {
        const QString lower = f.toLower();
        if (lower.endsWith(QStringLiteral(".pdf"))) {
            ++pdf;
        } else {
            ++other;
        }
    }
    const bool toPdf = target() == QStringLiteral("pdf");
    QString text = TR("其中 PDF %1 个、其他文档 %2 个。").arg(pdf).arg(other);
    if (toPdf && pdf > 0) text += TR(" 转 pdf 时这 %1 个 PDF 会被跳过（它们已经是 PDF）。").arg(pdf);
    if (!toPdf && other > 0) text += TR(" 只有 PDF 能转 docx，这 %1 个非 PDF 文件会被跳过。").arg(other);
    summary_->setText(text);
}

void ConvertDialog::pickOutputDir() {
    const QString dir = QFileDialog::getExistingDirectory(this, TR("选择输出目录"),
                                                          outDir_->text());
    if (!dir.isEmpty()) outDir_->setText(dir);
}

QStringList ConvertDialog::files() const {
    QStringList out;
    for (int i = 0; i < fileList_->count(); ++i) out << fileList_->item(i)->data(Qt::UserRole).toString();
    return out;
}

QString ConvertDialog::target() const {
    return target_->currentData().toString();
}

QString ConvertDialog::outputDir() const {
    return outDir_->text().trimmed();
}

QString ConvertDialog::pdfPassword() const {
    return password_->text();
}
