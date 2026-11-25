#pragma once

#include <QObject>
#include <QString>

class QNetworkAccessManager;
class QNetworkReply;

// 从 GitHub Releases 查最新版本。
//
// 仓库的发布方式会变：现在整批都是 prerelease（GitHub 的 /releases/latest 会排除它们，直接 404），
// 以后会发正式版。所以两条路都要能走：
//   1) 主路径：/releases 列表（预发布版、正式版都在里面），自己按版本号挑最大的一个；
//   2) 兜底：列表拿不到（404 / 空）时再试 /releases/latest（有正式版时它才可用）。
// 同一版本号同时存在预发布版和正式版时，正式版优先。
class UpdateChecker : public QObject {
    Q_OBJECT

public:
    struct Release {
        QString version;      // 归一化版本号：tag v0.0.3 / V0.0.3 -> 0.0.3
        QString tag;
        QString pageUrl;      // releases/tag/... 页面
        QString downloadUrl;  // 优先安装包，其次压缩包，都没有就退回页面地址
        QString assetName;
        qint64 assetSize = 0;
        QString publishedAt;
        QString notes;
        bool prerelease = false;  // GitHub 上的 prerelease 标记
    };

    explicit UpdateChecker(QObject* parent = nullptr);

    // 当前程序版本（来自 CMake 的 project(VERSION)，见 PDFWORD_APP_VERSION）
    static QString currentVersion();
    // 当前构建是不是按 prerelease 发的（CMake 的 PDFWORD_PRERELEASE，发正式版时关掉）
    static bool currentIsPrerelease();
    // 去掉空白与开头的 v/V，得到可比较的版本串
    static QString normalizeVersion(const QString& text);
    // candidate 是否比 current 新（数字段逐段比，正式版 > 同号预发布版）
    static bool isNewer(const QString& candidate, const QString& current);
    // 这个 release 值不值得让当前版本升级（比 isNewer 多认两种情况：
    // 预发布版用户等到了同号的正式版要提示；稳定版用户不收预发布版）
    static bool isUpgrade(const Release& release, const QString& currentVersion);

    // interactive=false：启动时的静默检查，只有真发现新版本才发信号，其它情况一声不吭。
    // interactive=true ：手动「检查更新」，没新版本/失败也要给界面一个说法。
    void check(bool interactive);
    // 同上，但拿指定版本当"当前版本"（测试和非 UI 入口用；界面里走上面那个）
    void check(bool interactive, const QString& currentVersion);

signals:
    void updateAvailable(const UpdateChecker::Release& release);
    void noUpdate(const QString& latestVersion);
    void failed(const QString& reason);

private:
    void requestReleases(bool interactive, const QString& currentVersion, bool latestOnly);
    void handleReply(QNetworkReply* reply, bool interactive, const QString& currentVersion,
                     bool latestOnly);

    QNetworkAccessManager* net_ = nullptr;
};
