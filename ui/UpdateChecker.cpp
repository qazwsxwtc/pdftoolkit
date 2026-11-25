#include "UpdateChecker.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRegularExpression>
#include <QUrl>

#include "I18nUi.h"

namespace {

constexpr int kTimeoutMs = 15000;
// 列表里既有 prerelease 也有正式版，按版本号自己挑；不依赖 /releases/latest（它只认正式版）
constexpr const char* kListApi =
    "https://api.github.com/repos/qazwsxwtc/pdftoolkit/releases?per_page=30";
// 兜底：等仓库发了正式版，这个接口才可用
constexpr const char* kLatestApi = "https://api.github.com/repos/qazwsxwtc/pdftoolkit/releases/latest";

struct VersionKey {
    QVector<int> nums;
    QString suffix;   // 预发布标记（beta / rc1 …），空 = 正式版
    bool ok = false;
};

VersionKey parseVersion(const QString& raw) {
    VersionKey key;
    static const QRegularExpression re(QStringLiteral("^([0-9]+(?:\\.[0-9]+)*)(.*)$"));
    const QString text = UpdateChecker::normalizeVersion(raw);
    const QRegularExpressionMatch m = re.match(text);
    if (!m.hasMatch()) return key;

    key.ok = true;
    const QStringList parts = m.captured(1).split(QLatin1Char('.'), Qt::SkipEmptyParts);
    for (const QString& part : parts) {
        bool ok = false;
        const int n = part.toInt(&ok);
        key.nums.append(ok ? n : 0);
    }
    QString rest = m.captured(2).trimmed();
    while (!rest.isEmpty() && (rest.at(0) == QLatin1Char('-') || rest.at(0) == QLatin1Char('+') ||
                               rest.at(0) == QLatin1Char('_'))) {
        rest.remove(0, 1);
    }
    key.suffix = rest;
    return key;
}

// -1 / 0 / 1：a 低于 / 等于 / 高于 b
int compareVersions(const VersionKey& a, const VersionKey& b) {
    if (a.ok != b.ok) return a.ok ? 1 : -1;
    const int count = qMax(a.nums.size(), b.nums.size());
    for (int i = 0; i < count; ++i) {
        const int left = i < a.nums.size() ? a.nums.at(i) : 0;
        const int right = i < b.nums.size() ? b.nums.at(i) : 0;
        if (left != right) return left < right ? -1 : 1;
    }
    // 数字相同：正式版比同号的预发布版新（0.0.3 > 0.0.3-beta）
    if (a.suffix.isEmpty() != b.suffix.isEmpty()) return a.suffix.isEmpty() ? 1 : -1;
    const int c = QString::compare(a.suffix, b.suffix, Qt::CaseInsensitive);
    return c < 0 ? -1 : (c > 0 ? 1 : 0);
}

// 列表里挑最大的一条；版本号相同时正式版压过预发布版
bool betterRelease(const UpdateChecker::Release& candidate,
                   const UpdateChecker::Release& current) {
    const int c = compareVersions(parseVersion(candidate.version), parseVersion(current.version));
    if (c != 0) return c > 0;
    return current.prerelease && !candidate.prerelease;
}

// 安装包优先：setup.exe > 其它 exe > zip；都没有就退回到 release 页面
QJsonObject pickAsset(const QJsonArray& assets) {
    QJsonObject best;
    int bestScore = 0;
    for (const QJsonValue& value : assets) {
        const QJsonObject asset = value.toObject();
        if (asset.value(QStringLiteral("name")).toString().isEmpty()) continue;
        const QString name = asset.value(QStringLiteral("name")).toString().toLower();
        int score = 0;
        if (name.endsWith(QStringLiteral(".exe"))) {
            score = name.contains(QStringLiteral("setup")) ? 3 : 2;
        } else if (name.endsWith(QStringLiteral(".zip"))) {
            score = 1;
        }
        if (score > bestScore) {
            bestScore = score;
            best = asset;
        }
    }
    return best;
}

// 把一个 release JSON 填成 Release；草稿、缺 tag 的返回 false
bool fillRelease(const QJsonObject& obj, UpdateChecker::Release* out) {
    if (obj.isEmpty() || obj.value(QStringLiteral("draft")).toBool()) return false;
    const QString tag = obj.value(QStringLiteral("tag_name")).toString().trimmed();
    if (tag.isEmpty()) return false;

    UpdateChecker::Release release;
    release.tag = tag;
    release.version = UpdateChecker::normalizeVersion(tag);
    release.prerelease = obj.value(QStringLiteral("prerelease")).toBool();
    release.pageUrl = obj.value(QStringLiteral("html_url")).toString();
    release.publishedAt = obj.value(QStringLiteral("published_at")).toString();
    release.notes = obj.value(QStringLiteral("body")).toString();

    const QJsonObject asset = pickAsset(obj.value(QStringLiteral("assets")).toArray());
    if (!asset.isEmpty()) {
        release.assetName = asset.value(QStringLiteral("name")).toString();
        release.assetSize = static_cast<qint64>(asset.value(QStringLiteral("size")).toDouble());
        release.downloadUrl = asset.value(QStringLiteral("browser_download_url")).toString();
    }
    if (release.downloadUrl.isEmpty()) release.downloadUrl = release.pageUrl;

    *out = release;
    return true;
}

}  // namespace

UpdateChecker::UpdateChecker(QObject* parent)
    : QObject(parent), net_(new QNetworkAccessManager(this)) {}

QString UpdateChecker::currentVersion() {
#ifdef PDFWORD_APP_VERSION
    return QStringLiteral(PDFWORD_APP_VERSION);
#else
    return QStringLiteral("0.0.4");
#endif
}

bool UpdateChecker::currentIsPrerelease() {
#ifdef PDFWORD_APP_PRERELEASE
    return true;
#else
    return false;
#endif
}

QString UpdateChecker::normalizeVersion(const QString& text) {
    QString v = text.trimmed();
    while (v.startsWith(QLatin1Char('v'), Qt::CaseInsensitive)) v.remove(0, 1);
    return v.trimmed();
}

bool UpdateChecker::isNewer(const QString& candidate, const QString& current) {
    const VersionKey a = parseVersion(candidate);
    const VersionKey b = parseVersion(current);
    if (!a.ok) return false;
    return compareVersions(a, b) > 0;
}

bool UpdateChecker::isUpgrade(const Release& release, const QString& currentVersionText) {
    const VersionKey candidate = parseVersion(release.version);
    if (!candidate.ok) return false;

    // 稳定版构建只收正式版：预发布版再新也不推。现在整批都是 prerelease，
    // 所以在发正式版之前，稳定通道（PDFWORD_PRERELEASE=OFF）不会提示任何东西。
    if (release.prerelease && !currentIsPrerelease()) return false;

    const int c = compareVersions(candidate, parseVersion(currentVersionText));
    if (c > 0) return true;
    // 同号的正式版：预发布版用户该切过去（版本号没变，但正式版才算定稿）
    if (c == 0 && !release.prerelease && currentIsPrerelease()) return true;
    return false;
}

void UpdateChecker::check(bool interactive) { check(interactive, currentVersion()); }

void UpdateChecker::check(bool interactive, const QString& currentVersionText) {
    requestReleases(interactive, currentVersionText, false);
}

void UpdateChecker::requestReleases(bool interactive, const QString& currentVersionText,
                                    bool latestOnly) {
    QNetworkRequest request(QUrl(QString::fromLatin1(latestOnly ? kLatestApi : kListApi)));
    // GitHub API 强制要求 User-Agent，缺了直接 403
    request.setRawHeader("User-Agent",
                         QStringLiteral("Pdf-Toolkit/%1").arg(currentVersionText).toLatin1());
    request.setRawHeader("Accept", "application/vnd.github+json");
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::NoLessSafeRedirectPolicy);
    request.setTransferTimeout(kTimeoutMs);

    QNetworkReply* reply = net_->get(request);
    connect(reply, &QNetworkReply::finished, this,
            [this, reply, interactive, currentVersionText, latestOnly] {
                reply->deleteLater();
                handleReply(reply, interactive, currentVersionText, latestOnly);
            });
}

void UpdateChecker::handleReply(QNetworkReply* reply, bool interactive,
                                const QString& currentVersionText, bool latestOnly) {
    // 列表这条路不通（限流、404、断网、空列表）：再试一次 /releases/latest 才判失败
    auto tryLatestOrFail = [this, interactive, currentVersionText, latestOnly](const QString& why) {
        if (!latestOnly) {
            requestReleases(interactive, currentVersionText, true);
            return;
        }
        if (interactive) emit failed(why);
    };

    if (reply->error() != QNetworkReply::NoError) {
        tryLatestOrFail(reply->errorString());
        return;
    }

    QJsonParseError parseError{};
    const QJsonDocument doc = QJsonDocument::fromJson(reply->readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError || (!doc.isArray() && !doc.isObject())) {
        tryLatestOrFail(
            QStringLiteral("GitHub 返回的内容无法解析（%1）").arg(parseError.errorString()));
        return;
    }

    Release best;
    bool found = false;
    bool skippedPrerelease = false;
    if (doc.isArray()) {
        const QJsonArray releases = doc.array();
        for (const QJsonValue& value : releases) {
            Release candidate;
            if (!fillRelease(value.toObject(), &candidate)) continue;
            if (candidate.prerelease && !currentIsPrerelease()) {
                skippedPrerelease = true;
                continue;
            }
            if (!found || betterRelease(candidate, best)) {
                best = candidate;
                found = true;
            }
        }
    } else {
        Release single;
        if (fillRelease(doc.object(), &single) &&
            !(single.prerelease && !currentIsPrerelease())) {
            best = single;
            found = true;
        } else if (single.prerelease) {
            skippedPrerelease = true;
        }
    }

    if (!found) {
        // 列表里只有预发布版（稳定通道全部跳过了）：不折腾 latest，直接把话说清楚
        if (skippedPrerelease) {
            if (interactive) emit failed(TR("目前只发布了预发布版，正式版发布后会提示"));
            return;
        }
        tryLatestOrFail(TR("仓库里还没有可用的发布版本"));
        return;
    }

    if (isUpgrade(best, currentVersionText)) {
        emit updateAvailable(best);
    } else if (interactive) {
        emit noUpdate(best.version);
    }
}
