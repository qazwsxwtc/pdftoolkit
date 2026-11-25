#include "BatchDialog.h"
#include "I18nUi.h"

#include <filesystem>
#include <mutex>
#include <vector>

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QMetaObject>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QSpinBox>
#include <QThread>
#include <QVBoxLayout>

#include <pdfword/ocr.hpp>
#include <pdfword/page_selection.hpp>

#include "DialogCommon.h"
#include "DropListWidget.h"
#include "pdfword/path_util.hpp"

namespace {

// 权限位与 SecurityDialog / core 的 pw::Permission 一致
constexpr int kPermPrint = 1 << 2;
constexpr int kPermModify = 1 << 3;
constexpr int kPermCopy = 1 << 4;
constexpr int kPermAnnotate = 1 << 5;

QString fileStem(const QString& path) { return QFileInfo(path).completeBaseName(); }

std::filesystem::path toFs(const QString& s) { return pw::fsPath(s.toUtf8().toStdString()); }

std::string toU8(const QString& s) { return s.toUtf8().toStdString(); }

// 一次批量运行的全部参数：GUI 线程填好后按值拷进每个任务，任务之间零共享。
struct BatchParams {
    BatchOp op = BatchOp::Split;
    QString outDir;
    std::string srcPwd;              // 加密源文件的打开密码（可选）
    int pagesPerFile = 20;           // Split
    std::vector<int> pages;          // Extract（0 基，已在开始时解析校验）
    pw::DocumentMetadata meta;       // Metadata
    bool metaKeepEmpty = true;
    bool encryptMode = true;         // Security
    std::string userPwd;
    std::string ownerPwd;
    int perms = 0;
    bool wmImage = false;            // Watermark
    std::string wmText;
    std::string wmPosition;
    QString wmImagePath;
    float wmOpacity = 0.15f;
    float wmScale = 0.30f;
    bool wmTile = false;
    std::string ocrLang;             // Ocr
    bool ocrPdf = true;
    bool ocrTxt = true;
};

// state: 0 成功 / 1 失败 / 2 跳过（取消）
struct BatchResult {
    int state = 0;
    QString detail;
};

BatchResult fail(const QString& file, const pw::Status& st) {
    return {1, QFileInfo(file).fileName() + QStringLiteral(" — ")
                   + QString::fromStdString(st.toString())};
}

// 一个任务只处理一个文件。后端每次调用自建 fz_context / OCR 引擎，线程安全。
BatchResult processOne(const std::shared_ptr<pw::IPdfBackend>& backend, const BatchParams& p,
                       const QString& file) {
    const QString stem = fileStem(file);
    const auto src = toFs(file);

    switch (p.op) {
        case BatchOp::Split: {
            // 每个源文件一个子文件夹，多个同名 PDF 也不会互相覆盖
            const QString sub = p.outDir + QLatin1Char('/') + stem + TR("_拆分");
            const pw::Status st = backend->split(src, toFs(sub), p.pagesPerFile);
            if (!st.ok()) return fail(file, st);
            return {0, TR("已拆分 %1（每 %2 页一个文件）-> %3")
                           .arg(QFileInfo(file).fileName())
                           .arg(p.pagesPerFile)
                           .arg(sub)};
        }
        case BatchOp::Extract: {
            const QString out = BatchDialog::uniquePath(
                p.outDir + QLatin1Char('/') + stem + TR("_抽取") + QStringLiteral(".pdf"));
            const pw::Status st = backend->extract(src, toFs(out), p.pages, p.srcPwd);
            if (!st.ok()) return fail(file, st);
            return {0, TR("已抽取 %1 页（%2）-> %3")
                           .arg(static_cast<int>(p.pages.size()))
                           .arg(QString::fromStdString(pw::formatPageSpec(p.pages)))
                           .arg(out)};
        }
        case BatchOp::Metadata: {
            pw::DocumentMetadata meta = p.meta;
            if (p.metaKeepEmpty) {
                // 留空的字段保留“这个文件自己”的原值，而不是被空串清掉
                auto cur = backend->metadata(src, p.srcPwd);
                if (!cur.ok()) {
                    return {1, QFileInfo(file).fileName() + QStringLiteral(" — ")
                                   + TR("读取元数据失败: ")
                                   + QString::fromStdString(cur.status().toString())};
                }
                if (meta.title.empty()) meta.title = cur.value().title;
                if (meta.author.empty()) meta.author = cur.value().author;
                if (meta.subject.empty()) meta.subject = cur.value().subject;
                if (meta.keywords.empty()) meta.keywords = cur.value().keywords;
            }
            const QString out = BatchDialog::uniquePath(
                p.outDir + QLatin1Char('/') + stem + TR("_元数据") + QStringLiteral(".pdf"));
            const pw::Status st = backend->setMetadata(src, toFs(out), meta, p.srcPwd);
            if (!st.ok()) return fail(file, st);
            return {0, TR("元数据已写入 -> ") + out};
        }
        case BatchOp::Security: {
            if (p.encryptMode) {
                const QString out = BatchDialog::uniquePath(
                    p.outDir + QLatin1Char('/') + stem + TR("_加密") + QStringLiteral(".pdf"));
                const pw::Status st =
                    backend->encrypt(src, toFs(out), p.userPwd, p.ownerPwd, p.perms, p.srcPwd);
                if (!st.ok()) return fail(file, st);
                return {0, TR("已加密（AES-128）-> ") + out};
            }
            const QString out = BatchDialog::uniquePath(
                p.outDir + QLatin1Char('/') + stem + TR("_解密") + QStringLiteral(".pdf"));
            const pw::Status st = backend->decrypt(src, toFs(out), p.userPwd);
            if (!st.ok()) return fail(file, st);
            return {0, TR("已解密 -> ") + out};
        }
        case BatchOp::Compress: {
            const QString out = BatchDialog::uniquePath(
                p.outDir + QLatin1Char('/') + stem + TR("_压缩") + QStringLiteral(".pdf"));
            const pw::Status st = backend->compress(src, toFs(out), p.srcPwd);
            if (!st.ok()) return fail(file, st);
            return {0, TR("已压缩 -> %1（%2 KB → %3 KB）")
                           .arg(out)
                           .arg(QFileInfo(file).size() / 1024)
                           .arg(QFileInfo(out).size() / 1024)};
        }
        case BatchOp::Watermark: {
            const QString out = BatchDialog::uniquePath(
                p.outDir + QLatin1Char('/') + stem + TR("_水印") + QStringLiteral(".pdf"));
            if (p.wmImage) {
                const pw::Status st = backend->imageWatermark(
                    src, toFs(out), toFs(p.wmImagePath), p.wmOpacity, p.wmScale, p.wmTile, p.srcPwd);
                if (!st.ok()) return fail(file, st);
                return {0, TR("已加图片水印%1 -> %2")
                               .arg(p.wmTile ? TR("（平铺）") : QString())
                               .arg(out)};
            }
            // 注意：文字水印接口不带 password 参数，加密源文件请先批量解密
            const pw::Status st =
                backend->watermark(src, toFs(out), p.wmText, p.wmOpacity, p.wmPosition);
            if (!st.ok()) return fail(file, st);
            return {0, TR("已加水印（%1） -> %2")
                           .arg(QString::fromStdString(p.wmPosition))
                           .arg(out)};
        }
        case BatchOp::Ocr: {
            const QString pdfOut =
                p.ocrPdf ? BatchDialog::uniquePath(p.outDir + QLatin1Char('/') + stem
                                                   + TR("_可检索") + QStringLiteral(".pdf"))
                         : QString();
            const QString txtOut =
                p.ocrTxt ? BatchDialog::uniquePath(p.outDir + QLatin1Char('/') + stem
                                                   + TR("_识别.txt"))
                         : QString();
            const pw::Status st = backend->ocr(
                src, p.ocrPdf ? toFs(pdfOut) : std::filesystem::path(),
                p.ocrTxt ? toFs(txtOut) : std::filesystem::path(), p.ocrLang, p.ocrPdf, p.ocrTxt,
                p.srcPwd);
            if (!st.ok()) return fail(file, st);
            QString detail;
            if (p.ocrPdf) {
                detail += TR("可检索 PDF -> %1（%2 KB）").arg(pdfOut).arg(QFileInfo(pdfOut).size() / 1024);
            }
            if (p.ocrTxt) {
                if (!detail.isEmpty()) detail += QStringLiteral("；");
                detail += TR("纯文本 -> %1（%2 KB）").arg(txtOut).arg(QFileInfo(txtOut).size() / 1024);
            }
            return {0, detail};
        }
    }
    return {1, QFileInfo(file).fileName()};
}

}  // namespace

QString BatchDialog::opName(BatchOp op) {
    switch (op) {
        case BatchOp::Split: return TR("拆分 PDF");
        case BatchOp::Extract: return TR("抽取页面");
        case BatchOp::Metadata: return TR("元数据");
        case BatchOp::Security: return TR("加密 / 解密");
        case BatchOp::Compress: return TR("压缩");
        case BatchOp::Watermark: return TR("水印");
        case BatchOp::Ocr: return TR("OCR 文字识别");
    }
    return QString();
}

QString BatchDialog::uniquePath(const QString& path) {
    // 判重和占名要原子：两个同名源文件可能被不同线程同时算输出名
    static std::mutex mutex;
    std::lock_guard<std::mutex> lock(mutex);
    const QFileInfo fi(path);
    const QString base = fi.absolutePath() + QLatin1Char('/') + fi.completeBaseName();
    const QString suffix = fi.suffix().isEmpty() ? QString() : QLatin1Char('.') + fi.suffix();
    QString candidate = base + suffix;
    for (int i = 2; QFileInfo::exists(candidate); ++i) {
        candidate = base + QStringLiteral("_%1").arg(i) + suffix;
    }
    return candidate;
}

BatchDialog::BatchDialog(std::shared_ptr<pw::IPdfBackend> backend, BatchOp op,
                         const QStringList& initialFiles, QWidget* parent)
    : QDialog(parent), backend_(std::move(backend)), op_(op) {
    setWindowTitle(TR("批量%1").arg(opName(op_)));
    resize(780, 680);

    auto* root = new QVBoxLayout(this);
    root->addWidget(new QLabel(
        TR("每个文件一个任务，线程池并行处理；源文件不改动，结果写到输出目录。"), this));

    const int cores = QThread::idealThreadCount();
    const int cap = qMax(1, static_cast<int>(cores * 1.3));
    auto* threadLabel = new QLabel(
        TR("线程上限 = 逻辑内核数 × 1.3（本机 %1 核，最多 %2 线程）").arg(cores).arg(cap), this);
    threadLabel->setStyleSheet(QStringLiteral("color:#666;"));
    root->addWidget(threadLabel);

    // ---- 文件列表 + 三个管理按钮 ----
    root->addWidget(new QLabel(TR("待处理 PDF（可拖入）"), this));
    auto* listRow = new QHBoxLayout();
    list_ = new DropListWidget(this);
    list_->setObjectName(QStringLiteral("batchFileList"));
    list_->setExtensions({QStringLiteral("pdf")});
    list_->setSelectionMode(QAbstractItemView::ExtendedSelection);
    list_->setMinimumHeight(120);
    for (const QString& path : initialFiles) {
        if (!QFileInfo::exists(path)) continue;
        auto* item = new QListWidgetItem(QFileInfo(path).fileName(), list_);
        item->setData(Qt::UserRole, path);
        item->setToolTip(path);
    }
    listRow->addWidget(list_, 1);

    auto* listBtns = new QWidget(this);
    auto* btnCol = new QVBoxLayout(listBtns);
    btnCol->setContentsMargins(0, 0, 0, 0);
    auto* addBtn = new QPushButton(TR("添加文件…"), listBtns);
    auto* removeBtn = new QPushButton(TR("移除选中"), listBtns);
    auto* clearBtn = new QPushButton(TR("清空列表"), listBtns);
    btnCol->addWidget(addBtn);
    btnCol->addWidget(removeBtn);
    btnCol->addWidget(clearBtn);
    btnCol->addStretch(1);
    listRow->addWidget(listBtns);
    root->addLayout(listRow);
    listButtons_ = listBtns;

    connect(addBtn, &QPushButton::clicked, this, &BatchDialog::addFiles);
    connect(removeBtn, &QPushButton::clicked, this, &BatchDialog::removeSelected);
    connect(clearBtn, &QPushButton::clicked, this, &BatchDialog::clearFiles);
    connect(list_, &DropListWidget::filesDropped, this, [this](const QStringList& paths) {
        const QStringList known = files();
        for (const QString& path : paths) {
            if (known.contains(path) || !QFileInfo::exists(path)) continue;
            auto* item = new QListWidgetItem(QFileInfo(path).fileName(), list_);
            item->setData(Qt::UserRole, path);
            item->setToolTip(path);
        }
    });

    // ---- 当前操作的参数区 ----
    paramsArea_ = buildOpParams();
    if (paramsArea_ != nullptr) root->addWidget(paramsArea_);

    // ---- 输出目录 / 源文件密码 ----
    auto* commonForm = new QFormLayout();
    auto* outRow = new QHBoxLayout();
    outDir_ = new QLineEdit(this);
    outDir_->setObjectName(QStringLiteral("batchOutDir"));
    if (!initialFiles.isEmpty()) outDir_->setText(QFileInfo(initialFiles.first()).absolutePath());
    auto* outBrowse = new QPushButton(TR("浏览…"), this);
    outRow->addWidget(outDir_, 1);
    outRow->addWidget(outBrowse);
    commonForm->addRow(TR("输出目录"), outRow);

    srcPassword_ = new QLineEdit(this);
    srcPassword_->setObjectName(QStringLiteral("batchSrcPassword"));
    srcPassword_->setEchoMode(QLineEdit::Password);
    srcPassword_->setPlaceholderText(TR("源文件已加密时填这里，未加密留空"));
    if (op_ != BatchOp::Split) {
        commonForm->addRow(TR("源文件打开密码（可选）"), srcPassword_);
    }
    root->addLayout(commonForm);
    connect(outBrowse, &QPushButton::clicked, this, &BatchDialog::pickOutputDir);

    // ---- 进度 / 状态 / 窗口内日志 ----
    progress_ = new QProgressBar(this);
    progress_->setObjectName(QStringLiteral("batchProgress"));
    progress_->setRange(0, 1);
    progress_->setValue(0);
    root->addWidget(progress_);
    statusLabel_ = new QLabel(TR("就绪"), this);
    statusLabel_->setObjectName(QStringLiteral("batchStatus"));
    root->addWidget(statusLabel_);
    logView_ = new QPlainTextEdit(this);
    logView_->setObjectName(QStringLiteral("batchLog"));
    logView_->setReadOnly(true);
    logView_->setMaximumHeight(140);
    root->addWidget(logView_);

    // ---- 开始 / 取消 / 关闭 ----
    auto* btnRow = new QHBoxLayout();
    startBtn_ = new QPushButton(TR("开始批量处理"), this);
    startBtn_->setObjectName(QStringLiteral("batchStartButton"));
    startBtn_->setDefault(true);
    cancelBtn_ = new QPushButton(TR("取消"), this);
    cancelBtn_->setObjectName(QStringLiteral("batchCancelButton"));
    cancelBtn_->setEnabled(false);
    btnRow->addWidget(startBtn_);
    btnRow->addWidget(cancelBtn_);
    btnRow->addStretch(1);
    root->addLayout(btnRow);

    auto* box = new QDialogButtonBox(QDialogButtonBox::Close, this);
    box->button(QDialogButtonBox::Close)->setText(TR("关闭"));
    root->addWidget(box);

    connect(startBtn_, &QPushButton::clicked, this, &BatchDialog::startBatch);
    connect(cancelBtn_, &QPushButton::clicked, this, &BatchDialog::cancelBatch);
    connect(box, &QDialogButtonBox::rejected, this, &BatchDialog::reject);
}

BatchDialog::~BatchDialog() {
    // 正常路径下关窗前任务已全部收尾；这里兜底，保证线程不会踩到已销毁的成员
    cancel_.store(true);
    pool_.waitForDone();
}

QWidget* BatchDialog::buildOpParams() {
    auto* page = new QWidget(this);
    auto* form = new QFormLayout(page);
    form->setContentsMargins(0, 0, 0, 0);

    switch (op_) {
        case BatchOp::Split: {
            pagesPerFile_ = new QSpinBox(page);
            pagesPerFile_->setObjectName(QStringLiteral("batchPagesPerFile"));
            pagesPerFile_->setRange(1, 100000);
            pagesPerFile_->setValue(20);
            form->addRow(TR("每个文件包含多少页："), pagesPerFile_);
            auto* hint = new QLabel(
                TR("每个源文件在输出目录下建“主名_拆分”子文件夹，避免同名冲突。"), page);
            hint->setStyleSheet(QStringLiteral("color:#666;"));
            form->addRow(hint);
            break;
        }
        case BatchOp::Extract: {
            pageSpec_ = new QLineEdit(page);
            pageSpec_->setObjectName(QStringLiteral("batchPageSpec"));
            pageSpec_->setPlaceholderText(QStringLiteral("1-3,5"));
            form->addRow(TR("页码范围"), pageSpec_);
            auto* hint = new QLabel(
                TR("同一份页码范围套用到每个文件；超出某文件页数的会按失败记入日志。"), page);
            hint->setStyleSheet(QStringLiteral("color:#666;"));
            form->addRow(hint);
            break;
        }
        case BatchOp::Metadata: {
            metaTitle_ = new QLineEdit(page);
            metaAuthor_ = new QLineEdit(page);
            metaSubject_ = new QLineEdit(page);
            metaKeywords_ = new QLineEdit(page);
            metaTitle_->setObjectName(QStringLiteral("batchMetaTitle"));
            metaAuthor_->setObjectName(QStringLiteral("batchMetaAuthor"));
            metaSubject_->setObjectName(QStringLiteral("batchMetaSubject"));
            metaKeywords_->setObjectName(QStringLiteral("batchMetaKeywords"));
            form->addRow(TR("标题"), metaTitle_);
            form->addRow(TR("作者"), metaAuthor_);
            form->addRow(TR("主题"), metaSubject_);
            form->addRow(TR("关键词"), metaKeywords_);
            metaKeepEmpty_ = new QCheckBox(TR("留空的字段保留各文件原值"), page);
            metaKeepEmpty_->setObjectName(QStringLiteral("batchMetaKeepEmpty"));
            metaKeepEmpty_->setChecked(true);
            form->addRow(QString(), metaKeepEmpty_);
            break;
        }
        case BatchOp::Security: {
            securityMode_ = new QComboBox(page);
            securityMode_->setObjectName(QStringLiteral("batchSecurityMode"));
            securityMode_->addItem(TR("加密（AES-128）"), true);
            securityMode_->addItem(TR("解密（去掉密码）"), false);
            form->addRow(TR("模式"), securityMode_);

            userPassword_ = new QLineEdit(page);
            ownerPassword_ = new QLineEdit(page);
            userPassword_->setObjectName(QStringLiteral("batchUserPassword"));
            ownerPassword_->setObjectName(QStringLiteral("batchOwnerPassword"));
            userPassword_->setEchoMode(QLineEdit::Password);
            ownerPassword_->setEchoMode(QLineEdit::Password);
            userPassword_->setPlaceholderText(TR("打开文档时要输入的密码"));
            ownerPassword_->setPlaceholderText(TR("留空则用打开密码代替"));
            form->addRow(TR("打开密码"), userPassword_);
            form->addRow(TR("所有者密码"), ownerPassword_);

            allowPrint_ = new QCheckBox(TR("允许打印"), page);
            allowCopy_ = new QCheckBox(TR("允许复制文本"), page);
            allowModify_ = new QCheckBox(TR("允许修改"), page);
            allowAnnotate_ = new QCheckBox(TR("允许批注"), page);
            allowPrint_->setChecked(true);
            allowCopy_->setChecked(true);
            auto* permRow = new QHBoxLayout();
            permRow->addWidget(allowPrint_);
            permRow->addWidget(allowCopy_);
            permRow->addWidget(allowModify_);
            permRow->addWidget(allowAnnotate_);
            form->addRow(TR("权限"), permRow);

            // 解密用不到所有者密码和权限位，切模式时直接灰掉
            connect(securityMode_, &QComboBox::currentIndexChanged, this, [this] {
                const bool enc = securityMode_->currentData().toBool();
                ownerPassword_->setEnabled(enc);
                allowPrint_->setEnabled(enc);
                allowCopy_->setEnabled(enc);
                allowModify_->setEnabled(enc);
                allowAnnotate_->setEnabled(enc);
            });
            break;
        }
        case BatchOp::Compress: {
            form->addRow(new QLabel(TR("做三件事：字体子集化、流压缩、垃圾对象回收。"), page));
            break;
        }
        case BatchOp::Watermark: {
            wmMode_ = new QComboBox(page);
            wmMode_->setObjectName(QStringLiteral("batchWatermarkMode"));
            wmMode_->addItem(TR("文字水印"), false);
            wmMode_->addItem(TR("图片水印"), true);
            form->addRow(TR("模式"), wmMode_);

            wmText_ = new QLineEdit(TR("内部资料"), page);
            wmText_->setObjectName(QStringLiteral("batchWatermarkText"));
            form->addRow(TR("文字"), wmText_);

            wmPosition_ = new QComboBox(page);
            wmPosition_->setObjectName(QStringLiteral("batchWatermarkPosition"));
            wmPosition_->addItem(TR("左上角"), QStringLiteral("top-left"));
            wmPosition_->addItem(TR("顶部居中"), QStringLiteral("top-center"));
            wmPosition_->addItem(TR("右上角"), QStringLiteral("top-right"));
            wmPosition_->addItem(TR("左侧居中"), QStringLiteral("middle-left"));
            wmPosition_->addItem(TR("页面正中"), QStringLiteral("center"));
            wmPosition_->addItem(TR("右侧居中"), QStringLiteral("middle-right"));
            wmPosition_->addItem(TR("左下角"), QStringLiteral("bottom-left"));
            wmPosition_->addItem(TR("底部居中（页脚）"), QStringLiteral("bottom-center"));
            wmPosition_->addItem(TR("右下角"), QStringLiteral("bottom-right"));
            wmPosition_->addItem(TR("居中斜放 45°"), QStringLiteral("center-diagonal"));
            wmPosition_->addItem(TR("水平平铺（铺满整页）"), QStringLiteral("tile-horizontal"));
            wmPosition_->addItem(TR("斜向 45° 铺满（经典机密）"), QStringLiteral("tile-diagonal"));
            form->addRow(TR("位置"), wmPosition_);

            auto* imageRow = new QHBoxLayout();
            wmImage_ = new QLineEdit(page);
            wmImage_->setObjectName(QStringLiteral("batchWatermarkImage"));
            wmImage_->setPlaceholderText(TR("PNG / JPG，透明底 PNG 效果最好"));
            auto* imageBrowse = new QPushButton(TR("浏览…"), page);
            imageRow->addWidget(wmImage_, 1);
            imageRow->addWidget(imageBrowse);
            form->addRow(TR("图片"), imageRow);
            connect(imageBrowse, &QPushButton::clicked, this, &BatchDialog::pickWatermarkImage);

            wmScale_ = new QDoubleSpinBox(page);
            wmScale_->setObjectName(QStringLiteral("batchWatermarkScale"));
            wmScale_->setRange(0.05, 1.0);
            wmScale_->setSingleStep(0.05);
            wmScale_->setDecimals(2);
            wmScale_->setValue(0.30);
            form->addRow(TR("宽度占页宽"), wmScale_);

            wmTile_ = new QCheckBox(TR("网格平铺（间距 = 水印宽的 1.6 倍）"), page);
            wmTile_->setObjectName(QStringLiteral("batchWatermarkTile"));
            form->addRow(QString(), wmTile_);

            wmOpacity_ = new QDoubleSpinBox(page);
            wmOpacity_->setObjectName(QStringLiteral("batchWatermarkOpacity"));
            wmOpacity_->setRange(0.02, 1.0);
            wmOpacity_->setSingleStep(0.05);
            wmOpacity_->setDecimals(2);
            wmOpacity_->setValue(0.15);
            form->addRow(TR("不透明度"), wmOpacity_);

            // 文字/图片两组控件按模式启用，不隐藏（窗口高度稳定，参数一眼全见）
            const auto syncMode = [this] {
                const bool image = wmMode_->currentData().toBool();
                wmText_->setEnabled(!image);
                wmPosition_->setEnabled(!image);
                wmImage_->setEnabled(image);
                wmScale_->setEnabled(image);
                wmTile_->setEnabled(image);
            };
            connect(wmMode_, &QComboBox::currentIndexChanged, this, syncMode);
            syncMode();
            break;
        }
        case BatchOp::Ocr: {
            auto* engineLabel =
                new QLabel(QString::fromStdString(pw::ocrCapabilityReport()), page);
            engineLabel->setWordWrap(true);
            form->addRow(engineLabel);

            ocrLang_ = new QComboBox(page);
            ocrLang_->setObjectName(QStringLiteral("batchOcrLanguage"));
            ocrLang_->addItem(TR("自动（跟随系统默认）"), QString());
            if (auto engine = pw::makeOcrEngine()) {
                for (const std::string& lang : engine->languages()) {
                    ocrLang_->addItem(QString::fromStdString(lang), QString::fromStdString(lang));
                }
            }
            for (int i = 0; i < ocrLang_->count(); ++i) {
                if (ocrLang_->itemData(i).toString().startsWith(QStringLiteral("zh"))) {
                    ocrLang_->setCurrentIndex(i);
                    break;
                }
            }
            form->addRow(TR("识别语言："), ocrLang_);

            ocrPdf_ = new QCheckBox(TR("输出可检索 PDF（不可见文字层）"), page);
            ocrPdf_->setObjectName(QStringLiteral("batchOcrPdf"));
            ocrPdf_->setChecked(true);
            form->addRow(QString(), ocrPdf_);
            ocrTxt_ = new QCheckBox(TR("导出纯文本（.txt，UTF-8）"), page);
            ocrTxt_->setObjectName(QStringLiteral("batchOcrTxt"));
            ocrTxt_->setChecked(true);
            form->addRow(QString(), ocrTxt_);
            break;
        }
    }
    return page;
}

QStringList BatchDialog::files() const {
    QStringList out;
    for (int i = 0; i < list_->count(); ++i) {
        out << list_->item(i)->data(Qt::UserRole).toString();
    }
    return out;
}

void BatchDialog::addFiles() {
    const QStringList known = files();
    const QString startDir = known.isEmpty() ? QString() : QFileInfo(known.first()).absolutePath();
    const QStringList paths = QFileDialog::getOpenFileNames(
        this, TR("添加文件…"), startDir, QStringLiteral("PDF (*.pdf)"));
    for (const QString& path : paths) {
        if (known.contains(path)) continue;
        auto* item = new QListWidgetItem(QFileInfo(path).fileName(), list_);
        item->setData(Qt::UserRole, path);
        item->setToolTip(path);
    }
}

void BatchDialog::removeSelected() {
    qDeleteAll(list_->selectedItems());
}

void BatchDialog::clearFiles() {
    list_->clear();
}

void BatchDialog::pickOutputDir() {
    const QString dir = QFileDialog::getExistingDirectory(this, TR("输出目录"), outDir_->text());
    if (!dir.isEmpty()) outDir_->setText(dir);
}

void BatchDialog::pickWatermarkImage() {
    const QString file = QFileDialog::getOpenFileName(
        this, TR("选择水印图片"), QString(),
        TR("图片 (*.png *.jpg *.jpeg *.bmp *.tif *.tiff);;所有文件 (*)"));
    if (!file.isEmpty()) wmImage_->setText(file);
}

void BatchDialog::setBusy(bool busy) {
    busy_ = busy;
    list_->setEnabled(!busy);
    listButtons_->setEnabled(!busy);
    paramsArea_->setEnabled(!busy);
    outDir_->setEnabled(!busy);
    srcPassword_->setEnabled(!busy);
    startBtn_->setEnabled(!busy);
    cancelBtn_->setEnabled(busy);
}

void BatchDialog::reject() {
    if (busy_) {
        statusLabel_->setText(TR("任务进行中：先点“取消”或等全部完成再关闭窗口"));
        return;
    }
    QDialog::reject();
}

void BatchDialog::cancelBatch() {
    cancel_.store(true);
    cancelBtn_->setEnabled(false);
    statusLabel_->setText(TR("正在取消…（已开始的任务会跑完）"));
}

void BatchDialog::startBatch() {
    if (busy_) return;

    const QStringList inputs = files();
    if (inputs.isEmpty()) {
        QMessageBox::information(this, windowTitle(), TR("没有可处理的 PDF：先添加文件"));
        return;
    }
    const QString outDir = outDir_->text().trimmed();
    if (outDir.isEmpty()) {
        QMessageBox::information(this, windowTitle(), TR("请先选择输出目录"));
        return;
    }
    if (!QDir().mkpath(outDir)) {
        QMessageBox::warning(this, windowTitle(), TR("输出目录创建失败: ") + outDir);
        return;
    }

    BatchParams p;
    p.op = op_;
    p.outDir = outDir;
    p.srcPwd = srcPassword_->text().toUtf8().toStdString();

    // ---- 各操作的参数校验（一次校验，套用到每个文件）----
    switch (op_) {
        case BatchOp::Split:
            p.pagesPerFile = pagesPerFile_->value();
            break;
        case BatchOp::Extract: {
            auto pages = pw::parsePageSpec(pageSpec_->text().trimmed().toUtf8().toStdString(), 0);
            if (!pages.ok()) {
                QMessageBox::warning(this, windowTitle(),
                                     TR("页码范围无效：")
                                         + QString::fromStdString(pages.status().message()));
                return;
            }
            p.pages = pages.value();
            break;
        }
        case BatchOp::Metadata:
            p.meta.title = toU8(metaTitle_->text().trimmed());
            p.meta.author = toU8(metaAuthor_->text().trimmed());
            p.meta.subject = toU8(metaSubject_->text().trimmed());
            p.meta.keywords = toU8(metaKeywords_->text().trimmed());
            p.metaKeepEmpty = metaKeepEmpty_->isChecked();
            break;
        case BatchOp::Security: {
            p.encryptMode = securityMode_->currentData().toBool();
            p.userPwd = userPassword_->text().toUtf8().toStdString();
            p.ownerPwd = ownerPassword_->text().toUtf8().toStdString();
            if (p.encryptMode) {
                if (p.userPwd.empty() && p.ownerPwd.empty()) {
                    QMessageBox::information(this, windowTitle(),
                                             TR("至少填一个打开密码或所有者密码"));
                    return;
                }
                int perms = 0;
                if (allowPrint_->isChecked()) perms |= kPermPrint | (1 << 11);
                if (allowCopy_->isChecked()) perms |= kPermCopy;
                if (allowModify_->isChecked()) perms |= kPermModify | (1 << 8) | (1 << 10);
                if (allowAnnotate_->isChecked()) perms |= kPermAnnotate;
                p.perms = perms;
            } else if (p.userPwd.empty()) {
                QMessageBox::information(this, windowTitle(),
                                         TR("先在“打开密码”里填该 PDF 当前的密码"));
                return;
            }
            break;
        }
        case BatchOp::Compress:
            break;
        case BatchOp::Watermark: {
            p.wmImage = wmMode_->currentData().toBool();
            p.wmOpacity = static_cast<float>(wmOpacity_->value());
            if (p.wmImage) {
                p.wmImagePath = wmImage_->text().trimmed();
                p.wmScale = static_cast<float>(wmScale_->value());
                p.wmTile = wmTile_->isChecked();
                if (p.wmImagePath.isEmpty()) {
                    QMessageBox::information(this, windowTitle(),
                                             TR("先选一张水印图片（PNG 透明底效果最好）"));
                    return;
                }
                if (!QFileInfo::exists(p.wmImagePath)) {
                    QMessageBox::warning(this, windowTitle(),
                                         TR("找不到这张图片：\n%1").arg(p.wmImagePath));
                    return;
                }
            } else {
                p.wmText = toU8(wmText_->text().trimmed());
                p.wmPosition = wmPosition_->currentData().toString().toStdString();
                if (p.wmText.empty()) {
                    QMessageBox::information(this, windowTitle(), TR("先填要打的水印文字"));
                    return;
                }
            }
            break;
        }
        case BatchOp::Ocr:
            p.ocrPdf = ocrPdf_->isChecked();
            p.ocrTxt = ocrTxt_->isChecked();
            p.ocrLang = ocrLang_->currentData().toString().toUtf8().toStdString();
            if (!p.ocrPdf && !p.ocrTxt) {
                QMessageBox::information(this, windowTitle(),
                                         TR("至少勾选“可检索 PDF”或“纯文本”其中一项。"));
                return;
            }
            break;
    }

    // ---- 起线程池：一线程一文件，上限 = min(文件数, floor(逻辑核数 × 1.3)) ----
    total_ = static_cast<int>(inputs.size());
    finished_ = okCount_ = failCount_ = skipCount_ = 0;
    cancel_.store(false);
    const int cap = qMax(1, static_cast<int>(QThread::idealThreadCount() * 1.3));
    pool_.setMaxThreadCount(qMin(cap, total_));

    setBusy(true);
    progress_->setRange(0, total_);
    progress_->setValue(0);
    statusLabel_->setText(TR("处理中… %1/%2").arg(0).arg(total_));
    logView_->clear();
    emit logMessage(TR("批量%1开始：%2 个文件，%3 线程")
                        .arg(opName(op_))
                        .arg(total_)
                        .arg(pool_.maxThreadCount()));

    auto backend = backend_;  // shared_ptr 拷进任务，窗口销毁兜底见析构 waitForDone
    for (const QString& file : inputs) {
        pool_.start([this, backend, p, file] {
            if (cancel_.load()) {
                const QString name = QFileInfo(file).fileName();
                QMetaObject::invokeMethod(
                    this, [this, name] { onFileFinished(2, name); }, Qt::QueuedConnection);
                return;
            }
            const BatchResult r = processOne(backend, p, file);
            QMetaObject::invokeMethod(
                this, [this, state = r.state, detail = r.detail] { onFileFinished(state, detail); },
                Qt::QueuedConnection);
        });
    }
}

void BatchDialog::onFileFinished(int state, const QString& detail) {
    ++finished_;
    if (state == 0) ++okCount_;
    else if (state == 1) ++failCount_;
    else ++skipCount_;

    const QString tag = state == 0 ? TR("成功") : state == 1 ? TR("失败") : TR("跳过");
    const QString line = QStringLiteral("[%1/%2] %3: %4").arg(finished_).arg(total_).arg(tag).arg(detail);
    logView_->appendPlainText(line);
    emit logMessage(line);
    progress_->setValue(finished_);

    if (finished_ < total_) {
        statusLabel_->setText(TR("处理中… %1/%2").arg(finished_).arg(total_));
        return;
    }
    setBusy(false);
    const QString summary = cancel_.load()
                                ? TR("已取消：成功 %1，失败 %2，跳过 %3")
                                : TR("全部完成：成功 %1，失败 %2，跳过 %3");
    statusLabel_->setText(summary.arg(okCount_).arg(failCount_).arg(skipCount_));
    emit logMessage(TR("批量%1结束：%2").arg(opName(op_)).arg(statusLabel_->text()));
}