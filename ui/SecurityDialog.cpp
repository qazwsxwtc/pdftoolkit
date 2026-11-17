#include "SecurityDialog.h"

#include <filesystem>

#include <QCheckBox>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QVBoxLayout>

#include "DialogCommon.h"

namespace {

// 权限位与 core 的 pw::Permission 一致
constexpr int kPermPrint = 1 << 2;
constexpr int kPermModify = 1 << 3;
constexpr int kPermCopy = 1 << 4;
constexpr int kPermAnnotate = 1 << 5;

}  // namespace

SecurityDialog::SecurityDialog(std::shared_ptr<pw::IPdfBackend> backend, const QString& source,
                               QWidget* parent)
    : QDialog(parent), backend_(std::move(backend)), source_(source) {
    setWindowTitle(QStringLiteral("加密 / 解密 — %1").arg(QFileInfo(source_).fileName()));
    resize(560, 340);

    auto* root = new QVBoxLayout(this);
    root->addWidget(new QLabel(QStringLiteral("加密：AES-128，按下面的权限位限制他人操作。"), this));
    root->addWidget(new QLabel(QStringLiteral("解密：输入源文件当前的打开密码，另存为不加密的副本。"), this));

    auto* form = new QFormLayout();
    userPassword_ = new QLineEdit(this);
    ownerPassword_ = new QLineEdit(this);
    userPassword_->setObjectName(QStringLiteral("userPassword"));
    ownerPassword_->setObjectName(QStringLiteral("ownerPassword"));
    userPassword_->setEchoMode(QLineEdit::Password);
    ownerPassword_->setEchoMode(QLineEdit::Password);
    userPassword_->setPlaceholderText(QStringLiteral("打开文档时要输入的密码"));
    ownerPassword_->setPlaceholderText(QStringLiteral("留空则用打开密码代替"));
    form->addRow(QStringLiteral("打开密码"), userPassword_);
    form->addRow(QStringLiteral("所有者密码"), ownerPassword_);

    allowPrint_ = new QCheckBox(QStringLiteral("允许打印"), this);
    allowCopy_ = new QCheckBox(QStringLiteral("允许复制文本"), this);
    allowModify_ = new QCheckBox(QStringLiteral("允许修改"), this);
    allowAnnotate_ = new QCheckBox(QStringLiteral("允许批注"), this);
    allowPrint_->setChecked(true);
    allowCopy_->setChecked(true);
    auto* permRow = new QHBoxLayout();
    permRow->addWidget(allowPrint_);
    permRow->addWidget(allowCopy_);
    permRow->addWidget(allowModify_);
    permRow->addWidget(allowAnnotate_);
    form->addRow(QStringLiteral("权限"), permRow);
    root->addLayout(form);

    auto* row = new QHBoxLayout();
    auto* encryptBtn = new QPushButton(QStringLiteral("加密另存为…"), this);
    auto* decryptBtn = new QPushButton(QStringLiteral("解密另存为…"), this);
    row->addWidget(encryptBtn);
    row->addWidget(decryptBtn);
    row->addStretch(1);
    root->addLayout(row);
    root->addStretch(1);

    auto* box = new QDialogButtonBox(QDialogButtonBox::Close, this);
    box->button(QDialogButtonBox::Close)->setText(QStringLiteral("关闭"));
    root->addWidget(box);

    connect(encryptBtn, &QPushButton::clicked, this, &SecurityDialog::encryptPdf);
    connect(decryptBtn, &QPushButton::clicked, this, &SecurityDialog::decryptPdf);
    connect(box, &QDialogButtonBox::rejected, this, &QDialog::reject);
}

void SecurityDialog::encryptPdf() {
    const QString user = userPassword_->text();
    if (user.isEmpty() && ownerPassword_->text().isEmpty()) {
        QMessageBox::information(this, QStringLiteral("需要密码"),
                                 QStringLiteral("至少填一个打开密码或所有者密码"));
        return;
    }
    const QString out = QFileDialog::getSaveFileName(this, QStringLiteral("加密输出"),
                                                     suggestPdfOutput(source_, QStringLiteral("_加密")),
                                                     QStringLiteral("PDF (*.pdf)"));
    if (out.isEmpty()) return;

    int perms = 0;
    if (allowPrint_->isChecked()) perms |= kPermPrint;
    if (allowCopy_->isChecked()) perms |= kPermCopy;
    if (allowModify_->isChecked()) perms |= kPermModify;
    if (allowAnnotate_->isChecked()) perms |= kPermAnnotate;
    // 其余位（表单/装配/高清打印）按"与打印一致"给出，避免出现半开半闭的组合
    if (allowPrint_->isChecked()) perms |= (1 << 11);
    if (allowModify_->isChecked()) perms |= (1 << 8) | (1 << 10);

    const pw::Status st =
        backend_->encrypt(std::filesystem::path(source_.toStdString()),
                          std::filesystem::path(out.toStdString()), user.toStdString(),
                          ownerPassword_->text().toStdString(), perms);
    if (!st.ok()) {
        QMessageBox::warning(this, QStringLiteral("加密失败"), QString::fromStdString(st.toString()));
        return;
    }
    emit logMessage(QStringLiteral("已加密（AES-128）-> ") + out);
}

void SecurityDialog::decryptPdf() {
    // 解密要读源文件，密码直接取窗口里的"打开密码"字段
    const QString pwd = userPassword_->text();
    if (pwd.isEmpty()) {
        QMessageBox::information(this, QStringLiteral("需要密码"),
                                 QStringLiteral("先在“打开密码”里填该 PDF 当前的密码"));
        return;
    }
    const QString out = QFileDialog::getSaveFileName(this, QStringLiteral("解密输出"),
                                                     suggestPdfOutput(source_, QStringLiteral("_解密")),
                                                     QStringLiteral("PDF (*.pdf)"));
    if (out.isEmpty()) return;

    const pw::Status st = backend_->decrypt(std::filesystem::path(source_.toStdString()),
                                            std::filesystem::path(out.toStdString()),
                                            pwd.toStdString());
    if (!st.ok()) {
        QMessageBox::warning(this, QStringLiteral("解密失败"), QString::fromStdString(st.toString()));
        return;
    }
    emit logMessage(QStringLiteral("已解密 -> ") + out);
}
