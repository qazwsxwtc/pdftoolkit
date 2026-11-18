// 界面冒烟测试：不需要真实屏幕，验证
//   1) 主窗口的菜单/停靠面板/工具栏能被真实构造
//   2) 各对话框（转换 / 合并 / 抽取 / 页面编辑 / 元数据 / 加解密 / 压缩 / 水印 / 图片转 PDF）
//      能被构造且校验逻辑正常
//   3) 任务队列带密码的转换真能跑完（可选，传密码参数才跑）
//   4) 把主窗口和各对话框 grab() 成 PNG，供人眼核查布局
// 只在 PDFWORD_BUILD_UI_SMOKE=ON 时编进来。
#include <QTableWidget>
#include "I18nUi.h"
#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDockWidget>
#include <QDoubleSpinBox>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFileInfo>
#include <QRectF>
#include <QGuiApplication>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QMimeData>
#include <QMouseEvent>
#include <QRubberBand>
#include <QPixmap>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QScreen>
#include <QScrollArea>
#include <QScrollBar>
#include <QSettings>
#include <QSpinBox>
#include <QSplitter>
#include <QStatusBar>
#include <QFile>
#include <QMessageBox>
#include <QTabWidget>
#include <QTreeWidget>
#include <QTimer>
#include <QToolBar>
#include <QToolButton>
#include <QUrl>
#include <QWheelEvent>
#include <cstdio>
#include <QDir>
#include <filesystem>
#include <functional>
#include <functional>
#include <memory>

#include "CompressDialog.h"
#include "ConvertDialog.h"
#include "DropListWidget.h"
#include "ExtractDialog.h"
#include "ImageToPdfDialog.h"
#include "EditDialogs.h"
#include "OcrDialog.h"
#include "MainWindow.h"
#include "MergeDialog.h"
#include "MetadataDialog.h"
#include "PageEditorDialog.h"
#include "PdfViewWidget.h"
#include "SecurityDialog.h"
#include "WatermarkDialog.h"
#include "pdfword/converter.hpp"
#include "pdfword/job.hpp"
#include "pdfword/path_util.hpp"

namespace {

int failures = 0;

void check(bool cond, const QString& what) {
    // 描述本身也走当前语言，所以中英两套界面下打出来的日志都能看
    std::printf("[%s] %s\n", cond ? " OK " : "FAIL", what.toUtf8().constData());
    if (!cond) ++failures;
}

}  // namespace

int main(int argc, char** argv) {
    QApplication app(argc, argv);

    // 视图状态会写到 QSettings 的 INI 里；冒烟测试把它指到临时目录，
    // 免得覆盖用户真实布局，也让"重开一次"能拿到刚写下的状态
    const QString settingsDir = QDir::cleanPath(
        QDir::temp().filePath(QStringLiteral("pdfword-ui-smoke-settings")));
    QDir(settingsDir).removeRecursively();
    QDir().mkpath(settingsDir);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settingsDir);
    // 冒烟里会 close() 主窗口再造第二个来验证持久化，别让 Qt 因此直接退出进程
    app.setQuitOnLastWindowClosed(false);

    pw::Config config = pw::loadConfig(pw::fsPath("pdfword.ini"));
    auto converter = std::make_shared<pw::Converter>(config);
    auto jobs = std::make_shared<pw::JobQueue>(converter);

    const QString sample = argc > 1 ? QString::fromLocal8Bit(argv[1]) : QString();
    const QStringList samples = sample.isEmpty() ? QStringList() : QStringList{sample};
    const QString password = argc > 2 ? QString::fromLocal8Bit(argv[2]) : QString();
    const QString dst = argc > 3 ? QString::fromLocal8Bit(argv[3]) : QString();
    const QString shotDir = argc > 4 ? QString::fromLocal8Bit(argv[4]) : QStringLiteral("ui-shots");

    // ---- 主窗口：菜单栏五项、停靠面板、工具栏、状态栏 ----
    MainWindow window(converter, jobs);
    window.show();

    check(window.menuBar()->actions().size() == 7,
          TR("主窗口有 7 个顶级菜单（文件/转换/PDF 工具/编辑与批注/视图/帮助/语言）"));
    check(window.windowTitle().startsWith(QStringLiteral("Pdf-Toolkit")), TR("窗口标题以 Pdf-Toolkit 开头"));
    check(!window.windowIcon().isNull(), TR("主窗口有 logo 图标（resources/icons.qrc 已编进二进制）"));
    check(!window.windowIcon().pixmap(32, 32).isNull(), TR("logo 能取到 32x32 的位图（任务栏/标题栏不空）"));
    // 顶部那排导航已经撤掉，全部挪到预览区右下角
    check(window.findChild<QToolBar*>() == nullptr, TR("顶部预览工具栏已移除（导航挪到预览区右下角）"));

    // 十个功能入口必须都在菜单里（原来那排按钮已撤掉，五个文档级工具各占一项）
    QStringList wanted{
        TR("开始转换…"), TR("合并 PDF…"), TR("拆分 PDF…"),
        TR("抽取页面…"), TR("页面编辑…"),
        TR("元数据…"), TR("加密 / 解密…"), TR("压缩…"),
        TR("水印…"), TR("图片转 PDF…"),
        TR("添加文件…"), TR("清空列表"),
        TR("修改文字、图片…"), TR("注释批注…"), TR("添加超链接…"),
        TR("查找替换…"), TR("表单处理…")};
    QStringList actions;
    for (auto* a : window.findChildren<QAction*>()) actions << a->text();
    QStringList missing;
    for (const auto& w : wanted) {
        if (!actions.contains(w)) missing << w;
    }
    check(missing.isEmpty(), TR("十个功能入口 + 文件操作菜单项齐全"));
    if (!missing.isEmpty()) std::printf("       缺少: %s\n", missing.join(QStringLiteral(" / ")).toLocal8Bit().constData());
    // 旧的合并式窗口已拆掉，菜单里不应再出现"文档工具"
    check(!actions.contains(TR("文档工具…")), TR("PDF 工具菜单里不再有“文档工具…”"));
    check(actions.contains(TR("关于 Pdf-Toolkit…")), TR("帮助菜单里有“关于 Pdf-Toolkit…”"));
    check(QString::fromLatin1(PDFWORD_APP_VERSION) == QStringLiteral("0.0.2"),
          TR("关于窗口写的版本号是 0.0.2"));

    // 视图菜单是两级：文件列表（靠左/靠右）、日志窗口（显示/隐藏）
    auto* fileListMenu = window.findChild<QMenu*>(QStringLiteral("menuFileList"));
    auto* logMenu = window.findChild<QMenu*>(QStringLiteral("menuLogWindow"));
    check(fileListMenu != nullptr && logMenu != nullptr,
          TR("视图菜单下有“文件列表”“日志窗口”两个子菜单"));
    if (fileListMenu != nullptr) {
        QStringList items;
        for (auto* a : fileListMenu->actions()) {
            if (!a->isSeparator()) items << a->text();
        }
        check(items.contains(TR("靠左")) && items.contains(TR("靠右")),
              TR("文件列表子菜单有“靠左”“靠右”"));
        check(items.contains(TR("显示文件列表")), TR("文件列表子菜单保留“显示文件列表”"));
    }
    if (logMenu != nullptr) {
        QStringList items;
        for (auto* a : logMenu->actions()) {
            if (!a->isSeparator()) items << a->text();
        }
        check(items.contains(TR("显示")) && items.contains(TR("隐藏")),
              TR("日志窗口子菜单有“显示”“隐藏”"));
    }

    auto* dock = window.findChild<QDockWidget*>(QStringLiteral("fileDock"));
    check(dock != nullptr, TR("待处理文件是 QDockWidget"));
    if (dock != nullptr) {
        check(dock->allowedAreas() == (Qt::LeftDockWidgetArea | Qt::RightDockWidgetArea),
              TR("停靠面板只允许左/右"));
        check(dock->findChild<QListWidget*>() != nullptr, TR("停靠面板里有文件列表"));
        auto* menuList = dock->findChild<QListWidget*>();
        check(menuList != nullptr && menuList->contextMenuPolicy() == Qt::CustomContextMenu,
              TR("待处理文件列表挂了右键菜单（删除/清空/重命名）"));
    }

    // ---- 拖放：主窗口任意位置拖入文件即入列，不支持的类型被忽略 ----
    check(window.acceptDrops(), TR("主窗口接受文件拖放"));
    if (!sample.isEmpty()) {
        QListWidget* fileList = dock != nullptr ? dock->findChild<QListWidget*>() : nullptr;
        if (fileList != nullptr) {
            const int before = fileList->count();
            QMimeData mime;
            mime.setUrls({QUrl::fromLocalFile(sample)});
            QDragEnterEvent enter(QPointF(30, 30), Qt::CopyAction, &mime, Qt::LeftButton, Qt::NoModifier);
            QApplication::sendEvent(&window, &enter);
            check(enter.isAccepted(), TR("拖入 PDF 时窗口接受拖放"));

            QDropEvent drop(QPointF(30, 30), Qt::CopyAction, &mime, Qt::LeftButton, Qt::NoModifier);
            QApplication::sendEvent(&window, &drop);
            check(fileList->count() == before + 1, TR("拖入的 PDF 进了待处理列表"));

            // 同一个文件再拖一次不应重复入列
            QDropEvent again(QPointF(30, 30), Qt::CopyAction, &mime, Qt::LeftButton, Qt::NoModifier);
            QApplication::sendEvent(&window, &again);
            check(fileList->count() == before + 1, TR("重复拖入同一文件被去重"));

            // 不支持的类型（.ini）不会被加进列表，但要在日志里说一声
            QMimeData bad;
            bad.setUrls({QUrl::fromLocalFile(QStringLiteral("C:/Windows/win.ini"))});
            const int beforeBad = fileList->count();
            QDragEnterEvent badEnter(QPointF(30, 30), Qt::CopyAction, &bad, Qt::LeftButton, Qt::NoModifier);
            QApplication::sendEvent(&window, &badEnter);
            check(badEnter.isAccepted(), TR("拖入任意本地文件时窗口进入可放置状态"));
            QDropEvent badDrop(QPointF(30, 30), Qt::CopyAction, &bad, Qt::LeftButton, Qt::NoModifier);
            QApplication::sendEvent(&window, &badDrop);
            check(fileList->count() == beforeBad, TR("不支持的扩展名没有被加进列表"));
            auto* logView = window.findChild<QPlainTextEdit*>();
            check(logView != nullptr &&
                      logView->toPlainText().contains(
                          TR("忽略 %1 个不支持的文件（只收 pdf/docx/doc/odt/rtf/txt/xlsx/pptx）")
                              .arg(1)),
                  TR("不支持的文件在日志里有提示"));

            // 右键菜单真的能弹出来：投一个 contextMenuRequested，再在事件循环里把弹窗关掉
            bool menuSeen = false;
            QTimer::singleShot(30, [&menuSeen] {
                if (auto* pop = QApplication::activePopupWidget()) {
                    menuSeen = pop->inherits("QMenu");
                    pop->close();
                }
            });
            QTimer::singleShot(400, [] {
                if (auto* pop = QApplication::activePopupWidget()) pop->close();
            });
            QMetaObject::invokeMethod(fileList, "customContextMenuRequested", Qt::DirectConnection,
                                      Q_ARG(QPoint, fileList->viewport()->rect().center()));
            check(menuSeen, TR("待处理文件列表的右键菜单真的弹出来了"));
        }
    }

    // ---- 三个新对话框：构造 + 关键控件 + 默认值 ----
    auto merge = std::make_unique<MergeDialog>(samples);
    check(merge->findChild<QListWidget*>(QStringLiteral("mergeList")) != nullptr, TR("合并窗口有文件列表"));
    auto* mergeDrop = merge->findChild<DropListWidget*>(QStringLiteral("mergeList"));
    check(mergeDrop != nullptr && mergeDrop->acceptDrops(), TR("合并窗口的列表接受拖入 PDF"));

    // 真的把第二个 PDF 投进合并列表的 viewport，确认 DropListWidget 的 dropEvent 链路通
    if (mergeDrop != nullptr && !sample.isEmpty()) {
        const QFileInfo sampleInfo(sample);
        QString second = sampleInfo.absolutePath() + QStringLiteral("/fw8ben.pdf");
        // 样本本身就叫 fw8ben.pdf 时换一个文件，否则等于把同一个文件再拖一次，
        // DropListWidget 会去重，这条断言会假失败
        if (second == sampleInfo.absoluteFilePath()) {
            second = sampleInfo.absolutePath() + QStringLiteral("/iw8ben.pdf");
        }
        if (QFileInfo::exists(second)) {
            const int before = mergeDrop->count();
            QMimeData mime;
            mime.setUrls({QUrl::fromLocalFile(second)});
            QDragEnterEvent enter(QPointF(10, 10), Qt::CopyAction, &mime, Qt::LeftButton, Qt::NoModifier);
            QApplication::sendEvent(mergeDrop->viewport(), &enter);
            QDropEvent drop(QPointF(10, 10), Qt::CopyAction, &mime, Qt::LeftButton, Qt::NoModifier);
            QApplication::sendEvent(mergeDrop->viewport(), &drop);
            check(mergeDrop->count() == before + 1, TR("把 PDF 拖进合并列表后真的多了一项"));
        }
    }
    check(merge->findChild<QLineEdit*>(QStringLiteral("mergeOutput")) != nullptr, TR("合并窗口有输出路径框"));
    if (!samples.isEmpty()) check(!merge->outputPath().isEmpty(), TR("合并默认输出路径非空"));

    auto extract = std::make_unique<ExtractDialog>(converter->pdfBackend(), samples, sample, nullptr);
    check(extract->findChild<QComboBox*>(QStringLiteral("sourceCombo")) != nullptr, TR("抽取窗口有源文件下拉"));
    if (!samples.isEmpty()) check(!extract->outputPath().isEmpty(), TR("抽取默认输出路径非空"));

    // 抽取窗口的实时校验：合法范围要算出页数，越界要当场报错。
    // ExtractDialog 自己不带密码（和真实界面一致），所以先用空密码探一次页数：
    // 读到页数才验证"合法范围"，读不到（加密件）时验证"必须拦住"。
    int extractablePages = 0;
    if (!sample.isEmpty()) {
        auto probe = converter->pdfBackend()->open(pw::fsPath(sample.toUtf8().toStdString()),
                                                   std::string());
        if (probe.ok()) extractablePages = probe.value()->info().pageCount;
    }
    auto* specEdit = extract->findChild<QLineEdit*>(QStringLiteral("specEdit"));
    auto* hint = extract->findChild<QLabel*>(QStringLiteral("hintLabel"));
    check(specEdit != nullptr && hint != nullptr, TR("抽取窗口有页码框与提示行"));
    if (specEdit != nullptr && hint != nullptr && !samples.isEmpty()) {
        if (extractablePages >= 2) {
            specEdit->setText(QStringLiteral("1-2"));
            // 文案整句是 TR("共 %1 页，将抽取 %2 页（%3）")，数字随语言不变：查数字即可
            check(hint->text().contains(QString::number(extractablePages)) &&
                      hint->text().contains(QString::number(2)),
                  TR("合法范围算对了页数（1-2 -> 2 页）"));
            check(extract->pages().size() == 2, TR("抽取窗口缓存了 2 个 0 基页码"));
        }
        specEdit->setText(QStringLiteral("1-99999"));
        check(hint->text().startsWith(TR("页码范围无效：")),
              extractablePages >= 2 ? TR("越界范围被当场拦下") : TR("读不到页数时范围被拦下"));
        check(extract->pages().empty(), TR("拦下时清空已缓存页码"));
    }

    // 混一个 .docx，验证"目标格式"切换时摘要按真实文件类型给出提示
    QStringList convertFiles = samples;
    if (!sample.isEmpty()) {
        const QFileInfo fi(sample);
        convertFiles << (fi.absolutePath() + QLatin1Char('/') + fi.completeBaseName() + QStringLiteral(".docx"));
    }
    auto convert = std::make_unique<ConvertDialog>(convertFiles);
    auto* format = convert->findChild<QComboBox*>(QStringLiteral("formatCombo"));
    check(format != nullptr && format->count() == 2, TR("目标格式下拉 2 项"));
    auto* summary = convert->findChild<QLabel*>(QStringLiteral("summaryLabel"));
    if (summary != nullptr && convertFiles.size() == 2) {
        int pdfCount = 0;
        for (const auto& f : convertFiles) {
            if (QFileInfo(f).suffix().compare(QStringLiteral("pdf"), Qt::CaseInsensitive) == 0)
                ++pdfCount;
        }
        check(summary->text().contains(
                  TR(" 转 pdf 时这 %1 个 PDF 会被跳过（它们已经是 PDF）。").arg(pdfCount)),
              TR("默认 pdf 目标提示 PDF 会被跳过"));
        format->setCurrentIndex(1);
        check(summary->text().contains(TR(" 只有 PDF 能转 docx，这 %1 个非 PDF 文件会被跳过。")
                                           .arg(convertFiles.size() - pdfCount)),
              TR("切到 docx 时摘要跟着变"));
        format->setCurrentIndex(0);
    }

    // ---- 页面编辑器 / 五个文档级工具窗口 ----
    std::unique_ptr<PageEditorDialog> editor;
    std::unique_ptr<MetadataDialog> metaDlg;
    std::unique_ptr<SecurityDialog> secDlg;
    std::unique_ptr<CompressDialog> cmpDlg;
    std::unique_ptr<WatermarkDialog> wmDlg;
    std::unique_ptr<ImageToPdfDialog> imgDlg;
    std::unique_ptr<OcrDialog> ocrDlg;
    std::unique_ptr<AnnotDialog> annotDlg;
    std::unique_ptr<LinkDialog> linkDlg;
    std::unique_ptr<FindReplaceDialog> findDlg;
    std::unique_ptr<ReplaceImageDialog> imgReplaceDlg;
    std::unique_ptr<FormDialog> formDlg;
    if (!samples.isEmpty()) {
        editor = std::make_unique<PageEditorDialog>(converter->pdfBackend(), sample, nullptr, QString(), nullptr);
        metaDlg = std::make_unique<MetadataDialog>(converter->pdfBackend(), sample, nullptr);
        secDlg = std::make_unique<SecurityDialog>(converter->pdfBackend(), sample, nullptr);
        cmpDlg = std::make_unique<CompressDialog>(converter->pdfBackend(), sample, nullptr);
        wmDlg = std::make_unique<WatermarkDialog>(converter->pdfBackend(), sample, nullptr);
        imgDlg = std::make_unique<ImageToPdfDialog>(converter->pdfBackend(),
                                                    QFileInfo(sample).absolutePath(), nullptr);
        ocrDlg = std::make_unique<OcrDialog>(converter->pdfBackend(), sample, nullptr);
        // 这几个窗口要拿到真实预览控件，框选链路才测得到（真实程序里 MainWindow 也是这么传的）
        auto* smokeViewer = window.findChild<PdfViewWidget*>();
        annotDlg = std::make_unique<AnnotDialog>(converter->pdfBackend(), sample, smokeViewer, nullptr);
        linkDlg = std::make_unique<LinkDialog>(converter->pdfBackend(), sample, smokeViewer, nullptr);
        findDlg = std::make_unique<FindReplaceDialog>(converter->pdfBackend(), sample, nullptr);
        imgReplaceDlg =
            std::make_unique<ReplaceImageDialog>(converter->pdfBackend(), sample, smokeViewer, nullptr);
        formDlg = std::make_unique<FormDialog>(converter->pdfBackend(), sample, smokeViewer, nullptr);
        check(editor != nullptr, TR("页面编辑器构造成功"));
        check(metaDlg->findChild<QLineEdit*>(QStringLiteral("metaTitle")) != nullptr, TR("元数据窗口有标题输入框"));
        check(secDlg->findChild<QLineEdit*>(QStringLiteral("userPassword")) != nullptr, TR("加解密窗口有打开密码输入框"));
        check(cmpDlg->findChild<QLabel*>() != nullptr, TR("压缩窗口显示源文件大小"));
        check(wmDlg->findChild<QLineEdit*>(QStringLiteral("watermarkText")) != nullptr, TR("水印窗口有文字输入框"));
        auto* wmPos = wmDlg->findChild<QComboBox*>(QStringLiteral("watermarkPosition"));
        check(wmPos != nullptr && wmPos->count() == 12, TR("文字水印有 12 种位置可选"));
        check(wmPos != nullptr && wmPos->itemData(0).toString() == QStringLiteral("top-left"),
              TR("位置默认左上角"));
        auto* wmTabs = wmDlg->findChild<QTabWidget*>(QStringLiteral("watermarkTabs"));
        check(wmTabs != nullptr && wmTabs->count() == 2, TR("水印窗口有文字/图片两个页签"));
        check(wmDlg->findChild<QLineEdit*>(QStringLiteral("watermarkImage")) != nullptr, TR("水印窗口有图片路径框"));
        check(wmDlg->findChild<QDoubleSpinBox*>(QStringLiteral("watermarkScale")) != nullptr, TR("水印窗口有图片宽度比例"));
        check(wmDlg->findChild<QCheckBox*>(QStringLiteral("watermarkTile")) != nullptr, TR("水印窗口有网格平铺开关"));
        check(wmDlg->findChild<QDoubleSpinBox*>(QStringLiteral("watermarkOpacity")) != nullptr, TR("两种水印共用不透明度"));
        check(imgDlg->findChild<QListWidget*>(QStringLiteral("imageList")) != nullptr, TR("图片转 PDF 窗口有图片列表"));
        auto* imgDrop = imgDlg->findChild<DropListWidget*>(QStringLiteral("imageList"));
        check(imgDrop != nullptr && imgDrop->acceptDrops(), TR("图片转 PDF 窗口的列表接受拖入图片"));
        check(ocrDlg->findChild<QComboBox*>(QStringLiteral("ocrLanguageCombo")) != nullptr,
              TR("OCR 窗口有识别语言下拉"));
        auto* ocrLang = ocrDlg->findChild<QComboBox*>(QStringLiteral("ocrLanguageCombo"));
        check(ocrLang != nullptr && ocrLang->count() >= 2,
              TR("语言下拉里有“自动”和至少一个真实语言"));
        check(ocrDlg->findChild<QCheckBox*>(QStringLiteral("ocrPdfCheck")) != nullptr &&
                  ocrDlg->findChild<QCheckBox*>(QStringLiteral("ocrTxtCheck")) != nullptr,
              TR("OCR 窗口有“可检索 PDF / 纯文本”两个开关"));
        auto* ocrPdfEdit = ocrDlg->findChild<QLineEdit*>(QStringLiteral("ocrPdfOutput"));
        check(ocrPdfEdit != nullptr && ocrPdfEdit->text().contains(TR("_可检索")),
              TR("OCR 默认输出名带 _可检索 后缀"));
        check(ocrDlg->findChild<QLabel*>(QStringLiteral("ocrEngineLabel")) != nullptr,
              TR("OCR 窗口显示引擎能力（可用语言/不可用原因）"));
        check(ocrDlg->findChild<QPushButton*>(QStringLiteral("ocrStartButton")) != nullptr,
              TR("OCR 窗口有开始识别按钮"));

        // ---- 编辑与批注：5 个窗口的关键控件都在 ----
        auto* annotKind = annotDlg->findChild<QComboBox*>(QStringLiteral("annotKind"));
        check(annotKind != nullptr && annotKind->count() == 7,
              TR("批注窗口有 7 种批注类型（高亮/下划线/删除线/波浪线/便签/文本框/手绘）"));
        check(annotDlg->findChild<QPlainTextEdit*>(QStringLiteral("annotText")) != nullptr,
              TR("批注窗口有内容输入框（便签/文本框用）"));
        check(annotDlg->findChild<QPushButton*>(QStringLiteral("annotPick")) != nullptr,
              TR("批注窗口有「在预览里框选位置」按钮"));
        check(linkDlg->findChild<QLineEdit*>(QStringLiteral("linkUri")) != nullptr,
              TR("超链接窗口有目标地址框"));
        check(linkDlg->findChild<QSpinBox*>(QStringLiteral("linkPage")) != nullptr,
              TR("超链接窗口有页码框（页内跳转用）"));
        check(findDlg->findChild<QLineEdit*>(QStringLiteral("findText")) != nullptr &&
                  findDlg->findChild<QLineEdit*>(QStringLiteral("replaceText")) != nullptr,
              TR("查找替换窗口有查找框和替换框"));
        check(findDlg->findChild<QPushButton*>(QStringLiteral("findPreview")) != nullptr &&
                  findDlg->findChild<QPushButton*>(QStringLiteral("replaceApply")) != nullptr,
              TR("查找替换窗口有「查找」和「替换并另存为」"));
        check(imgReplaceDlg->findChild<QLineEdit*>(QStringLiteral("replaceImage")) != nullptr,
              TR("替换图片窗口有新图片路径框"));
        auto* formTabs = formDlg->findChild<QTabWidget*>(QStringLiteral("formTabs"));
        check(formTabs != nullptr && formTabs->count() == 3,
              TR("表单窗口有三个页签（填写/制作/提取）"));
        check(formDlg->findChild<QTableWidget*>(QStringLiteral("formFillTable")) != nullptr,
              TR("表单填写页有字段表格"));
        check(formDlg->findChild<QListWidget*>(QStringLiteral("formSpecList")) != nullptr,
              TR("表单制作页有字段清单"));
        check(formDlg->findChild<QTableWidget*>(QStringLiteral("formExtractTable")) != nullptr,
              TR("表单提取页有数据表格"));
        // 预览框选模式可以开也可以关
        check(window.findChild<PdfViewWidget*>() != nullptr, TR("主窗口里有预览控件"));
        {
            auto* v = window.findChild<PdfViewWidget*>();
            if (v != nullptr) {
                check(!v->isSelectMode(), TR("默认不在框选模式（拖框不干扰正常浏览）"));
                v->setSelectMode(true);
                check(v->isSelectMode(), TR("能在预览里打开框选模式"));
                v->setSelectMode(false);
                check(!v->isSelectMode(), TR("框选模式能关掉"));
            }
        }
    }

    // ---- 任务队列带密码：加密 PDF -> docx，验证 password 真透传到 ConvertOptions ----
    if (!password.isEmpty()) {
        const std::filesystem::path out(dst.toStdString());
        jobs->enqueue(pw::fsPath(sample.toUtf8().toStdString()), out, "docx", password.toStdString());
        jobs->waitAll();
        auto snap = jobs->snapshot();
        check(!snap.empty(), TR("任务已入队"));
        if (!snap.empty()) {
            check(snap.back().state == pw::JobState::Done, TR("带密码的转换任务执行完成"));
            check(std::filesystem::exists(out), TR("带密码的转换产物已落盘"));
        }
    }

    // ---- 把界面渲染成 PNG：不需要真实屏幕也能看布局 ----
    merge->show();
    extract->show();
    convert->show();
    if (editor) editor->show();
    if (metaDlg) metaDlg->show();
    if (secDlg) secDlg->show();
    if (cmpDlg) cmpDlg->show();
    if (wmDlg) wmDlg->show();
    if (imgDlg) imgDlg->show();
    if (ocrDlg) ocrDlg->show();
    if (annotDlg) annotDlg->show();
    if (linkDlg) linkDlg->show();
    if (findDlg) findDlg->show();
    if (imgReplaceDlg) imgReplaceDlg->show();
    if (formDlg) formDlg->show();

    QTimer::singleShot(1000, &app, [&] {
        std::filesystem::create_directories(shotDir.toStdString());
        const std::filesystem::path dir(shotDir.toStdString());
        auto shoot = [&dir](QWidget* w, const char* name) {
            QPixmap pm = w->grab();
            const auto path = dir / (std::string(name) + ".png");
            const bool ok = pm.save(QString::fromStdString(path.string()), "PNG");
            std::printf("[%s] 截图 %s (%dx%d)\n", ok ? " OK " : "FAIL", path.string().c_str(),
                        pm.width(), pm.height());
            if (!ok) ++failures;
        };
        shoot(&window, "main-window");

        // 「帮助 → 关于」：模态 exec，只能让定时器抓一张再替用户点掉
        {
            std::function<void(int)> closeAbout = [&dir, &closeAbout](int left) {
                QTimer::singleShot(80, [&dir, &closeAbout, left]() {
                    for (QWidget* w : QApplication::topLevelWidgets()) {
                        auto* box = qobject_cast<QMessageBox*>(w);
                        if (box == nullptr || !box->isVisible()) continue;
                        const auto path = dir / "about-dialog.png";
                        box->grab().save(QString::fromStdString(path.string()), "PNG");
                        std::printf("[%s] \u622a\u56fe %s\n", " OK ",
                                    path.string().c_str());
                        box->accept();
                        return;
                    }
                    if (left > 1) closeAbout(left - 1);
                });
            };
            closeAbout(20);
            QMetaObject::invokeMethod(&window, "showAboutDialog");
        }

        // 居中：窗口中心应落在主屏可用区域的中心附近
        if (QScreen* screen = QGuiApplication::primaryScreen()) {
            const QRect avail = screen->availableGeometry();
            const QPoint expect(avail.x() + (avail.width() - window.width()) / 2,
                                avail.y() + (avail.height() - window.height()) / 2);
            const QPoint actual = window.pos();
            const int dx = qAbs(actual.x() - expect.x());
            const int dy = qAbs(actual.y() - expect.y());
            std::printf("       屏幕可用区 %dx%d, 期望 (%d,%d), 实际 (%d,%d)\n", avail.width(),
                        avail.height(), expect.x(), expect.y(), actual.x(), actual.y());
            check(dx <= 12 && dy <= 12, TR("窗口启动位置居中（±12px）"));
        }

        // 用菜单把文件面板切到右侧，验证停靠位置真的变了（两级菜单里的动作，按 objectName 找）
        QAction* toRight = window.findChild<QAction*>(QStringLiteral("dockRightAction"));
        if (toRight != nullptr) {
            toRight->trigger();
            QApplication::processEvents();
            if (dock != nullptr) {
                check(window.dockWidgetArea(dock) == Qt::RightDockWidgetArea,
                      TR("菜单选“靠右”后停靠区变成右侧"));
            }
            shoot(&window, "main-window-right");

            // 再切回左侧，确认两侧都能回去
            QAction* toLeft = window.findChild<QAction*>(QStringLiteral("dockLeftAction"));
            if (toLeft != nullptr) {
                toLeft->trigger();
                QApplication::processEvents();
                if (dock != nullptr) {
                    check(window.dockWidgetArea(dock) == Qt::LeftDockWidgetArea,
                          TR("菜单选“靠左”后停靠区回到左侧"));
                }
            }
        }

        // 日志窗口显隐：隐藏后日志在 splitter 里高度归零、预览区铺满；显示后回来
        auto* logBox = window.findChild<QPlainTextEdit*>(QStringLiteral("logView"));
        auto* logSplitter = window.findChild<QSplitter*>(QStringLiteral("centralSplitter"));
        auto* hideAct = window.findChild<QAction*>(QStringLiteral("logHideAction"));
        auto* showAct = window.findChild<QAction*>(QStringLiteral("logShowAction"));
        check(logBox != nullptr && logSplitter != nullptr && hideAct != nullptr && showAct != nullptr,
              TR("日志窗口是 splitter 的下半区，显隐动作都在"));
        if (logBox != nullptr && logSplitter != nullptr && hideAct != nullptr && showAct != nullptr) {
            check(!logBox->isHidden() && logSplitter->sizes().at(1) > 0, TR("默认显示日志窗口"));
            hideAct->trigger();
            QApplication::processEvents();
            check(logBox->isHidden() && logSplitter->sizes().at(1) == 0,
                  TR("点“隐藏”后日志收起、预览区铺满"));
            check(showAct->isChecked() == false && hideAct->isChecked(), TR("“隐藏”勾选状态跟着切"));
            shoot(&window, "main-window-log-hidden");
            showAct->trigger();
            QApplication::processEvents();
            check(!logBox->isHidden() && logSplitter->sizes().at(1) > 0,
                  TR("点“显示”后日志窗口回到原高度"));
        }

        // 视图-文件列表：显示 / 隐藏两项，隐藏后预览区变宽
        auto* hideFilesAct = window.findChild<QAction*>(QStringLiteral("fileHideAction"));
        auto* showFilesAct = window.findChild<QAction*>(QStringLiteral("fileShowAction"));
        check(hideFilesAct != nullptr && showFilesAct != nullptr,
              TR("视图-文件列表里有“显示文件列表”“隐藏文件列表”"));
        if (dock != nullptr && hideFilesAct != nullptr && showFilesAct != nullptr) {
            hideFilesAct->trigger();
            QApplication::processEvents();
            check(dock->isHidden(), TR("点“隐藏文件列表”后面板收起"));
            showFilesAct->trigger();
            QApplication::processEvents();
            check(!dock->isHidden() && showFilesAct->isChecked(), TR("点“显示文件列表”后面板回来"));
        }

        // 缩放与滚动条：放大后右侧/底部滚动条可拉，缩小后消失，适应宽度贴合视口
        auto* view = window.findChild<PdfViewWidget*>();
        auto* scroll = window.findChild<QScrollArea*>(QStringLiteral("pdfScroll"));
        check(view != nullptr && scroll != nullptr, TR("预览区是 QScrollArea + 可滚动画布"));
        if (view != nullptr && scroll != nullptr && view->document() != nullptr) {
            QScrollBar* vs = scroll->verticalScrollBar();
            QScrollBar* hs = scroll->horizontalScrollBar();

            // 连续滚动：所有页竖着排成一条，拖着滚条/滚轮能一直拉到最后一页
            auto* canvasWidget = window.findChild<QWidget*>(QStringLiteral("pdfCanvas"));
            check(view->pageItemCount() == view->pageCount(),
                  TR("画布把全部页面竖排成一条（页数 = 文档页数）"));
            if (canvasWidget != nullptr) {
                const auto pageItems = canvasWidget->findChildren<QLabel*>(QStringLiteral("pageItem"));
                check(int(pageItems.size()) == view->pageCount(),
                      TR("画布里每一页都有一个页面控件（9 页 = 9 个）"));
                check(canvasWidget->height() > scroll->viewport()->height() || vs->maximum() > 0,
                      TR("整叠页面比视口高，滚动条可以一直往下拉"));
                vs->setValue(vs->maximum());
                QApplication::processEvents();
                check(view->currentPage() == view->pageCount(),
                      TR("滚到最底部时当前页自动变成最后一页"));
                if (int(pageItems.size()) >= 2) {
                    // 停在第一页与第二页的接缝处，截图能看出"页与页是接着的、往下拉就是下一页"
                    const QRect secondInCanvas(
                        QPoint(pageItems.at(1)->mapTo(canvasWidget, QPoint(0, 0))),
                        pageItems.at(1)->size());
                    vs->setValue(qMax(0, secondInCanvas.top() - 60));
                    QApplication::processEvents();
                    shoot(&window, "main-window-scroll");
                }
                vs->setValue(0);
                QApplication::processEvents();
                check(view->currentPage() == 1, TR("滚回最顶部时当前页是第 1 页"));

                // ---- 框选链路：点「框选」→ 窗口先藏起来 → 预览上拖框 → 坐标回填 → 窗口回来 ----
                // 这条链之前只有"开关能切"的断言，用户实际点的时候模态窗口把预览挡住了。
                if (annotDlg != nullptr && !pageItems.isEmpty()) {
                    auto* pickBtn = annotDlg->findChild<QPushButton*>(QStringLiteral("annotPick"));
                    auto* ax0 = annotDlg->findChild<QDoubleSpinBox*>(QStringLiteral("annotX0"));
                    check(pickBtn != nullptr && ax0 != nullptr, TR("批注窗口有框选按钮与四个坐标框"));
                    if (pickBtn != nullptr && ax0 != nullptr) {
                        annotDlg->show();
                        QApplication::processEvents();
                        pickBtn->click();
                        QApplication::processEvents();
                        check(!annotDlg->isVisible(),
                              TR("点「在预览里框选」后窗口自动隐藏（模态窗口不再挡住预览）"));
                        check(view->isSelectMode(), TR("点框选后预览进入框选模式"));

                        bool gotSelection = false;
                        int gotPage = 0;
                        QRectF gotRect;
                        QObject::connect(view, &PdfViewWidget::rectSelected, view,
                                         [&](int page, QRectF rect) {
                                             gotSelection = true;
                                             gotPage = page;
                                             gotRect = rect;
                                         });
                        QLabel* page0 = pageItems.first();
                        const QPoint from(40, 50);
                        const QPoint to(220, 200);
                        QMouseEvent press(QEvent::MouseButtonPress, QPointF(from),
                                          page0->mapToGlobal(from), Qt::LeftButton, Qt::LeftButton,
                                          Qt::NoModifier);
                        QApplication::sendEvent(page0, &press);
                        QMouseEvent move(QEvent::MouseMove, QPointF(to), page0->mapToGlobal(to),
                                         Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
                        QApplication::sendEvent(page0, &move);
                        QMouseEvent release(QEvent::MouseButtonRelease, QPointF(to),
                                            page0->mapToGlobal(to), Qt::LeftButton, Qt::NoButton,
                                            Qt::NoModifier);
                        QApplication::sendEvent(page0, &release);
                        QApplication::processEvents();

                        const double z = view->zoom() > 1e-6 ? view->zoom() : 1.0;
                        check(gotSelection, TR("在预览上按住左键拖一个框，会发出 rectSelected"));
                        if (gotSelection) {
                            check(gotPage == 1 && qAbs(gotRect.width() - 180.0 / z) < 6.0 &&
                                      qAbs(gotRect.height() - 150.0 / z) < 6.0,
                                  TR("框选结果是第 1 页 + 显示空间 pt 矩形（像素 ÷ 缩放）"));
                        }
                        check(annotDlg->isVisible(), TR("拖完框后窗口自己弹回来"));
                        check(ax0->value() > 1.0 && ax0->value() < gotRect.right() + 1.0,
                              TR("框选坐标已回填到批注窗口的 x0 框"));
                        check(!view->isSelectMode(), TR("回填后自动退出框选模式（不会误拖下一页）"));
                    }
                }

                // ---- 窗口是非模态的：弹着的时候主窗口和预览都还能用 ----
                // 之前这些窗口走 exec()，模态把主窗口和预览一起框死，框选点了也没反应。
                if (annotDlg != nullptr) {
                    annotDlg->show();
                    QApplication::processEvents();
                    check(!annotDlg->isModal() && QApplication::activeModalWidget() == nullptr &&
                              window.isEnabled(),
                          TR("编辑窗口是非模态的（弹着时没有模态窗口在挡，主窗口仍可用）"));
                    annotDlg->hide();
                }

                // ---- 从菜单真点一次「注释批注」：真实入口开出来的窗口必须是非模态的 ----
                // 上面测的是冒烟自己 new 的例子；这里走 MainWindow 的槽 + presentEditDialog，
                // 和用户点菜单完全同一条路径。
                {
                    QAction* annotAct = nullptr;
                    for (auto* act : window.menuBar()->findChildren<QAction*>()) {
                        if (act->text().contains(TR("注释批注…"))) {
                            annotAct = act;
                            break;
                        }
                    }
                    check(annotAct != nullptr, TR("菜单里有「注释批注」这一项"));
                    if (annotAct != nullptr) {
                        annotAct->trigger();
                        QApplication::processEvents();
                        auto* opened = window.findChild<AnnotDialog*>();
                        check(opened != nullptr && opened->isVisible(),
                              TR("点菜单后批注窗口直接显示出来"));
                        check(opened != nullptr && !opened->isModal() &&
                                  QApplication::activeModalWidget() == nullptr && window.isEnabled(),
                              TR("菜单开出来的批注窗口是非模态的（主窗口没被禁用、没有模态窗口在挡）"));
                        if (opened != nullptr) opened->close();
                        QApplication::processEvents();
                    }
                }
            }

            view->setZoom(3.0);
            QApplication::processEvents();
            check(vs->maximum() > 0 && hs->maximum() > 0,
                  TR("放大到 300% 后右侧/底部都出现可拉动的滚动条"));
            vs->setValue(vs->maximum() / 2);
            check(vs->value() > 0 && vs->value() <= vs->maximum(),
                  TR("竖直滚动条能拖到页面中部"));
            shoot(&window, "main-window-zoom");
            view->setZoom(0.5);
            QApplication::processEvents();
            check(hs->maximum() == 0, TR("缩小到 50% 后横向不再需要滚动、整页宽度装得下"));
            view->fitWidth();
            QApplication::processEvents();
            check(hs->maximum() == 0 && scroll->widget()->width() <= scroll->viewport()->width() + 2,
                  TR("“适应宽度”后横向不再需要滚动、页面铺满视口"));
            view->fitPage();
            QApplication::processEvents();
            const auto fittedPages =
                canvasWidget->findChildren<QLabel*>(QStringLiteral("pageItem"));
            check(hs->maximum() == 0 && !fittedPages.isEmpty() &&
                      fittedPages.first()->height() <= scroll->viewport()->height(),
                  TR("“适应窗口”后横向不滚、单页整页高度装进视口（后续页继续往下滚）"));
            const double beforeWheel = view->zoom();
            QWheelEvent wheel(QPointF(20, 20), QPointF(20, 20), QPoint(), QPoint(0, 120),
                              Qt::NoButton, Qt::ControlModifier, Qt::NoScrollPhase, false);
            QApplication::sendEvent(scroll->viewport(), &wheel);
            QApplication::processEvents();
            check(view->zoom() > beforeWheel, TR("Ctrl + 滚轮在预览区里放大一档"));

            // 导航条挂在状态栏上：和右下角 0% 进度条同一行，预览区不再被占一行
            auto* pageNav = window.findChild<QWidget*>(QStringLiteral("pageNav"));
            auto* pageSpin = window.findChild<QSpinBox*>(QStringLiteral("pageSpin"));
            check(pageNav != nullptr && pageSpin != nullptr,
                  TR("导航条带上一页/下一页/放大/缩小/页码输入"));
            if (pageNav != nullptr && pageSpin != nullptr) {
                auto* progressBar = window.findChild<QProgressBar*>(QStringLiteral("progressBar"));
                check(pageNav->parentWidget() == window.statusBar(),
                      TR("导航条挂在状态栏上（和 0% 进度条同一个容器）"));
                if (progressBar != nullptr) {
                    const QRect navInWin(pageNav->mapTo(&window, QPoint(0, 0)), pageNav->size());
                    const QRect progressInWin(progressBar->mapTo(&window, QPoint(0, 0)),
                                              progressBar->size());
                    check(qAbs(navInWin.center().y() - progressInWin.center().y()) <= 14 &&
                              navInWin.bottom() >= window.height() - 6,
                          TR("导航按钮与底部 0% 进度条在同一行（都贴窗口最底）"));
                }
                check(pageNav->findChildren<QToolButton*>().size() >= 5,
                      TR("导航条里有四个翻页缩放按钮 + 一个折叠按钮"));
                auto* nextBtn = window.findChild<QToolButton*>(QStringLiteral("nextPageButton"));
                auto* zoomInBtn = window.findChild<QToolButton*>(QStringLiteral("zoomInButton"));
                check(nextBtn != nullptr && nextBtn->isEnabled() && zoomInBtn != nullptr &&
                          zoomInBtn->isEnabled(),
                      TR("打开文档后“下一页”“放大”是可用状态（不是灰的）"));
                check(pageSpin->minimum() == 1 && pageSpin->maximum() == view->pageCount(),
                      TR("页码框范围是 1..总页数"));
                if (view->pageCount() >= 2) {
                    pageSpin->setValue(view->pageCount());
                    QApplication::processEvents();
                    check(view->currentPage() == view->pageCount(), TR("输入页码后直接跳到最后一页"));
                    pageSpin->setValue(1);
                    QApplication::processEvents();
                    check(view->currentPage() == 1, TR("再输入 1 跳回第一页"));
                }

                // 折叠按钮：收起后只剩它自己，再点回来
                auto* foldBtn = window.findChild<QToolButton*>(QStringLiteral("navFoldButton"));
                check(foldBtn != nullptr, TR("导航条最右侧有向右折叠的小按钮"));
                if (foldBtn != nullptr) {
                    foldBtn->click();
                    QApplication::processEvents();
                    bool othersHidden = pageSpin->isHidden();
                    for (auto* btn : pageNav->findChildren<QToolButton*>()) {
                        if (btn != foldBtn) othersHidden = othersHidden && !btn->isVisible();
                    }
                    check(othersHidden, TR("折叠后上一页/下一页/放大/缩小/页码/百分比全部隐藏"));
                    shoot(&window, "main-window-nav-collapsed");
                    foldBtn->click();
                    QApplication::processEvents();
                    check(!pageSpin->isHidden(), TR("再点一次导航条展开、控件都回来"));
                }
            }
        }

        shoot(merge.get(), "merge-dialog");
        shoot(extract.get(), "extract-dialog");
        shoot(convert.get(), "convert-dialog");
        if (editor) shoot(editor.get(), "page-editor-dialog");
        if (metaDlg) shoot(metaDlg.get(), "metadata-dialog");
        if (secDlg) shoot(secDlg.get(), "security-dialog");
        if (cmpDlg) shoot(cmpDlg.get(), "compress-dialog");
        if (wmDlg) shoot(wmDlg.get(), "watermark-dialog");
        if (wmDlg) {
            // 再截一张图片页签的图（新增功能的视觉基线）
            auto* tabs = wmDlg->findChild<QTabWidget*>(QStringLiteral("watermarkTabs"));
            if (tabs != nullptr) {
                tabs->setCurrentIndex(1);
                QApplication::processEvents();
                shoot(wmDlg.get(), "watermark-dialog-image");
            }
        }
        if (imgDlg) shoot(imgDlg.get(), "imgtopdf-dialog");
        if (ocrDlg) shoot(ocrDlg.get(), "ocr-dialog");
        if (annotDlg) shoot(annotDlg.get(), "annot-dialog");
        if (linkDlg) shoot(linkDlg.get(), "link-dialog");
        if (findDlg) shoot(findDlg.get(), "find-replace-dialog");
        if (imgReplaceDlg) shoot(imgReplaceDlg.get(), "replace-image-dialog");
        if (formDlg) shoot(formDlg.get(), "form-dialog");

        // ---- 书签 / 目录面板：读既有目录 → 显隐 → 双击跳页 → 上移下移 → 删除 → 应用 ----
        {
            auto* bmMenu = window.findChild<QMenu*>(QStringLiteral("menuBookmark"));
            auto* bmDock = window.findChild<QDockWidget*>(QStringLiteral("bookmarkDock"));
            auto* bmTree = window.findChild<QTreeWidget*>(QStringLiteral("bookmarkTree"));
            auto* bmHide = window.findChild<QAction*>(QStringLiteral("bookmarkHideAction"));
            auto* bmShow = window.findChild<QAction*>(QStringLiteral("bookmarkShowAction"));
            check(bmMenu != nullptr && bmDock != nullptr && bmTree != nullptr,
                  TR("视图里有「书签目录」菜单 + 书签停靠面板 + 目录树"));
            check(bmHide != nullptr && bmShow != nullptr,
                  TR("「书签目录」里有显示 / 隐藏两项（和文件列表、日志窗口同一套两级写法）"));
            check(bmDock != nullptr && bmDock->parentWidget() == &window,
                  TR("书签面板是可停靠的面板（能左右挪、也能拖成独立窗口）"));

            if (bmDock != nullptr && bmHide != nullptr && bmShow != nullptr) {
                check(!bmDock->isHidden(), TR("书签面板默认显示"));
                bmHide->trigger();
                QApplication::processEvents();
                check(bmDock->isHidden(), TR("点「隐藏书签目录」后面板收起"));
                shoot(&window, "main-window-bookmark-hidden");
                bmShow->trigger();
                QApplication::processEvents();
                check(!bmDock->isHidden() && bmShow->isChecked(), TR("点「显示书签目录」后面板回来"));
            }

            // 用样本的一份副本走"列表双击 → 预览 → 读目录"，产物落在测试目录里，不写样本目录
            const std::filesystem::path workDir = pw::fsPath(dst.toUtf8().toStdString()).parent_path();
            const QString workCopy = QString::fromStdString((workDir / "ol_smoke.pdf").string());
            QFile::remove(workCopy);
            const bool copied = QFile::copy(sample, workCopy);
            check(copied, TR("把样本复制成书签测试用的副本"));
            // 主窗口里唯一的 QListWidget 就是待处理文件那个（书签面板用的是 QTreeWidget）
            auto* fileListForBm = window.findChild<QListWidget*>();
            if (copied && fileListForBm != nullptr) {
                auto* item = new QListWidgetItem(QFileInfo(workCopy).fileName(), fileListForBm);
                item->setData(Qt::UserRole, workCopy);
                fileListForBm->setCurrentItem(item);
                fileListForBm->itemDoubleClicked(item);  // 和用户双击列表项同一条路径
                QApplication::processEvents();
            }

            // 树里的条数要和 core 直接读出来的一致（不写死 12，换样本也不会误报）
            int treeCount = 0;
            bool hasNested = false;
            std::function<void(QTreeWidgetItem*)> walkTree = [&](QTreeWidgetItem* node) {
                for (int i = 0; i < node->childCount(); ++i) {
                    ++treeCount;
                    QTreeWidgetItem* child = node->child(i);
                    if (child->childCount() > 0) hasNested = true;
                    walkTree(child);
                }
            };
            if (bmTree != nullptr) walkTree(bmTree->invisibleRootItem());
            const auto refOutline = converter->pdfBackend()->outline(
                pw::fsPath(sample.toUtf8().toStdString()), std::string());
            check(refOutline.ok(), TR("core 能读出样本 PDF 既有书签"));
            check(bmTree != nullptr && treeCount == int(refOutline.value().size()),
                  TR("面板里的条目数 = core 读出的书签条数"));
            check(bmTree != nullptr && bmTree->topLevelItemCount() > 0, TR("目录树里有顶级书签"));
            check(hasNested, TR("多级目录按层级挂成了子树（不是拍平的一列）"));

            // 双击跳页：挑一条目标页 > 1 的条目
            QTreeWidgetItem* jumpTarget = nullptr;
            std::function<void(QTreeWidgetItem*)> findJump = [&](QTreeWidgetItem* node) {
                for (int i = 0; i < node->childCount() && jumpTarget == nullptr; ++i) {
                    QTreeWidgetItem* child = node->child(i);
                    if (child->data(0, Qt::UserRole).toInt() > 1 && jumpTarget == nullptr) {
                        jumpTarget = child;
                    } else {
                        findJump(child);
                    }
                }
            };
            if (bmTree != nullptr) findJump(bmTree->invisibleRootItem());
            auto* bmView = window.findChild<PdfViewWidget*>();
            check(jumpTarget != nullptr && bmView != nullptr,
                  TR("目录里有一条能跳到第 2 页及以后的书签"));
            if (jumpTarget != nullptr && bmView != nullptr && bmTree != nullptr) {
                const int page = jumpTarget->data(0, Qt::UserRole).toInt();
                bmTree->itemDoubleClicked(jumpTarget, 0);  // 和用户双击同一条路径
                QApplication::processEvents();
                check(bmView->currentPage() == page, TR("双击书签直接跳到对应页"));
            }

            auto* panel = bmDock != nullptr ? bmDock->widget() : nullptr;
            auto* upBtn = panel != nullptr ? panel->findChild<QPushButton*>(QStringLiteral("bookmarkUp")) : nullptr;
            auto* downBtn =
                panel != nullptr ? panel->findChild<QPushButton*>(QStringLiteral("bookmarkDown")) : nullptr;
            auto* rmBtn =
                panel != nullptr ? panel->findChild<QPushButton*>(QStringLiteral("bookmarkRemove")) : nullptr;
            auto* applyBtn =
                panel != nullptr ? panel->findChild<QPushButton*>(QStringLiteral("bookmarkApply")) : nullptr;
            auto* exportBtn =
                panel != nullptr ? panel->findChild<QPushButton*>(QStringLiteral("bookmarkExport")) : nullptr;
            check(panel != nullptr && panel->findChild<QPushButton*>(QStringLiteral("bookmarkAddSibling")) != nullptr &&
                      panel->findChild<QPushButton*>(QStringLiteral("bookmarkAddChild")) != nullptr &&
                      panel->findChild<QPushButton*>(QStringLiteral("bookmarkEdit")) != nullptr,
                  TR("面板有新增同级 / 新增子级 / 修改三个按钮"));
            check(upBtn != nullptr && downBtn != nullptr && rmBtn != nullptr && applyBtn != nullptr &&
                      exportBtn != nullptr,
                  TR("面板有上移 / 下移 / 删除 / 应用 / 导出"));

            // 上下移：在同层里换位，移完能原样移回来
            if (bmTree != nullptr && upBtn != nullptr && downBtn != nullptr) {
                QTreeWidgetItem* first = bmTree->topLevelItem(0);
                const QString firstName = first != nullptr ? first->text(0) : QString();
                bmTree->setCurrentItem(first);
                downBtn->click();
                QApplication::processEvents();
                check(bmTree->topLevelItemCount() > 1 && bmTree->topLevelItem(1) == first,
                      TR("点「下移」把选中书签往后挪一位"));
                upBtn->click();
                QApplication::processEvents();
                check(bmTree->topLevelItem(0) == first && first->text(0) == firstName,
                      TR("再点「上移」挪回原位"));
            }

            // 删除最后一条，然后应用：写回的书签条数 = 面板上剩下的条数
            if (bmTree != nullptr && rmBtn != nullptr) {
                const int before = bmTree->topLevelItemCount();
                const int beforeAll = treeCount;
                bmTree->setCurrentItem(bmTree->topLevelItem(before - 1));
                rmBtn->click();
                QApplication::processEvents();
                check(bmTree->topLevelItemCount() == before - 1, TR("点「删除」去掉一条顶级书签"));
                treeCount = beforeAll - 1;
            }
            if (bmTree != nullptr && applyBtn != nullptr && !workCopy.isEmpty()) {
                applyBtn->click();
                QApplication::processEvents();
                // 「应用」弹的是非阻塞提示框，关掉它免得挡住后面的截图
                for (auto* box : window.findChildren<QMessageBox*>()) box->close();
                QApplication::processEvents();
                const std::filesystem::path applied = workDir / "ol_smoke_书签.pdf";
                check(std::filesystem::exists(applied), TR("点「应用」把书签另存成了 <原名>_书签.pdf"));
                const auto appliedOutline = converter->pdfBackend()->outline(applied, std::string());
                check(appliedOutline.ok() && int(appliedOutline.value().size()) == treeCount,
                      TR("写回去的书签条数 = 面板上剩下的条数"));
                int withPage = 0;
                int nested = 0;
                if (appliedOutline.ok()) {
                    for (const pw::OutlineItem& it : appliedOutline.value()) {
                        if (it.page > 0) ++withPage;
                        if (it.level > 0) ++nested;
                    }
                }
                check(appliedOutline.ok() && withPage == treeCount, TR("写回去的每条书签都还带着页码"));
                check(nested > 0, TR("写回去以后子级书签的层级没丢"));
            }

            // 面板在书签与"目录"之间只有一棵树：这个文档本来就有目录，所以上面这些也是"编辑既有目录"
            shoot(&window, "main-window-bookmarks");
        }

        // ---- 预览里就地编辑：框住一块就改、拖个框打字就加，写完预览自动重载 ----
        {
            const std::filesystem::path workDir =
                pw::fsPath(dst.toUtf8().toStdString()).parent_path();
            const std::filesystem::path srcCopy = workDir / "inline_smoke.pdf";
            QFile::remove(QString::fromStdString(srcCopy.string()));
            check(QFile::copy(sample, QString::fromStdString(srcCopy.string())),
                  TR("把样本复制成就地编辑用的副本（不碰原样本）"));

            auto* view = window.findChild<PdfViewWidget*>();
            QString openErr;
            const QString srcPath = QString::fromStdString(srcCopy.string());
            check(view != nullptr && view->openDocument(srcPath, &openErr),
                  TR("预览里打开了就地编辑用的副本"));
            if (view != nullptr) view->showPage(2);
            QApplication::processEvents();

            // 第 2 页 "instructions for" 那一块（和 CLI find 出来的矩形是同一个）
            const QRectF block(315.0, 203.3, 64.5, 14.3);
            const QString outPath = window.previewReplaceText(2, block, "SMOKE-REPLACED", false);
            check(!outPath.isEmpty() && QFileInfo::exists(outPath),
                  TR("预览里框住一块就能改字出件（不用开窗口填坐标）"));
            check(QFileInfo(outPath).fileName() == QStringLiteral("inline_smoke") + TR("_已编辑.pdf"),
                  TR("改完的文件叫 <原名>_已编辑.pdf"));
            check(view != nullptr && view->currentPath() == outPath, TR("改完预览自动切到结果文件"));
            check(view != nullptr && view->currentPage() == 2, TR("改完预览还停在原来那一页"));

            auto hits = converter->pdfBackend()->findText(
                pw::fsPath(outPath.toUtf8().toStdString()), std::string("SMOKE-REPLACED"));
            check(hits.ok() && !hits.value().empty(), TR("改进去的新文字能在结果文件里搜到"));
            check(hits.ok() && !hits.value().empty() && hits.value().front().page == 1,
                  TR("新文字落在第 2 页（0 基页号 1）"));

            // 只该动这一块：拿第 9 页文本对比，必须一字不差
            bool bothOpen = false;
            std::string t9orig;
            std::string t9edit;
            {
                // 用完就放：这两个文档握着文件句柄，不放掉后面“原地加字”会删不掉旧件
                auto docOrig = converter->pdfBackend()->open(pw::fsPath(srcCopy.string()));
                auto docEdit = converter->pdfBackend()->open(pw::fsPath(outPath.toUtf8().toStdString()));
                bothOpen = docOrig.ok() && docEdit.ok();
                if (bothOpen) {
                    docOrig.value()->pageText(8, &t9orig);
                    docEdit.value()->pageText(8, &t9edit);
                }
            }
            check(bothOpen && t9orig == t9edit, TR("改第 2 页一块，第 9 页文本一字未动"));

            // 加文字：拖个框直接打字
            const QString addPath = window.previewAddText(3, QRectF(60.0, 600.0, 260.0, 22.0),
                                                          TR("加字冒烟 ABC 123"));
            if (addPath.isEmpty() || !QFileInfo::exists(addPath)) {
                if (auto* lv = window.findChild<QPlainTextEdit*>(QStringLiteral("logView"))) {
                    std::printf("---- UI 日志尾部 ----\n%s\n",
                                lv->toPlainText().right(800).toLocal8Bit().constData());
                }
            }
            check(!addPath.isEmpty() && QFileInfo::exists(addPath),
                  TR("拖个框直接打字就能往页面上加文字"));
            auto added = converter->pdfBackend()->findText(pw::fsPath(addPath.toUtf8().toStdString()),
                                                           std::string("加字冒烟"));
            check(added.ok() && added.value().size() == 1,
                  TR("加进去的文字能在结果文件里搜到（中文也认）"));

            // 连改 12 次，走真实链路（右键选动作 → 拖框 → 写回），只跳过输入框：
            // 崩溃、跳回首页、视图位置乱跑都在这一轮里压出来
            const int stressPageBefore = view != nullptr ? view->currentPage() : 0;
            const int stressScrollBefore = view != nullptr ? view->verticalScrollValue() : -1;
            int stressOk = 0;
            for (int i = 0; i < 12; ++i) {
                const QString tag = TR("压测%1").arg(i + 1);
                // 9 = 改字，13 = 加字，10 = 删字（与 MainWindow 里的 InlineAction 一致）
                const int action = (i % 3 == 0) ? 9 : (i % 3 == 1 ? 13 : 10);
                window.previewInlineEditForTest(action, 3, QRectF(60.0, 560.0 + i * 18.0, 200.0, 18.0), tag);
                QApplication::processEvents();
                const QString out = QString::fromStdString(
                    (workDir / (std::string("inline_smoke") + pw::i18n::t("_已编辑.pdf"))).string());
                if (QFileInfo::exists(out)) ++stressOk;
            }
            check(stressOk == 12, TR("同一页连改 12 次（改字/加字/删字交替）不崩、每次都出件"));
            check(view != nullptr && view->currentPage() == stressPageBefore,
                  TR("连改之后页码没变（没跳回首页）"));
            check(view != nullptr && view->verticalScrollValue() == stressScrollBefore,
                  TR("连改之后滚动位置没变（视图停在原处）"));
            auto stressHits = converter->pdfBackend()->findText(
                workDir / (std::string("inline_smoke") + pw::i18n::t("_已编辑.pdf")),
                std::string("\u538b\u6d4b11"));
            check(stressHits.ok() && !stressHits.value().empty(),
                  TR("第 11 次加进去的字也能在产物里搜到"));

            // 真实交互顺序压测：每轮先"拖个框"（这会建 QRubberBand，父是页 label），
            // 再写回重载（页 label 整批重建）。上一版第二轮拖框就会踩到已销毁的橡皮筋而崩。
            if (view != nullptr) view->showPage(3);
            QApplication::processEvents();
            int rectHits = 0;
            if (view != nullptr) {
                QObject::connect(view, &PdfViewWidget::rectSelected,
                                 [&rectHits](int, const QRectF&) { ++rectHits; });
            }
            const int dragScrollBefore = view != nullptr ? view->verticalScrollValue() : -1;
            int dragCycles = 0;
            for (int i = 0; i < 12; ++i) {
                QLabel* pageLabel = view->pageLabelAt(3);
                if (pageLabel != nullptr) {
                    view->setSelectMode(true);
                    const QPointF a(40.0, 40.0);
                    const QPointF b(140.0, 90.0);
                    QMouseEvent press(QEvent::MouseButtonPress, a, pageLabel->mapToGlobal(a.toPoint()),
                                      Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
                    QMouseEvent move(QEvent::MouseMove, b, pageLabel->mapToGlobal(b.toPoint()),
                                     Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
                    QMouseEvent rel(QEvent::MouseButtonRelease, b, pageLabel->mapToGlobal(b.toPoint()),
                                    Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
                    QApplication::sendEvent(pageLabel, &press);
                    QApplication::sendEvent(pageLabel, &move);
                    QApplication::sendEvent(pageLabel, &rel);
                    QApplication::processEvents();
                    ++dragCycles;
                }
                window.previewInlineEditForTest(i % 2 == 0 ? 9 : 13, 3,
                                                QRectF(60.0, 400.0 + i * 20.0, 200.0, 18.0),
                                                TR("框后%1").arg(i + 1));
                QApplication::processEvents();
            }
            check(dragCycles == 12, TR("拖框 12 轮都真的走到了（每轮都能建起选择框）"));
            check(rectHits == 12, TR("12 轮拖框松手都发出了框选信号（每轮都真的走完框选）"));
            check(view != nullptr && view->verticalScrollValue() == dragScrollBefore,
                  TR("拖框 + 写回交替 12 轮后滚动位置仍未变"));

            // 完整交互压测：和真人操作同一条路——startInlineEdit 进框选态 → 拖框 →
            // 选中就触发 applyInlineEdit → 弹模态输入框（用定时器代填并回车）→ 写回重载。
            // 这一轮做的是"再拖一次框"，正是上一版踩到已销毁橡皮筋的地方。
            int dialogCycles = 0;
            int dialogFilled = 0;
            for (int i = 0; i < 6; ++i) {
                const QString body = TR("对话%1").arg(i + 1);
                if (!QMetaObject::invokeMethod(&window, "startInlineEdit", Q_ARG(int, 13))) break;
                QApplication::processEvents();
                // invokeMethod 只负责把 inlineAction_ 设好；这里再显式确认框选态已开，
                // 把"没弹框"和"事件没送到"两件事分开
                if (view != nullptr) view->setSelectMode(true);

                QLabel* pageLabel = view->pageLabelAt(3);
                if (pageLabel == nullptr) break;
                // 模态输入框一出来就填好并回车（30ms 足够让 exec 进入嵌套事件循环）
                // 输入框是模态的，只能在它的嵌套事件循环里代填：每 50ms 试一次
                auto* poll = new QTimer(&window);
                poll->setInterval(50);
                poll->setProperty("tries", 0);
                QObject::connect(poll, &QTimer::timeout, poll, [body, poll, &dialogFilled]() {
                    const int tries = poll->property("tries").toInt() + 1;
                    poll->setProperty("tries", tries);
                    for (QWidget* w : QApplication::topLevelWidgets()) {
                        auto* dlg = qobject_cast<QInputDialog*>(w);
                        if (dlg == nullptr || !dlg->isVisible()) continue;
                        if (auto* edit = dlg->findChild<QLineEdit*>()) edit->setText(body);
                        dlg->accept();
                        ++dialogFilled;
                        poll->stop();
                        poll->deleteLater();
                        return;
                    }
                    if (tries > 40) {
                        poll->stop();
                        poll->deleteLater();
                    }
                });
                poll->start();
                const QPointF a(60.0, 200.0 + i * 24.0);
                const QPointF b(260.0, 218.0 + i * 24.0);
                QMouseEvent press(QEvent::MouseButtonPress, a, pageLabel->mapToGlobal(a.toPoint()),
                                  Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
                QMouseEvent move(QEvent::MouseMove, b, pageLabel->mapToGlobal(b.toPoint()), Qt::NoButton,
                                 Qt::LeftButton, Qt::NoModifier);
                QMouseEvent rel(QEvent::MouseButtonRelease, b, pageLabel->mapToGlobal(b.toPoint()),
                                Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
                QApplication::sendEvent(pageLabel, &press);
                QApplication::sendEvent(pageLabel, &move);
                QApplication::sendEvent(pageLabel, &rel);
                QApplication::processEvents();
                ++dialogCycles;
            }
            check(rectHits == 18, TR("6 轮完整交互也是每轮都真的走完了框选"));
            check(dialogFilled == 6, TR("模态输入框 6 轮都被真正填好并回车"));
            check(dialogCycles == 6, TR("走完整交互（拖框 → 模态输入框 → 写回）连做 6 轮不崩"));
            auto dialogHits = converter->pdfBackend()->findText(
                workDir / (std::string("inline_smoke") + pw::i18n::t("_已编辑.pdf")),
                std::string("对话6"));
            check(dialogHits.ok() && !dialogHits.value().empty(),
                  TR("第 6 轮通过输入框加的字也进了产物"));
            check(view != nullptr && view->verticalScrollValue() == dragScrollBefore,
                  TR("6 轮交互压测后滚动位置还没变"));

            shoot(&window, "main-window-inline-edit");
        }

        // ---- 视图状态持久化：藏起来的面板、挪过去的停靠侧、收起的导航条，重开一次还在 ----
        {
            QAction* hideLogAgain = window.findChild<QAction*>(QStringLiteral("logHideAction"));
            QAction* hideFilesAgain = window.findChild<QAction*>(QStringLiteral("fileHideAction"));
            QAction* toRightAgain = window.findChild<QAction*>(QStringLiteral("dockRightAction"));
            QToolButton* foldAgain = window.findChild<QToolButton*>(QStringLiteral("navFoldButton"));
            check(hideLogAgain != nullptr && hideFilesAgain != nullptr && toRightAgain != nullptr &&
                      foldAgain != nullptr,
                  TR("持久化测试要用的四个动作都在"));
            if (hideLogAgain != nullptr && hideFilesAgain != nullptr && toRightAgain != nullptr) {
                // 顺序有意义：先切靠右（会自动把面板显示出来）→ 收起导航条 → 最后再做隐藏类动作
                QAction* hideBmAgain = window.findChild<QAction*>(QStringLiteral("bookmarkHideAction"));
                toRightAgain->trigger();
                if (foldAgain != nullptr) foldAgain->click();
                hideLogAgain->trigger();
                hideFilesAgain->trigger();
                if (hideBmAgain != nullptr) hideBmAgain->trigger();
                QApplication::processEvents();
            }
            window.close();  // closeEvent 里写 INI
            QApplication::processEvents();

            MainWindow second(converter, jobs);  // 同一个 INI，重开一次
            second.show();
            QApplication::processEvents();
            auto* logSecond = second.findChild<QPlainTextEdit*>(QStringLiteral("logView"));
            auto* dockSecond = second.findChild<QDockWidget*>(QStringLiteral("fileDock"));
            auto* rightSecond = second.findChild<QAction*>(QStringLiteral("dockRightAction"));
            check(logSecond != nullptr && logSecond->isHidden(), TR("重开后日志窗口仍是隐藏状态"));
            check(dockSecond != nullptr && dockSecond->isHidden(), TR("重开后文件列表仍是隐藏状态"));
            check(rightSecond != nullptr && rightSecond->isChecked(), TR("重开后文件列表仍是靠右"));
            auto* bmSecond = second.findChild<QDockWidget*>(QStringLiteral("bookmarkDock"));
            check(bmSecond != nullptr && bmSecond->isHidden(), TR("重开后书签面板仍是隐藏状态"));
            // 收尾把书签面板放回显示，别让下一次启动少一块
            QAction* bmShowSecond = second.findChild<QAction*>(QStringLiteral("bookmarkShowAction"));
            if (bmShowSecond != nullptr) bmShowSecond->trigger();
            QApplication::processEvents();
            auto* viewSecond = second.findChild<PdfViewWidget*>();
            check(viewSecond != nullptr && viewSecond->isNavCollapsed(), TR("重开后底部导航条仍是收起状态"));
            check(QDir(settingsDir).exists(), TR("视图状态确实写进了 INI 目录"));
            second.close();
        }

        std::printf("smoke done, failures=%d\n", failures);
        app.exit(failures == 0 ? 0 : 1);
    });
    return app.exec();
}
