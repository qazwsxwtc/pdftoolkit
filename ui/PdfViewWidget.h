#pragma once

#include <QRect>
#include <QWidget>
#include <memory>
#include <vector>

#include "pdfword/pdf_backend.hpp"

class QLabel;
class QMouseEvent;
class QResizeEvent;
class QRubberBand;
class QScrollArea;
class QSpinBox;
class QToolButton;
class QVBoxLayout;

// PDF 预览控件：只依赖 core 的 IPdfBackend 接口，不直接碰 MuPDF。
// 换成 web 壳时，这一整块被前端 canvas 替代，core 侧不需要任何改动。
//
// 连续滚动：画布把**所有页竖着排成一条**（页与页之间留 14px 缝），拖着滚动条/滚轮就能
// 一直往下滚到最后一页；只有落在视口附近（±400px）的页才真正渲染成位图，
// 其它页先按页面尺寸占位并标出页码，滚过去再补渲染，所以 80 页的文档也不会一次吃掉几百 MB。
// 当前页按"视口顶部落在哪一页"自动更新，页码框和页面都跟着走。
//
// 缩放：以视口中心或鼠标位置为锚点，放大不会跳到角落。
// 导航：上一页 / 下一页 / 放大 / 缩小 / [页码] / 总页数 / 百分比 做在一条 `pageNav` 里，
// 由主窗口挂到**状态栏**（和 0% 进度条同一行），预览区自己不再占一行；最右边有折叠按钮。
class PdfViewWidget : public QWidget {
    Q_OBJECT

public:
    explicit PdfViewWidget(std::shared_ptr<pw::IPdfBackend> backend, QWidget* parent = nullptr);

    bool openDocument(const QString& path, QString* error);
    void closeDocument();
    void setZoom(double zoom);
    void showPage(int page1);
    // 放大 / 缩小一档（固定档位，反复点击会停在整档上，不会飘成 1.2345）
    void zoomIn();
    void zoomOut();
    // 让页面宽度刚好填满视口 / 整页放进视口（右下角滚动条随之消失）
    void fitWidth();
    void fitPage();
    // 底部导航条收起 / 展开（状态会被主窗口写进 INI，下次启动沿用）
    bool isNavCollapsed() const;
    void setNavCollapsed(bool collapsed);
    // 导航条本身不占预览区的位置：主窗口把它挂到状态栏上（和进度条同一行）
    QWidget* navBar() const { return navBar_; }

    // 当前预览的 PDF 路径（没打开时为空），供"抽取页面"等操作确定源文件
    QString currentPath() const { return path_; }
    // 已打开的文档句柄，页面编辑器直接复用，避免再解析一遍
    std::shared_ptr<pw::IPdfDocument> document() const { return document_; }

    double zoom() const { return zoom_; }
    int pageCount() const { return pageCount_; }
    int currentPage() const { return currentPage_; }
    // 缩放百分比（1.0 = 100%），给工具栏上的标签用
    int zoomPercent() const { return static_cast<int>(zoom_ * 100.0 + 0.5); }
    // 画布里排了多少页（冒烟测试用：应等于 pageCount）
    int pageItemCount() const { return static_cast<int>(pageLabels_.size()); }

    // ---- 在预览上框选一块区域（批注 / 超链接 / 替换图片都用它定位）----
    // 打开框选模式后，在任意页面上按下拖动就能画出一个框，松手时通过 rectSelected
    // 把"第几页 + 显示空间 pt 矩形（左上原点、y 向下）"发给对话框。
    void setSelectMode(bool on);
    bool isSelectMode() const { return selectMode_; }
    int selectedPage() const { return selPage_; }        // 1 基
    QRectF selectedRect() const { return selRect_; }     // 显示空间 pt

signals:
    void pageChanged(int page, int total);
    void zoomChanged(double zoom);
    // 框选完成（page 为 1 基页号，rect 为显示空间 pt）
    void rectSelected(int page, QRectF rect);

protected:
    // Ctrl + 滚轮缩放：滚轮事件落在 QScrollArea 的 viewport 上，只能靠事件过滤器拦
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    // 重排所有页（按当前 zoom 算像素尺寸）并刷新可见页
    void render();
    void showHint(const QString& text);
    // 以 viewportPos（视口坐标）为锚点缩放：缩放前后该点对应的页面位置不动
    void zoomAt(double zoom, const QPoint& viewportPos);
    // 当前页尺寸（pt）；zoom=1 时 1pt 渲染成 1px，所以缩放比例可直接用它算
    bool pageSizePt(double* widthPt, double* heightPt) const;
    // 底部那条导航横条：建一次，之后只更新页码/缩放文字
    void createNavBar();
    void updateNavBar();

    // --- 连续滚动相关 ---
    void buildPages();            // 打开文档后按页数建占位 label
    void clearPages();            // 关文档时清掉
    void applyZoomToPages();      // 按当前 zoom 给每页设尺寸并清掉旧位图
    void updateVisiblePages();    // 只渲染视口附近那几页，远处页释放位图
    void renderPageAt(int index); // 真正渲染第 index 页
    void syncCurrentPage();       // 视口顶部落在哪页就是当前页
    // 在某个页面 label 上处理框选的按下 / 拖动 / 松手，返回 true 表示已消费事件
    bool handleSelectEvent(QLabel* label, QEvent* event);

    std::shared_ptr<pw::IPdfBackend> backend_;
    std::shared_ptr<pw::IPdfDocument> document_;
    QString path_;
    QScrollArea* scroll_ = nullptr;
    QWidget* canvas_ = nullptr;
    QVBoxLayout* canvasLayout_ = nullptr;
    QLabel* hint_ = nullptr;
    std::vector<QLabel*> pageLabels_;
    std::vector<bool> pageRendered_;
    std::vector<double> pageWpt_;
    std::vector<double> pageHpt_;
    QWidget* navBar_ = nullptr;
    QToolButton* prevBtn_ = nullptr;
    QToolButton* nextBtn_ = nullptr;
    QToolButton* zoomInBtn_ = nullptr;
    QToolButton* zoomOutBtn_ = nullptr;
    QToolButton* foldBtn_ = nullptr;
    QSpinBox* pageSpin_ = nullptr;
    QLabel* pageTotal_ = nullptr;
    QLabel* zoomText_ = nullptr;
    bool navCollapsed_ = false;
    double zoom_ = 1.5;
    int pageCount_ = 0;
    int currentPage_ = 1;

    // 框选状态
    bool selectMode_ = false;
    bool dragging_ = false;
    QPoint dragOrigin_;
    QRubberBand* rubber_ = nullptr;
    int selPage_ = 1;
    QRectF selRect_;
};

