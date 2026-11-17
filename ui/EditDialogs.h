#pragma once

#include <QRect>
#include <QString>
#include <QWidget>
#include <memory>

#include <QDialog>

#include "pdfword/pdf_backend.hpp"

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QSpinBox;
class QTableWidget;
class QTabWidget;
class QListWidget;
class PdfViewWidget;

// ---------------------------------------------------------------------------
// 「编辑与批注」菜单下的 5 个窗口。
//
// 定位方式有两种，都在同一个窗口里：
//   1) 在预览里拖一个框（点「在预览里框选」，框选结果自动填进下面的四个坐标）；
//   2) 手输页码 + 坐标（没有预览时用，比如批处理）。
// 坐标一律是**页面显示空间**的 pt：左上角是原点、y 轴向下，和预览、OCR 一套。
// ---------------------------------------------------------------------------

// 批注：高亮 / 下划线 / 删除线 / 波浪线 / 便签 / 文本框 / 手绘
class AnnotDialog : public QDialog {
    Q_OBJECT

public:
    AnnotDialog(std::shared_ptr<pw::IPdfBackend> backend, const QString& source,
                PdfViewWidget* viewer, QWidget* parent = nullptr);

signals:
    void logMessage(const QString& text);

private slots:
    void pickSelection();
    void onKindChanged();
    void apply();

private:
    void fillFromSelection(int page, QRectF rect);

    std::shared_ptr<pw::IPdfBackend> backend_;
    QString source_;
    PdfViewWidget* viewer_ = nullptr;
    bool picking_ = false;   // 正在预览里拖框：窗口暂时藏着

    QComboBox* kind_ = nullptr;
    QPlainTextEdit* text_ = nullptr;
    QComboBox* color_ = nullptr;
    QDoubleSpinBox* opacity_ = nullptr;
    QLineEdit* author_ = nullptr;
    QSpinBox* page_ = nullptr;
    QDoubleSpinBox* x0_ = nullptr;
    QDoubleSpinBox* y0_ = nullptr;
    QDoubleSpinBox* x1_ = nullptr;
    QDoubleSpinBox* y1_ = nullptr;
    QLabel* hint_ = nullptr;
};

// 超链接：外链网址 / 页内跳转
class LinkDialog : public QDialog {
    Q_OBJECT

public:
    LinkDialog(std::shared_ptr<pw::IPdfBackend> backend, const QString& source, PdfViewWidget* viewer,
               QWidget* parent = nullptr);

signals:
    void logMessage(const QString& text);

private slots:
    void pickSelection();
    void apply();

private:
    void fillFromSelection(int page, QRectF rect);

    std::shared_ptr<pw::IPdfBackend> backend_;
    QString source_;
    PdfViewWidget* viewer_ = nullptr;
    bool picking_ = false;

    QLineEdit* uri_ = nullptr;
    QSpinBox* page_ = nullptr;
    QDoubleSpinBox* x0_ = nullptr;
    QDoubleSpinBox* y0_ = nullptr;
    QDoubleSpinBox* x1_ = nullptr;
    QDoubleSpinBox* y1_ = nullptr;
    QLabel* hint_ = nullptr;
};

// 查找替换：先查一遍看有多少处，再批量替换 / 只删除
class FindReplaceDialog : public QDialog {
    Q_OBJECT

public:
    FindReplaceDialog(std::shared_ptr<pw::IPdfBackend> backend, const QString& source,
                      QWidget* parent = nullptr);

signals:
    void logMessage(const QString& text);

private slots:
    void previewFind();
    void applyReplace();

private:
    std::shared_ptr<pw::IPdfBackend> backend_;
    QString source_;

    QLineEdit* find_ = nullptr;
    QLineEdit* replace_ = nullptr;
    QCheckBox* deleteOnly_ = nullptr;
    QCheckBox* caseSensitive_ = nullptr;
    QDoubleSpinBox* fontSize_ = nullptr;
    QComboBox* color_ = nullptr;
    QSpinBox* limit_ = nullptr;
    QLabel* result_ = nullptr;
};

// 替换图片：把页面上某块区域的图片换成新图
class ReplaceImageDialog : public QDialog {
    Q_OBJECT

public:
    ReplaceImageDialog(std::shared_ptr<pw::IPdfBackend> backend, const QString& source,
                       PdfViewWidget* viewer, QWidget* parent = nullptr);

signals:
    void logMessage(const QString& text);

private slots:
    void pickSelection();
    void pickImage();
    void apply();

private:
    void fillFromSelection(int page, QRectF rect);

    std::shared_ptr<pw::IPdfBackend> backend_;
    QString source_;
    PdfViewWidget* viewer_ = nullptr;
    bool picking_ = false;

    QLineEdit* image_ = nullptr;
    QSpinBox* page_ = nullptr;
    QDoubleSpinBox* x0_ = nullptr;
    QDoubleSpinBox* y0_ = nullptr;
    QDoubleSpinBox* x1_ = nullptr;
    QDoubleSpinBox* y1_ = nullptr;
    QLabel* hint_ = nullptr;
};

// 表单处理：填写 / 制作可交互表单 / 提取数据
class FormDialog : public QDialog {
    Q_OBJECT

public:
    FormDialog(std::shared_ptr<pw::IPdfBackend> backend, const QString& source,
               PdfViewWidget* viewer, QWidget* parent = nullptr);

signals:
    void logMessage(const QString& text);

private slots:
    void reloadFields();
    void fillForm();
    void addFieldSpec();
    void removeFieldSpec();
    void createForm();
    void exportCsv();

private:
    std::shared_ptr<pw::IPdfBackend> backend_;
    QString source_;
    PdfViewWidget* viewer_ = nullptr;
    bool picking_ = false;

    QTabWidget* tabs_ = nullptr;
    // 填写
    QTableWidget* fillTable_ = nullptr;
    QLabel* fillHint_ = nullptr;
    // 制作
    QLineEdit* fieldName_ = nullptr;
    QComboBox* fieldKind_ = nullptr;
    QSpinBox* fieldPage_ = nullptr;
    QDoubleSpinBox* fieldX0_ = nullptr;
    QDoubleSpinBox* fieldY0_ = nullptr;
    QDoubleSpinBox* fieldX1_ = nullptr;
    QDoubleSpinBox* fieldY1_ = nullptr;
    QLineEdit* fieldOptions_ = nullptr;
    QListWidget* specList_ = nullptr;
    // 提取
    QTableWidget* extractTable_ = nullptr;

    std::vector<pw::FormField> fields_;
};
