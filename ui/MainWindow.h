#pragma once

#include <QDialog>
#include <QMainWindow>
#include <QPointer>
#include <memory>
#include <vector>

#include "pdfword/converter.hpp"
#include "pdfword/job.hpp"

class QAction;
class QActionGroup;
class QCloseEvent;
class QDockWidget;
class QDragEnterEvent;
class QDragMoveEvent;
class QDropEvent;
class QLabel;
class QListWidget;
class QMimeData;
class QPlainTextEdit;
class QProgressBar;
class QSplitter;
class QTreeWidget;
class QTreeWidgetItem;
class PdfViewWidget;

// 界面层只做三件事：收文件、显示预览、把任务丢给 core 的 JobQueue / PdfBackend。
// 所有格式转换/PDF 处理逻辑都在 core，界面里不出现任何业务判断。
//
// 这一版把原来那排按钮全部收进菜单栏：
//   文件 / 转换 / PDF 工具 / 视图
// "待处理文件"是一个 QDockWidget，可以用菜单在左侧、右侧之间切换，也可以拖出来当独立窗口。
class MainWindow : public QMainWindow {
    Q_OBJECT

public:
    MainWindow(std::shared_ptr<pw::Converter> converter, std::shared_ptr<pw::JobQueue> jobs,
               QWidget* parent = nullptr);
    ~MainWindow() override;

private slots:
    void addFiles();
    void removeSelected();
    void clearFiles();
    void previewSelected();
    void startConversion();
    void mergePdfs();
    void splitPdf();
    void extractPages();
    void editPages();
    void openMetadata();
    void openSecurity();
    void openCompress();
    void openWatermark();
    void openImageToPdf();
    void openOcr();
    // 编辑与批注：5 个子功能，各开一个窗口
    void openTextImage();
    void openAnnot();
    void openLink();
    void openFindReplace();
    void openForm();
    void showCapabilityReport();

protected:
    // 从资源管理器把文件拖到窗口任意位置都能入列
    void dragEnterEvent(QDragEnterEvent* e) override;
    void dragMoveEvent(QDragMoveEvent* e) override;
    void dropEvent(QDropEvent* e) override;
    // 关窗时把视图状态写进 INI，下次启动还原
    void closeEvent(QCloseEvent* e) override;

private:
    void createMenus();
    void createFileDock();
    void createCentral();
    void appendLog(const QString& text);
    void applyJob(const pw::JobInfo& info);
    // 首次显示前把窗口摆到主屏可用区域的中心（多屏时跟随主屏，不压到任务栏）
    void centerOnScreen();
    void dockFileList(bool rightSide);
    // 文件列表整个面板的显示 / 隐藏（「视图 → 文件列表」里的两项）
    void showFileList(bool visible);
    // 日志窗口显示/隐藏：隐藏后预览区由 splitter 自动铺满
    void setLogVisible(bool visible);
    // 视图状态持久化：面板显隐/停靠侧/日志高度/窗口几何
    void saveViewState();
    void restoreViewState();
    QStringList allFiles() const;
    QStringList selectedFiles() const;
    // 列表里被选中的 PDF（没选就取全部），按列表顺序
    QStringList selectedPdfs() const;
    // 需要用"某个 PDF"来操作时的取源逻辑：当前预览 > 唯一选中 > 询问用户
    QString choosePdfSource();
    // 可入列的文档扩展名（小写、不带点）
    static QStringList supportedExtensions();
    // 把一批路径加进待处理列表，返回真正新增的条数（重复路径跳过）
    int addPathsToFileList(const QStringList& paths);
    // 拖入单个 PDF 时直接预览；失败只写日志，不弹窗
    void previewPath(const QString& path);
    // 文件列表右键菜单：预览 / 删除 / 清空 / 重命名文件…
    void showFileListMenu(const QPoint& pos);
    void renameSelectedFile();

    // ---- 书签 / 目录面板（视图 → 书签目录）----
    void createBookmarkDock();
    // 面板显隐（菜单两项互斥，dock 自带关闭按钮也会同步勾选）
    void setBookmarksVisible(bool visible);
    // 换文档时重读书签；路径为空就只是清空树
    void refreshBookmarkTree(const QString& path);
    // 把 core 的扁平列表（level 表示层级）还原成树
    void rebuildBookmarkTree(const std::vector<pw::OutlineItem>& items);
    // 反过来：把树拍平成扁平列表，层级 = 树的深度
    void collectBookmarks(QTreeWidgetItem* node, int level, std::vector<pw::OutlineItem>& out) const;
    void bookmarkAdd(bool asChild);
    void bookmarkEdit();
    void bookmarkRemove();
    void bookmarkMove(int delta);
    void bookmarkApply();
    void bookmarkExport();
    // 双击条目跳到对应页
    void bookmarkActivated(QTreeWidgetItem* item, int column);

    std::shared_ptr<pw::Converter> converter_;
    std::shared_ptr<pw::JobQueue> jobs_;

    QDockWidget* fileDock_ = nullptr;
    QListWidget* fileList_ = nullptr;
    QSplitter* splitter_ = nullptr;
    PdfViewWidget* viewer_ = nullptr;
    QPlainTextEdit* logView_ = nullptr;
    QLabel* statusLabel_ = nullptr;
    QProgressBar* progress_ = nullptr;
    QActionGroup* dockSideGroup_ = nullptr;
    QAction* dockLeftAct_ = nullptr;
    QAction* dockRightAct_ = nullptr;
    QActionGroup* fileVisibleGroup_ = nullptr;
    QAction* showFilesAct_ = nullptr;
    QAction* hideFilesAct_ = nullptr;
    QActionGroup* logGroup_ = nullptr;
    QAction* logShowAct_ = nullptr;
    QAction* logHideAct_ = nullptr;
    QDockWidget* bookmarkDock_ = nullptr;
    QTreeWidget* bookmarkTree_ = nullptr;
    QActionGroup* bookmarkGroup_ = nullptr;
    QAction* bookmarkShowAct_ = nullptr;
    QAction* bookmarkHideAct_ = nullptr;
    // 面板里这棵树是从哪个 PDF 读来的；空 = 还没预览过
    QString bookmarkPath_;
    // 日志窗口隐藏前的高度，显示/下次启动时按它还原
    int lastLogHeight_ = 140;
    // 当前打开的「编辑与批注」窗口。非模态，弹着的时候主窗口照常能点、预览照常能滚
    QPointer<QDialog> editDialog_;
};
