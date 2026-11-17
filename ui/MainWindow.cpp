#include "MainWindow.h"

#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QComboBox>
#include <QDockWidget>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QStatusBar>
#include <QStringList>
#include <QToolBar>
#include <QVBoxLayout>
#include <filesystem>
#include <vector>

#include "ConvertDialog.h"
#include "DocumentToolsDialog.h"
#include "ExtractDialog.h"
#include "MergeDialog.h"
#include "PageEditorDialog.h"
#include "PdfViewWidget.h"
#include "pdfword/page_selection.hpp"

namespace {

QString fileExt(const QString& path) {
    const int dot = path.lastIndexOf('.');
    if (dot < 0) return QString();
    return path.mid(dot + 1).toLower();
}

}  // namespace

MainWindow::MainWindow(std::shared_ptr<pw::Converter> converter, std::shared_ptr<pw::JobQueue> jobs,
                       QWidget* parent)
    : QMainWindow(parent), converter_(std::move(converter)), jobs_(std::move(jobs)) {
    setWindowTitle(QStringLiteral("PdfWord — PDF/Word 转换与预览"));
    resize(1280, 860);

    createCentral();
    createFileDock();
    createMenus();

    connect(fileList_, &QListWidget::itemDoubleClicked, this, &MainWindow::previewSelected);

    // core 的 JobQueue 在 worker 线程回调，这里用 QueuedConnection 切回 UI 线程
    jobs_->setListener([this](const pw::JobInfo& info) {
        QMetaObject::invokeMethod(
            this, [this, info] { applyJob(info); }, Qt::QueuedConnection);
    });

    // 状态栏显示依赖状态，一眼能看出是"没装 LibreOffice"还是"没编 MuPDF"
    std::string report = converter_->capabilityReport();
    std::string firstLine = report.substr(0, report.find('\n'));
    statusBar()->showMessage(QString::fromStdString(firstLine));
    appendLog(QString::fromStdString(report));
}

MainWindow::~MainWindow() {
    jobs_->setListener(nullptr);
}

void MainWindow::createCentral() {
    // ---- 预览 + 日志（导航按钮放工具栏）----
    auto* central = new QWidget(this);
    auto* layout = new QVBoxLayout(central);

    viewer_ = new PdfViewWidget(converter_->pdfBackend(), central);
    layout->addWidget(viewer_, 1);

    logView_ = new QPlainTextEdit(central);
    logView_->setReadOnly(true);
    logView_->setMaximumBlockCount(2000);
    logView_->setMaximumHeight(150);
    layout->addWidget(logView_);

    setCentralWidget(central);

    auto* nav = addToolBar(QStringLiteral("预览"));
    nav->setMovable(false);
    auto* prevAct = nav->addAction(QStringLiteral("上一页"));
    auto* nextAct = nav->addAction(QStringLiteral("下一页"));
    nav->addSeparator();
    auto* zoomInAct = nav->addAction(QStringLiteral("放大"));
    auto* zoomOutAct = nav->addAction(QStringLiteral("缩小"));
    nav->addSeparator();
    pageLabel_ = new QLabel(QStringLiteral("— / —"), this);
    nav->addWidget(pageLabel_);

    connect(prevAct, &QAction::triggered, this, [this] { viewer_->showPage(viewer_->currentPage() - 1); });
    connect(nextAct, &QAction::triggered, this, [this] { viewer_->showPage(viewer_->currentPage() + 1); });
    connect(zoomInAct, &QAction::triggered, this, [this] { viewer_->setZoom(viewer_->zoom() * 1.25); });
    connect(zoomOutAct, &QAction::triggered, this, [this] { viewer_->setZoom(viewer_->zoom() / 1.25); });
    connect(viewer_, &PdfViewWidget::pageChanged, this, &MainWindow::pageChanged);

    progress_ = new QProgressBar(this);
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
    auto* layout = new QVBoxLayout(panel);
    layout->setContentsMargins(6, 6, 6, 6);

    auto* hint = new QLabel(QStringLiteral("双击 PDF 可预览"), panel);
    hint->setStyleSheet(QStringLiteral("color:#666;"));
    layout->addWidget(hint);

    fileList_ = new QListWidget(panel);
    fileList_->setSelectionMode(QAbstractItemView::ExtendedSelection);
    layout->addWidget(fileList_, 1);

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
    auto* pdfMenu = menuBar()->addMenu(QStringLiteral("PDF 工具(&P)"));
    pdfMenu->addAction(QStringLiteral("合并 PDF…"), this, &MainWindow::mergePdfs);
    pdfMenu->addAction(QStringLiteral("拆分 PDF…"), this, &MainWindow::splitPdf);
    pdfMenu->addAction(QStringLiteral("抽取页面…"), this, &MainWindow::extractPages);
    pdfMenu->addSeparator();
    pdfMenu->addAction(QStringLiteral("页面编辑…"), this, &MainWindow::editPages);
    pdfMenu->addAction(QStringLiteral("文档工具…"), this, &MainWindow::openDocumentTools);

    // ---- 视图：文件面板靠左 / 靠右 ----
    auto* viewMenu = menuBar()->addMenu(QStringLiteral("视图(&V)"));
    viewMenu->addAction(QStringLiteral("显示待处理文件"), this, [this] {
        fileDock_->show();
        fileDock_->raise();
    });
    viewMenu->addSeparator();
    dockSideGroup_ = new QActionGroup(this);
    dockSideGroup_->setExclusive(true);

    auto* leftAct = viewMenu->addAction(QStringLiteral("待处理文件靠左"));
    leftAct->setCheckable(true);
    leftAct->setChecked(true);
    leftAct->setActionGroup(dockSideGroup_);
    dockLeftAct_ = leftAct;
    connect(leftAct, &QAction::triggered, this, [this] { dockFileList(false); });

    auto* rightAct = viewMenu->addAction(QStringLiteral("待处理文件靠右"));
    rightAct->setCheckable(true);
    rightAct->setActionGroup(dockSideGroup_);
    dockRightAct_ = rightAct;
    connect(rightAct, &QAction::triggered, this, [this] { dockFileList(true); });

    // 用户自己拖动面板（或拖成浮动窗口）后，把菜单勾选状态跟真实位置对齐
    connect(fileDock_, &QDockWidget::dockLocationChanged, this, [this](Qt::DockWidgetArea area) {
        const bool right = (area == Qt::RightDockWidgetArea);
        if (dockRightAct_ != nullptr) dockRightAct_->setChecked(right);
        if (dockLeftAct_ != nullptr) dockLeftAct_->setChecked(!right);
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

void MainWindow::addFiles() {
    const QStringList files = QFileDialog::getOpenFileNames(
        this, QStringLiteral("选择文件"), QString(),
        QStringLiteral("文档 (*.pdf *.docx *.doc *.odt *.rtf *.txt *.xlsx *.pptx);;所有文件 (*.*)"));
    for (const auto& f : files) {
        auto* item = new QListWidgetItem(QFileInfo(f).fileName(), fileList_);
        item->setData(Qt::UserRole, f);
        item->setToolTip(f);
    }
}

void MainWindow::removeSelected() {
    for (auto* item : fileList_->selectedItems()) delete item;
}

void MainWindow::clearFiles() {
    fileList_->clear();
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
    QString error;
    if (!viewer_->openDocument(path, &error)) {
        appendLog(QStringLiteral("预览失败: ") + error);
    }
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
    for (const auto& f : dlg.files()) {
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

void MainWindow::openDocumentTools() {
    const QString source = choosePdfSource();
    if (source.isEmpty()) return;
    DocumentToolsDialog dlg(converter_->pdfBackend(), source, this);
    connect(&dlg, &DocumentToolsDialog::logMessage, this, [this](const QString& text) { appendLog(text); });
    dlg.exec();
}

void MainWindow::showCapabilityReport() {
    QMessageBox::information(this, QStringLiteral("能力报告"),
                             QString::fromStdString(converter_->capabilityReport()));
}

void MainWindow::pageChanged(int page, int total) {
    pageLabel_->setText(QStringLiteral("%1 / %2").arg(page).arg(total));
}

void MainWindow::appendLog(const QString& text) {
    logView_->appendPlainText(text);
}
