#include "EditDialogs.h"

#include <filesystem>
#include <map>
#include <vector>

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QCursor>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMainWindow>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSpinBox>
#include <QStatusBar>
#include <QStringConverter>
#include <QTableWidget>
#include <QTabWidget>
#include <QTextStream>
#include <QTimer>
#include <QToolTip>
#include <QVBoxLayout>

#include "DialogCommon.h"
#include "PdfViewWidget.h"
#include "pdfword/path_util.hpp"

namespace {

// ---------------------------------------------------------------------------
// 「在预览里框选」的进入/退出。
//
// 这 5 个窗口都是用 exec() 开的模态窗口：模态期间主窗口收不到鼠标，
// 预览也被窗口本身挡住，用户根本拖不了框。Qt 判断"是否被模态挡住"看的是
// 模态窗口**是否可见**，所以这里先把自己 hide() 起来，主窗口立刻可交互，
// 拖完框再由 rectSelected 槽把窗口 show 回来。
// ---------------------------------------------------------------------------
void enterPickMode(QDialog* dlg, PdfViewWidget* viewer, bool* picking, const QString& source) {
    if (viewer == nullptr || viewer->currentPath().isEmpty()) {
        QMessageBox::information(dlg, QStringLiteral("没有预览"),
                                 QStringLiteral("先在主窗口预览这个 PDF，再回来点框选。"));
        return;
    }
    // 预览里必须是同一个文件，否则框出来的坐标会落到别的页面上
    const QString shown = QFileInfo(viewer->currentPath()).canonicalFilePath();
    const QString wanted = QFileInfo(source).canonicalFilePath();
    if (!shown.isEmpty() && !wanted.isEmpty() && shown != wanted) {
        QMessageBox::information(
            dlg, QStringLiteral("预览的不是这个文件"),
            QStringLiteral("正在编辑：%1\n预览里打开的是：%2\n\n先在主窗口打开要编辑的文件，再来框选。")
                .arg(source, viewer->currentPath()));
        return;
    }
    *picking = true;
    viewer->setSelectMode(true);

    QWidget* main = dlg->parentWidget() != nullptr ? dlg->parentWidget()->window() : nullptr;
    dlg->hide();
    if (main != nullptr) {
        main->raise();
        main->activateWindow();
        if (auto* mw = qobject_cast<QMainWindow*>(main)) {
            mw->statusBar()->showMessage(
                QStringLiteral("框选中：在预览上按住左键拖一个框，松手后自动回到窗口（30 秒不拖框会自动返回）"),
                3000);
        }
    }
    QToolTip::showText(QCursor::pos(),
                       QStringLiteral("在预览区按住左键拖一个框，松开后自动回到这个窗口"), main);

    // 兜底：窗口藏着又没有框选时，不能让它一直不回来
    QTimer::singleShot(30000, dlg, [dlg, viewer, picking]() {
        if (!*picking) return;
        *picking = false;
        viewer->setSelectMode(false);
        dlg->show();
        dlg->raise();
        dlg->activateWindow();
    });
}

void finishPickMode(QDialog* dlg, PdfViewWidget* viewer, bool* picking) {
    *picking = false;
    if (viewer != nullptr) viewer->setSelectMode(false);
    dlg->show();
    dlg->raise();
    dlg->activateWindow();
}

QDoubleSpinBox* coordSpin(QWidget* parent) {
    auto* s = new QDoubleSpinBox(parent);
    s->setRange(0.0, 5000.0);
    s->setDecimals(1);
    s->setSingleStep(5.0);
    s->setValue(0.0);
    s->setMaximumWidth(90);
    return s;
}

// 一行 "左上 x0/y0 → 右下 x1/y1" 的坐标输入；prefix 决定控件 objectName（冒烟测试要按名字取）
void buildRectRow(QHBoxLayout* row, QWidget* parent, QDoubleSpinBox** a, QDoubleSpinBox** b,
                  QDoubleSpinBox** c, QDoubleSpinBox** d, const char* prefix = "rect") {
    *a = coordSpin(parent);
    *b = coordSpin(parent);
    *c = coordSpin(parent);
    *d = coordSpin(parent);
    (*a)->setObjectName(QStringLiteral("%1X0").arg(QString::fromUtf8(prefix)));
    (*b)->setObjectName(QStringLiteral("%1Y0").arg(QString::fromUtf8(prefix)));
    (*c)->setObjectName(QStringLiteral("%1X1").arg(QString::fromUtf8(prefix)));
    (*d)->setObjectName(QStringLiteral("%1Y1").arg(QString::fromUtf8(prefix)));
    row->addWidget(new QLabel(QStringLiteral("左上"), parent));
    row->addWidget(*a);
    row->addWidget(*b);
    row->addWidget(new QLabel(QStringLiteral("右下"), parent));
    row->addWidget(*c);
    row->addWidget(*d);
    row->addStretch(1);
}

const char* const kColors[] = {"#FFD400", "#FF0000", "#00A000", "#0060D0", "#8000C0", "#000000"};

QComboBox* colorCombo(QWidget* parent) {
    auto* c = new QComboBox(parent);
    c->addItem(QStringLiteral("黄色 #FFD400"), QStringLiteral("#FFD400"));
    c->addItem(QStringLiteral("红色 #FF0000"), QStringLiteral("#FF0000"));
    c->addItem(QStringLiteral("绿色 #00A000"), QStringLiteral("#00A000"));
    c->addItem(QStringLiteral("蓝色 #0060D0"), QStringLiteral("#0060D0"));
    c->addItem(QStringLiteral("紫色 #8000C0"), QStringLiteral("#8000C0"));
    c->addItem(QStringLiteral("黑色 #000000"), QStringLiteral("#000000"));
    return c;
}

pw::RectD rectOf(double x0, double y0, double x1, double y1) {
    pw::RectD r;
    r.x0 = x0;
    r.y0 = y0;
    r.x1 = x1;
    r.y1 = y1;
    return r;
}

}  // namespace

// ===========================================================================
// 批注窗口
// ===========================================================================

AnnotDialog::AnnotDialog(std::shared_ptr<pw::IPdfBackend> backend, const QString& source,
                         PdfViewWidget* viewer, QWidget* parent)
    : QDialog(parent), backend_(std::move(backend)), source_(source), viewer_(viewer) {
    setWindowTitle(QStringLiteral("注释批注 — %1").arg(QFileInfo(source_).fileName()));
    resize(640, 480);

    auto* root = new QVBoxLayout(this);
    auto* form = new QFormLayout();

    kind_ = new QComboBox(this);
    kind_->setObjectName(QStringLiteral("annotKind"));
    kind_->addItem(QStringLiteral("高亮"), int(pw::AnnotKind::Highlight));
    kind_->addItem(QStringLiteral("下划线"), int(pw::AnnotKind::Underline));
    kind_->addItem(QStringLiteral("删除线"), int(pw::AnnotKind::StrikeOut));
    kind_->addItem(QStringLiteral("波浪线"), int(pw::AnnotKind::Squiggly));
    kind_->addItem(QStringLiteral("便签"), int(pw::AnnotKind::Note));
    kind_->addItem(QStringLiteral("文本框"), int(pw::AnnotKind::FreeText));
    kind_->addItem(QStringLiteral("手绘标记"), int(pw::AnnotKind::Ink));
    form->addRow(QStringLiteral("类型"), kind_);

    text_ = new QPlainTextEdit(this);
    text_->setObjectName(QStringLiteral("annotText"));
    text_->setPlaceholderText(QStringLiteral("便签 / 文本框的内容（其它类型忽略）"));
    text_->setPlainText(QStringLiteral("这里是批注"));
    text_->setMaximumHeight(64);
    form->addRow(QStringLiteral("内容"), text_);

    color_ = colorCombo(this);
    form->addRow(QStringLiteral("颜色"), color_);

    opacity_ = new QDoubleSpinBox(this);
    opacity_->setRange(0.1, 1.0);
    opacity_->setSingleStep(0.05);
    opacity_->setDecimals(2);
    opacity_->setValue(1.0);
    form->addRow(QStringLiteral("不透明度"), opacity_);

    author_ = new QLineEdit(this);
    author_->setText(QStringLiteral("Pdf-Toolkit"));
    form->addRow(QStringLiteral("作者"), author_);

    auto* pageRow = new QHBoxLayout();
    pageRow->addWidget(new QLabel(QStringLiteral("页码"), this));
    page_ = new QSpinBox(this);
    page_->setObjectName(QStringLiteral("annotPage"));
    page_->setRange(1, 9999);
    page_->setValue(1);
    pageRow->addWidget(page_);
    auto* pick = new QPushButton(QStringLiteral("在预览里框选位置"), this);
    pick->setObjectName(QStringLiteral("annotPick"));
    pageRow->addWidget(pick);
    pageRow->addStretch(1);
    form->addRow(QStringLiteral("位置"), pageRow);

    auto* rectRow = new QHBoxLayout();
    buildRectRow(rectRow, this, &x0_, &y0_, &x1_, &y1_, "annot");
    form->addRow(QStringLiteral("矩形(pt)"), rectRow);
    root->addLayout(form);

    hint_ = new QLabel(QStringLiteral("坐标是页面左上原点、y 向下的 pt；不填也可以直接点「在预览里框选」。"), this);
    hint_->setStyleSheet(QStringLiteral("color:#666;"));
    root->addWidget(hint_);
    root->addStretch(1);

    auto* btnRow = new QHBoxLayout();
    auto* applyBtn = new QPushButton(QStringLiteral("加批注并另存为…"), this);
    applyBtn->setObjectName(QStringLiteral("annotApply"));
    btnRow->addStretch(1);
    btnRow->addWidget(applyBtn);
    root->addLayout(btnRow);

    auto* box = new QDialogButtonBox(QDialogButtonBox::Close, this);
    box->button(QDialogButtonBox::Close)->setText(QStringLiteral("关闭"));
    root->addWidget(box);

    connect(pick, &QPushButton::clicked, this, &AnnotDialog::pickSelection);
    connect(kind_, &QComboBox::currentIndexChanged, this, &AnnotDialog::onKindChanged);
    connect(applyBtn, &QPushButton::clicked, this, &AnnotDialog::apply);
    connect(box, &QDialogButtonBox::rejected, this, &QDialog::reject);
    if (viewer_ != nullptr) {
        connect(viewer_, &PdfViewWidget::rectSelected, this, &AnnotDialog::fillFromSelection);
    }
    onKindChanged();
}

void AnnotDialog::onKindChanged() {
    const int kind = kind_->currentData().toInt();
    const bool needText = (kind == int(pw::AnnotKind::Note) || kind == int(pw::AnnotKind::FreeText));
    text_->setEnabled(needText);
}

void AnnotDialog::pickSelection() {
    enterPickMode(this, viewer_, &picking_, source_);
}

void AnnotDialog::fillFromSelection(int page, QRectF rect) {
    if (!picking_) return;
    page_->setValue(page);
    x0_->setValue(rect.left());
    y0_->setValue(rect.top());
    x1_->setValue(rect.right());
    y1_->setValue(rect.bottom());
    hint_->setText(QStringLiteral("已取到第 %1 页的选区：%2,%3 → %4,%5")
                       .arg(page)
                       .arg(rect.left(), 0, 'f', 1)
                       .arg(rect.top(), 0, 'f', 1)
                       .arg(rect.right(), 0, 'f', 1)
                       .arg(rect.bottom(), 0, 'f', 1));
    finishPickMode(this, viewer_, &picking_);
}

void AnnotDialog::apply() {
    pw::AnnotSpec spec;
    spec.kind = static_cast<pw::AnnotKind>(kind_->currentData().toInt());
    spec.page = page_->value() - 1;  // 界面 1 基、core 0 基
    spec.rect = rectOf(x0_->value(), y0_->value(), x1_->value(), y1_->value());
    if (spec.rect.empty()) {
        QMessageBox::information(this, QStringLiteral("选区太小"),
                                 QStringLiteral("先框选或手输一个不小于 0.5pt 的矩形。"));
        return;
    }
    spec.text = text_->toPlainText().toStdString();
    spec.colorHex = color_->currentData().toString().toStdString();
    spec.author = author_->text().trimmed().toStdString();
    spec.opacity = opacity_->value();
    if (spec.kind == pw::AnnotKind::Ink) {
        // 手绘：沿矩形画一条波浪折线，够看得出是"手绘痕迹"
        std::vector<pw::PointD> pts;
        const int steps = 6;
        for (int i = 0; i <= steps; ++i) {
            pw::PointD p;
            p.x = spec.rect.x0 + spec.rect.width() * i / steps;
            p.y = spec.rect.y0 + spec.rect.height() * (i % 2 == 0 ? 0.75 : 0.25);
            pts.push_back(p);
        }
        spec.inkPaths.push_back(pts);
    }

    const QString out = QFileDialog::getSaveFileName(this, QStringLiteral("批注输出"),
                                                     suggestPdfOutput(source_, QStringLiteral("_批注")),
                                                     QStringLiteral("PDF (*.pdf)"));
    if (out.isEmpty()) return;

    QApplication::setOverrideCursor(Qt::WaitCursor);
    std::vector<pw::AnnotSpec> specs{spec};
    const pw::Status st = backend_->annotate(pw::fsPath(source_.toUtf8().toStdString()),
                                             pw::fsPath(out.toUtf8().toStdString()), specs);
    QApplication::restoreOverrideCursor();

    if (!st.ok()) {
        QMessageBox::warning(this, QStringLiteral("加批注失败"),
                             QString::fromStdString(st.toString()));
        return;
    }
    emit logMessage(QStringLiteral("已加批注（%1，第 %2 页） -> %3")
                        .arg(kind_->currentText())
                        .arg(page_->value())
                        .arg(out));
}

// ===========================================================================
// 超链接窗口
// ===========================================================================

LinkDialog::LinkDialog(std::shared_ptr<pw::IPdfBackend> backend, const QString& source,
                       PdfViewWidget* viewer, QWidget* parent)
    : QDialog(parent), backend_(std::move(backend)), source_(source), viewer_(viewer) {
    setWindowTitle(QStringLiteral("添加超链接 — %1").arg(QFileInfo(source_).fileName()));
    resize(620, 360);

    auto* root = new QVBoxLayout(this);
    auto* form = new QFormLayout();

    uri_ = new QLineEdit(this);
    uri_->setObjectName(QStringLiteral("linkUri"));
    uri_->setPlaceholderText(QStringLiteral("https://example.com 或 第3页 / #page=3"));
    uri_->setText(QStringLiteral("https://"));
    form->addRow(QStringLiteral("目标"), uri_);

    auto* pageRow = new QHBoxLayout();
    pageRow->addWidget(new QLabel(QStringLiteral("页码"), this));
    page_ = new QSpinBox(this);
    page_->setObjectName(QStringLiteral("linkPage"));
    page_->setRange(1, 9999);
    pageRow->addWidget(page_);
    auto* pick = new QPushButton(QStringLiteral("在预览里框选位置"), this);
    pageRow->addWidget(pick);
    pageRow->addStretch(1);
    form->addRow(QStringLiteral("位置"), pageRow);

    auto* rectRow = new QHBoxLayout();
    buildRectRow(rectRow, this, &x0_, &y0_, &x1_, &y1_, "link");
    form->addRow(QStringLiteral("矩形(pt)"), rectRow);
    root->addLayout(form);

    hint_ = new QLabel(QStringLiteral("「第3页」「P3」「3」都会当成页内跳转；其它一律按外链网址。"), this);
    hint_->setStyleSheet(QStringLiteral("color:#666;"));
    root->addWidget(hint_);
    root->addStretch(1);

    auto* btnRow = new QHBoxLayout();
    auto* applyBtn = new QPushButton(QStringLiteral("加链接并另存为…"), this);
    btnRow->addStretch(1);
    btnRow->addWidget(applyBtn);
    root->addLayout(btnRow);

    auto* box = new QDialogButtonBox(QDialogButtonBox::Close, this);
    box->button(QDialogButtonBox::Close)->setText(QStringLiteral("关闭"));
    root->addWidget(box);

    connect(pick, &QPushButton::clicked, this, &LinkDialog::pickSelection);
    connect(applyBtn, &QPushButton::clicked, this, &LinkDialog::apply);
    connect(box, &QDialogButtonBox::rejected, this, &QDialog::reject);
    if (viewer_ != nullptr) {
        connect(viewer_, &PdfViewWidget::rectSelected, this, &LinkDialog::fillFromSelection);
    }
}

void LinkDialog::pickSelection() {
    enterPickMode(this, viewer_, &picking_, source_);
}

void LinkDialog::fillFromSelection(int page, QRectF rect) {
    if (!picking_) return;
    page_->setValue(page);
    x0_->setValue(rect.left());
    y0_->setValue(rect.top());
    x1_->setValue(rect.right());
    y1_->setValue(rect.bottom());
    if (hint_ != nullptr) {
        hint_->setText(QStringLiteral("已取到第 %1 页的选区：%2,%3 → %4,%5")
                           .arg(page)
                           .arg(rect.left(), 0, 'f', 1)
                           .arg(rect.top(), 0, 'f', 1)
                           .arg(rect.right(), 0, 'f', 1)
                           .arg(rect.bottom(), 0, 'f', 1));
    }
    finishPickMode(this, viewer_, &picking_);
}

void LinkDialog::apply() {
    const QString uri = uri_->text().trimmed();
    if (uri.isEmpty()) {
        QMessageBox::information(this, QStringLiteral("需要目标"),
                                 QStringLiteral("填一个网址，或者要跳转的页码（如 第3页）。"));
        return;
    }
    pw::LinkSpec spec;
    spec.page = page_->value() - 1;
    spec.rect = rectOf(x0_->value(), y0_->value(), x1_->value(), y1_->value());
    if (spec.rect.empty()) {
        QMessageBox::information(this, QStringLiteral("选区太小"), QStringLiteral("先框选一个区域。"));
        return;
    }
    spec.uri = uri.toStdString();

    const QString out = QFileDialog::getSaveFileName(this, QStringLiteral("链接输出"),
                                                     suggestPdfOutput(source_, QStringLiteral("_链接")),
                                                     QStringLiteral("PDF (*.pdf)"));
    if (out.isEmpty()) return;

    QApplication::setOverrideCursor(Qt::WaitCursor);
    const pw::Status st = backend_->addLink(pw::fsPath(source_.toUtf8().toStdString()),
                                            pw::fsPath(out.toUtf8().toStdString()), spec);
    QApplication::restoreOverrideCursor();
    if (!st.ok()) {
        QMessageBox::warning(this, QStringLiteral("加链接失败"),
                             QString::fromStdString(st.toString()));
        return;
    }
    emit logMessage(QStringLiteral("已加超链接（%1） -> %2").arg(uri).arg(out));
}

// ===========================================================================
// 查找替换窗口
// ===========================================================================

FindReplaceDialog::FindReplaceDialog(std::shared_ptr<pw::IPdfBackend> backend, const QString& source,
                                     QWidget* parent)
    : QDialog(parent), backend_(std::move(backend)), source_(source) {
    setWindowTitle(QStringLiteral("查找替换 — %1").arg(QFileInfo(source_).fileName()));
    resize(620, 380);

    auto* root = new QVBoxLayout(this);
    auto* form = new QFormLayout();

    find_ = new QLineEdit(this);
    find_->setObjectName(QStringLiteral("findText"));
    find_->setPlaceholderText(QStringLiteral("要查找的文字，如 book"));
    form->addRow(QStringLiteral("查找"), find_);

    replace_ = new QLineEdit(this);
    replace_->setObjectName(QStringLiteral("replaceText"));
    replace_->setPlaceholderText(QStringLiteral("替换成什么（勾了「只删除」就忽略）"));
    form->addRow(QStringLiteral("替换为"), replace_);

    auto* optRow = new QHBoxLayout();
    deleteOnly_ = new QCheckBox(QStringLiteral("只删除（白底盖掉，不写新字）"), this);
    caseSensitive_ = new QCheckBox(QStringLiteral("区分大小写"), this);
    optRow->addWidget(deleteOnly_);
    optRow->addWidget(caseSensitive_);
    optRow->addStretch(1);
    form->addRow(QStringLiteral("选项"), optRow);

    auto* styleRow = new QHBoxLayout();
    styleRow->addWidget(new QLabel(QStringLiteral("字号"), this));
    fontSize_ = new QDoubleSpinBox(this);
    fontSize_->setRange(0.0, 200.0);
    fontSize_->setDecimals(1);
    fontSize_->setSpecialValueText(QStringLiteral("自适应"));
    fontSize_->setValue(0.0);
    styleRow->addWidget(fontSize_);
    styleRow->addWidget(new QLabel(QStringLiteral("颜色"), this));
    color_ = colorCombo(this);
    color_->setCurrentIndex(5);  // 黑色
    styleRow->addWidget(color_);
    styleRow->addWidget(new QLabel(QStringLiteral("最多替换"), this));
    limit_ = new QSpinBox(this);
    limit_->setRange(0, 9999);
    limit_->setSpecialValueText(QStringLiteral("不限"));
    styleRow->addWidget(limit_);
    styleRow->addStretch(1);
    form->addRow(QStringLiteral("新字样式"), styleRow);
    root->addLayout(form);

    result_ = new QLabel(QStringLiteral("先点「查找」，看看这份文档里有多少处。"), this);
    result_->setObjectName(QStringLiteral("findResult"));
    result_->setStyleSheet(QStringLiteral("color:#444;"));
    result_->setWordWrap(true);
    root->addWidget(result_);
    root->addStretch(1);

    auto* btnRow = new QHBoxLayout();
    auto* findBtn = new QPushButton(QStringLiteral("查找"), this);
    findBtn->setObjectName(QStringLiteral("findPreview"));
    auto* applyBtn = new QPushButton(QStringLiteral("替换并另存为…"), this);
    applyBtn->setObjectName(QStringLiteral("replaceApply"));
    btnRow->addStretch(1);
    btnRow->addWidget(findBtn);
    btnRow->addWidget(applyBtn);
    root->addLayout(btnRow);

    auto* box = new QDialogButtonBox(QDialogButtonBox::Close, this);
    box->button(QDialogButtonBox::Close)->setText(QStringLiteral("关闭"));
    root->addWidget(box);

    connect(findBtn, &QPushButton::clicked, this, &FindReplaceDialog::previewFind);
    connect(applyBtn, &QPushButton::clicked, this, &FindReplaceDialog::applyReplace);
    connect(box, &QDialogButtonBox::rejected, this, &QDialog::reject);
}

void FindReplaceDialog::previewFind() {
    const QString needle = find_->text();
    if (needle.isEmpty()) {
        QMessageBox::information(this, QStringLiteral("需要查找内容"), QStringLiteral("先填要查找的文字。"));
        return;
    }
    QApplication::setOverrideCursor(Qt::WaitCursor);
    auto hits = backend_->findText(pw::fsPath(source_.toUtf8().toStdString()), needle.toStdString());
    QApplication::restoreOverrideCursor();
    if (!hits.ok()) {
        result_->setText(QStringLiteral("查找失败：%1").arg(QString::fromStdString(hits.status().toString())));
        return;
    }
    if (hits.value().empty()) {
        result_->setText(QStringLiteral("没找到「%1」。").arg(needle));
        return;
    }
    std::map<int, int> perPage;
    for (const pw::TextHit& h : hits.value()) perPage[h.page] += 1;
    QString pages;
    for (const auto& kv : perPage) {
        pages += QStringLiteral("第%1页×%2  ").arg(kv.first + 1).arg(kv.second);
    }
    result_->setText(QStringLiteral("找到 %1 处：%2").arg(hits.value().size()).arg(pages));
    emit logMessage(QStringLiteral("查找「%1」命中 %2 处").arg(needle).arg(hits.value().size()));
}

void FindReplaceDialog::applyReplace() {
    const QString needle = find_->text();
    if (needle.isEmpty()) {
        QMessageBox::information(this, QStringLiteral("需要查找内容"), QStringLiteral("先填要查找的文字。"));
        return;
    }
    const QString replacement = deleteOnly_->isChecked() ? QString() : replace_->text();
    if (replacement.isEmpty() && !deleteOnly_->isChecked()) {
        QMessageBox::information(this, QStringLiteral("需要替换内容"),
                                 QStringLiteral("填替换文字，或勾上「只删除」。"));
        return;
    }
    const QString out = QFileDialog::getSaveFileName(this, QStringLiteral("替换结果"),
                                                     suggestPdfOutput(source_, QStringLiteral("_替换")),
                                                     QStringLiteral("PDF (*.pdf)"));
    if (out.isEmpty()) return;

    pw::ReplaceOptions opts;
    opts.deleteOnly = deleteOnly_->isChecked();
    opts.fontSize = fontSize_->value();
    opts.colorHex = color_->currentData().toString().toStdString();
    opts.limit = limit_->value();
    opts.caseSensitive = caseSensitive_->isChecked();

    QApplication::setOverrideCursor(Qt::WaitCursor);
    const pw::Status st = backend_->replaceText(
        pw::fsPath(source_.toUtf8().toStdString()), pw::fsPath(out.toUtf8().toStdString()),
        needle.toStdString(), replacement.toStdString(), opts);
    QApplication::restoreOverrideCursor();
    if (!st.ok()) {
        QMessageBox::warning(this, QStringLiteral("替换失败"), QString::fromStdString(st.toString()));
        return;
    }
    emit logMessage(QStringLiteral("已替换「%1」%2 -> %3")
                        .arg(needle, deleteOnly_->isChecked() ? QStringLiteral("（只删除）") : QString())
                        .arg(out));
}

// ===========================================================================
// 替换图片窗口
// ===========================================================================

ReplaceImageDialog::ReplaceImageDialog(std::shared_ptr<pw::IPdfBackend> backend, const QString& source,
                                       PdfViewWidget* viewer, QWidget* parent)
    : QDialog(parent), backend_(std::move(backend)), source_(source), viewer_(viewer) {
    setWindowTitle(QStringLiteral("替换图片 — %1").arg(QFileInfo(source_).fileName()));
    resize(620, 360);

    auto* root = new QVBoxLayout(this);
    auto* form = new QFormLayout();

    auto* imageRow = new QHBoxLayout();
    image_ = new QLineEdit(this);
    image_->setObjectName(QStringLiteral("replaceImage"));
    image_->setPlaceholderText(QStringLiteral("PNG / JPG"));
    auto* browse = new QPushButton(QStringLiteral("浏览…"), this);
    imageRow->addWidget(image_, 1);
    imageRow->addWidget(browse);
    form->addRow(QStringLiteral("新图片"), imageRow);

    auto* pageRow = new QHBoxLayout();
    pageRow->addWidget(new QLabel(QStringLiteral("页码"), this));
    page_ = new QSpinBox(this);
    page_->setObjectName(QStringLiteral("imagePage"));
    page_->setRange(1, 9999);
    pageRow->addWidget(page_);
    auto* pick = new QPushButton(QStringLiteral("在预览里框选位置"), this);
    pageRow->addWidget(pick);
    pageRow->addStretch(1);
    form->addRow(QStringLiteral("位置"), pageRow);

    auto* rectRow = new QHBoxLayout();
    buildRectRow(rectRow, this, &x0_, &y0_, &x1_, &y1_, "image");
    form->addRow(QStringLiteral("矩形(pt)"), rectRow);
    root->addLayout(form);

    hint_ = new QLabel(QStringLiteral("新图会等比装进这个矩形并居中，原图被白底盖掉。"), this);
    hint_->setStyleSheet(QStringLiteral("color:#666;"));
    root->addWidget(hint_);
    root->addStretch(1);

    auto* btnRow = new QHBoxLayout();
    auto* applyBtn = new QPushButton(QStringLiteral("替换并另存为…"), this);
    btnRow->addStretch(1);
    btnRow->addWidget(applyBtn);
    root->addLayout(btnRow);

    auto* box = new QDialogButtonBox(QDialogButtonBox::Close, this);
    box->button(QDialogButtonBox::Close)->setText(QStringLiteral("关闭"));
    root->addWidget(box);

    connect(browse, &QPushButton::clicked, this, &ReplaceImageDialog::pickImage);
    connect(pick, &QPushButton::clicked, this, &ReplaceImageDialog::pickSelection);
    connect(applyBtn, &QPushButton::clicked, this, &ReplaceImageDialog::apply);
    connect(box, &QDialogButtonBox::rejected, this, &QDialog::reject);
    if (viewer_ != nullptr) {
        connect(viewer_, &PdfViewWidget::rectSelected, this, &ReplaceImageDialog::fillFromSelection);
    }
}

void ReplaceImageDialog::pickImage() {
    const QString file = QFileDialog::getOpenFileName(
        this, QStringLiteral("选择新图片"), QFileInfo(source_).absolutePath(),
        QStringLiteral("图片 (*.png *.jpg *.jpeg *.bmp *.tif *.tiff);;所有文件 (*)"));
    if (file.isEmpty()) return;
    image_->setText(file);
    hint_->setText(QStringLiteral("已选择：%1").arg(QFileInfo(file).fileName()));
}

void ReplaceImageDialog::pickSelection() {
    enterPickMode(this, viewer_, &picking_, source_);
}

void ReplaceImageDialog::fillFromSelection(int page, QRectF rect) {
    if (!picking_) return;
    page_->setValue(page);
    x0_->setValue(rect.left());
    y0_->setValue(rect.top());
    x1_->setValue(rect.right());
    y1_->setValue(rect.bottom());
    if (hint_ != nullptr) {
        hint_->setText(QStringLiteral("已取到第 %1 页的选区：%2,%3 → %4,%5")
                           .arg(page)
                           .arg(rect.left(), 0, 'f', 1)
                           .arg(rect.top(), 0, 'f', 1)
                           .arg(rect.right(), 0, 'f', 1)
                           .arg(rect.bottom(), 0, 'f', 1));
    }
    finishPickMode(this, viewer_, &picking_);
}

void ReplaceImageDialog::apply() {
    const QString imagePath = image_->text().trimmed();
    if (imagePath.isEmpty() || !QFileInfo::exists(imagePath)) {
        QMessageBox::information(this, QStringLiteral("需要图片"),
                                 QStringLiteral("先选一张存在的图片。"));
        return;
    }
    pw::RectD rect = rectOf(x0_->value(), y0_->value(), x1_->value(), y1_->value());
    if (rect.empty()) {
        QMessageBox::information(this, QStringLiteral("选区太小"), QStringLiteral("先框选一个区域。"));
        return;
    }
    const QString out = QFileDialog::getSaveFileName(this, QStringLiteral("替换结果"),
                                                     suggestPdfOutput(source_, QStringLiteral("_换图")),
                                                     QStringLiteral("PDF (*.pdf)"));
    if (out.isEmpty()) return;

    QApplication::setOverrideCursor(Qt::WaitCursor);
    const pw::Status st = backend_->replaceImage(
        pw::fsPath(source_.toUtf8().toStdString()), pw::fsPath(out.toUtf8().toStdString()),
        page_->value() - 1, rect, pw::fsPath(imagePath.toUtf8().toStdString()));
    QApplication::restoreOverrideCursor();
    if (!st.ok()) {
        QMessageBox::warning(this, QStringLiteral("替换图片失败"),
                             QString::fromStdString(st.toString()));
        return;
    }
    emit logMessage(QStringLiteral("已替换第 %1 页图片 -> %2").arg(page_->value()).arg(out));
}

// ===========================================================================
// 表单处理窗口（填写 / 制作 / 提取）
// ===========================================================================

FormDialog::FormDialog(std::shared_ptr<pw::IPdfBackend> backend, const QString& source,
                       PdfViewWidget* viewer, QWidget* parent)
    : QDialog(parent), backend_(std::move(backend)), source_(source), viewer_(viewer) {
    setWindowTitle(QStringLiteral("表单处理 — %1").arg(QFileInfo(source_).fileName()));
    resize(720, 520);

    auto* root = new QVBoxLayout(this);
    tabs_ = new QTabWidget(this);
    tabs_->setObjectName(QStringLiteral("formTabs"));

    // ---- 页签一：填写 ----
    auto* fillPage = new QWidget(tabs_);
    auto* fillLayout = new QVBoxLayout(fillPage);
    fillTable_ = new QTableWidget(0, 4, fillPage);
    fillTable_->setObjectName(QStringLiteral("formFillTable"));
    fillTable_->setHorizontalHeaderLabels(
        {QStringLiteral("字段"), QStringLiteral("类型"), QStringLiteral("当前值"), QStringLiteral("要填成的值")});
    fillTable_->horizontalHeader()->setStretchLastSection(true);
    fillLayout->addWidget(fillTable_, 1);
    auto* fillRow = new QHBoxLayout();
    auto* reload = new QPushButton(QStringLiteral("读取表单字段"), fillPage);
    auto* fillBtn = new QPushButton(QStringLiteral("填写并另存为…"), fillPage);
    fillBtn->setObjectName(QStringLiteral("formFillApply"));
    fillRow->addStretch(1);
    fillRow->addWidget(reload);
    fillRow->addWidget(fillBtn);
    fillLayout->addLayout(fillRow);
    fillHint_ = new QLabel(QStringLiteral("勾选框填 1/0（也认 on/off），下拉填候选项原文。"), fillPage);
    fillHint_->setStyleSheet(QStringLiteral("color:#666;"));
    fillLayout->addWidget(fillHint_);
    tabs_->addTab(fillPage, QStringLiteral("填写表单"));

    // ---- 页签二：制作 ----
    auto* createPage = new QWidget(tabs_);
    auto* createLayout = new QVBoxLayout(createPage);
    auto* createForm = new QFormLayout();
    fieldName_ = new QLineEdit(createPage);
    fieldName_->setPlaceholderText(QStringLiteral("如 customer"));
    createForm->addRow(QStringLiteral("字段名"), fieldName_);
    fieldKind_ = new QComboBox(createPage);
    fieldKind_->addItem(QStringLiteral("文本"), QStringLiteral("text"));
    fieldKind_->addItem(QStringLiteral("勾选框"), QStringLiteral("checkbox"));
    fieldKind_->addItem(QStringLiteral("下拉/列表"), QStringLiteral("choice"));
    createForm->addRow(QStringLiteral("类型"), fieldKind_);
    fieldOptions_ = new QLineEdit(createPage);
    fieldOptions_->setPlaceholderText(QStringLiteral("下拉候选项，用 | 分隔：上海|北京|广州"));
    createForm->addRow(QStringLiteral("候选项"), fieldOptions_);

    auto* pageRow = new QHBoxLayout();
    pageRow->addWidget(new QLabel(QStringLiteral("页码"), createPage));
    fieldPage_ = new QSpinBox(createPage);
    fieldPage_->setRange(1, 9999);
    pageRow->addWidget(fieldPage_);
    auto* pick = new QPushButton(QStringLiteral("在预览里框选位置"), createPage);
    pageRow->addWidget(pick);
    pageRow->addStretch(1);
    createForm->addRow(QStringLiteral("位置"), pageRow);
    auto* rectRow = new QHBoxLayout();
    buildRectRow(rectRow, createPage, &fieldX0_, &fieldY0_, &fieldX1_, &fieldY1_, "form");
    createForm->addRow(QStringLiteral("矩形(pt)"), rectRow);
    createLayout->addLayout(createForm);

    auto* specRow = new QHBoxLayout();
    auto* addSpec = new QPushButton(QStringLiteral("添加这个字段"), createPage);
    auto* delSpec = new QPushButton(QStringLiteral("移除选中"), createPage);
    specRow->addStretch(1);
    specRow->addWidget(addSpec);
    specRow->addWidget(delSpec);
    createLayout->addLayout(specRow);

    specList_ = new QListWidget(createPage);
    specList_->setObjectName(QStringLiteral("formSpecList"));
    createLayout->addWidget(specList_, 1);
    auto* createRow = new QHBoxLayout();
    auto* createBtn = new QPushButton(QStringLiteral("生成表单并另存为…"), createPage);
    createBtn->setObjectName(QStringLiteral("formCreateApply"));
    createRow->addStretch(1);
    createRow->addWidget(createBtn);
    createLayout->addLayout(createRow);
    tabs_->addTab(createPage, QStringLiteral("制作表单"));

    // ---- 页签三：提取 ----
    auto* extractPage = new QWidget(tabs_);
    auto* extractLayout = new QVBoxLayout(extractPage);
    extractTable_ = new QTableWidget(0, 5, extractPage);
    extractTable_->setObjectName(QStringLiteral("formExtractTable"));
    extractTable_->setHorizontalHeaderLabels({QStringLiteral("字段"), QStringLiteral("类型"),
                                              QStringLiteral("值"), QStringLiteral("页"),
                                              QStringLiteral("位置")});
    extractTable_->horizontalHeader()->setStretchLastSection(true);
    extractLayout->addWidget(extractTable_, 1);
    auto* extractRow = new QHBoxLayout();
    auto* refresh = new QPushButton(QStringLiteral("刷新"), extractPage);
    auto* csv = new QPushButton(QStringLiteral("导出 CSV…"), extractPage);
    csv->setObjectName(QStringLiteral("formCsv"));
    extractRow->addStretch(1);
    extractRow->addWidget(refresh);
    extractRow->addWidget(csv);
    extractLayout->addLayout(extractRow);
    tabs_->addTab(extractPage, QStringLiteral("提取数据"));

    root->addWidget(tabs_, 1);
    auto* box = new QDialogButtonBox(QDialogButtonBox::Close, this);
    box->button(QDialogButtonBox::Close)->setText(QStringLiteral("关闭"));
    root->addWidget(box);

    connect(reload, &QPushButton::clicked, this, &FormDialog::reloadFields);
    connect(fillBtn, &QPushButton::clicked, this, &FormDialog::fillForm);
    connect(addSpec, &QPushButton::clicked, this, &FormDialog::addFieldSpec);
    connect(delSpec, &QPushButton::clicked, this, &FormDialog::removeFieldSpec);
    connect(createBtn, &QPushButton::clicked, this, &FormDialog::createForm);
    connect(refresh, &QPushButton::clicked, this, &FormDialog::reloadFields);
    connect(csv, &QPushButton::clicked, this, &FormDialog::exportCsv);
    connect(box, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(pick, &QPushButton::clicked, this, [this]() {
        enterPickMode(this, viewer_, &picking_, source_);
    });
    if (viewer_ != nullptr) {
        connect(viewer_, &PdfViewWidget::rectSelected, this, [this](int page, QRectF rect) {
            if (!picking_) return;
            fieldPage_->setValue(page);
            fieldX0_->setValue(rect.left());
            fieldY0_->setValue(rect.top());
            fieldX1_->setValue(rect.right());
            fieldY1_->setValue(rect.bottom());
            if (fillHint_ != nullptr) {
                fillHint_->setText(QStringLiteral("已取到第 %1 页的选区：%2,%3 → %4,%5")
                                       .arg(page)
                                       .arg(rect.left(), 0, 'f', 1)
                                       .arg(rect.top(), 0, 'f', 1)
                                       .arg(rect.right(), 0, 'f', 1)
                                       .arg(rect.bottom(), 0, 'f', 1));
            }
            finishPickMode(this, viewer_, &picking_);
        });
    }

    reloadFields();
}

void FormDialog::reloadFields() {
    QApplication::setOverrideCursor(Qt::WaitCursor);
    auto fields = backend_->formFields(pw::fsPath(source_.toUtf8().toStdString()));
    QApplication::restoreOverrideCursor();
    if (!fields.ok()) {
        fillHint_->setText(QStringLiteral("读取失败：%1").arg(QString::fromStdString(fields.status().toString())));
        return;
    }
    fields_ = fields.value();

    fillTable_->setRowCount(static_cast<int>(fields_.size()));
    extractTable_->setRowCount(static_cast<int>(fields_.size()));
    for (int i = 0; i < static_cast<int>(fields_.size()); ++i) {
        const pw::FormField& f = fields_[i];
        QString kind;
        switch (f.kind) {
            case pw::FieldKind::Text: kind = QStringLiteral("文本"); break;
            case pw::FieldKind::Checkbox: kind = QStringLiteral("勾选"); break;
            case pw::FieldKind::Choice: kind = QStringLiteral("下拉"); break;
            case pw::FieldKind::Signature: kind = QStringLiteral("签名"); break;
            default: kind = QStringLiteral("未知"); break;
        }
        fillTable_->setItem(i, 0, new QTableWidgetItem(QString::fromStdString(f.name)));
        fillTable_->setItem(i, 1, new QTableWidgetItem(kind));
        fillTable_->setItem(i, 2, new QTableWidgetItem(QString::fromStdString(f.value)));
        fillTable_->setItem(i, 3, new QTableWidgetItem(QString::fromStdString(f.value)));

        extractTable_->setItem(i, 0, new QTableWidgetItem(QString::fromStdString(f.name)));
        extractTable_->setItem(i, 1, new QTableWidgetItem(kind));
        extractTable_->setItem(i, 2, new QTableWidgetItem(QString::fromStdString(f.value)));
        extractTable_->setItem(i, 3, new QTableWidgetItem(QString::number(f.page + 1)));
        extractTable_->setItem(i, 4, new QTableWidgetItem(
            QStringLiteral("%1,%2 → %3,%4")
                .arg(f.rect.x0, 0, 'f', 1).arg(f.rect.y0, 0, 'f', 1)
                .arg(f.rect.x1, 0, 'f', 1).arg(f.rect.y1, 0, 'f', 1)));
    }
    fillHint_->setText(fields_.empty()
                           ? QStringLiteral("这份 PDF 没有表单字段。可以在「制作表单」里新建。")
                           : QStringLiteral("共 %1 个字段。").arg(fields_.size()));
    emit logMessage(QStringLiteral("读取到 %1 个表单字段").arg(fields_.size()));
}

void FormDialog::fillForm() {
    if (fields_.empty()) {
        QMessageBox::information(this, QStringLiteral("没有字段"),
                                 QStringLiteral("先点「读取表单字段」。"));
        return;
    }
    std::vector<pw::FieldValue> values;
    for (int i = 0; i < fillTable_->rowCount(); ++i) {
        auto* nameItem = fillTable_->item(i, 0);
        auto* valueItem = fillTable_->item(i, 3);
        if (nameItem == nullptr || valueItem == nullptr) continue;
        const QString target = valueItem->text();
        if (target.trimmed().isEmpty()) continue;
        pw::FieldValue fv;
        fv.name = nameItem->text().toStdString();
        fv.value = target.toStdString();
        values.push_back(fv);
    }
    if (values.empty()) {
        QMessageBox::information(this, QStringLiteral("没有要填的值"),
                                 QStringLiteral("在「要填成的值」列里写点什么。"));
        return;
    }
    const QString out = QFileDialog::getSaveFileName(this, QStringLiteral("填写结果"),
                                                     suggestPdfOutput(source_, QStringLiteral("_已填")),
                                                     QStringLiteral("PDF (*.pdf)"));
    if (out.isEmpty()) return;

    QApplication::setOverrideCursor(Qt::WaitCursor);
    const pw::Status st = backend_->fillForm(pw::fsPath(source_.toUtf8().toStdString()),
                                             pw::fsPath(out.toUtf8().toStdString()), values);
    QApplication::restoreOverrideCursor();
    if (!st.ok()) {
        QMessageBox::warning(this, QStringLiteral("填写失败"), QString::fromStdString(st.toString()));
        return;
    }
    emit logMessage(QStringLiteral("已填写 %1 个字段 -> %2").arg(values.size()).arg(out));
}

void FormDialog::addFieldSpec() {
    const QString name = fieldName_->text().trimmed();
    if (name.isEmpty()) {
        QMessageBox::information(this, QStringLiteral("需要字段名"), QStringLiteral("给字段起个名字。"));
        return;
    }
    pw::RectD rect =
        rectOf(fieldX0_->value(), fieldY0_->value(), fieldX1_->value(), fieldY1_->value());
    if (rect.empty()) {
        QMessageBox::information(this, QStringLiteral("选区太小"), QStringLiteral("先框选或手输一个矩形。"));
        return;
    }
    const QString kind = fieldKind_->currentData().toString();
    QString spec = QStringLiteral("%1:%2:%3:%4,%5,%6,%7")
                       .arg(name, kind)
                       .arg(fieldPage_->value())
                       .arg(rect.x0, 0, 'f', 1)
                       .arg(rect.y0, 0, 'f', 1)
                       .arg(rect.x1, 0, 'f', 1)
                       .arg(rect.y1, 0, 'f', 1);
    const QString options = fieldOptions_->text().trimmed();
    if (kind == QLatin1String("choice") && !options.isEmpty()) spec += QStringLiteral(":") + options;
    specList_->addItem(spec);
}

void FormDialog::removeFieldSpec() {
    const int row = specList_->currentRow();
    if (row >= 0) delete specList_->takeItem(row);
}

void FormDialog::createForm() {
    if (specList_->count() == 0) {
        QMessageBox::information(this, QStringLiteral("还没有字段"),
                                 QStringLiteral("先在上面添加至少一个字段。"));
        return;
    }
    const QString out = QFileDialog::getSaveFileName(this, QStringLiteral("表单输出"),
                                                     suggestPdfOutput(source_, QStringLiteral("_表单")),
                                                     QStringLiteral("PDF (*.pdf)"));
    if (out.isEmpty()) return;

    // 与 core 的 FieldSpec 对齐：按 "name:type:page:x0,y0,x1,y1[:opts]" 拆
    std::vector<pw::FieldSpec> specs;
    for (int i = 0; i < specList_->count(); ++i) {
        const QString text = specList_->item(i)->text();
        QStringList parts = text.split(QLatin1Char(':'));
        if (parts.size() < 4) continue;
        pw::FieldSpec spec;
        spec.name = parts[0].toStdString();
        const QString kind = parts[1];
        if (kind == QLatin1String("checkbox")) spec.kind = pw::FieldKind::Checkbox;
        else if (kind == QLatin1String("choice")) spec.kind = pw::FieldKind::Choice;
        else spec.kind = pw::FieldKind::Text;
        spec.page = parts[2].toInt() - 1;
        if (spec.page < 0) spec.page = 0;
        QStringList xy = parts[3].split(QLatin1Char(','));
        if (xy.size() == 4) {
            spec.rect.x0 = xy[0].toDouble();
            spec.rect.y0 = xy[1].toDouble();
            spec.rect.x1 = xy[2].toDouble();
            spec.rect.y1 = xy[3].toDouble();
        }
        if (parts.size() >= 5 && !parts[4].isEmpty()) {
            for (const QString& opt : parts[4].split(QLatin1Char('|')))
                if (!opt.isEmpty()) spec.options.push_back(opt.toStdString());
        }
        specs.push_back(spec);
    }
    if (specs.empty()) {
        QMessageBox::information(this, QStringLiteral("没有有效字段"), QStringLiteral("先添加字段。"));
        return;
    }

    QApplication::setOverrideCursor(Qt::WaitCursor);
    const pw::Status st = backend_->createForm(pw::fsPath(source_.toUtf8().toStdString()),
                                               pw::fsPath(out.toUtf8().toStdString()), specs);
    QApplication::restoreOverrideCursor();
    if (!st.ok()) {
        QMessageBox::warning(this, QStringLiteral("生成表单失败"),
                             QString::fromStdString(st.toString()));
        return;
    }
    emit logMessage(QStringLiteral("已生成 %1 个可交互字段 -> %2").arg(specs.size()).arg(out));
}

void FormDialog::exportCsv() {
    if (fields_.empty()) {
        QMessageBox::information(this, QStringLiteral("没有数据"),
                                 QStringLiteral("先点「读取表单字段」。"));
        return;
    }
    const QString out = QFileDialog::getSaveFileName(
        this, QStringLiteral("导出表单数据"),
        QFileInfo(source_).absolutePath() + QLatin1Char('/') +
            QFileInfo(source_).completeBaseName() + QStringLiteral("_表单.csv"),
        QStringLiteral("CSV (*.csv)"));
    if (out.isEmpty()) return;

    // Excel 打开中文 CSV 需要 UTF-8 BOM
    QFile f(out);
    if (!f.open(QIODevice::WriteOnly)) {
        QMessageBox::warning(this, QStringLiteral("写不了文件"), out);
        return;
    }
    QTextStream ts(&f);
    ts.setEncoding(QStringConverter::Utf8);
    ts << QChar(0xFEFF);
    ts << QStringLiteral("字段,类型,值,页码,x0,y0,x1,y1\n");
    for (const pw::FormField& fld : fields_) {
        QString kind;
        switch (fld.kind) {
            case pw::FieldKind::Text: kind = QStringLiteral("文本"); break;
            case pw::FieldKind::Checkbox: kind = QStringLiteral("勾选"); break;
            case pw::FieldKind::Choice: kind = QStringLiteral("下拉"); break;
            case pw::FieldKind::Signature: kind = QStringLiteral("签名"); break;
            default: kind = QStringLiteral("未知"); break;
        }
        ts << QString::fromStdString(fld.name) << ',' << kind << ','
           << QString::fromStdString(fld.value).replace(QLatin1Char(','), QLatin1Char(';')) << ','
           << (fld.page + 1) << ',' << fld.rect.x0 << ',' << fld.rect.y0 << ',' << fld.rect.x1 << ','
           << fld.rect.y1 << '\n';
    }
    f.close();
    emit logMessage(QStringLiteral("已导出表单数据 -> %1").arg(out));
}
