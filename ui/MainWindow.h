#pragma once

#include <QDialog>
#include <QMainWindow>
#include <QPointer>
#include <memory>
#include <vector>

#include "pdfword/converter.hpp"
#include "pdfword/i18n.hpp"
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
class QRectF;
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

    // 预览就地编辑的"无交互"入口：UI 里走右键菜单 + 拖框，这两条给冒烟和脚本直接调。
    // 返回值是产物文件路径，失败返回空串。rect 是页面显示空间 pt（左上原点、y 向下）。
    QString previewReplaceText(int page, const QRectF& rect, const QString& text, bool deleteOnly);
    QString previewAddText(int page, const QRectF& rect, const QString& text, double fontSize = 0.0);
    // 冒烟用：走真实"选了动作 → 拖框 → 写回"的全链路，只是不弹输入框（文字由参数给）
    void previewInlineEditForTest(int action, int page, const QRectF& rect, const QString& text);

    // 换界面语言时要把"用户正在看的东西"带到新窗口：文件列表、选中项、预览的文件与页码缩放。
    // 界面文本都是在构造时算好的，重开一个窗口是最干净的换语言方式（也不会漏掉任何控件）。
    struct Session {
        QStringList files;
        QString previewPath;
        QString selectedPath;
        int page = 1;
        double zoom = 0.0;
    };
    Session captureSession() const;
    void applySession(const Session& session);

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
    void showAboutDialog();
    // 界面语言：跟随系统 / 简体中文 / English。切换后立刻生效（整窗重建，会话搬过去）。
    void switchLanguage(pw::i18n::Lang lang);
    // ---- 预览里"就地编辑"：右键选动作 → 在页面上拖一下 → 直接写回并刷新预览 ----
    void showPreviewMenu(int page, const QPoint& globalPos);
    void startInlineEdit(int action);
    void applyInlineEdit(int page, const QRectF& rect);

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

    // ---- 预览里的直接编辑 ----
    // 就地编辑的输出路径：a.pdf → a_已编辑.pdf；已经是 a_已编辑.pdf 就原地更新（走临时文件换名）
    static QString inlineOutputPath(const QString& source);
    // 预览里"按矩形写字"的统一流程：出件、原地换名、写日志、重载预览
    QString runPreviewTextEdit(int page, const QRectF& rect, const QString& text, bool deleteOnly,
                               double fontSize, bool backdrop, const QString& what);
    // 写完把预览切到新文件并留在原来那一页（顺手把新文件补进待处理列表）
    void reloadAfterInline(const QString& output, int page, int scrollValue = -1);
    // 在预览里直接加一条书签（标题默认"第 N 页"）
    void addBookmarkHere(int page);

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
    // 预览里正等着框选的那个编辑动作（0 = 没有；取值见 MainWindow.cpp 的 InlineAction）
    int inlineAction_ = 0;
    // 冒烟用：非空时 applyInlineEdit 不弹输入框，直接用这里的文本
    QString inlineTextForTest_;
    bool inlineTextForTestSet_ = false;
};
