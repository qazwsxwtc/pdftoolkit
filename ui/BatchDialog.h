#pragma once

#include <atomic>
#include <memory>

#include <QDialog>
#include <QString>
#include <QStringList>
#include <QThreadPool>

#include <pdfword/pdf_backend.hpp>

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QProgressBar;
class QPushButton;
class QSpinBox;
class QWidget;
class DropListWidget;

// 「批量」菜单下的七类操作。一个对话框只服务一种操作（构造时定死），
// 参数区按操作只建需要的那几行，避免七套控件挤在一个窗口里。
enum class BatchOp { Split, Extract, Metadata, Security, Compress, Watermark, Ocr };

// 批量处理窗口：文件列表 + 操作参数 + 输出目录，点开始后交给**独立线程池**并行跑。
//
// 并行规则（全工程统一约定）：
//   * 一个任务（线程）只处理一个文件，文件之间互不共享状态；
//   * 线程总数 = min(文件数, floor(逻辑内核数 × 1.3))，至少 1；
//   * 用自己的 QThreadPool，不占全局池（OCR 单文件窗口等还在用 QtConcurrent 全局池）。
//
// 后端每次调用都新建 fz_context / OCR 引擎按线程初始化公寓，天然线程安全；
// 界面回填一律经 QMetaObject::invokeMethod 排队回 GUI 线程，工作线程不碰控件。
// 任务没跑完不允许关窗（先取消或等完成），保证排队回调里的 this 始终有效。
class BatchDialog : public QDialog {
    Q_OBJECT

public:
    BatchDialog(std::shared_ptr<pw::IPdfBackend> backend, BatchOp op,
                const QStringList& initialFiles, QWidget* parent = nullptr);
    ~BatchDialog() override;

    // 操作显示名（窗口标题 / 日志用），如 "拆分 PDF"
    static QString opName(BatchOp op);
    // 输出名已存在就补 _2/_3，不覆盖旧产物（判重加锁，线程安全）
    static QString uniquePath(const QString& path);

signals:
    void logMessage(const QString& text);

protected:
    // 任务进行中不允许关闭：后台线程还要往这个窗口回填进度
    void reject() override;

private:
    void addFiles();
    void removeSelected();
    void clearFiles();
    void pickOutputDir();
    void pickWatermarkImage();
    void startBatch();
    void cancelBatch();
    void setBusy(bool busy);
    // GUI 线程：一个文件的收尾（计数、进度、日志），全部完成时复位界面
    void onFileFinished(int state, const QString& line);
    QStringList files() const;
    QWidget* buildOpParams();          // 按 op_ 只建当前操作的参数区

    std::shared_ptr<pw::IPdfBackend> backend_;
    BatchOp op_;

    // ---- 公共控件 ----
    DropListWidget* list_ = nullptr;
    QWidget* listButtons_ = nullptr;   // 添加/移除/清空三个按钮的容器，跑批时整体禁用
    QLineEdit* outDir_ = nullptr;
    QLineEdit* srcPassword_ = nullptr;   // 加密源文件的打开密码（可选）
    QPushButton* startBtn_ = nullptr;
    QPushButton* cancelBtn_ = nullptr;
    QProgressBar* progress_ = nullptr;
    QLabel* statusLabel_ = nullptr;
    QPlainTextEdit* logView_ = nullptr;
    QWidget* paramsArea_ = nullptr;

    // ---- 各操作参数（只有当前 op 的那组会被创建）----
    QSpinBox* pagesPerFile_ = nullptr;                       // Split
    QLineEdit* pageSpec_ = nullptr;                          // Extract
    QLineEdit* metaTitle_ = nullptr;                         // Metadata
    QLineEdit* metaAuthor_ = nullptr;
    QLineEdit* metaSubject_ = nullptr;
    QLineEdit* metaKeywords_ = nullptr;
    QCheckBox* metaKeepEmpty_ = nullptr;
    QComboBox* securityMode_ = nullptr;                      // Security
    QLineEdit* userPassword_ = nullptr;
    QLineEdit* ownerPassword_ = nullptr;
    QCheckBox* allowPrint_ = nullptr;
    QCheckBox* allowCopy_ = nullptr;
    QCheckBox* allowModify_ = nullptr;
    QCheckBox* allowAnnotate_ = nullptr;
    QComboBox* wmMode_ = nullptr;                            // Watermark
    QLineEdit* wmText_ = nullptr;
    QComboBox* wmPosition_ = nullptr;
    QLineEdit* wmImage_ = nullptr;
    QDoubleSpinBox* wmScale_ = nullptr;
    QCheckBox* wmTile_ = nullptr;
    QDoubleSpinBox* wmOpacity_ = nullptr;
    QComboBox* ocrLang_ = nullptr;                           // Ocr
    QCheckBox* ocrPdf_ = nullptr;
    QCheckBox* ocrTxt_ = nullptr;

    // ---- 运行状态 ----
    QThreadPool pool_;                 // 私有池：上限 = min(文件数, 逻辑核数×1.3)
    std::atomic<bool> cancel_{false};  // 工作线程只读它；排队中的任务见 true 即跳过
    bool busy_ = false;
    int total_ = 0;
    int finished_ = 0;                 // 下面四个只在 GUI 线程改（排队回调）
    int okCount_ = 0;
    int failCount_ = 0;
    int skipCount_ = 0;
};