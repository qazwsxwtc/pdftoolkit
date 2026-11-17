#pragma once

#include <QDialog>
#include <QString>
#include <memory>
#include <vector>

#include "pdfword/pdf_backend.hpp"

class QLabel;
class QListWidget;
class QSpinBox;
class QDoubleSpinBox;
class QLineEdit;

// 页面编辑器：界面上这张"输出页列表"就是后端 assemble() 的入参。
// 删除 = 从列表移除，重排 = 在列表里挪位置，旋转/裁剪 = 给该条打标记，
// 插入空白/其他 PDF 页 = 往列表里塞一条新记录。保存时才真正落盘。
class PageEditorDialog : public QDialog {
    Q_OBJECT

public:
    // source 可以带密码打开；document 用于取页数与原始尺寸（可为空，则页数从后端读）
    PageEditorDialog(std::shared_ptr<pw::IPdfBackend> backend, const QString& source,
                     std::shared_ptr<pw::IPdfDocument> document, const QString& password,
                     QWidget* parent = nullptr);

signals:
    void logMessage(const QString& text);

private slots:
    void deleteSelected();
    void moveUp();
    void moveDown();
    void moveToTop();
    void moveToBottom();
    void rotate(int degrees);
    void applyCrop();
    void insertBlank();
    void insertOtherPdf();
    void saveAs();

private:
    void rebuildList();
    int currentRow() const;
    std::vector<int> selectedRows() const;   // 升序
    void refreshSummary();
    QString describe(const pw::PageRef& ref, int index) const;
    QString suggestOutput() const;

    std::shared_ptr<pw::IPdfBackend> backend_;
    QString source_;
    QString password_;
    std::vector<pw::PageRef> pages_;
    // 源文件每页的原始尺寸，用于在列表里显示与裁剪提示（可能为空表）
    std::vector<pw::PageInfo> baseInfo_;

    QListWidget* list_ = nullptr;
    QLabel* summary_ = nullptr;
    QDoubleSpinBox* cropLeft_ = nullptr;
    QDoubleSpinBox* cropBottom_ = nullptr;
    QDoubleSpinBox* cropRight_ = nullptr;
    QDoubleSpinBox* cropTop_ = nullptr;
    QSpinBox* blankCount_ = nullptr;
    QLineEdit* rotateCustom_ = nullptr;
};
