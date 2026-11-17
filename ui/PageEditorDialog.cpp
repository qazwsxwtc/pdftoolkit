#include "PageEditorDialog.h"

#include <algorithm>
#include <filesystem>

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
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
#include <QSpinBox>
#include <QVBoxLayout>

#include "pdfword/page_selection.hpp"

namespace fs = std::filesystem;

namespace {

/// 打开待编辑的源文件；加密时反复问密码，用户取消则返回空
std::shared_ptr<pw::IPdfDocument> openWithPassword(const std::shared_ptr<pw::IPdfBackend>& backend,
                                                   const QString& path, QString* password,
                                                   QWidget* parent) {
    for (int attempt = 0; attempt < 4; ++attempt) {
        auto doc = backend->open(fs::path(path.toStdString()), password->toStdString());
        if (doc.ok()) return doc.value();

        const std::string msg = doc.status().toString();
        if (QString::fromStdString(msg).contains(QStringLiteral("密码"))) {
            bool ok = false;
            const QString input = QInputDialog::getText(parent, QStringLiteral("需要密码"),
                                                        QStringLiteral("这个 PDF 已加密，请输入打开密码："),
                                                        QLineEdit::Password, *password, &ok);
            if (!ok) return nullptr;
            *password = input;
            continue;
        }
        QMessageBox::warning(parent, QStringLiteral("打开失败"), QString::fromStdString(msg));
        return nullptr;
    }
    return nullptr;
}

}  // namespace

PageEditorDialog::PageEditorDialog(std::shared_ptr<pw::IPdfBackend> backend, const QString& source,
                                   std::shared_ptr<pw::IPdfDocument> document,
                                   const QString& password, QWidget* parent)
    : QDialog(parent), backend_(std::move(backend)), source_(source), password_(password) {
    setWindowTitle(QStringLiteral("页面编辑 — %1").arg(QFileInfo(source_).fileName()));
    resize(820, 620);

    std::shared_ptr<pw::IPdfDocument> doc = document;
    if (doc == nullptr) doc = openWithPassword(backend_, source_, &password_, this);

    const int pageCount = (doc != nullptr) ? doc->info().pageCount : 0;
    baseInfo_.resize(static_cast<std::size_t>(std::max(pageCount, 0)));
    for (int i = 0; i < pageCount; ++i) {
        doc->pageInfo(i, &baseInfo_[static_cast<std::size_t>(i)]);
    }
    pages_.clear();
    for (int i = 0; i < pageCount; ++i) {
        pw::PageRef ref;
        ref.source = source_.toStdString();
        ref.pageIndex = i;
        pages_.push_back(ref);
    }

    auto* root = new QHBoxLayout(this);

    // ---- 左：输出页列表 ----
    auto* left = new QVBoxLayout();
    left->addWidget(new QLabel(QStringLiteral("输出页顺序（可多选；这里是最终 PDF 的页序列）"), this));
    list_ = new QListWidget(this);
    list_->setSelectionMode(QAbstractItemView::ExtendedSelection);
    left->addWidget(list_, 1);
    summary_ = new QLabel(this);
    summary_->setWordWrap(true);
    left->addWidget(summary_);
    root->addLayout(left, 1);

    // ---- 右：操作区 ----
    auto* right = new QVBoxLayout();

    auto* moveBox = new QGroupBox(QStringLiteral("顺序"), this);
    auto* moveGrid = new QHBoxLayout(moveBox);
    auto* upBtn = new QPushButton(QStringLiteral("上移"), moveBox);
    auto* downBtn = new QPushButton(QStringLiteral("下移"), moveBox);
    auto* topBtn = new QPushButton(QStringLiteral("置顶"), moveBox);
    auto* bottomBtn = new QPushButton(QStringLiteral("置底"), moveBox);
    auto* delBtn = new QPushButton(QStringLiteral("删除选中"), moveBox);
    moveGrid->addWidget(upBtn);
    moveGrid->addWidget(downBtn);
    moveGrid->addWidget(topBtn);
    moveGrid->addWidget(bottomBtn);
    moveGrid->addWidget(delBtn);
    right->addWidget(moveBox);

    auto* rotBox = new QGroupBox(QStringLiteral("旋转选中页"), this);
    auto* rotGrid = new QHBoxLayout(rotBox);
    auto* rot90 = new QPushButton(QStringLiteral("+90°"), rotBox);
    auto* rot180 = new QPushButton(QStringLiteral("180°"), rotBox);
    auto* rot270 = new QPushButton(QStringLiteral("+270°"), rotBox);
    auto* rotMinus = new QPushButton(QStringLiteral("-90°"), rotBox);
    rotGrid->addWidget(rot90);
    rotGrid->addWidget(rot180);
    rotGrid->addWidget(rot270);
    rotGrid->addWidget(rotMinus);
    right->addWidget(rotBox);

    auto* cropBox = new QGroupBox(QStringLiteral("裁剪选中页（从各边裁掉多少 pt，1pt≈0.35mm）"), this);
    auto* cropForm = new QFormLayout(cropBox);
    cropLeft_ = new QDoubleSpinBox(cropBox);
    cropBottom_ = new QDoubleSpinBox(cropBox);
    cropRight_ = new QDoubleSpinBox(cropBox);
    cropTop_ = new QDoubleSpinBox(cropBox);
    for (auto* spin : {cropLeft_, cropBottom_, cropRight_, cropTop_}) {
        spin->setRange(0.0, 2000.0);
        spin->setDecimals(1);
        spin->setSingleStep(5.0);
    }
    cropForm->addRow(QStringLiteral("左"), cropLeft_);
    cropForm->addRow(QStringLiteral("下"), cropBottom_);
    cropForm->addRow(QStringLiteral("右"), cropRight_);
    cropForm->addRow(QStringLiteral("上"), cropTop_);
    auto* cropBtn = new QPushButton(QStringLiteral("应用到选中页"), cropBox);
    cropForm->addRow(cropBtn);
    auto* cropClearBtn = new QPushButton(QStringLiteral("取消选中页的裁剪"), cropBox);
    cropForm->addRow(cropClearBtn);
    right->addWidget(cropBox);

    auto* insertBox = new QGroupBox(QStringLiteral("插入"), this);
    auto* insertForm = new QFormLayout(insertBox);
    blankCount_ = new QSpinBox(insertBox);
    blankCount_->setRange(1, 100);
    auto* blankBtn = new QPushButton(QStringLiteral("在选中页之后插入空白页"), insertBox);
    insertForm->addRow(QStringLiteral("空白页数量"), blankCount_);
    insertForm->addRow(blankBtn);
    auto* insertPdfBtn = new QPushButton(QStringLiteral("插入其他 PDF 的页面…"), insertBox);
    insertForm->addRow(insertPdfBtn);
    right->addWidget(insertBox);

    right->addStretch(1);

    auto* closeBtn = new QPushButton(QStringLiteral("关闭"), this);
    auto* saveBtn = new QPushButton(QStringLiteral("另存为…"), this);
    saveBtn->setDefault(true);
    auto* bottomRow = new QHBoxLayout();
    bottomRow->addWidget(closeBtn);
    bottomRow->addWidget(saveBtn);
    right->addLayout(bottomRow);
    root->addLayout(right);

    connect(upBtn, &QPushButton::clicked, this, &PageEditorDialog::moveUp);
    connect(downBtn, &QPushButton::clicked, this, &PageEditorDialog::moveDown);
    connect(topBtn, &QPushButton::clicked, this, &PageEditorDialog::moveToTop);
    connect(bottomBtn, &QPushButton::clicked, this, &PageEditorDialog::moveToBottom);
    connect(delBtn, &QPushButton::clicked, this, &PageEditorDialog::deleteSelected);
    connect(rot90, &QPushButton::clicked, this, [this] { rotate(90); });
    connect(rot180, &QPushButton::clicked, this, [this] { rotate(180); });
    connect(rot270, &QPushButton::clicked, this, [this] { rotate(270); });
    connect(rotMinus, &QPushButton::clicked, this, [this] { rotate(-90); });
    connect(cropBtn, &QPushButton::clicked, this, &PageEditorDialog::applyCrop);
    connect(cropClearBtn, &QPushButton::clicked, this, [this] {
        for (int row : selectedRows()) pages_[static_cast<std::size_t>(row)].hasCrop = false;
        rebuildList();
    });
    connect(blankBtn, &QPushButton::clicked, this, &PageEditorDialog::insertBlank);
    connect(insertPdfBtn, &QPushButton::clicked, this, &PageEditorDialog::insertOtherPdf);
    connect(saveBtn, &QPushButton::clicked, this, &PageEditorDialog::saveAs);
    connect(closeBtn, &QPushButton::clicked, this, &QDialog::reject);

    rebuildList();
}

int PageEditorDialog::currentRow() const { return list_->currentRow(); }

std::vector<int> PageEditorDialog::selectedRows() const {
    std::vector<int> rows;
    for (auto* item : list_->selectedItems()) {
        const int row = list_->row(item);
        if (row >= 0) rows.push_back(row);
    }
    std::sort(rows.begin(), rows.end());
    rows.erase(std::unique(rows.begin(), rows.end()), rows.end());
    return rows;
}

QString PageEditorDialog::describe(const pw::PageRef& ref, int index) const {
    QString text = QStringLiteral("%1. ").arg(index + 1);
    if (ref.source.empty()) {
        text += QStringLiteral("[空白页 %1×%2pt]")
                    .arg(ref.blankWidthPt, 0, 'f', 0)
                    .arg(ref.blankHeightPt, 0, 'f', 0);
        return text;
    }

    const QString file = QFileInfo(QString::fromStdString(ref.source.string())).fileName();
    text += QStringLiteral("原第 %1 页（%2）").arg(ref.pageIndex + 1).arg(file);

    const int rot = ((ref.rotateDelta % 360) + 360) % 360;
    if (rot != 0) text += QStringLiteral(" 旋转%1°").arg(rot);
    if (ref.hasCrop) {
        text += QStringLiteral(" 裁剪 L%1 B%2 R%3 T%4")
                    .arg(ref.cropLeft, 0, 'f', 1)
                    .arg(ref.cropBottom, 0, 'f', 1)
                    .arg(ref.cropRight, 0, 'f', 1)
                    .arg(ref.cropTop, 0, 'f', 1);
    }
    return text;
}

void PageEditorDialog::rebuildList() {
    const int keepRow = list_->currentRow();
    list_->clear();
    for (std::size_t i = 0; i < pages_.size(); ++i) {
        list_->addItem(describe(pages_[i], static_cast<int>(i)));
    }
    if (keepRow >= 0 && keepRow < list_->count()) list_->setCurrentRow(keepRow);
    refreshSummary();
}

void PageEditorDialog::refreshSummary() {
    int blanks = 0, rotated = 0, cropped = 0;
    for (const auto& ref : pages_) {
        if (ref.source.empty()) ++blanks;
        if (((ref.rotateDelta % 360) + 360) % 360 != 0) ++rotated;
        if (ref.hasCrop) ++cropped;
    }
    summary_->setText(QStringLiteral("共 %1 页｜空白页 %2｜旋转 %3｜裁剪 %4")
                          .arg(pages_.size())
                          .arg(blanks)
                          .arg(rotated)
                          .arg(cropped));
}

void PageEditorDialog::deleteSelected() {
    auto rows = selectedRows();
    if (rows.empty()) return;
    if (static_cast<int>(rows.size()) == static_cast<int>(pages_.size())) {
        QMessageBox::information(this, QStringLiteral("不能全删"), QStringLiteral("至少保留一页"));
        return;
    }
    for (auto it = rows.rbegin(); it != rows.rend(); ++it) {
        pages_.erase(pages_.begin() + *it);
    }
    rebuildList();
}

void PageEditorDialog::moveUp() {
    auto rows = selectedRows();
    if (rows.empty() || rows.front() == 0) return;
    for (std::size_t i = 0; i < rows.size(); ++i) {
        const int from = rows[i];
        std::swap(pages_[static_cast<std::size_t>(from)], pages_[static_cast<std::size_t>(from - 1)]);
    }
    rebuildList();
    for (int r : rows) list_->item(r - 1)->setSelected(true);
}

void PageEditorDialog::moveDown() {
    auto rows = selectedRows();
    if (rows.empty() || rows.back() == static_cast<int>(pages_.size()) - 1) return;
    for (auto it = rows.rbegin(); it != rows.rend(); ++it) {
        const int from = *it;
        std::swap(pages_[static_cast<std::size_t>(from)], pages_[static_cast<std::size_t>(from + 1)]);
    }
    rebuildList();
    for (int r : rows) list_->item(r + 1)->setSelected(true);
}

void PageEditorDialog::moveToTop() {
    auto rows = selectedRows();
    if (rows.empty()) return;
    std::vector<pw::PageRef> picked;
    for (int r : rows) picked.push_back(pages_[static_cast<std::size_t>(r)]);
    for (auto it = rows.rbegin(); it != rows.rend(); ++it) pages_.erase(pages_.begin() + *it);
    pages_.insert(pages_.begin(), picked.begin(), picked.end());
    rebuildList();
}

void PageEditorDialog::moveToBottom() {
    auto rows = selectedRows();
    if (rows.empty()) return;
    std::vector<pw::PageRef> picked;
    for (int r : rows) picked.push_back(pages_[static_cast<std::size_t>(r)]);
    for (auto it = rows.rbegin(); it != rows.rend(); ++it) pages_.erase(pages_.begin() + *it);
    pages_.insert(pages_.end(), picked.begin(), picked.end());
    rebuildList();
}

void PageEditorDialog::rotate(int degrees) {
    auto rows = selectedRows();
    if (rows.empty()) {
        QMessageBox::information(this, QStringLiteral("未选中页面"),
                                 QStringLiteral("先在左边列表里选中要旋转的页"));
        return;
    }
    for (int r : rows) pages_[static_cast<std::size_t>(r)].rotateDelta += degrees;
    rebuildList();
}

void PageEditorDialog::applyCrop() {
    auto rows = selectedRows();
    if (rows.empty()) {
        QMessageBox::information(this, QStringLiteral("未选中页面"),
                                 QStringLiteral("先在左边列表里选中要裁剪的页"));
        return;
    }
    for (int r : rows) {
        auto& ref = pages_[static_cast<std::size_t>(r)];
        ref.hasCrop = true;
        ref.cropLeft = cropLeft_->value();
        ref.cropBottom = cropBottom_->value();
        ref.cropRight = cropRight_->value();
        ref.cropTop = cropTop_->value();
    }
    rebuildList();
}

void PageEditorDialog::insertBlank() {
    int at = 0;
    auto rows = selectedRows();
    if (!rows.empty()) at = rows.back() + 1;

    const int count = blankCount_->value();
    for (int i = 0; i < count; ++i) {
        pw::PageRef ref;   // source 为空 = 空白页
        pages_.insert(pages_.begin() + at + i, ref);
    }
    rebuildList();
}

void PageEditorDialog::insertOtherPdf() {
    const QString other = QFileDialog::getOpenFileName(this, QStringLiteral("选择要插入的 PDF"),
                                                       QFileInfo(source_).absolutePath(),
                                                       QStringLiteral("PDF (*.pdf)"));
    if (other.isEmpty()) return;

    auto otherDoc = openWithPassword(backend_, other, &password_, this);
    if (otherDoc == nullptr) return;
    const int otherPages = otherDoc->info().pageCount;

    bool ok = false;
    const QString spec = QInputDialog::getText(this, QStringLiteral("插入哪些页"),
                                               QStringLiteral("页码范围（共 %1 页，空=全部）：").arg(otherPages),
                                               QLineEdit::Normal,
                                               (otherPages <= 20) ? QStringLiteral("1-%1").arg(otherPages) : QString(),
                                               &ok);
    if (!ok) return;

    auto pages = pw::parsePageSpec(spec.toStdString(), otherPages);
    if (!pages.ok()) {
        QMessageBox::warning(this, QStringLiteral("页码范围无效"),
                             QString::fromStdString(pages.status().toString()));
        return;
    }

    int at = 0;
    auto rows = selectedRows();
    if (!rows.empty()) at = rows.back() + 1;

    for (std::size_t i = 0; i < pages.value().size(); ++i) {
        pw::PageRef ref;
        ref.source = other.toStdString();
        ref.pageIndex = pages.value()[i];
        pages_.insert(pages_.begin() + at + static_cast<int>(i), ref);
    }
    rebuildList();
}

QString PageEditorDialog::suggestOutput() const {
    const QFileInfo fi(source_);
    return fi.absolutePath() + QLatin1Char('/') + fi.completeBaseName() + QStringLiteral("_编辑.pdf");
}

void PageEditorDialog::saveAs() {
    if (pages_.empty()) {
        QMessageBox::warning(this, QStringLiteral("没有页面"), QStringLiteral("输出至少需要一页"));
        return;
    }
    const QString out = QFileDialog::getSaveFileName(this, QStringLiteral("保存编辑结果"),
                                                     suggestOutput(), QStringLiteral("PDF (*.pdf)"));
    if (out.isEmpty()) return;

    QApplication::setOverrideCursor(Qt::WaitCursor);
    const pw::Status st = backend_->assemble(pages_, fs::path(out.toStdString()), password_.toStdString());
    QApplication::restoreOverrideCursor();

    if (!st.ok()) {
        QMessageBox::warning(this, QStringLiteral("保存失败"), QString::fromStdString(st.toString()));
        emit logMessage(QStringLiteral("页面编辑失败: ") + QString::fromStdString(st.toString()));
        return;
    }
    emit logMessage(QStringLiteral("页面编辑完成：%1 页 -> %2").arg(pages_.size()).arg(out));
    accept();
}
