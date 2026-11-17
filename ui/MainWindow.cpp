#include "MainWindow.h"

#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QCloseEvent>
#include <QComboBox>
#include <QDockWidget>
#include <QCursor>
#include <QDialogButtonBox>
#include <QDragEnterEvent>
#include <QDragMoveEvent>
#include <QDir>
#include <QDropEvent>
#include <QFileDialog>
#include <QFormLayout>
#include <QFileInfo>
#include <QGuiApplication>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QMimeData>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QScreen>
#include <QSettings>
#include <QSpinBox>
#include <QSplitter>
#include <QStatusBar>
#include <QStringList>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QToolBar>
#include <QUrl>
#include <QVBoxLayout>
#include <filesystem>
#include <vector>

#include "CompressDialog.h"
#include "ConvertDialog.h"
#include "DropListWidget.h"
#include "ExtractDialog.h"
#include "ImageToPdfDialog.h"
#include "EditDialogs.h"
#include "OcrDialog.h"
#include "MergeDialog.h"
#include "MetadataDialog.h"
#include "PageEditorDialog.h"
#include "PdfViewWidget.h"
#include "SecurityDialog.h"
#include "WatermarkDialog.h"
#include "pdfword/page_selection.hpp"

namespace {

QString fileExt(const QString& path) {
    const int dot = path.lastIndexOf('.');
    if (dot < 0) return QString();
    return path.mid(dot + 1).toLower();
}

// 视图状态（面板显隐、停靠侧、日志高度、窗口几何）存在 INI 里，
// 位置：%APPDATA%\Pdf-Toolkit\Pdf-Toolkit.ini；删掉这个文件就等于恢复默认布局。
QSettings viewSettings() {
    return QSettings(QSettings::IniFormat, QSettings::UserScope, QStringLiteral("Pdf-Toolkit"),
                     QStringLiteral("Pdf-Toolkit"));
}

// 非阻塞提示框：写书签这类主动操作要给出明确结果，但不能像模态框那样把主窗口的
// 鼠标事件拦掉（非模态窗口的教训见「框选不可用」那一版）
void showMessageBox(QWidget* parent, const QString& title, const QString& text) {
    auto* box = new QMessageBox(QMessageBox::Information, title, text, QMessageBox::Ok, parent);
    box->setAttribute(Qt::WA_DeleteOnClose);
    box->setAttribute(Qt::WA_ShowWithoutActivating);
    box->setModal(false);
    box->show();
}

// 书签标题 / 页码一次改完的小窗；pageCount 决定页码上限，0 表示"没有跳转目标"
bool editOutlineItem(QWidget* parent, QString* title, int* page, int pageCount) {
    QDialog dlg(parent);
    dlg.setWindowTitle(QStringLiteral("修改书签"));
    auto* layout = new QVBoxLayout(&dlg);

    auto* form = new QFormLayout();
    auto* titleEdit = new QLineEdit(*title, &dlg);
    titleEdit->setObjectName(QStringLiteral("bookmarkTitleEdit"));
    titleEdit->setMinimumWidth(260);
    auto* pageSpin = new QSpinBox(&dlg);
    pageSpin->setObjectName(QStringLiteral("bookmarkPageSpin"));
    pageSpin->setRange(0, qMax(1, pageCount));
    pageSpin->setValue(qBound(0, *page, qMax(1, pageCount)));
    // 0 号位显示成"没有跳转目标"，而不是孤零零的 0
    pageSpin->setSpecialValueText(QStringLiteral("没有跳转目标"));
    form->addRow(QStringLiteral("标题"), titleEdit);
    form->addRow(QStringLiteral("跳到第几页"), pageSpin);
    layout->addLayout(form);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
    buttons->button(QDialogButtonBox::Ok)->setText(QStringLiteral("确定"));
    buttons->button(QDialogButtonBox::Cancel)->setText(QStringLiteral("取消"));
    layout->addWidget(buttons);
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);

    if (dlg.exec() != QDialog::Accepted) return false;
    *title = titleEdit->text().trimmed();
    *page = pageSpin->value();
    return !title->isEmpty();
}

// 预览里"就地编辑"的动作编号：右键菜单项 -> 框选 -> 直接写文件
enum InlineAction {
    InlineNone = 0,
    InlineHighlight = 1,
    InlineUnderline = 2,
    InlineStrikeOut = 3,
    InlineSquiggly = 4,
    InlineInk = 5,
    InlineFreeText = 6,
    InlineNote = 7,
    InlineLink = 8,
    InlineReplaceText = 9,
    InlineDeleteText = 10,
    InlineReplaceImage = 11,
    InlineBookmark = 12,
    InlineAddText = 13,
    InlineCancel = -1,
};

QString inlineActionName(int action) {
    switch (action) {
        case InlineHighlight: return QStringLiteral("高亮");
        case InlineUnderline: return QStringLiteral("下划线");
        case InlineStrikeOut: return QStringLiteral("删除线");
        case InlineSquiggly: return QStringLiteral("波浪线");
        case InlineInk: return QStringLiteral("手绘标记");
        case InlineFreeText: return QStringLiteral("文本框");
        case InlineNote: return QStringLiteral("便签");
        case InlineLink: return QStringLiteral("超链接");
        case InlineReplaceText: return QStringLiteral("改这里的文字");
        case InlineDeleteText: return QStringLiteral("删掉这里的文字");
        case InlineReplaceImage: return QStringLiteral("换这里的图片");
        case InlineAddText: return QStringLiteral("在框里加文字");
        default: return QStringLiteral("编辑");
    }
}

}  // namespace

MainWindow::MainWindow(std::shared_ptr<pw::Converter> converter, std::shared_ptr<pw::JobQueue> jobs,
                       QWidget* parent)
    : QMainWindow(parent), converter_(std::move(converter)), jobs_(std::move(jobs)) {
    setWindowTitle(QStringLiteral("Pdf-Toolkit — PDF/Word 转换与预览"));
    // 允许把文件从资源管理器拖到窗口任意位置
    setAcceptDrops(true);
    centerOnScreen();

    createCentral();
    createFileDock();
    createBookmarkDock();
    createMenus();

    connect(fileList_, &QListWidget::itemDoubleClicked, this, &MainWindow::previewSelected);

    // 预览里右键就出编辑菜单；框选完成直接落到 applyInlineEdit（只有"就地编辑"等着的框才算它的）
    connect(viewer_, &PdfViewWidget::contextMenuRequested, this, &MainWindow::showPreviewMenu);
    connect(viewer_, &PdfViewWidget::rectSelected, this, &MainWindow::applyInlineEdit);

    // core 的 JobQueue 在 worker 线程回调，这里用 QueuedConnection 切回 UI 线程
    jobs_->setListener([this](const pw::JobInfo& info) {
        QMetaObject::invokeMethod(
            this, [this, info] { applyJob(info); }, Qt::QueuedConnection);
    });

    // 状态栏常驻显示依赖状态，一眼能看出是"没装 LibreOffice"还是"没编 MuPDF"。
    // 用常驻 label 而不是 showMessage：showMessage 是临时消息，会被底部导航条/进度条那一行挤掉。
    std::string report = converter_->capabilityReport();
    std::string firstLine = report.substr(0, report.find('\n'));
    if (statusLabel_ != nullptr) statusLabel_->setText(QString::fromStdString(firstLine));
    appendLog(QString::fromStdString(report));

    // 上一次退出时的视图状态（面板显隐/停靠侧/日志高度/窗口几何）优先于默认值
    restoreViewState();
}

MainWindow::~MainWindow() {
    jobs_->setListener(nullptr);
}

void MainWindow::createCentral() {
    // ---- 预览 + 日志（导航按钮放工具栏）----
    // 用竖直 QSplitter：日志窗口可以由用户自己拖高度，也可以在「视图 → 日志窗口」里隐藏，
    // 隐藏后预览区自动铺满整个中央区域；显示时回到上次拖出来的高度。
    auto* central = new QWidget(this);
    auto* layout = new QVBoxLayout(central);
    layout->setContentsMargins(0, 0, 0, 0);

    splitter_ = new QSplitter(Qt::Vertical, central);
    splitter_->setObjectName(QStringLiteral("centralSplitter"));
    splitter_->setChildrenCollapsible(false);

    viewer_ = new PdfViewWidget(converter_->pdfBackend(), splitter_);
    splitter_->addWidget(viewer_);

    logView_ = new QPlainTextEdit(splitter_);
    logView_->setObjectName(QStringLiteral("logView"));
    logView_->setReadOnly(true);
    logView_->setMaximumBlockCount(2000);
    logView_->setMinimumHeight(60);
    splitter_->addWidget(logView_);

    splitter_->setStretchFactor(0, 1);
    splitter_->setStretchFactor(1, 0);
    splitter_->setSizes({640, 140});
    layout->addWidget(splitter_);

    setCentralWidget(central);

    // 底部最下面这一行统一定居：左边常驻后端状态文字，中间/右边是导航按钮，最右是 0% 进度条。
    // 这样"上一页/下一页/放大/缩小/页码/百分比"和进度条同排，预览区不再被单独占一行。
    statusBar()->setMinimumHeight(30);
    statusLabel_ = new QLabel(this);
    statusLabel_->setObjectName(QStringLiteral("backendState"));
    statusLabel_->setText(QStringLiteral("PDF 后端: 检测中…"));
    statusBar()->addWidget(statusLabel_, 1);
    if (QWidget* nav = viewer_->navBar()) {
        // 不给 stretch：只按自身宽度排在进度条左边，左边剩下的是状态文字
        statusBar()->addPermanentWidget(nav);
    }

    progress_ = new QProgressBar(this);
    progress_->setObjectName(QStringLiteral("progressBar"));
    progress_->setRange(0, 100);
    progress_->setValue(0);
    progress_->setMaximumWidth(240);
    statusBar()->addPermanentWidget(progress_);
}

void MainWindow::createFileDock() {
    // 待处理文件放独立面板：可停靠左/右、可拖出来当浮动窗口
    fileDock_ = new QDockWidget(QStringLiteral("待处理文件"), this);
    fileDock_->setObjectName(QStringLiteral("fileDock"));
    fileDock_->setAllowedAreas(Qt::LeftDockWidgetArea | Qt::RightDockWidgetArea);
    fileDock_->setFeatures(QDockWidget::DockWidgetMovable | QDockWidget::DockWidgetFloatable);

    auto* panel = new QWidget(fileDock_);
    panel->setMinimumWidth(250);
    auto* layout = new QVBoxLayout(panel);
    layout->setContentsMargins(6, 6, 6, 6);

    auto* hint = new QLabel(QStringLiteral("双击 PDF 可预览；可直接把文件拖到这里"), panel);
    hint->setStyleSheet(QStringLiteral("color:#666;"));
    layout->addWidget(hint);

    // 列表自己也收拖放：用户把文件拖到面板上时就加进列表
    auto* dropList = new DropListWidget(panel);
    dropList->setExtensions(supportedExtensions());
    fileList_ = dropList;
    fileList_->setSelectionMode(QAbstractItemView::ExtendedSelection);
    // 右键菜单：删除 / 清空 / 重命名（重命名改的是磁盘上的真实文件名）
    fileList_->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(fileList_, &QWidget::customContextMenuRequested, this, &MainWindow::showFileListMenu);
    layout->addWidget(fileList_, 1);
    connect(dropList, &DropListWidget::filesDropped, this, [this](const QStringList& paths) {
        if (addPathsToFileList(paths) > 0 && paths.size() == 1) previewPath(paths.first());
    });

    auto* row = new QHBoxLayout();
    auto* addBtn = new QPushButton(QStringLiteral("添加文件"), panel);
    auto* removeBtn = new QPushButton(QStringLiteral("移除选中"), panel);
    row->addWidget(addBtn);
    row->addWidget(removeBtn);
    layout->addLayout(row);

    fileDock_->setWidget(panel);
    addDockWidget(Qt::LeftDockWidgetArea, fileDock_);

    connect(addBtn, &QPushButton::clicked, this, &MainWindow::addFiles);
    connect(removeBtn, &QPushButton::clicked, this, &MainWindow::removeSelected);
}

void MainWindow::createBookmarkDock() {
    // 书签和"目录"在 PDF 里是同一棵 /Outlines 树，所以只做一个面板
    bookmarkDock_ = new QDockWidget(QStringLiteral("书签 / 目录"), this);
    bookmarkDock_->setObjectName(QStringLiteral("bookmarkDock"));
    bookmarkDock_->setAllowedAreas(Qt::LeftDockWidgetArea | Qt::RightDockWidgetArea);
    bookmarkDock_->setFeatures(QDockWidget::DockWidgetMovable | QDockWidget::DockWidgetFloatable);

    auto* panel = new QWidget(bookmarkDock_);
    panel->setMinimumWidth(250);
    auto* layout = new QVBoxLayout(panel);
    layout->setContentsMargins(6, 6, 6, 6);

    auto* hint = new QLabel(QStringLiteral("双击条目跳到对应页；改完点「应用」另存为 _书签.pdf"), panel);
    hint->setObjectName(QStringLiteral("bookmarkHint"));
    hint->setStyleSheet(QStringLiteral("color:#666;"));
    hint->setWordWrap(true);
    layout->addWidget(hint);

    bookmarkTree_ = new QTreeWidget(panel);
    bookmarkTree_->setObjectName(QStringLiteral("bookmarkTree"));
    bookmarkTree_->setColumnCount(2);
    bookmarkTree_->setHeaderLabels({QStringLiteral("标题"), QStringLiteral("页")});
    // 标题列吃掉剩余宽度、页码列按内容收窄：长标题不会被截断，页码列也不占地方。
    // 必须先关掉 header 的 stretchLastSection，否则最后一列会把剩余宽度全吃掉。
    bookmarkTree_->header()->setStretchLastSection(false);
    bookmarkTree_->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    bookmarkTree_->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    bookmarkTree_->setAlternatingRowColors(true);
    bookmarkTree_->setSelectionMode(QAbstractItemView::SingleSelection);
    bookmarkTree_->setUniformRowHeights(true);
    layout->addWidget(bookmarkTree_, 1);

    // 两行按钮：先增删改，再排序 / 落盘
    auto* row1 = new QHBoxLayout();
    auto* addSiblingBtn = new QPushButton(QStringLiteral("新增同级"), panel);
    addSiblingBtn->setObjectName(QStringLiteral("bookmarkAddSibling"));
    addSiblingBtn->setToolTip(QStringLiteral("在当前书签的同一层插入一条新书签"));
    auto* addChildBtn = new QPushButton(QStringLiteral("新增子级"), panel);
    addChildBtn->setObjectName(QStringLiteral("bookmarkAddChild"));
    addChildBtn->setToolTip(QStringLiteral("在当前书签下面插入一条下一层书签"));
    auto* editBtn = new QPushButton(QStringLiteral("修改"), panel);
    editBtn->setObjectName(QStringLiteral("bookmarkEdit"));
    editBtn->setToolTip(QStringLiteral("改标题和跳转页码"));
    row1->addWidget(addSiblingBtn);
    row1->addWidget(addChildBtn);
    row1->addWidget(editBtn);
    layout->addLayout(row1);

    auto* row2 = new QHBoxLayout();
    auto* removeBtn = new QPushButton(QStringLiteral("删除"), panel);
    removeBtn->setObjectName(QStringLiteral("bookmarkRemove"));
    auto* upBtn = new QPushButton(QStringLiteral("上移"), panel);
    upBtn->setObjectName(QStringLiteral("bookmarkUp"));
    auto* downBtn = new QPushButton(QStringLiteral("下移"), panel);
    downBtn->setObjectName(QStringLiteral("bookmarkDown"));
    row2->addWidget(removeBtn);
    row2->addWidget(upBtn);
    row2->addWidget(downBtn);
    layout->addLayout(row2);

    auto* row3 = new QHBoxLayout();
    auto* applyBtn = new QPushButton(QStringLiteral("应用"), panel);
    applyBtn->setObjectName(QStringLiteral("bookmarkApply"));
    applyBtn->setToolTip(QStringLiteral("把当前这棵树写进文档，另存为 <原名>_书签.pdf"));
    auto* exportBtn = new QPushButton(QStringLiteral("导出…"), panel);
    exportBtn->setObjectName(QStringLiteral("bookmarkExport"));
    exportBtn->setToolTip(QStringLiteral("把书签导出成文本或 JSON"));
    row3->addWidget(applyBtn);
    row3->addWidget(exportBtn);
    layout->addLayout(row3);

    bookmarkDock_->setWidget(panel);
    // 默认靠右：左侧留给"待处理文件"，同侧时两者会各占一条，不会互相挡
    addDockWidget(Qt::RightDockWidgetArea, bookmarkDock_);

    connect(addSiblingBtn, &QPushButton::clicked, this, [this] { bookmarkAdd(false); });
    connect(addChildBtn, &QPushButton::clicked, this, [this] { bookmarkAdd(true); });
    connect(editBtn, &QPushButton::clicked, this, &MainWindow::bookmarkEdit);
    connect(removeBtn, &QPushButton::clicked, this, &MainWindow::bookmarkRemove);
    connect(upBtn, &QPushButton::clicked, this, [this] { bookmarkMove(-1); });
    connect(downBtn, &QPushButton::clicked, this, [this] { bookmarkMove(1); });
    connect(applyBtn, &QPushButton::clicked, this, &MainWindow::bookmarkApply);
    connect(exportBtn, &QPushButton::clicked, this, &MainWindow::bookmarkExport);
    connect(bookmarkTree_, &QTreeWidget::itemDoubleClicked, this, &MainWindow::bookmarkActivated);
    // 面板被拖走、拖成浮动、点标题栏关掉：菜单勾选跟着真实状态走
    connect(bookmarkDock_, &QDockWidget::visibilityChanged, this, [this](bool visible) {
        if (bookmarkShowAct_ != nullptr) bookmarkShowAct_->setChecked(visible);
        if (bookmarkHideAct_ != nullptr) bookmarkHideAct_->setChecked(!visible);
    });
}

void MainWindow::setBookmarksVisible(bool visible) {
    if (bookmarkDock_ == nullptr) return;
    bookmarkDock_->setVisible(visible);
    if (visible) bookmarkDock_->raise();
    if (bookmarkShowAct_ != nullptr) bookmarkShowAct_->setChecked(visible);
    if (bookmarkHideAct_ != nullptr) bookmarkHideAct_->setChecked(!visible);
}

void MainWindow::rebuildBookmarkTree(const std::vector<pw::OutlineItem>& items) {
    if (bookmarkTree_ == nullptr) return;
    bookmarkTree_->clear();

    // chain[k] = 当前第 k 层的"最近一次出现的节点"，新项按 level 挂到 chain[level-1] 下面
    std::vector<QTreeWidgetItem*> chain;
    for (const pw::OutlineItem& src : items) {
        auto* item = new QTreeWidgetItem();
        const QString title = QString::fromStdString(src.title);
        item->setText(0, title);
        item->setText(1, src.page > 0 ? QString::number(src.page) : QString());
        // 面板窄的时候标题会被省略号截掉，悬停能看到全文
        item->setToolTip(0, title);
        item->setToolTip(1, src.page > 0 ? QStringLiteral("跳到第 %1 页").arg(src.page)
                                         : QStringLiteral("没有跳转目标"));
        item->setData(0, Qt::UserRole, src.page);

        int level = src.level < 0 ? 0 : src.level;
        // 最多只往下走一层：数据里跳级（比如 0 → 2）就夹到当前层数
        if (level > static_cast<int>(chain.size())) level = static_cast<int>(chain.size());
        if (level == 0 || chain.empty()) {
            bookmarkTree_->addTopLevelItem(item);
            chain.assign(1, item);
        } else {
            QTreeWidgetItem* parent = chain[static_cast<std::size_t>(level - 1)];
            parent->addChild(item);
            chain.resize(static_cast<std::size_t>(level));
            chain.push_back(item);
        }
    }
    bookmarkTree_->expandAll();
}

void MainWindow::collectBookmarks(QTreeWidgetItem* node, int level,
                                  std::vector<pw::OutlineItem>& out) const {
    for (int i = 0; i < node->childCount(); ++i) {
        QTreeWidgetItem* child = node->child(i);
        pw::OutlineItem item;
        item.title = child->text(0).toStdString();
        item.page = child->data(0, Qt::UserRole).toInt();
        item.level = level;
        out.push_back(item);
        collectBookmarks(child, level + 1, out);
    }
}

void MainWindow::refreshBookmarkTree(const QString& path) {
    if (bookmarkTree_ == nullptr) return;
    bookmarkTree_->clear();
    bookmarkPath_ = path;
    if (path.isEmpty() || fileExt(path) != QStringLiteral("pdf")) return;

    auto backend = converter_ != nullptr ? converter_->pdfBackend() : nullptr;
    if (backend == nullptr) return;
    const pw::Result<std::vector<pw::OutlineItem>> res =
        backend->outline(std::filesystem::path(path.toStdString()), std::string());
    if (!res.ok()) {
        appendLog(QStringLiteral("读取书签失败: ") + QString::fromStdString(res.status().message()));
        return;
    }
    rebuildBookmarkTree(res.value());
    appendLog(QStringLiteral("已读取书签: ") + QString::number(res.value().size()) + QStringLiteral(" 条"));
}

void MainWindow::bookmarkAdd(bool asChild) {
    if (bookmarkTree_ == nullptr) return;
    if (bookmarkPath_.isEmpty()) {
        appendLog(QStringLiteral("先在左边预览一个 PDF，再来编辑它的书签"));
        return;
    }

    const int pageCount = viewer_ != nullptr ? viewer_->pageCount() : 0;
    QTreeWidgetItem* current = bookmarkTree_->currentItem();
    int page = current != nullptr ? current->data(0, Qt::UserRole).toInt() : 0;
    if (page <= 0 && viewer_ != nullptr) page = viewer_->currentPage();

    QString title = asChild ? QStringLiteral("新的子书签") : QStringLiteral("新的书签");
    if (!editOutlineItem(this, &title, &page, pageCount)) return;

    auto* item = new QTreeWidgetItem();
    item->setText(0, title);
    item->setText(1, page > 0 ? QString::number(page) : QString());
    item->setData(0, Qt::UserRole, page);

    if (asChild) {
        // 选中的项就是父项；没选中就退化成顶级
        if (current != nullptr) {
            current->addChild(item);
            current->setExpanded(true);
        } else {
            bookmarkTree_->addTopLevelItem(item);
        }
    } else if (current != nullptr && current->parent() != nullptr) {
        QTreeWidgetItem* parent = current->parent();
        parent->insertChild(parent->indexOfChild(current) + 1, item);
        parent->setExpanded(true);
    } else if (current != nullptr) {
        bookmarkTree_->insertTopLevelItem(bookmarkTree_->indexOfTopLevelItem(current) + 1, item);
    } else {
        bookmarkTree_->addTopLevelItem(item);
    }
    bookmarkTree_->setCurrentItem(item);
    appendLog(QStringLiteral("已新增书签: ") + title);
}

void MainWindow::bookmarkEdit() {
    if (bookmarkTree_ == nullptr) return;
    QTreeWidgetItem* item = bookmarkTree_->currentItem();
    if (item == nullptr) {
        appendLog(QStringLiteral("先选中一条书签再改"));
        return;
    }
    const int pageCount = viewer_ != nullptr ? viewer_->pageCount() : 0;
    QString title = item->text(0);
    int page = item->data(0, Qt::UserRole).toInt();
    if (!editOutlineItem(this, &title, &page, pageCount)) return;
    item->setText(0, title);
    item->setText(1, page > 0 ? QString::number(page) : QString());
    item->setData(0, Qt::UserRole, page);
    appendLog(QStringLiteral("已修改书签: ") + title + QStringLiteral(" → 第 ") + QString::number(page) +
              QStringLiteral(" 页"));
}

void MainWindow::bookmarkRemove() {
    if (bookmarkTree_ == nullptr) return;
    QTreeWidgetItem* item = bookmarkTree_->currentItem();
    if (item == nullptr) {
        appendLog(QStringLiteral("先选中一条书签再删"));
        return;
    }
    const QString title = item->text(0);
    QTreeWidgetItem* parent = item->parent();
    if (parent != nullptr) {
        delete parent->takeChild(parent->indexOfChild(item));
    } else {
        delete bookmarkTree_->takeTopLevelItem(bookmarkTree_->indexOfTopLevelItem(item));
    }
    appendLog(QStringLiteral("已删除书签: ") + title);
}

void MainWindow::bookmarkMove(int delta) {
    if (bookmarkTree_ == nullptr || delta == 0) return;
    QTreeWidgetItem* item = bookmarkTree_->currentItem();
    if (item == nullptr) return;
    QTreeWidgetItem* parent = item->parent();
    const int index = parent != nullptr ? parent->indexOfChild(item)
                                        : bookmarkTree_->indexOfTopLevelItem(item);
    const int target = index + (delta > 0 ? 1 : -1);
    const int count = parent != nullptr ? parent->childCount() : bookmarkTree_->topLevelItemCount();
    if (index < 0 || target < 0 || target >= count) return; // 已经在头/尾

    if (parent != nullptr) {
        QTreeWidgetItem* moved = parent->takeChild(index);
        parent->insertChild(target, moved);
    } else {
        QTreeWidgetItem* moved = bookmarkTree_->takeTopLevelItem(index);
        bookmarkTree_->insertTopLevelItem(target, moved);
    }
    bookmarkTree_->setCurrentItem(item);
}

void MainWindow::bookmarkApply() {
    if (bookmarkTree_ == nullptr || bookmarkPath_.isEmpty()) {
        appendLog(QStringLiteral("先预览一个 PDF，再来写书签"));
        return;
    }

    std::vector<pw::OutlineItem> items;
    collectBookmarks(bookmarkTree_->invisibleRootItem(), 0, items);

    auto backend = converter_ != nullptr ? converter_->pdfBackend() : nullptr;
    if (backend == nullptr) return;

    const std::filesystem::path input(bookmarkPath_.toStdString());
    const std::filesystem::path output =
        input.parent_path() / (input.stem().string() + "_书签.pdf");
    const pw::Status st = backend->setOutline(input, output, items, std::string());
    if (!st.ok()) {
        appendLog(QStringLiteral("写入书签失败: ") + QString::fromStdString(st.message()));
        showMessageBox(this, QStringLiteral("写入书签失败"), QString::fromStdString(st.message()));
        return;
    }
    const QString outPath = QString::fromStdString(output.string());
    appendLog(QStringLiteral("已写入 ") + QString::number(items.size()) + QStringLiteral(" 条书签 -> ") +
              outPath);
    showMessageBox(this, QStringLiteral("书签已写入"),
                   QStringLiteral("已写入 %1 条书签：\n%2").arg(items.size()).arg(outPath));
}

void MainWindow::bookmarkExport() {
    if (bookmarkTree_ == nullptr) return;
    if (bookmarkTree_->topLevelItemCount() == 0) {
        appendLog(QStringLiteral("当前文档没有书签可导出"));
        return;
    }

    std::vector<pw::OutlineItem> items;
    collectBookmarks(bookmarkTree_->invisibleRootItem(), 0, items);

    QString suggested;
    if (!bookmarkPath_.isEmpty()) {
        const std::filesystem::path input(bookmarkPath_.toStdString());
        suggested = QString::fromStdString((input.parent_path() / (input.stem().string() + "_书签.txt")).string());
    }
    const QString path = QFileDialog::getSaveFileName(
        this, QStringLiteral("导出书签"), suggested,
        QStringLiteral("文本 (*.txt);;JSON (*.json)"));
    if (path.isEmpty()) return;

    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        appendLog(QStringLiteral("写不了: ") + path);
        return;
    }
    QTextStream out(&file);
    out.setEncoding(QStringConverter::Utf8);

    if (fileExt(path) == QStringLiteral("json")) {
        // 与 C ABI 的 pw_pdf_outline 同一套字段，web 壳可以直接吃
        out << "[\n";
        for (std::size_t i = 0; i < items.size(); ++i) {
            out << "  {\"title\": \"" << QString::fromStdString(items[i].title).replace('"', "\\\"")
                << "\", \"page\": " << items[i].page << ", \"level\": " << items[i].level << "}";
            if (i + 1 < items.size()) out << ",";
            out << "\n";
        }
        out << "]\n";
    } else {
        // 与 CLI 的 outline --txt 一致：层级 <TAB> 页码 <TAB> 标题
        for (const pw::OutlineItem& item : items) {
            out << item.level << "\t" << item.page << "\t" << QString::fromStdString(item.title) << "\n";
        }
    }
    file.close();
    appendLog(QStringLiteral("已导出书签 -> ") + path);
}

void MainWindow::bookmarkActivated(QTreeWidgetItem* item, int column) {
    Q_UNUSED(column);
    if (item == nullptr || viewer_ == nullptr) return;
    const int page = item->data(0, Qt::UserRole).toInt();
    if (page <= 0) return; // 没有跳转目标的条目
    viewer_->showPage(page);
}

// ---- 预览里的直接编辑 -------------------------------------------------------
//
// 之前每个编辑功能都是"开窗口 → 填页码和四个坐标 → 保存"，用户反馈"只能预览、改不了"。
// 这里补一条就地路径：在页面上右键选动作，然后在页面上拖一个框，松手就写文件，
// 写完把预览切到结果文件并留在同一页——改完立刻看得见，不用来回填坐标。

void MainWindow::showPreviewMenu(int page, const QPoint& globalPos) {
    if (viewer_ == nullptr || viewer_->currentPath().isEmpty()) {
        appendLog(QStringLiteral("预览里还没有打开的 PDF，先双击一个 PDF 再改"));
        return;
    }

    QMenu menu(this);
    menu.setObjectName(QStringLiteral("previewMenu"));
    auto add = [this, &menu](const QString& text, int action) {
        QAction* act = menu.addAction(text);
        act->setData(action);
        return act;
    };
    add(QStringLiteral("高亮（拖框）"), InlineHighlight);
    add(QStringLiteral("下划线（拖框）"), InlineUnderline);
    add(QStringLiteral("删除线（拖框）"), InlineStrikeOut);
    add(QStringLiteral("波浪线（拖框）"), InlineSquiggly);
    add(QStringLiteral("手绘标记（拖框）"), InlineInk);
    menu.addSeparator();
    add(QStringLiteral("文本框注释…（拖框后写字）"), InlineFreeText);
    add(QStringLiteral("便签…（拖框后写字）"), InlineNote);
    add(QStringLiteral("超链接…（拖框后填网址）"), InlineLink);
    menu.addSeparator();
    add(QStringLiteral("改这里的文字…（拖框自动带出原文）"), InlineReplaceText);
    add(QStringLiteral("删掉这里的文字（拖框）"), InlineDeleteText);
    add(QStringLiteral("在这里加文字…（拖框后打字）"), InlineAddText);
    add(QStringLiteral("换这里的图片…（拖框后选图）"), InlineReplaceImage);
    menu.addSeparator();
    add(QStringLiteral("在第 %1 页加书签").arg(page), InlineBookmark);
    add(QStringLiteral("取消框选"), InlineCancel);

    QAction* chosen = menu.exec(globalPos);
    if (chosen == nullptr) return;
    const int action = chosen->data().toInt();
    if (action == InlineNone) return;
    if (action == InlineCancel) {
        inlineAction_ = 0;
        viewer_->setSelectMode(false);
        appendLog(QStringLiteral("已取消框选"));
        return;
    }
    if (action == InlineBookmark) {
        addBookmarkHere(page);
        return;
    }
    startInlineEdit(action);
}

void MainWindow::startInlineEdit(int action) {
    if (viewer_ == nullptr) return;
    inlineAction_ = action;
    viewer_->setSelectMode(true);
    appendLog(QStringLiteral("「%1」：在第 %2 页（或其它页）上拖一个框，松手即写入；右键可取消")
                  .arg(inlineActionName(action))
                  .arg(viewer_->currentPage()));
}

void MainWindow::applyInlineEdit(int page, const QRectF& rect) {
    const int action = inlineAction_;
    if (action == 0 || viewer_ == nullptr) return;  // 不是就地编辑在等框选（可能是某个编辑窗口的框选）
    inlineAction_ = 0;
    viewer_->setSelectMode(false);

    const QString source = viewer_->currentPath();
    if (source.isEmpty()) return;
    // 重载之后要回到同一处：先把当前滚动位置记下来（closeDocument 会把滚动条归零）
    const int savedScroll = viewer_->verticalScrollValue();

    pw::RectD area;
    area.x0 = rect.left();
    area.y0 = rect.top();
    area.x1 = rect.right();
    area.y1 = rect.bottom();
    if (area.empty()) {
        appendLog(QStringLiteral("框太小了，什么也没改"));
        return;
    }

    const QString output = inlineOutputPath(source);
    // 结果就写回当前文件时（已经叫 xxx_已编辑.pdf），先落临时文件再换名：
    // MuPDF 一边读输入一边截断同名输出会把源文件写坏。
    const bool inPlace = QFileInfo(output).absoluteFilePath() == QFileInfo(source).absoluteFilePath();
    const QFileInfo outInfo(output);
    QString target = output;
    if (inPlace) {
        target = QDir(outInfo.absolutePath())
                     .filePath(outInfo.completeBaseName() +
                               QStringLiteral("_编辑中_%1.pdf")
                                   .arg(QDateTime::currentMSecsSinceEpoch()));
    }

    auto backend = converter_->pdfBackend();
    if (backend == nullptr) return;

    QApplication::setOverrideCursor(Qt::WaitCursor);
    pw::Status st;
    bool ran = true;
    switch (action) {
        case InlineHighlight:
        case InlineUnderline:
        case InlineStrikeOut:
        case InlineSquiggly: {
            pw::AnnotSpec spec;
            spec.kind = action == InlineHighlight    ? pw::AnnotKind::Highlight
                        : action == InlineUnderline  ? pw::AnnotKind::Underline
                        : action == InlineStrikeOut  ? pw::AnnotKind::StrikeOut
                                                     : pw::AnnotKind::Squiggly;
            spec.page = page - 1;
            spec.rect = area;
            spec.opacity = action == InlineHighlight ? 0.55 : 1.0;
            std::vector<pw::AnnotSpec> specs{spec};
            st = backend->annotate(std::filesystem::path(source.toStdString()),
                                   std::filesystem::path(target.toStdString()), specs);
            break;
        }
        case InlineInk: {
            pw::AnnotSpec spec;
            spec.kind = pw::AnnotKind::Ink;
            spec.page = page - 1;
            spec.rect = area;
            std::vector<pw::PointD> pts;
            const int steps = 6;
            for (int i = 0; i <= steps; ++i) {
                pw::PointD pt;
                pt.x = spec.rect.x0 + spec.rect.width() * i / steps;
                pt.y = spec.rect.y0 + spec.rect.height() * (i % 2 == 0 ? 0.75 : 0.25);
                pts.push_back(pt);
            }
            spec.inkPaths.push_back(pts);
            std::vector<pw::AnnotSpec> specs{spec};
            st = backend->annotate(std::filesystem::path(source.toStdString()),
                                   std::filesystem::path(target.toStdString()), specs);
            break;
        }
        case InlineFreeText:
        case InlineNote: {
            const QString body = QInputDialog::getMultiLineText(
                this, action == InlineFreeText ? QStringLiteral("文本框内容") : QStringLiteral("便签内容"),
                QStringLiteral("写在这块区域里的文字："), QString());
            if (body.isEmpty()) {
                ran = false;  // 取消
                break;
            }
            pw::AnnotSpec spec;
            spec.kind = action == InlineFreeText ? pw::AnnotKind::FreeText : pw::AnnotKind::Note;
            spec.page = page - 1;
            spec.rect = area;
            spec.text = body.toStdString();
            std::vector<pw::AnnotSpec> specs{spec};
            st = backend->annotate(std::filesystem::path(source.toStdString()),
                                   std::filesystem::path(target.toStdString()), specs);
            break;
        }
        case InlineLink: {
            const QString uri = QInputDialog::getText(
                this, QStringLiteral("超链接目标"),
                QStringLiteral("网址（https://…）或者要跳到的页码（第3页）："), QLineEdit::Normal,
                QStringLiteral("https://"));
            if (uri.trimmed().isEmpty()) {
                ran = false;
                break;
            }
            pw::LinkSpec spec;
            spec.page = page - 1;
            spec.rect = area;
            spec.uri = uri.trimmed().toStdString();
            st = backend->addLink(std::filesystem::path(source.toStdString()),
                                  std::filesystem::path(target.toStdString()), spec);
            break;
        }
        case InlineReplaceText:
        case InlineDeleteText: {
            // 先把框里的原文读出来（core 的 textInRect），省得用户照着页面手打
            QString oldText;
            auto got = backend->textInRect(std::filesystem::path(source.toStdString()), page - 1, area);
            if (got.ok()) oldText = QString::fromStdString(got.value());
            oldText.replace('\n', ' ').replace('\r', ' ').replace(QStringLiteral("  "), QStringLiteral(" "));
            oldText = oldText.trimmed();
            if (oldText.isEmpty()) {
                ran = false;
                QApplication::restoreOverrideCursor();
                showMessageBox(this, QStringLiteral("框里没有文字"),
                               QStringLiteral("这块区域取不到文字（可能是图片或扫描页）。"
                                              "扫描页要先做 OCR；按整词替换可以用「查找替换…」。"));
                return;
            }
            QString newText = oldText;
            if (action == InlineReplaceText) {
                if (inlineTextForTestSet_) {
                    newText = inlineTextForTest_;
                } else {
                    newText = QInputDialog::getText(
                        this, QStringLiteral("改这里的文字"),
                        QStringLiteral("框里的原文已经带出来了（只改这一块，别处的同名文字不动）："),
                        QLineEdit::Normal, oldText);
                    if (newText.isNull() || newText.isEmpty()) {
                        ran = false;
                        break;
                    }
                }
            }
            pw::ReplaceOptions opts;
            opts.deleteOnly = action == InlineDeleteText;
            // 按矩形写回，不整篇搜 oldText：同一句在文档里出现多次时，全篇替换会把别处一起改掉；
            // textInRect 取出来的文本带换行时也可能根本搜不到（原来的"改了没反应"就是这个）。
            st = backend->replaceTextAt(std::filesystem::path(source.toStdString()),
                                        std::filesystem::path(target.toStdString()), page - 1, area,
                                        newText.toStdString(), opts);
            break;
        }
        case InlineAddText: {
            QString body = inlineTextForTestSet_ ? inlineTextForTest_
                                                 : QInputDialog::getText(
                                                       this, QStringLiteral("在这里加文字"),
                                                       QStringLiteral("框就是文字落的位置（框高定字号，字太长会自动缩，不压扁）："),
                                                       QLineEdit::Normal, QString());
            if (body.isEmpty()) {
                ran = false;
                break;
            }
            pw::ReplaceOptions opts;
            st = backend->addTextAt(std::filesystem::path(source.toStdString()),
                                    std::filesystem::path(target.toStdString()), page - 1, area,
                                    body.toStdString(), opts);
            break;
        }
        case InlineReplaceImage: {
            const QString img = QFileDialog::getOpenFileName(
                this, QStringLiteral("选一张替换图片"), QString(),
                QStringLiteral("图片 (*.png *.jpg *.jpeg *.bmp *.tif *.tiff)"));
            if (img.isEmpty()) {
                ran = false;
                break;
            }
            st = backend->replaceImage(std::filesystem::path(source.toStdString()),
                                       std::filesystem::path(target.toStdString()), page - 1, area,
                                       std::filesystem::path(img.toStdString()));
            break;
        }
        default:
            ran = false;
            break;
    }
    QApplication::restoreOverrideCursor();

    if (!ran) return;
    if (!st.ok()) {
        if (inPlace) QFile::remove(target);
        showMessageBox(this, QStringLiteral("没改成"), QString::fromStdString(st.toString()));
        appendLog(QStringLiteral("就地编辑失败: ") + QString::fromStdString(st.toString()));
        return;
    }

    if (inPlace) {
        // 原地更新：预览此刻还打开着这个文件（MuPDF 的 document 握着文件句柄），
        // 不先放掉它，Windows 上的 remove / rename 会因为文件被占用而失败。
        if (viewer_ != nullptr) viewer_->closeDocument();
        const bool removed = QFile::remove(output);
        if (!QFile::rename(target, output)) {
            appendLog(QStringLiteral("换名失败（旧件删除%1），结果留在: %2")
                          .arg(removed ? QStringLiteral("成功") : QStringLiteral("没成功"))
                          .arg(target));
            return;
        }
    }

    appendLog(QStringLiteral("已在预览里改好「%1」（第 %2 页）-> %3")
                  .arg(inlineActionName(action))
                  .arg(page)
                  .arg(QFileInfo(output).fileName()));
    reloadAfterInline(output, page, savedScroll);
}

QString MainWindow::runPreviewTextEdit(int page, const QRectF& rect, const QString& text,
                                       bool deleteOnly, double fontSize, bool backdrop,
                                       const QString& what) {
    if (viewer_ == nullptr) return QString();
    const QString source = viewer_->currentPath();
    if (source.isEmpty()) {
        appendLog(QStringLiteral("预览里还没有打开的 PDF，先双击一个 PDF 再改"));
        return QString();
    }
    if (rect.width() < 1.0 || rect.height() < 1.0) {
        appendLog(QStringLiteral("框太小了，什么也没改"));
        return QString();
    }
    auto backend = converter_ ? converter_->pdfBackend() : nullptr;
    if (backend == nullptr) return QString();
    // 重载之后要回到同一处：滚动位置先记下（不是只按页码跳回页顶）
    const int savedScroll = viewer_->verticalScrollValue();

    pw::RectD area;
    area.x0 = rect.left();
    area.y0 = rect.top();
    area.x1 = rect.right();
    area.y1 = rect.bottom();

    const QString output = inlineOutputPath(source);
    const bool inPlace = QFileInfo(output).absoluteFilePath() == QFileInfo(source).absoluteFilePath();
    const QFileInfo outInfo(output);
    QString target = output;
    if (inPlace) {
        target = QDir(outInfo.absolutePath())
                     .filePath(outInfo.completeBaseName() +
                               QStringLiteral("_编辑中_%1.pdf").arg(QDateTime::currentMSecsSinceEpoch()));
    }

    pw::ReplaceOptions opts;
    opts.deleteOnly = deleteOnly;
    opts.fontSize = fontSize;

    QApplication::setOverrideCursor(Qt::WaitCursor);
    pw::Status st = backdrop
                        ? backend->replaceTextAt(std::filesystem::path(source.toStdString()),
                                                 std::filesystem::path(target.toStdString()),
                                                 page - 1, area, text.toStdString(), opts)
                        : backend->addTextAt(std::filesystem::path(source.toStdString()),
                                             std::filesystem::path(target.toStdString()), page - 1,
                                             area, text.toStdString(), opts);
    QApplication::restoreOverrideCursor();

    if (!st.ok()) {
        if (inPlace) QFile::remove(target);
        showMessageBox(this, QStringLiteral("没改成"), QString::fromStdString(st.toString()));
        appendLog(QStringLiteral("就地编辑失败: ") + QString::fromStdString(st.toString()));
        return QString();
    }
    if (inPlace) {
        // 先把预览放掉再换名：MuPDF 的 document 握着文件句柄，Windows 上删不掉也改不了名
        if (viewer_ != nullptr) viewer_->closeDocument();
        const bool removed = QFile::remove(output);
        if (!QFile::rename(target, output)) {
            appendLog(QStringLiteral("换名失败（旧件删除%1），结果留在: %2")
                          .arg(removed ? QStringLiteral("成功") : QStringLiteral("没成功"))
                          .arg(target));
            return QString();
        }
    }

    appendLog(QStringLiteral("已在预览里%1（第 %2 页）-> %3").arg(what).arg(page).arg(QFileInfo(output).fileName()));
    reloadAfterInline(output, page, savedScroll);
    return output;
}

QString MainWindow::previewReplaceText(int page, const QRectF& rect, const QString& text,
                                       bool deleteOnly) {
    return runPreviewTextEdit(page, rect, text, deleteOnly, 0.0, true,
                              deleteOnly ? QStringLiteral("删掉这块文字") : QStringLiteral("改好这块文字"));
}

QString MainWindow::previewAddText(int page, const QRectF& rect, const QString& text, double fontSize) {
    return runPreviewTextEdit(page, rect, text, false, fontSize, false, QStringLiteral("加了文字"));
}

void MainWindow::previewInlineEditForTest(int action, int page, const QRectF& rect,
                                          const QString& text) {
    inlineAction_ = action;
    inlineTextForTest_ = text;
    inlineTextForTestSet_ = true;
    applyInlineEdit(page, rect);
    inlineTextForTestSet_ = false;
    inlineTextForTest_ = QString();
    inlineAction_ = 0;
}

QString MainWindow::inlineOutputPath(const QString& source) {
    const QFileInfo info(source);
    const QString stem = info.completeBaseName();
    // 已经叫 xxx_已编辑.pdf：继续改同一个（走临时文件换名，不会写坏）
    if (stem.endsWith(QStringLiteral("_已编辑"))) return info.absoluteFilePath();
    return QDir(info.absolutePath()).filePath(stem + QStringLiteral("_已编辑.pdf"));
}

void MainWindow::reloadAfterInline(const QString& output, int page, int scrollValue) {
    if (viewer_ == nullptr) return;
    QString error;
    if (!viewer_->openDocument(output, &error)) {
        appendLog(QStringLiteral("刷新预览失败: ") + error);
        return;
    }
    // 重载会把滚动条归零：先按页跳回去，再按改之前记下的滚动值还原到同一处（不是只回页顶）
    if (page > 1) viewer_->showPage(page);
    if (scrollValue >= 0) viewer_->setVerticalScrollValue(scrollValue);
    // 书签面板跟着换到新文件，免得对着旧文档的目录继续改
    refreshBookmarkTree(output);
    // 结果文件补进待处理列表，后面合并/拆分可以直接选它
    addPathsToFileList(QStringList{output});
}

void MainWindow::addBookmarkHere(int page) {
    if (bookmarkTree_ == nullptr || viewer_ == nullptr) return;
    if (bookmarkPath_.isEmpty()) {
        appendLog(QStringLiteral("先预览一个 PDF，再往它的目录里加条目"));
        return;
    }

    auto* item = new QTreeWidgetItem();
    const QString title = QStringLiteral("第 %1 页").arg(page);
    item->setText(0, title);
    item->setText(1, QString::number(page));
    item->setToolTip(0, title);
    item->setToolTip(1, QStringLiteral("跳到第 %1 页").arg(page));
    item->setData(0, Qt::UserRole, page);
    bookmarkTree_->addTopLevelItem(item);
    bookmarkTree_->setCurrentItem(item);
    bookmarkTree_->scrollToItem(item);
    appendLog(QStringLiteral("已在书签面板加了一条「%1」；要改名点「修改」，改完点「应用」写回 PDF")
                  .arg(title));
}

void MainWindow::createMenus() {
    // ---- 文件 ----
    auto* fileMenu = menuBar()->addMenu(QStringLiteral("文件(&F)"));
    fileMenu->addAction(QStringLiteral("添加文件…"), QKeySequence::Open, this, &MainWindow::addFiles);
    fileMenu->addAction(QStringLiteral("移除选中"), this, &MainWindow::removeSelected);
    fileMenu->addAction(QStringLiteral("清空列表"), this, &MainWindow::clearFiles);
    fileMenu->addSeparator();
    fileMenu->addAction(QStringLiteral("退出"), QKeySequence::Quit, this, &QWidget::close);

    // ---- 转换 ----
    auto* convertMenu = menuBar()->addMenu(QStringLiteral("转换(&C)"));
    convertMenu->addAction(QStringLiteral("开始转换…"), QKeySequence(QStringLiteral("Ctrl+Return")), this,
                           &MainWindow::startConversion);

    // ---- PDF 工具 ----
    // 页面级操作在前，文档级操作在后（中间隔一条线），五项各开一个独立窗口
    auto* pdfMenu = menuBar()->addMenu(QStringLiteral("PDF 工具(&P)"));
    pdfMenu->addAction(QStringLiteral("合并 PDF…"), this, &MainWindow::mergePdfs);
    pdfMenu->addAction(QStringLiteral("拆分 PDF…"), this, &MainWindow::splitPdf);
    pdfMenu->addAction(QStringLiteral("抽取页面…"), this, &MainWindow::extractPages);
    pdfMenu->addAction(QStringLiteral("页面编辑…"), this, &MainWindow::editPages);
    pdfMenu->addSeparator();
    pdfMenu->addAction(QStringLiteral("元数据…"), this, &MainWindow::openMetadata);
    pdfMenu->addAction(QStringLiteral("加密 / 解密…"), this, &MainWindow::openSecurity);
    pdfMenu->addAction(QStringLiteral("压缩…"), this, &MainWindow::openCompress);
    pdfMenu->addAction(QStringLiteral("水印…"), this, &MainWindow::openWatermark);
    pdfMenu->addAction(QStringLiteral("图片转 PDF…"), this, &MainWindow::openImageToPdf);
    pdfMenu->addAction(QStringLiteral("OCR 文字识别…"), this, &MainWindow::openOcr);

    // ---- 编辑与批注 ----
    // 五项各开一个窗口；改文字/图片拆成"文字"和"图片"两条路径，由同一个入口选择
    auto* editMenu = menuBar()->addMenu(QStringLiteral("编辑与批注(&E)"));
    editMenu->setObjectName(QStringLiteral("menuEditAnnot"));
    editMenu->addAction(QStringLiteral("修改文字、图片…"), this, &MainWindow::openTextImage);
    editMenu->addAction(QStringLiteral("注释批注…"), this, &MainWindow::openAnnot);
    editMenu->addAction(QStringLiteral("添加超链接…"), this, &MainWindow::openLink);
    editMenu->addAction(QStringLiteral("查找替换…"), this, &MainWindow::openFindReplace);
    editMenu->addAction(QStringLiteral("表单处理…"), this, &MainWindow::openForm);
    editMenu->addSeparator();
    // 不进窗口、直接在页面上框一下就改：和预览右键同一个菜单
    editMenu->addAction(QStringLiteral("在预览上直接编辑（右键）…"), this, [this] {
        showPreviewMenu(viewer_ != nullptr ? viewer_->currentPage() : 1, QCursor::pos());
    });

    // ---- 视图：多级菜单，文件列表（显示/隐藏 + 靠左/靠右）+ 日志窗口（显示/隐藏）+ 缩放 ----
    auto* viewMenu = menuBar()->addMenu(QStringLiteral("视图(&V)"));

    auto* fileListMenu = viewMenu->addMenu(QStringLiteral("文件列表"));
    fileListMenu->setObjectName(QStringLiteral("menuFileList"));

    fileVisibleGroup_ = new QActionGroup(this);
    fileVisibleGroup_->setExclusive(true);

    // 显隐两项互斥，跟日志窗口同一套写法；dock 自带的关闭按钮也会把勾选同步过来
    showFilesAct_ = fileListMenu->addAction(QStringLiteral("显示文件列表"));
    showFilesAct_->setObjectName(QStringLiteral("fileShowAction"));
    showFilesAct_->setCheckable(true);
    showFilesAct_->setChecked(true);
    showFilesAct_->setActionGroup(fileVisibleGroup_);
    connect(showFilesAct_, &QAction::triggered, this, [this] { showFileList(true); });

    hideFilesAct_ = fileListMenu->addAction(QStringLiteral("隐藏文件列表"));
    hideFilesAct_->setObjectName(QStringLiteral("fileHideAction"));
    hideFilesAct_->setCheckable(true);
    hideFilesAct_->setActionGroup(fileVisibleGroup_);
    connect(hideFilesAct_, &QAction::triggered, this, [this] { showFileList(false); });

    fileListMenu->addSeparator();

    dockSideGroup_ = new QActionGroup(this);
    dockSideGroup_->setExclusive(true);

    auto* leftAct = fileListMenu->addAction(QStringLiteral("靠左"));
    leftAct->setObjectName(QStringLiteral("dockLeftAction"));
    leftAct->setCheckable(true);
    leftAct->setChecked(true);
    leftAct->setActionGroup(dockSideGroup_);
    dockLeftAct_ = leftAct;
    connect(leftAct, &QAction::triggered, this, [this] { dockFileList(false); });

    auto* rightAct = fileListMenu->addAction(QStringLiteral("靠右"));
    rightAct->setObjectName(QStringLiteral("dockRightAction"));
    rightAct->setCheckable(true);
    rightAct->setActionGroup(dockSideGroup_);
    dockRightAct_ = rightAct;
    connect(rightAct, &QAction::triggered, this, [this] { dockFileList(true); });

    auto* logMenu = viewMenu->addMenu(QStringLiteral("日志窗口"));
    logMenu->setObjectName(QStringLiteral("menuLogWindow"));
    logGroup_ = new QActionGroup(this);
    logGroup_->setExclusive(true);

    logShowAct_ = logMenu->addAction(QStringLiteral("显示"));
    logShowAct_->setObjectName(QStringLiteral("logShowAction"));
    logShowAct_->setCheckable(true);
    logShowAct_->setChecked(true);
    logShowAct_->setActionGroup(logGroup_);
    connect(logShowAct_, &QAction::triggered, this, [this] { setLogVisible(true); });

    logHideAct_ = logMenu->addAction(QStringLiteral("隐藏"));
    logHideAct_->setObjectName(QStringLiteral("logHideAction"));
    logHideAct_->setCheckable(true);
    logHideAct_->setActionGroup(logGroup_);
    connect(logHideAct_, &QAction::triggered, this, [this] { setLogVisible(false); });

    // 书签 / 目录面板：跟日志窗口同一套两级写法（显示 / 隐藏 互斥）
    auto* bookmarkMenu = viewMenu->addMenu(QStringLiteral("书签目录"));
    bookmarkMenu->setObjectName(QStringLiteral("menuBookmark"));
    bookmarkGroup_ = new QActionGroup(this);
    bookmarkGroup_->setExclusive(true);

    bookmarkShowAct_ = bookmarkMenu->addAction(QStringLiteral("显示书签目录"));
    bookmarkShowAct_->setObjectName(QStringLiteral("bookmarkShowAction"));
    bookmarkShowAct_->setCheckable(true);
    bookmarkShowAct_->setChecked(true);
    bookmarkShowAct_->setActionGroup(bookmarkGroup_);
    connect(bookmarkShowAct_, &QAction::triggered, this, [this] { setBookmarksVisible(true); });

    bookmarkHideAct_ = bookmarkMenu->addAction(QStringLiteral("隐藏书签目录"));
    bookmarkHideAct_->setObjectName(QStringLiteral("bookmarkHideAction"));
    bookmarkHideAct_->setCheckable(true);
    bookmarkHideAct_->setActionGroup(bookmarkGroup_);
    connect(bookmarkHideAct_, &QAction::triggered, this, [this] { setBookmarksVisible(false); });

    // 缩放：导航按钮已经挪到预览区右下角，这里留键盘/菜单入口，并保留"适应宽度 / 适应窗口"
    auto* zoomMenu = viewMenu->addMenu(QStringLiteral("缩放"));
    zoomMenu->setObjectName(QStringLiteral("menuZoom"));
    auto* zoomInAct = zoomMenu->addAction(QStringLiteral("放大一档"));
    zoomInAct->setObjectName(QStringLiteral("zoomInAction"));
    zoomInAct->setShortcuts({QKeySequence::ZoomIn, QKeySequence(QStringLiteral("Ctrl+="))});
    connect(zoomInAct, &QAction::triggered, this, [this] { viewer_->zoomIn(); });
    auto* zoomOutAct = zoomMenu->addAction(QStringLiteral("缩小一档"));
    zoomOutAct->setObjectName(QStringLiteral("zoomOutAction"));
    zoomOutAct->setShortcut(QKeySequence::ZoomOut);
    connect(zoomOutAct, &QAction::triggered, this, [this] { viewer_->zoomOut(); });
    zoomMenu->addSeparator();
    auto* fitWidthAct = zoomMenu->addAction(QStringLiteral("适应宽度"));
    fitWidthAct->setObjectName(QStringLiteral("fitWidthAction"));
    fitWidthAct->setShortcut(QKeySequence(QStringLiteral("Ctrl+1")));
    connect(fitWidthAct, &QAction::triggered, this, [this] { viewer_->fitWidth(); });
    auto* fitPageAct = zoomMenu->addAction(QStringLiteral("适应窗口"));
    fitPageAct->setObjectName(QStringLiteral("fitPageAction"));
    fitPageAct->setShortcut(QKeySequence(QStringLiteral("Ctrl+0")));
    connect(fitPageAct, &QAction::triggered, this, [this] { viewer_->fitPage(); });
    auto* actualAct = zoomMenu->addAction(QStringLiteral("实际大小（100%）"));
    actualAct->setObjectName(QStringLiteral("actualSizeAction"));
    actualAct->setShortcut(QKeySequence(QStringLiteral("Ctrl+Alt+0")));
    connect(actualAct, &QAction::triggered, this, [this] { viewer_->setZoom(1.0); });

    // 用户自己拖动面板（或拖成浮动窗口、点标题栏关闭）后，把菜单勾选状态跟真实状态对齐
    connect(fileDock_, &QDockWidget::dockLocationChanged, this, [this](Qt::DockWidgetArea area) {
        const bool right = (area == Qt::RightDockWidgetArea);
        if (dockRightAct_ != nullptr) dockRightAct_->setChecked(right);
        if (dockLeftAct_ != nullptr) dockLeftAct_->setChecked(!right);
    });
    connect(fileDock_, &QDockWidget::visibilityChanged, this, [this](bool visible) {
        if (showFilesAct_ != nullptr) showFilesAct_->setChecked(visible);
        if (hideFilesAct_ != nullptr) hideFilesAct_->setChecked(!visible);
    });

    // ---- 帮助 ----
    auto* helpMenu = menuBar()->addMenu(QStringLiteral("帮助(&H)"));
    helpMenu->addAction(QStringLiteral("能力报告"), this, &MainWindow::showCapabilityReport);
}

void MainWindow::dockFileList(bool rightSide) {
    if (fileDock_ == nullptr) return;
    addDockWidget(rightSide ? Qt::RightDockWidgetArea : Qt::LeftDockWidgetArea, fileDock_);
    fileDock_->show();
    fileDock_->raise();
}

void MainWindow::showFileList(bool visible) {
    if (fileDock_ == nullptr) return;
    fileDock_->setVisible(visible);
    if (visible) fileDock_->raise();
    if (showFilesAct_ != nullptr) showFilesAct_->setChecked(visible);
    if (hideFilesAct_ != nullptr) hideFilesAct_->setChecked(!visible);
}

void MainWindow::setLogVisible(bool visible) {
    if (logView_ == nullptr) return;
    // 隐藏前把当前高度记下来，下次显示（以及下次启动）沿用它
    if (!visible && logView_->isVisible() && logView_->height() > 40) {
        lastLogHeight_ = logView_->height();
    }
    logView_->setVisible(visible);
    // 勾选状态跟着走：hide 之后预览区由 splitter 自动铺满
    if (logShowAct_ != nullptr) logShowAct_->setChecked(visible);
    if (logHideAct_ != nullptr) logHideAct_->setChecked(!visible);
}

void MainWindow::saveViewState() {
    QSettings settings = viewSettings();
    settings.setValue(QStringLiteral("window/geometry"), saveGeometry());
    // QMainWindow::saveState() 一并记住 dock 的显隐、停靠侧、宽度和是否浮动
    settings.setValue(QStringLiteral("window/dockState"), saveState());
    settings.setValue(QStringLiteral("view/logVisible"), logView_ != nullptr && !logView_->isHidden());
    settings.setValue(QStringLiteral("view/logHeight"), lastLogHeight_);
    // QMainWindow::saveState() 不保存"被隐藏的 dock"，所以显隐单独存一份
    settings.setValue(QStringLiteral("view/fileListVisible"),
                      fileDock_ != nullptr && !fileDock_->isHidden());
    settings.setValue(QStringLiteral("view/bookmarkVisible"),
                      bookmarkDock_ != nullptr && !bookmarkDock_->isHidden());
    settings.setValue(QStringLiteral("view/navCollapsed"),
                      viewer_ != nullptr && viewer_->isNavCollapsed());
    settings.sync();
}

void MainWindow::restoreViewState() {
    QSettings settings = viewSettings();

    // 几何：有记录就用记录的（没记录时用构造里 centerOnScreen() 的居中位置）
    const QByteArray geometry = settings.value(QStringLiteral("window/geometry")).toByteArray();
    if (!geometry.isEmpty()) restoreGeometry(geometry);
    const QByteArray dockState = settings.value(QStringLiteral("window/dockState")).toByteArray();
    if (!dockState.isEmpty()) restoreState(dockState);

    lastLogHeight_ = qMax(60, settings.value(QStringLiteral("view/logHeight"), 140).toInt());
    if (splitter_ != nullptr) splitter_->setSizes({640, lastLogHeight_});
    setLogVisible(settings.value(QStringLiteral("view/logVisible"), true).toBool());
    // 底部导航条上次是收起状态的话，这次也别自己弹开
    if (viewer_ != nullptr) {
        viewer_->setNavCollapsed(settings.value(QStringLiteral("view/navCollapsed"), false).toBool());
    }

    // restoreState 之后 dock 的真实状态可能和菜单的初始勾选不同：统一按真实状态同步一遍
    if (fileDock_ != nullptr) {
        // 注意 restoreState 会把隐藏的 dock 重新显示出来，所以显隐要用自己存的键再压一次
        showFileList(settings.value(QStringLiteral("view/fileListVisible"), true).toBool());
        const bool right = dockWidgetArea(fileDock_) == Qt::RightDockWidgetArea;
        if (dockRightAct_ != nullptr) dockRightAct_->setChecked(right);
        if (dockLeftAct_ != nullptr) dockLeftAct_->setChecked(!right);
    }
    // 书签面板：restoreState 会把隐藏的 dock 重新显示出来，所以这里用自己的键再压一次
    setBookmarksVisible(settings.value(QStringLiteral("view/bookmarkVisible"), true).toBool());
}

void MainWindow::closeEvent(QCloseEvent* event) {
    // 只记视图状态（面板显隐/停靠侧/日志高度/窗口几何），任务数据不落盘
    saveViewState();
    QMainWindow::closeEvent(event);
}

void MainWindow::centerOnScreen() {
    QScreen* screen = QGuiApplication::primaryScreen();
    if (screen == nullptr) return;
    const QRect avail = screen->availableGeometry();

    // 先按可用区域收缩尺寸：1366×768 / 1536×864 这些本机分辨率下，
    // 1280×860 会高过任务栏上方的可用区，硬居中会把底边切掉。
    const QSize preferred(1280, 860);
    const int w = qMin(preferred.width(), qMax(960, avail.width() - 64));
    const int h = qMin(preferred.height(), qMax(600, avail.height() - 64));
    resize(w, h);

    const int x = avail.x() + (avail.width() - w) / 2;
    const int y = avail.y() + (avail.height() - h) / 2;
    move(qMax(avail.x(), x), qMax(avail.y(), y));
}

void MainWindow::addFiles() {
    const QStringList files = QFileDialog::getOpenFileNames(
        this, QStringLiteral("选择文件"), QString(),
        QStringLiteral("文档 (*.pdf *.docx *.doc *.odt *.rtf *.txt *.xlsx *.pptx);;所有文件 (*.*)"));
    addPathsToFileList(files);
}

QStringList MainWindow::supportedExtensions() {
    // 与"添加文件…"的过滤器保持一致；拖拽时按这个表挑文件，别的一律忽略
    return {QStringLiteral("pdf"), QStringLiteral("docx"), QStringLiteral("doc"),
            QStringLiteral("odt"), QStringLiteral("rtf"), QStringLiteral("txt"),
            QStringLiteral("xlsx"), QStringLiteral("pptx")};
}

int MainWindow::addPathsToFileList(const QStringList& paths) {
    if (paths.isEmpty() || fileList_ == nullptr) return 0;

    const QStringList exts = supportedExtensions();
    int added = 0;
    int rejected = 0;
    for (const QString& raw : paths) {
        if (raw.trimmed().isEmpty()) continue;
        const QString absolute = QFileInfo(raw).absoluteFilePath();
        const QString ext = fileExt(absolute);
        if (!exts.contains(ext)) {
            ++rejected;
            continue;
        }
        // 同一个文件重复拖进来没有意义，跳过
        bool exists = false;
        for (int i = 0; i < fileList_->count(); ++i) {
            if (fileList_->item(i)->data(Qt::UserRole).toString() == absolute) {
                exists = true;
                break;
            }
        }
        if (exists) continue;

        auto* item = new QListWidgetItem(QFileInfo(absolute).fileName(), fileList_);
        item->setData(Qt::UserRole, absolute);
        item->setToolTip(absolute);
        ++added;
    }

    if (added > 0) {
        appendLog(QStringLiteral("已添加 %1 个文件").arg(added));
    }
    if (rejected > 0) {
        appendLog(QStringLiteral("忽略 %1 个不支持的文件（只收 pdf/docx/doc/odt/rtf/txt/xlsx/pptx）")
                      .arg(rejected));
    }
    return added;
}

void MainWindow::previewPath(const QString& path) {
    if (fileExt(path) != QStringLiteral("pdf")) return;
    QString error;
    if (!viewer_->openDocument(path, &error)) {
        appendLog(QStringLiteral("预览失败: ") + error);
        refreshBookmarkTree(QString());
        return;
    }
    // 切换文档时把书签面板换成新文档的目录树
    refreshBookmarkTree(path);
}

// ---- 窗口级拖放：拖到预览区、日志区、空白处都能入列 ----

void MainWindow::dragEnterEvent(QDragEnterEvent* e) {
    if (e->mimeData() != nullptr && e->mimeData()->hasUrls()) {
        e->acceptProposedAction();
        return;
    }
    QMainWindow::dragEnterEvent(e);
}

void MainWindow::dragMoveEvent(QDragMoveEvent* e) {
    if (e->mimeData() != nullptr && e->mimeData()->hasUrls()) {
        e->acceptProposedAction();
        return;
    }
    QMainWindow::dragMoveEvent(e);
}

void MainWindow::dropEvent(QDropEvent* e) {
    if (e->mimeData() == nullptr || !e->mimeData()->hasUrls()) {
        QMainWindow::dropEvent(e);
        return;
    }
    QStringList paths;
    for (const QUrl& url : e->mimeData()->urls()) {
        if (!url.isLocalFile()) continue;
        paths << QFileInfo(url.toLocalFile()).absoluteFilePath();
    }
    e->acceptProposedAction();
    e->setDropAction(Qt::CopyAction);

    if (paths.empty()) {
        appendLog(QStringLiteral("拖入的内容不是本地文件，已忽略"));
        return;
    }
    if (addPathsToFileList(paths) > 0 && paths.size() == 1) previewPath(paths.first());
}

void MainWindow::removeSelected() {
    for (auto* item : fileList_->selectedItems()) delete item;
}

void MainWindow::clearFiles() {
    fileList_->clear();
    // 列表清空后预览也会被关，书签面板跟着空掉，避免留着上一份文档的树被误写
    refreshBookmarkTree(QString());
}

void MainWindow::showFileListMenu(const QPoint& pos) {
    // 右键点在未选中的行上时，先把它选中，避免"右键 A 却删掉了之前选的 B"
    auto* item = fileList_->itemAt(pos);
    if (item != nullptr && !item->isSelected()) {
        fileList_->clearSelection();
        item->setSelected(true);
        fileList_->setCurrentItem(item);
    }

    QMenu menu(this);
    auto* previewAct = menu.addAction(QStringLiteral("预览"));
    previewAct->setEnabled(item != nullptr &&
                           fileExt(item->data(Qt::UserRole).toString()) == QStringLiteral("pdf"));
    menu.addSeparator();
    auto* removeAct = menu.addAction(QStringLiteral("删除"));
    removeAct->setToolTip(QStringLiteral("从列表移除选中的文件，磁盘上的文件不动"));
    auto* clearAct = menu.addAction(QStringLiteral("清空"));
    clearAct->setToolTip(QStringLiteral("清空整个待处理列表，磁盘上的文件不动"));
    auto* renameAct = menu.addAction(QStringLiteral("重命名文件…"));
    renameAct->setToolTip(QStringLiteral("改磁盘上的真实文件名（选中单个文件时可用）"));

    const bool hasItems = fileList_->count() > 0;
    removeAct->setEnabled(hasItems);
    clearAct->setEnabled(hasItems);
    renameAct->setEnabled(fileList_->selectedItems().size() == 1);

    QAction* chosen = menu.exec(fileList_->viewport()->mapToGlobal(pos));
    if (chosen == nullptr) return;
    if (chosen == previewAct) {
        previewSelected();
    } else if (chosen == removeAct) {
        const int n = fileList_->selectedItems().size();
        removeSelected();
        appendLog(QStringLiteral("已从列表删除 %1 个文件（磁盘上的文件未改动）").arg(n));
    } else if (chosen == clearAct) {
        const int n = fileList_->count();
        clearFiles();
        appendLog(QStringLiteral("已清空待处理列表（%1 个文件，磁盘上的文件未改动）").arg(n));
    } else if (chosen == renameAct) {
        renameSelectedFile();
    }
}

void MainWindow::renameSelectedFile() {
    auto* item = fileList_->currentItem();
    if (item == nullptr || fileList_->selectedItems().size() != 1) {
        QMessageBox::information(this, QStringLiteral("重命名"),
                                 QStringLiteral("先在列表里选中一个文件再重命名"));
        return;
    }

    const QString oldPath = item->data(Qt::UserRole).toString();
    const QFileInfo info(oldPath);
    bool ok = false;
    const QString newName = QInputDialog::getText(
                                this, QStringLiteral("重命名文件"),
                                QStringLiteral("新文件名（含扩展名）：\n当前：%1").arg(oldPath),
                                QLineEdit::Normal, info.fileName(), &ok)
                                .trimmed();
    if (!ok || newName.isEmpty() || newName == info.fileName()) return;

    // Windows 文件名非法字符先本地拦下，别等 rename 抛一个看不懂的系统错误
    static const QString kIllegal = QStringLiteral("\\/:*?\"<>|");
    for (const QChar c : newName) {
        if (kIllegal.contains(c)) {
            QMessageBox::warning(this, QStringLiteral("文件名不合法"),
                                 QStringLiteral("文件名里不能有 \\ / : * ? \" < > | 这些字符"));
            return;
        }
    }
    if (newName.endsWith(QLatin1Char(' ')) || newName.endsWith(QLatin1Char('.'))) {
        QMessageBox::warning(this, QStringLiteral("文件名不合法"),
                             QStringLiteral("文件名结尾不能是空格或句点"));
        return;
    }

    const QString newPath = info.absolutePath() + QLatin1Char('/') + newName;
    if (QFileInfo::exists(newPath)) {
        QMessageBox::warning(this, QStringLiteral("重命名失败"),
                             QStringLiteral("目标文件已存在：\n%1").arg(newPath));
        return;
    }

    std::error_code ec;
    std::filesystem::rename(std::filesystem::path(oldPath.toStdString()),
                            std::filesystem::path(newPath.toStdString()), ec);
    if (ec) {
        QMessageBox::warning(this, QStringLiteral("重命名失败"),
                             QStringLiteral("系统返回：%1\n（文件可能正被其他程序占用）")
                                 .arg(QString::fromStdString(ec.message())));
        return;
    }

    item->setText(newName);
    item->setData(Qt::UserRole, newPath);
    item->setToolTip(newPath);
    // 正在预览的就是这个文件时，跟着重开预览，免得预览还指向已经不存在的旧路径
    if (viewer_ != nullptr && viewer_->currentPath() == oldPath) previewPath(newPath);
    appendLog(QStringLiteral("已重命名: %1 -> %2").arg(info.fileName(), newName));
}

QStringList MainWindow::allFiles() const {
    QStringList out;
    for (int i = 0; i < fileList_->count(); ++i) out << fileList_->item(i)->data(Qt::UserRole).toString();
    return out;
}

void MainWindow::previewSelected() {
    auto* item = fileList_->currentItem();
    if (item == nullptr) return;
    const QString path = item->data(Qt::UserRole).toString();
    if (fileExt(path) != QStringLiteral("pdf")) {
        appendLog(QStringLiteral("只有 PDF 可以预览，其他格式请先转成 PDF"));
        return;
    }
    // 统一走 previewPath：双击列表换文档时书签面板也要跟着换成新文档的目录
    previewPath(path);
}

void MainWindow::startConversion() {
    const QStringList files = selectedFiles();
    if (files.isEmpty()) {
        appendLog(QStringLiteral("没有可转换的文件：先在待处理文件里添加文件"));
        return;
    }

    // 选好文件后弹目标格式（不直接在主界面猜"用户想要什么格式"）
    ConvertDialog dlg(files, this);
    if (dlg.exec() != QDialog::Accepted) return;

    const QString target = dlg.target();
    const QString outDir = dlg.outputDir();
    const QString password = dlg.pdfPassword();

    // 方向不匹配的文件不入队：core 里 toPdf 会直接拒绝"源文件已经是 PDF"，
    // 界面上提前挡掉，免得日志里一片红。
    int skipped = 0;
    for (const auto& f : dlg.files()) {
        const QString ext = fileExt(f);
        const bool isPdf = (ext == QStringLiteral("pdf"));
        if (target == QStringLiteral("pdf") && isPdf) {
            appendLog(QStringLiteral("跳过（已经是 PDF）: ") + f);
            ++skipped;
            continue;
        }
        if (target == QStringLiteral("docx") && !isPdf) {
            appendLog(QStringLiteral("跳过（只有 PDF 能转 docx）: ") + f);
            ++skipped;
            continue;
        }

        std::filesystem::path out;
        if (!outDir.isEmpty()) {
            const std::filesystem::path src(f.toStdString());
            const std::string stem = src.stem().string();
            out = std::filesystem::path(outDir.toStdString()) / (stem + "." + target.toStdString());
        }
        int id = jobs_->enqueue(std::filesystem::path(f.toStdString()), out, target.toStdString(),
                                password.toStdString());
        appendLog(QStringLiteral("任务 #%1 已提交: %2 -> %3").arg(id).arg(f, target));
    }

    if (skipped > 0) {
        statusBar()->showMessage(QStringLiteral("已跳过 %1 个方向不匹配的文件").arg(skipped), 5000);
    }
}

void MainWindow::applyJob(const pw::JobInfo& info) {
    progress_->setValue(info.progress);
    appendLog(QStringLiteral("[#%1] %2%% %3 — %4")
                  .arg(info.id)
                  .arg(info.progress)
                  .arg(QString::fromStdString(info.message))
                  .arg(QString::fromStdString(info.source.filename().string())));
}

QStringList MainWindow::selectedFiles() const {
    QStringList out;
    for (auto* item : fileList_->selectedItems()) out << item->data(Qt::UserRole).toString();
    if (out.isEmpty()) out = allFiles();
    return out;
}

QStringList MainWindow::selectedPdfs() const {
    QStringList out;
    const QStringList candidates = selectedFiles();
    for (const auto& path : candidates) {
        if (fileExt(path) == QStringLiteral("pdf")) out << path;
    }
    return out;
}

void MainWindow::mergePdfs() {
    // 合并对话框自己负责选文件，所以这里不要求"先在列表里多选"
    MergeDialog dlg(selectedPdfs(), this);
    if (dlg.exec() != QDialog::Accepted) return;

    const QStringList files = dlg.files();
    std::vector<std::filesystem::path> inputs;
    inputs.reserve(static_cast<std::size_t>(files.size()));
    for (const auto& f : files) inputs.emplace_back(f.toStdString());

    QApplication::setOverrideCursor(Qt::WaitCursor);
    pw::Status st = converter_->pdfBackend()->merge(inputs, std::filesystem::path(dlg.outputPath().toStdString()));
    QApplication::restoreOverrideCursor();

    if (!st.ok()) {
        appendLog(QStringLiteral("合并失败: ") + QString::fromStdString(st.toString()));
        statusBar()->showMessage(QStringLiteral("合并失败"), 5000);
        return;
    }
    appendLog(QStringLiteral("已合并 %1 个 PDF -> %2").arg(files.size()).arg(dlg.outputPath()));
    statusBar()->showMessage(QStringLiteral("合并完成: ") + dlg.outputPath(), 5000);
}

void MainWindow::splitPdf() {
    const QString source = choosePdfSource();
    if (source.isEmpty()) return;

    bool ok = false;
    const int pagesPerFile = QInputDialog::getInt(this, QStringLiteral("拆分 PDF"),
                                                  QStringLiteral("每个文件包含多少页："), 20, 1, 100000, 1, &ok);
    if (!ok) return;

    const QString dir = QFileDialog::getExistingDirectory(this, QStringLiteral("拆分输出目录"),
                                                          QFileInfo(source).absolutePath());
    if (dir.isEmpty()) return;

    QApplication::setOverrideCursor(Qt::WaitCursor);
    pw::Status st = converter_->pdfBackend()->split(source.toStdString(), dir.toStdString(), pagesPerFile);
    QApplication::restoreOverrideCursor();

    if (!st.ok()) {
        appendLog(QStringLiteral("拆分失败: ") + QString::fromStdString(st.toString()));
        statusBar()->showMessage(QStringLiteral("拆分失败"), 5000);
        return;
    }
    appendLog(QStringLiteral("已拆分 %1（每 %2 页一个文件）-> %3")
                  .arg(QFileInfo(source).fileName())
                  .arg(pagesPerFile)
                  .arg(dir));
    statusBar()->showMessage(QStringLiteral("拆分完成: ") + dir, 5000);
}

void MainWindow::extractPages() {
    // 源文件与页码范围都在对话框里选，主界面不再挂输入框
    const QString current = viewer_ != nullptr ? viewer_->currentPath() : QString();
    const QStringList selectedPdfList = selectedPdfs();
    QStringList candidates;
    for (const auto& path : (selectedPdfList.isEmpty() ? allFiles() : selectedPdfList)) {
        if (fileExt(path) == QStringLiteral("pdf")) candidates << path;
    }
    ExtractDialog dlg(converter_->pdfBackend(), candidates, current, this);
    if (dlg.exec() != QDialog::Accepted) return;

    const QString source = dlg.sourcePath();
    const std::vector<int> pages = dlg.pages();
    if (pages.empty()) {
        appendLog(QStringLiteral("没有选中任何页"));
        return;
    }

    QApplication::setOverrideCursor(Qt::WaitCursor);
    pw::Status st = converter_->pdfBackend()->extract(source.toStdString(),
                                                      dlg.outputPath().toStdString(), pages);
    QApplication::restoreOverrideCursor();

    if (!st.ok()) {
        appendLog(QStringLiteral("抽取失败: ") + QString::fromStdString(st.toString()));
        statusBar()->showMessage(QStringLiteral("抽取失败"), 5000);
        return;
    }
    appendLog(QStringLiteral("已抽取 %1 页（%2）-> %3")
                  .arg(pages.size())
                  .arg(QString::fromStdString(pw::formatPageSpec(pages)))
                  .arg(dlg.outputPath()));
    statusBar()->showMessage(QStringLiteral("抽取完成: ") + dlg.outputPath(), 5000);
}

QString MainWindow::choosePdfSource() {
    const QString previewed = viewer_ != nullptr ? viewer_->currentPath() : QString();
    if (!previewed.isEmpty()) return previewed;
    const QStringList pdfs = selectedPdfs();
    if (pdfs.size() == 1) return pdfs.first();
    if (pdfs.isEmpty()) {
        appendLog(QStringLiteral("这里需要一个 PDF：先在待处理文件里添加或选中一个 PDF"));
        return QString();
    }
    bool ok = false;
    const QString chosen = QInputDialog::getItem(this, QStringLiteral("选择 PDF"),
                                                 QStringLiteral("对哪一个 PDF 执行："), pdfs, 0, false, &ok);
    return ok ? chosen : QString();
}

void MainWindow::editPages() {
    const QString source = choosePdfSource();
    if (source.isEmpty()) return;
    auto document = (source == viewer_->currentPath()) ? viewer_->document() : nullptr;
    PageEditorDialog dlg(converter_->pdfBackend(), source, document, QString(), this);
    connect(&dlg, &PageEditorDialog::logMessage, this, [this](const QString& text) { appendLog(text); });
    if (dlg.exec() == QDialog::Accepted) {
        statusBar()->showMessage(QStringLiteral("页面编辑完成，见日志"), 5000);
    }
}

// ---- 文档级工具的五个独立入口 ----
// 共同点：先确定"对哪个 PDF 干活"（当前预览 > 唯一选中 > 让用户选），
// 再开一个只负责一件事的窗口；窗口里的操作结果通过 logMessage 回流到主界面日志。

void MainWindow::openMetadata() {
    const QString source = choosePdfSource();
    if (source.isEmpty()) return;
    MetadataDialog dlg(converter_->pdfBackend(), source, this);
    connect(&dlg, &MetadataDialog::logMessage, this, [this](const QString& text) { appendLog(text); });
    dlg.exec();
}

void MainWindow::openSecurity() {
    const QString source = choosePdfSource();
    if (source.isEmpty()) return;
    SecurityDialog dlg(converter_->pdfBackend(), source, this);
    connect(&dlg, &SecurityDialog::logMessage, this, [this](const QString& text) { appendLog(text); });
    dlg.exec();
}

void MainWindow::openCompress() {
    const QString source = choosePdfSource();
    if (source.isEmpty()) return;
    CompressDialog dlg(converter_->pdfBackend(), source, this);
    connect(&dlg, &CompressDialog::logMessage, this, [this](const QString& text) { appendLog(text); });
    dlg.exec();
}

void MainWindow::openWatermark() {
    const QString source = choosePdfSource();
    if (source.isEmpty()) return;
    WatermarkDialog dlg(converter_->pdfBackend(), source, this);
    connect(&dlg, &WatermarkDialog::logMessage, this, [this](const QString& text) { appendLog(text); });
    dlg.exec();
}

void MainWindow::openImageToPdf() {
    // 图片转 PDF 不需要源 PDF：默认输出目录取当前预览文件所在目录，否则用列表第一个文件
    QString dir;
    if (viewer_ != nullptr && !viewer_->currentPath().isEmpty()) {
        dir = QFileInfo(viewer_->currentPath()).absolutePath();
    } else if (!allFiles().isEmpty()) {
        dir = QFileInfo(allFiles().first()).absolutePath();
    }
    ImageToPdfDialog dlg(converter_->pdfBackend(), dir, this);
    connect(&dlg, &ImageToPdfDialog::logMessage, this, [this](const QString& text) { appendLog(text); });
    dlg.exec();
}

void MainWindow::openOcr() {
    const QString source = choosePdfSource();
    if (source.isEmpty()) return;
    OcrDialog dlg(converter_->pdfBackend(), source, this);
    connect(&dlg, &OcrDialog::logMessage, this, [this](const QString& text) { appendLog(text); });
    dlg.exec();
}

// ---- 编辑与批注：5 个窗口 ---------------------------------------------------
//
// 这些窗口一律**非模态**（show() 而不是 exec()）。它们是"一边看预览一边填"的工具：
// 模态会把主窗口和预览一起框死——用户点「在预览里框选」时，鼠标事件根本到不了预览，
// 这才是"框选点不动、窗口挡住预览"的根子。同一时间只留一个，开新窗口先收旧的。
namespace {
void presentEditDialog(QPointer<QDialog>& slot, QDialog* dlg) {
    if (slot != nullptr) {
        slot->close();
        slot->deleteLater();
    }
    dlg->setAttribute(Qt::WA_DeleteOnClose);
    slot = dlg;
    dlg->show();
    dlg->raise();
    dlg->activateWindow();
}
} // namespace

void MainWindow::openTextImage() {
    const QString source = choosePdfSource();
    if (source.isEmpty()) return;

    // 「修改文字、图片」下面其实两条路：批量改文字走查找替换，换图走替换图片窗口
    QMessageBox box(this);
    box.setWindowTitle(QStringLiteral("修改文字、图片"));
    box.setText(QStringLiteral("要改哪一类内容？"));
    auto* textBtn = box.addButton(QStringLiteral("增删改文字"), QMessageBox::ActionRole);
    auto* imageBtn = box.addButton(QStringLiteral("替换图片"), QMessageBox::ActionRole);
    box.addButton(QStringLiteral("取消"), QMessageBox::RejectRole);
    box.exec();
    if (box.clickedButton() == textBtn) {
        auto* dlg = new FindReplaceDialog(converter_->pdfBackend(), source, this);
        connect(dlg, &FindReplaceDialog::logMessage, this, &MainWindow::appendLog);
        presentEditDialog(editDialog_, dlg);
    } else if (box.clickedButton() == imageBtn) {
        auto* dlg = new ReplaceImageDialog(converter_->pdfBackend(), source, viewer_, this);
        connect(dlg, &ReplaceImageDialog::logMessage, this, &MainWindow::appendLog);
        connect(dlg, &QDialog::finished, this, [this]() {
            if (viewer_ != nullptr) viewer_->setSelectMode(false);
        });
        presentEditDialog(editDialog_, dlg);
    }
}

void MainWindow::openAnnot() {
    const QString source = choosePdfSource();
    if (source.isEmpty()) return;
    auto* dlg = new AnnotDialog(converter_->pdfBackend(), source, viewer_, this);
    connect(dlg, &AnnotDialog::logMessage, this, &MainWindow::appendLog);
    connect(dlg, &QDialog::finished, this, [this]() {
        if (viewer_ != nullptr) viewer_->setSelectMode(false);
    });
    presentEditDialog(editDialog_, dlg);
}

void MainWindow::openLink() {
    const QString source = choosePdfSource();
    if (source.isEmpty()) return;
    auto* dlg = new LinkDialog(converter_->pdfBackend(), source, viewer_, this);
    connect(dlg, &LinkDialog::logMessage, this, &MainWindow::appendLog);
    connect(dlg, &QDialog::finished, this, [this]() {
        if (viewer_ != nullptr) viewer_->setSelectMode(false);
    });
    presentEditDialog(editDialog_, dlg);
}

void MainWindow::openFindReplace() {
    const QString source = choosePdfSource();
    if (source.isEmpty()) return;
    auto* dlg = new FindReplaceDialog(converter_->pdfBackend(), source, this);
    connect(dlg, &FindReplaceDialog::logMessage, this, &MainWindow::appendLog);
    presentEditDialog(editDialog_, dlg);
}

void MainWindow::openForm() {
    const QString source = choosePdfSource();
    if (source.isEmpty()) return;
    auto* dlg = new FormDialog(converter_->pdfBackend(), source, viewer_, this);
    connect(dlg, &FormDialog::logMessage, this, &MainWindow::appendLog);
    connect(dlg, &QDialog::finished, this, [this]() {
        if (viewer_ != nullptr) viewer_->setSelectMode(false);
    });
    presentEditDialog(editDialog_, dlg);
}

void MainWindow::showCapabilityReport() {
    QMessageBox::information(this, QStringLiteral("能力报告"),
                             QString::fromStdString(converter_->capabilityReport()));
}

void MainWindow::appendLog(const QString& text) {
    logView_->appendPlainText(text);
}
