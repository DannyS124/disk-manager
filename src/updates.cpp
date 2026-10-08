// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#include "updates.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QVersionNumber>

namespace {

QString stripV(QString version)
{
    if (version.startsWith(QLatin1Char('v')))
        version.remove(0, 1);
    return version;
}

} // namespace

bool isNewerVersion(const QString &latest, const QString &current)
{
    return QVersionNumber::fromString(stripV(latest)) > QVersionNumber::fromString(stripV(current));
}

UpdateChecker::UpdateChecker(QObject *parent)
    : QObject(parent)
{
}

void UpdateChecker::check()
{
    QNetworkRequest request(QUrl(QStringLiteral("https://api.github.com/repos/DannyS124/diskforge/releases/latest")));
    request.setRawHeader("Accept", "application/vnd.github+json");
    request.setRawHeader("User-Agent", "diskforge/" APP_VERSION); // GitHub rejects requests without one
    request.setTransferTimeout(15000);

    QNetworkReply *reply = m_network.get(request);
    connect(reply, &QNetworkReply::finished, this, [this, reply] {
        reply->deleteLater();
        if (reply->error() == QNetworkReply::ContentNotFoundError) {
            emit finished({}, {}, tr("There's no release on GitHub right now."));
            return;
        }
        if (reply->error() != QNetworkReply::NoError) {
            emit finished({}, {}, reply->errorString());
            return;
        }
        const QJsonObject release = QJsonDocument::fromJson(reply->readAll()).object();
        const QString tag = release.value(QStringLiteral("tag_name")).toString();
        if (tag.isEmpty()) {
            emit finished({}, {}, tr("GitHub sent an unexpected reply"));
            return;
        }
        emit finished(stripV(tag), release.value(QStringLiteral("html_url")).toString(), {});
    });
}
