#include "MergeDialog.h"
#include "I18nUi.h"

#include <QAbstractItemView>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QVBoxLayout>

#include "DropListWidget.h"

namespace {
const char* kPathKey = "pdfPath";
}

MergeDialog::MergeDialog(const QStringList& candidates, QWidget* parent) : QDialog(parent) {
    setWindowTitle(TR("合并 PDF — 选择要合并的文件"));
    resize(560, 460);

    auto* root = new QVBoxLayout(this);
    root->addWidget(new QLabel(TR("待合并的文件（按列表顺序拼接，越靠上越前；可拖入 PDF）："), this));

    // 支持把 PDF 从资源管理器直接拖进列表
    auto* dropList = new DropListWidget(this);
    dropList->setExtensions({QStringLiteral("pdf")});
    list_ = dropList;
    list_->setObjectName(QStringLiteral("mergeList"));
    list_->setSelectionMode(QAbstractItemView::ExtendedSelection);
    root->addWidget(list_, 1);

    auto* fileRow = new QHBoxLayout();
    auto* addBtn = new QPushButton(TR("添加 PDF…"), this);
    auto* removeBtn = new QPushButton(TR("移除选中"), this);
    auto* upBtn = new QPushButton(TR("上移"), this);
    auto* downBtn = new QPushButton(TR("下移"), this);
    fileRow->addWidget(addBtn);
    fileRow->addWidget(removeBtn);
    fileRow->addWidget(upBtn);
    fileRow->addWidget(downBtn);
    fileRow->addStretch(1);
    root->addLayout(fileRow);

    auto* outRow = new QHBoxLayout();
    outRow->addWidget(new QLabel(TR("输出文件"), this));
    output_ = new QLineEdit(this);
    output_->setObjectName(QStringLiteral("mergeOutput"));
    auto* browseBtn = new QPushButton(TR("浏览…"), this);
    outRow->addWidget(output_, 1);
    outRow->addWidget(browseBtn);
    root->addLayout(outRow);

    auto* box = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    box->button(QDialogButtonBox::Ok)->setText(TR("合并"));
    root->addWidget(box);

    for (const auto& path : candidates) appendFile(path);
    if (list_->count() > 0) output_->setText(suggestOutput());

    connect(addBtn, &QPushButton::clicked, this, &MergeDialog::addFiles);
    connect(dropList, &DropListWidget::filesDropped, this, [this](const QStringList& paths) {
        for (const auto& p : paths) appendFile(p);
        // 拖进来的第一个文件决定默认输出名
        if (output_->text().trimmed().isEmpty() && list_->count() > 0) output_->setText(suggestOutput());
    });
    connect(removeBtn, &QPushButton::clicked, this, &MergeDialog::removeSelected);
    connect(upBtn, &QPushButton::clicked, this, &MergeDialog::moveUp);
    connect(downBtn, &QPushButton::clicked, this, &MergeDialog::moveDown);
    connect(browseBtn, &QPushButton::clicked, this, &MergeDialog::pickOutput);
    connect(box, &QDialogButtonBox::accepted, this, [this] {
        if (list_->count() < 2) {
            QMessageBox::information(this, TR("至少两个文件"),
                                     TR("合并至少需要 2 个 PDF 文件"));
            return;
        }
        if (output_->text().trimmed().isEmpty()) {
            QMessageBox::information(this, TR("需要输出路径"),
                                     TR("请先指定合并后的输出文件"));
            return;
        }
        accept();
    });
    connect(box, &QDialogButtonBox::rejected, this, &QDialog::reject);
}

void MergeDialog::appendFile(const QString& path) {
    if (path.isEmpty()) return;
    const QString file = QFileInfo(path).absoluteFilePath();
    // 同一个文件重复添加没有意义，直接跳过
    for (int i = 0; i < list_->count(); ++i) {
        if (list_->item(i)->data(Qt::UserRole).toString() == file) return;
    }
    auto* item = new QListWidgetItem(QFileInfo(file).fileName(), list_);
    item->setData(Qt::UserRole, file);
    item->setToolTip(file);
}

void MergeDialog::addFiles() {
    const QString dir = list_->count() > 0
                            ? QFileInfo(list_->item(list_->count() - 1)->data(Qt::UserRole).toString()).absolutePath()
                            : QString();
    const QStringList picked = QFileDialog::getOpenFileNames(
        this, TR("添加要合并的 PDF"), dir, QStringLiteral("PDF (*.pdf)"));
    for (const auto& p : picked) appendFile(p);
    if (output_->text().trimmed().isEmpty() && list_->count() > 0) output_->setText(suggestOutput());
}

void MergeDialog::removeSelected() {
    for (auto* item : list_->selectedItems()) delete item;
}

void MergeDialog::moveUp() {
    auto* item = list_->currentItem();
    if (item == nullptr) return;
    const int row = list_->row(item);
    if (row <= 0) return;
    list_->takeItem(row);
    list_->insertItem(row - 1, item);
    list_->setCurrentItem(item);
}

void MergeDialog::moveDown() {
    auto* item = list_->currentItem();
    if (item == nullptr) return;
    const int row = list_->row(item);
    if (row < 0 || row >= list_->count() - 1) return;
    list_->takeItem(row);
    list_->insertItem(row + 1, item);
    list_->setCurrentItem(item);
}

void MergeDialog::pickOutput() {
    const QString suggested = output_->text().trimmed().isEmpty() ? suggestOutput() : output_->text();
    const QString out = QFileDialog::getSaveFileName(this, TR("合并输出文件"), suggested,
                                                     QStringLiteral("PDF (*.pdf)"));
    if (!out.isEmpty()) output_->setText(out);
}

QString MergeDialog::suggestOutput() const {
    if (list_->count() == 0) return QString();
    const QFileInfo first(list_->item(0)->data(Qt::UserRole).toString());
    QString base = first.absolutePath() + QLatin1Char('/') + first.completeBaseName();
    if (list_->count() > 1) base += TR("_等%1个").arg(list_->count());
    return base + TR("_合并.pdf");
}

QStringList MergeDialog::files() const {
    QStringList out;
    for (int i = 0; i < list_->count(); ++i) out << list_->item(i)->data(Qt::UserRole).toString();
    return out;
}

QString MergeDialog::outputPath() const {
    return output_->text().trimmed();
}
