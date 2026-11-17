#pragma once

#include <QMainWindow>
#include <memory>

#include "pdfword/converter.hpp"
#include "pdfword/job.hpp"

class QAction;
class QActionGroup;
class QDockWidget;
class QLabel;
class QListWidget;
class QPlainTextEdit;
class QProgressBar;
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
    void openDocumentTools();
    void pageChanged(int page, int total);
    void showCapabilityReport();

private:
    void createMenus();
    void createFileDock();
    void createCentral();
    void appendLog(const QString& text);
    void applyJob(const pw::JobInfo& info);
    void dockFileList(bool rightSide);
    QStringList allFiles() const;
    QStringList selectedFiles() const;
    // 列表里被选中的 PDF（没选就取全部），按列表顺序
    QStringList selectedPdfs() const;
    // 需要用"某个 PDF"来操作时的取源逻辑：当前预览 > 唯一选中 > 询问用户
    QString choosePdfSource();

    std::shared_ptr<pw::Converter> converter_;
    std::shared_ptr<pw::JobQueue> jobs_;

    QDockWidget* fileDock_ = nullptr;
    QListWidget* fileList_ = nullptr;
    PdfViewWidget* viewer_ = nullptr;
    QPlainTextEdit* logView_ = nullptr;
    QProgressBar* progress_ = nullptr;
    QLabel* pageLabel_ = nullptr;
    QActionGroup* dockSideGroup_ = nullptr;
    QAction* dockLeftAct_ = nullptr;
    QAction* dockRightAct_ = nullptr;
};
