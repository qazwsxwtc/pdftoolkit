#include "PdfViewWidget.h"

#include <QRubberBand>

#include <QEvent>
#include <QFontMetrics>
#include <QFrame>
#include <QHBoxLayout>
#include <QImage>
#include <QLabel>
#include <QPixmap>
#include <QPalette>
#include <QRect>
#include <QResizeEvent>
#include <QScrollArea>
#include <QScrollBar>
#include <QShortcut>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QStyle>
#include <QToolButton>
#include <QVBoxLayout>
#include <QWheelEvent>
#include <QtGlobal>
#include <vector>

namespace {
// 固定缩放档位：8% 到 800%，每次移动一档，比纯乘 1.25 更容易回到整值
const double kZoomSteps[] = {0.08, 0.1, 0.15, 0.25, 0.33, 0.5, 0.67, 0.75, 1.0,
                             1.25, 1.5, 1.75, 2.0, 2.5, 3.0, 4.0, 6.0, 8.0};
const int kZoomStepCount = static_cast<int>(sizeof(kZoomSteps) / sizeof(kZoomSteps[0]));

double nextZoomUp(double current) {
    for (int i = 0; i < kZoomStepCount; ++i) {
        if (kZoomSteps[i] > current + 1e-6) return kZoomSteps[i];
    }
    return kZoomSteps[kZoomStepCount - 1];
}

double nextZoomDown(double current) {
    for (int i = kZoomStepCount - 1; i >= 0; --i) {
        if (kZoomSteps[i] < current - 1e-6) return kZoomSteps[i];
    }
    return kZoomSteps[0];
}

// 导航条的高度：这条最后挂在主窗口状态栏里，和 0% 进度条同一行
const int kNavBarHeight = 30;
// 连续滚动的版式参数：页间距、四周留白、预渲染范围（视口上下各多渲染这么多像素）
const int kPageGap = 14;
const int kPageMargin = 12;
const int kPrefetchPx = 400;
}  // namespace

PdfViewWidget::PdfViewWidget(std::shared_ptr<pw::IPdfBackend> backend, QWidget* parent)
    : QWidget(parent), backend_(std::move(backend)) {
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);

    canvas_ = new QWidget(this);
    canvas_->setObjectName(QStringLiteral("pdfCanvas"));
    // 页与页之间的缝要能看出来，所以画布本身铺一层浅灰，页是白的
    canvas_->setStyleSheet(QStringLiteral("QWidget#pdfCanvas { background: #ececec; }"));
    canvas_->setAutoFillBackground(true);
    // 所有页竖着排成一条：页与页之间 14px 缝，四周留 12px，滚条拉到哪就看到哪
    canvasLayout_ = new QVBoxLayout(canvas_);
    canvasLayout_->setContentsMargins(kPageMargin, kPageMargin, kPageMargin, kPageMargin);
    canvasLayout_->setSpacing(kPageGap);
    canvasLayout_->setAlignment(Qt::AlignHCenter | Qt::AlignTop);

    scroll_ = new QScrollArea(this);
    scroll_->setObjectName(QStringLiteral("pdfScroll"));
    scroll_->setWidget(canvas_);
    // 画布尺寸由页叠决定；视口小于它就出现滚动条（AsNeeded）
    scroll_->setWidgetResizable(false);
    scroll_->setAlignment(Qt::AlignHCenter);
    scroll_->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    scroll_->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    scroll_->setBackgroundRole(QPalette::Midlight);
    // 滚轮事件先到 viewport，Ctrl + 滚轮要在这里拦下来做缩放
    scroll_->viewport()->installEventFilter(this);
    // 滚动 → 补渲染视口附近的页 + 更新"当前第几页"
    connect(scroll_->verticalScrollBar(), &QScrollBar::valueChanged, this, [this] {
        updateVisiblePages();
        syncCurrentPage();
    });
    // 窗口变大/换文档后可滚范围变了，同样要补渲染
    connect(scroll_->verticalScrollBar(), &QScrollBar::rangeChanged, this,
            [this] { updateVisiblePages(); });
    // 滚轮一格走 3 行；单步给大一点，长文档才不用滚半天
    scroll_->verticalScrollBar()->setSingleStep(48);
    // 预览占满剩余空间；底部导航条挂在状态栏上，不在这里
    layout->addWidget(scroll_, 1);

    hint_ = new QLabel(this);
    hint_->setAlignment(Qt::AlignCenter);
    hint_->setWordWrap(true);
    hint_->setStyleSheet("color:#666; padding:32px;");
    layout->addWidget(hint_);

    createNavBar();

    showHint(backend_ && backend_->available()
                 ? QStringLiteral("打开一个 PDF 开始预览")
                 : QStringLiteral("PDF 后端不可用：%1\n\n构建时启用 -DPDFWORD_WITH_MUPDF=ON 并指定 MuPDF SDK 路径。")
                       .arg(backend_ ? QString::fromStdString(backend_->unavailableReason())
                                     : QStringLiteral("backend 为空")));

    // 键盘也能翻页：PageUp / PageDown 跟上一页 / 下一页对应
    auto* prevShortcut = new QShortcut(QKeySequence(Qt::Key_PageUp), this);
    connect(prevShortcut, &QShortcut::activated, this, [this] { showPage(currentPage_ - 1); });
    auto* nextShortcut = new QShortcut(QKeySequence(Qt::Key_PageDown), this);
    connect(nextShortcut, &QShortcut::activated, this, [this] { showPage(currentPage_ + 1); });
}

void PdfViewWidget::createNavBar() {
    // 导航控件条：不占预览区的位置，交给主窗口挂到状态栏上（和 0% 进度条同一行）。
    // 左边排 上一页/下一页/放大/缩小/[页码]/总页数/百分比，最右边一个折叠按钮。
    navBar_ = new QWidget(this);
    navBar_->setObjectName(QStringLiteral("pageNav"));
    // 挂在状态栏上，背景透明；按钮给深一点的字色，免得在状态栏灰底上像"禁用"
    navBar_->setStyleSheet(QStringLiteral(
        "#pageNav { background: transparent; }"
        "#pageNav QToolButton { color: #202020; padding: 2px 6px; border-radius: 3px; }"
        "#pageNav QToolButton:hover { background: #e4e4e4; }"
        "#pageNav QToolButton:disabled { color: #a0a0a0; }"));
    navBar_->setFixedHeight(kNavBarHeight);
    navBar_->hide();

    auto* navLayout = new QHBoxLayout(navBar_);
    navLayout->setContentsMargins(2, 0, 8, 0);
    navLayout->setSpacing(6);

    auto addButton = [this, navLayout](const QString& text, const QString& tip) {
        auto* button = new QToolButton(navBar_);
        button->setText(text);
        button->setToolTip(tip);
        button->setAutoRaise(true);
        button->setCursor(Qt::PointingHandCursor);
        navLayout->addWidget(button);
        return button;
    };
    auto addSeparator = [navLayout]() {
        auto* line = new QFrame(navLayout->parentWidget());
        line->setFrameShape(QFrame::VLine);
        line->setFrameShadow(QFrame::Sunken);
        navLayout->addWidget(line);
    };

    prevBtn_ = addButton(QStringLiteral("上一页"), QStringLiteral("上一页（PageUp）"));
    nextBtn_ = addButton(QStringLiteral("下一页"), QStringLiteral("下一页（PageDown）"));
    prevBtn_->setObjectName(QStringLiteral("prevPageButton"));
    nextBtn_->setObjectName(QStringLiteral("nextPageButton"));
    connect(prevBtn_, &QToolButton::clicked, this, [this] { showPage(currentPage_ - 1); });
    connect(nextBtn_, &QToolButton::clicked, this, [this] { showPage(currentPage_ + 1); });

    addSeparator();
    zoomInBtn_ = addButton(QStringLiteral("放大"), QStringLiteral("放大一档（Ctrl + 滚轮向上）"));
    zoomOutBtn_ = addButton(QStringLiteral("缩小"), QStringLiteral("缩小一档（Ctrl + 滚轮向下）"));
    zoomInBtn_->setObjectName(QStringLiteral("zoomInButton"));
    zoomOutBtn_->setObjectName(QStringLiteral("zoomOutButton"));
    connect(zoomInBtn_, &QToolButton::clicked, this, [this] { zoomIn(); });
    connect(zoomOutBtn_, &QToolButton::clicked, this, [this] { zoomOut(); });

    addSeparator();
    pageSpin_ = new QSpinBox(navBar_);
    pageSpin_->setObjectName(QStringLiteral("pageSpin"));
    pageSpin_->setRange(1, 1);
    pageSpin_->setAlignment(Qt::AlignRight);
    pageSpin_->setKeyboardTracking(false);  // 输入完回车/失焦才跳，不要边打字边跳
    pageSpin_->setFixedWidth(56);
    pageSpin_->setToolTip(QStringLiteral("输入页码后回车，直接跳到那一页"));
    navLayout->addWidget(pageSpin_);
    connect(pageSpin_, QOverload<int>::of(&QSpinBox::valueChanged), this, [this](int page) {
        if (document_ && page != currentPage_) showPage(page);
    });

    pageTotal_ = new QLabel(QStringLiteral(" / 0"), navBar_);
    navLayout->addWidget(pageTotal_);

    zoomText_ = new QLabel(QStringLiteral("150%"), navBar_);
    zoomText_->setMinimumWidth(40);
    zoomText_->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    zoomText_->setStyleSheet("color:#555;");
    navLayout->addWidget(zoomText_);

    // 控件靠左，折叠按钮钉在最右
    navLayout->addStretch(1);
    foldBtn_ = new QToolButton(navBar_);
    foldBtn_->setObjectName(QStringLiteral("navFoldButton"));
    foldBtn_->setText(QStringLiteral("收起 ▸"));
    foldBtn_->setToolTip(QStringLiteral("向右收起导航条（上一页/下一页/缩放/页码）"));
    foldBtn_->setAutoRaise(true);
    foldBtn_->setCursor(Qt::PointingHandCursor);
    navLayout->addWidget(foldBtn_);
    connect(foldBtn_, &QToolButton::clicked, this, [this] { setNavCollapsed(!navCollapsed_); });
}

bool PdfViewWidget::isNavCollapsed() const { return navCollapsed_; }

void PdfViewWidget::setNavCollapsed(bool collapsed) {
    navCollapsed_ = collapsed;
    QWidget* const controls[] = {prevBtn_, nextBtn_, zoomInBtn_, zoomOutBtn_, pageSpin_, pageTotal_,
                                 zoomText_};
    for (QWidget* control : controls) {
        if (control != nullptr) control->setVisible(!collapsed);
    }
    if (foldBtn_ != nullptr) {
        foldBtn_->setText(collapsed ? QStringLiteral("◂ 展开") : QStringLiteral("收起 ▸"));
        foldBtn_->setToolTip(collapsed
                                 ? QStringLiteral("展开导航条（上一页/下一页/缩放/页码）")
                                 : QStringLiteral("向右收起导航条（上一页/下一页/缩放/页码）"));
    }
    // 高度保持不动：这条挂在状态栏上，一收一放改高度会让最底下那行来回跳
}

void PdfViewWidget::updateNavBar() {
    if (pageSpin_ == nullptr) return;
    const bool hasDoc = document_ != nullptr;
    {
        QSignalBlocker blocker(pageSpin_);
        pageSpin_->setRange(1, qMax(1, pageCount_));
        pageSpin_->setValue(qMax(1, currentPage_));
    }
    pageSpin_->setEnabled(hasDoc);
    pageTotal_->setText(QStringLiteral(" / %1").arg(pageCount_));
    zoomText_->setText(QStringLiteral("%1%").arg(zoomPercent()));
    prevBtn_->setEnabled(hasDoc && currentPage_ > 1);
    nextBtn_->setEnabled(hasDoc && currentPage_ < pageCount_);
    // 有文档时缩放按钮始终可用（已经在最大/最小档位时也允许点，点完就停在那儿）
    zoomInBtn_->setEnabled(hasDoc && zoom_ < 8.0 - 1e-6);
    zoomOutBtn_->setEnabled(hasDoc && zoom_ > 0.08 + 1e-6);
    navBar_->setVisible(hasDoc);
}

bool PdfViewWidget::eventFilter(QObject* watched, QEvent* event) {
    if (watched == (scroll_ != nullptr ? scroll_->viewport() : nullptr) &&
        event->type() == QEvent::Wheel) {
        auto* wheel = static_cast<QWheelEvent*>(event);
        if ((wheel->modifiers() & Qt::ControlModifier) != 0 && document_) {
            // 上滚放大、下滚缩小；锚点用鼠标所在位置，缩放后指针下的内容不动
            const int dy = wheel->angleDelta().y();
            if (dy != 0) {
                const double target = dy > 0 ? nextZoomUp(zoom_) : nextZoomDown(zoom_);
                zoomAt(target, wheel->position().toPoint());
            }
            return true;
        }
    }

    // 框选：拖动画一个矩形，松手把"页号 + 显示空间 pt 矩形"发出去
    if (selectMode_) {
        if (auto* label = qobject_cast<QLabel*>(watched)) {
            if (handleSelectEvent(label, event)) return true;
        }
    }
    return QWidget::eventFilter(watched, event);
}

void PdfViewWidget::setSelectMode(bool on) {
    selectMode_ = on;
    for (QLabel* lb : pageLabels_) {
        if (lb != nullptr) lb->setCursor(on ? Qt::CrossCursor : Qt::ArrowCursor);
    }
    if (!on && rubber_ != nullptr) rubber_->hide();
    dragging_ = false;
}

bool PdfViewWidget::handleSelectEvent(QLabel* label, QEvent* event) {
    int index = -1;
    for (std::size_t i = 0; i < pageLabels_.size(); ++i) {
        if (pageLabels_[i] == label) {
            index = static_cast<int>(i);
            break;
        }
    }
    if (index < 0) return false;

    switch (event->type()) {
        case QEvent::MouseButtonPress: {
            auto* me = static_cast<QMouseEvent*>(event);
            if (me->button() != Qt::LeftButton) return false;
            dragging_ = true;
            dragOrigin_ = me->position().toPoint();
            if (rubber_ == nullptr) {
                rubber_ = new QRubberBand(QRubberBand::Rectangle, label);
                rubber_->setStyleSheet(QStringLiteral("QRubberBand { border: 1px dashed #c0392b;"
                                                      " background: rgba(192,57,43,0.15); }"));
            }
            rubber_->setGeometry(QRect(dragOrigin_, QSize(1, 1)));
            rubber_->show();
            return true;
        }
        case QEvent::MouseMove: {
            if (!dragging_ || rubber_ == nullptr) return false;
            auto* me = static_cast<QMouseEvent*>(event);
            const QRect area = label->rect();
            QPoint p = me->position().toPoint();
            p.setX(qBound(area.left(), p.x(), area.right()));
            p.setY(qBound(area.top(), p.y(), area.bottom()));
            rubber_->setGeometry(QRect(dragOrigin_, p).normalized());
            return true;
        }
        case QEvent::MouseButtonRelease: {
            if (!dragging_) return false;
            dragging_ = false;
            auto* me = static_cast<QMouseEvent*>(event);
            const QRect px = rubber_ != nullptr ? rubber_->geometry()
                                                : QRect(dragOrigin_, me->position().toPoint());
            const double z = zoom_ > 1e-6 ? zoom_ : 1.0;
            QRectF pt(px.x() / z, px.y() / z, px.width() / z, px.height() / z);
            if (pt.width() < 3.0 || pt.height() < 3.0) {
                // 点一下不算框选，避免误触
                if (rubber_ != nullptr) rubber_->hide();
                return true;
            }
            selPage_ = index + 1;
            selRect_ = pt;
            emit rectSelected(selPage_, selRect_);
            return true;
        }
        default:
            break;
    }
    return false;
}

void PdfViewWidget::showHint(const QString& text) {
    hint_->setText(text);
    hint_->show();
    scroll_->hide();
    if (navBar_ != nullptr) navBar_->hide();
}

bool PdfViewWidget::openDocument(const QString& path, QString* error) {
    if (!backend_ || !backend_->available()) {
        if (error != nullptr) {
            *error = QStringLiteral("PDF 后端不可用：%1")
                         .arg(backend_ ? QString::fromStdString(backend_->unavailableReason())
                                       : QStringLiteral("backend 为空"));
        }
        return false;
    }

    auto opened = backend_->open(std::filesystem::path(path.toStdString()));
    if (!opened.ok()) {
        if (error != nullptr) *error = QString::fromStdString(opened.status().toString());
        return false;
    }

    document_ = opened.value();
    path_ = path;
    pageCount_ = document_->info().pageCount;
    currentPage_ = 1;
    scroll_->show();
    hint_->hide();
    buildPages();  // 所有页先占位排好，只见视口附近那几页真的渲染
    // 新文档从第 1 页顶部看起：竖直回到顶部、水平居中
    scroll_->verticalScrollBar()->setValue(0);
    QScrollBar* hs = scroll_->horizontalScrollBar();
    hs->setValue(hs->maximum() / 2);
    updateNavBar();
    emit pageChanged(currentPage_, pageCount_);
    emit zoomChanged(zoom_);
    return true;
}

void PdfViewWidget::closeDocument() {
    document_.reset();
    path_.clear();
    pageCount_ = 0;
    currentPage_ = 1;
    clearPages();
    showHint(QStringLiteral("打开一个 PDF 开始预览"));
    updateNavBar();
}

void PdfViewWidget::clearPages() {
    for (QLabel* label : pageLabels_) {
        canvasLayout_->removeWidget(label);
        label->deleteLater();
    }
    pageLabels_.clear();
    pageRendered_.clear();
    pageWpt_.clear();
    pageHpt_.clear();
}

void PdfViewWidget::buildPages() {
    clearPages();
    if (!document_ || pageCount_ <= 0) return;

    pageWpt_.reserve(pageCount_);
    pageHpt_.reserve(pageCount_);
    for (int i = 0; i < pageCount_; ++i) {
        pw::PageInfo info;
        if (!document_->pageInfo(i, &info).ok() || info.widthPt <= 0.0 || info.heightPt <= 0.0) {
            // 单页信息读不出来时按 A4 处理，至少让版式不断
            pageWpt_.push_back(612.0);
            pageHpt_.push_back(792.0);
        } else {
            pageWpt_.push_back(info.widthPt);
            pageHpt_.push_back(info.heightPt);
        }
    }

    for (int i = 0; i < pageCount_; ++i) {
        auto* label = new QLabel(canvas_);
        label->setObjectName(QStringLiteral("pageItem"));
        label->setAlignment(Qt::AlignCenter);
        label->setAutoFillBackground(true);
        label->setBackgroundRole(QPalette::Base);
        label->setTextInteractionFlags(Qt::NoTextInteraction);
        // 还没渲染时只显示页码，用户能看出这是第几页、也知道正在往下滚
        label->setText(QStringLiteral("第 %1 页").arg(i + 1));
        label->setStyleSheet(QStringLiteral("QLabel { background: #ffffff; color: #999999; }"));
        if (selectMode_) label->setCursor(Qt::CrossCursor);
        // 框选要靠页面 label 上的鼠标事件，不装过滤器就收不到
        label->installEventFilter(this);
        canvasLayout_->addWidget(label);
        pageLabels_.push_back(label);
        pageRendered_.push_back(false);
    }

    applyZoomToPages();
    updateVisiblePages();
}

void PdfViewWidget::applyZoomToPages() {
    if (pageLabels_.empty()) return;
    int contentWidth = 0;
    int contentHeight = 2 * kPageMargin + kPageGap * (static_cast<int>(pageLabels_.size()) - 1);
    for (int i = 0; i < static_cast<int>(pageLabels_.size()); ++i) {
        const int w = qMax(1, int(pageWpt_[i] * zoom_ + 0.5));
        const int h = qMax(1, int(pageHpt_[i] * zoom_ + 0.5));
        pageLabels_[i]->setFixedSize(w, h);
        pageLabels_[i]->clear();
        pageLabels_[i]->setText(QStringLiteral("第 %1 页").arg(i + 1));
        pageRendered_[i] = false;
        contentWidth = qMax(contentWidth, w);
        contentHeight += h;
    }
    // 画布尺寸自己算准，别指望 QScrollArea 帮忙：widgetResizable=false 时它会把
    // 画布压在视口大小，滚条就永远出不来，也就没法"一直往下拉"
    contentWidth += 2 * kPageMargin;
    canvasLayout_->activate();
    canvas_->setMinimumSize(contentWidth, contentHeight);
    canvas_->resize(contentWidth, contentHeight);
}

void PdfViewWidget::updateVisiblePages() {
    if (!document_ || pageLabels_.empty()) return;
    QWidget* viewport = scroll_->viewport();
    // 画布原点在视口坐标系里的位置（滚动后是负值）；反过来就是视口顶部落在画布上的位置
    const QPoint canvasInViewport = canvas_->mapTo(viewport, QPoint(0, 0));
    const int visibleTop = -canvasInViewport.y();
    const int visibleBottom = visibleTop + viewport->height();
    const int topLimit = visibleTop - kPrefetchPx;
    const int bottomLimit = visibleBottom + kPrefetchPx;

    for (int i = 0; i < static_cast<int>(pageLabels_.size()); ++i) {
        const QRect geometry = pageLabels_[i]->geometry();
        const bool nearViewport = geometry.bottom() >= topLimit && geometry.top() <= bottomLimit;
        if (nearViewport && !pageRendered_[i]) {
            renderPageAt(i);
        } else if (!nearViewport && pageRendered_[i]) {
            // 离得远的页把位图放掉，80 页文档滚一遍也不会把内存吃满
            pageLabels_[i]->clear();
            pageLabels_[i]->setText(QStringLiteral("第 %1 页").arg(i + 1));
            pageRendered_[i] = false;
        }
    }
}

void PdfViewWidget::renderPageAt(int index) {
    if (!document_ || index < 0 || index >= static_cast<int>(pageLabels_.size())) return;
    pw::RasterImage image;
    pw::Status status = document_->renderPage(index, static_cast<float>(zoom_), &image);
    if (!status.ok()) {
        pageLabels_[index]->setText(QString::fromStdString(status.toString()));
        return;
    }
    QImage qimage(image.rgba.data(), image.width, image.height, image.width * 4,
                  QImage::Format_RGBA8888);
    pageLabels_[index]->setPixmap(QPixmap::fromImage(qimage));
    pageRendered_[index] = true;
}

void PdfViewWidget::syncCurrentPage() {
    if (!document_ || pageLabels_.empty()) return;
    QScrollBar* vs = scroll_->verticalScrollBar();
    QWidget* viewport = scroll_->viewport();
    const int topY = -canvas_->mapTo(viewport, QPoint(0, 0)).y();
    int target = currentPage_;
    if (topY <= 0) {
        target = 1;
    } else if (topY + viewport->height() >= canvas_->height() - 2 && vs->maximum() > 0) {
        // 已经拉到底：算最后一页，不然离页脚还缺点距离时会被判成倒数第二页
        target = pageCount_;
    } else {
        const int probe = topY + 1;  // 视口顶部往下 1px 落在哪页
        target = pageCount_;
        // 取"探测点还没越过底部"的第一页：probe 落在页与页之间的缝里时算下一页，
        // 否则 showPage() 把页顶精确滚到视口顶部后，页码会被判成最后一页。
        for (int i = 0; i < static_cast<int>(pageLabels_.size()); ++i) {
            const QRect geometry = pageLabels_[i]->geometry();
            if (probe < geometry.top() + geometry.height()) {
                target = i + 1;
                break;
            }
        }
    }
    if (target == currentPage_) return;
    currentPage_ = target;
    updateNavBar();
    emit pageChanged(currentPage_, pageCount_);
}

bool PdfViewWidget::pageSizePt(double* widthPt, double* heightPt) const {
    if (!document_) return false;
    pw::PageInfo info;
    if (!document_->pageInfo(currentPage_ - 1, &info).ok()) return false;
    if (info.widthPt <= 0.0 || info.heightPt <= 0.0) return false;
    *widthPt = info.widthPt;
    *heightPt = info.heightPt;
    return true;
}

void PdfViewWidget::zoomAt(double zoom, const QPoint& viewportPos) {
    if (!document_) {
        zoom_ = qBound(0.08, zoom, 8.0);
        emit zoomChanged(zoom_);
        return;
    }

    // 记下锚点落在整叠画布上的相对位置（0..1），重排后按同一比例反推滚动条位置
    QScrollBar* hs = scroll_->horizontalScrollBar();
    QScrollBar* vs = scroll_->verticalScrollBar();
    const int cw = qMax(1, canvas_->width());
    const int ch = qMax(1, canvas_->height());
    const double rx = qBound(0.0, (hs->value() + viewportPos.x()) / double(cw), 1.0);
    const double ry = qBound(0.0, (vs->value() + viewportPos.y()) / double(ch), 1.0);

    zoom_ = qBound(0.08, zoom, 8.0);
    render();

    hs->setValue(int(rx * canvas_->width()) - viewportPos.x());
    vs->setValue(int(ry * canvas_->height()) - viewportPos.y());
    updateVisiblePages();
    updateNavBar();
    emit zoomChanged(zoom_);
}

void PdfViewWidget::setZoom(double zoom) {
    // 工具栏按钮没有鼠标位置，用视口中心当锚点
    const QPoint center(scroll_->viewport()->width() / 2, scroll_->viewport()->height() / 2);
    zoomAt(zoom, center);
}

void PdfViewWidget::zoomIn() {
    zoomAt(nextZoomUp(zoom_), QPoint(scroll_->viewport()->width() / 2, scroll_->viewport()->height() / 2));
}

void PdfViewWidget::zoomOut() {
    zoomAt(nextZoomDown(zoom_), QPoint(scroll_->viewport()->width() / 2, scroll_->viewport()->height() / 2));
}

void PdfViewWidget::fitWidth() {
    double w = 0.0;
    double h = 0.0;
    if (!pageSizePt(&w, &h)) return;
    const int availW = qMax(64, scroll_->viewport()->width() - 4);
    const int sbExtent = scroll_->style()->pixelMetric(QStyle::PM_ScrollBarExtent, nullptr, scroll_) + 2;
    // 连续滚动时竖直滚条基本一直在，它会吃掉十几像素宽度；画布两侧还各留了 12px 页边距，
    // 这两笔都必须先扣掉，否则算出的"刚好贴合"会冒出横向滚动条
    const int fitW = qMax(64, availW - sbExtent - 2 * kPageMargin);
    setZoom(fitW / w);
}

void PdfViewWidget::fitPage() {
    double w = 0.0;
    double h = 0.0;
    if (!pageSizePt(&w, &h)) return;
    const int sbExtent = scroll_->style()->pixelMetric(QStyle::PM_ScrollBarExtent, nullptr, scroll_) + 2;
    // 一页装进视口：宽高都扣掉滚条、页面控件外的留白和页间距，放大后只需上下滚着看后面几页
    const int availW = qMax(64, scroll_->viewport()->width() - 4 - sbExtent - 2 * kPageMargin);
    const int availH = qMax(64, scroll_->viewport()->height() - 4 - sbExtent - kPageGap - 2 * kPageMargin);
    setZoom(qMin(availW / w, availH / h));
}

void PdfViewWidget::showPage(int page1) {
    if (!document_ || pageLabels_.empty()) return;
    if (page1 < 1) page1 = 1;
    if (page1 > pageCount_) page1 = pageCount_;
    currentPage_ = page1;

    // 先把这一页渲染出来，再滚到它的页顶（不是滚到画布最上面）
    renderPageAt(page1 - 1);
    const QRect geometry = pageLabels_[page1 - 1]->geometry();
    // 让页顶正好落在视口顶部：竖直滚条按页顶再往上让出一点页间距
    scroll_->verticalScrollBar()->setValue(qMax(0, geometry.top() - kPageMargin));
    updateVisiblePages();
    updateNavBar();
    emit pageChanged(currentPage_, pageCount_);
}

void PdfViewWidget::render() {
    if (!document_) return;
    applyZoomToPages();
    updateVisiblePages();
}
