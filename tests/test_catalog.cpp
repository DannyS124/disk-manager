// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

// The add-on catalog, offline: what the list accepts and what an install checks.

#include "testkit.h"

#include "../src/addons.h"

#include <QCryptographicHash>
#include <QFile>
#include <QProcess>
#include <QTemporaryDir>

namespace {

const QString kCommit = QStringLiteral("0123456789abcdef0123456789abcdef01234567");

QString pinned(const QString &path)
{
    return QStringLiteral("https://raw.githubusercontent.com/DannyS124/diskforge-addons/%1/%2").arg(kCommit, path);
}

QString sha(const QByteArray &data)
{
    return QString::fromLatin1(QCryptographicHash::hash(data, QCryptographicHash::Sha256).toHex());
}

QByteArray entry(const QString &id, const QString &url, const QString &hash)
{
    return QStringLiteral(R"({"id":"%1","name":"N","version":"1.0","url":"%2","sha256":"%3"})").arg(id, url, hash).toUtf8();
}

} // namespace

void catalogTests()
{
    QTemporaryDir data;
    qputenv("XDG_DATA_HOME", QFile::encodeName(data.path()));

    const QByteArray manifest = R"({"id":"hello","name":"Hello","actions":[{"label":"Say hello","command":["echo","{device}"]}]})";
    const QString good = sha(manifest);
    const QByteArray list = "{\"format\":\"diskforge-addon-catalog\",\"version\":1,\"addons\":["
        + entry(QStringLiteral("hello"), pinned(QStringLiteral("addons/hello/addon.json")), good) + ","
        + entry(QStringLiteral("other"), pinned(QStringLiteral("addons/other/addon.json")), good) + ","
        + entry(QStringLiteral("plain-http"), pinned(QStringLiteral("a.json")).replace(QStringLiteral("https"), QStringLiteral("http")), good) + ","
        + entry(QStringLiteral("elsewhere"), QStringLiteral("https://evil.example.com/%1/a.json").arg(kCommit), good) + ","
        + entry(QStringLiteral("branch"), QStringLiteral("https://raw.githubusercontent.com/DannyS124/diskforge-addons/main/a.json"), good) + ","
        + entry(QStringLiteral("dots"), pinned(QStringLiteral("../../other-repo/a.json")), good) + ","
        + entry(QStringLiteral("short-hash"), pinned(QStringLiteral("a.json")), good.left(63)) + ","
        + entry(QStringLiteral("Bad Id"), pinned(QStringLiteral("a.json")), good) + "]}";
    QString error;
    const QVector<CatalogEntry> entries = Addons::parseCatalog(list, &error);
    report(entries.size() == 2 && entries[0].id == QLatin1String("hello"), QStringLiteral("only add-ons pinned to a commit of the catalog repo are listed"),
           QString::number(entries.size()));
    report(error.contains(QLatin1String("6")), QStringLiteral("the rest are counted as left out"), error);
    error.clear();
    report(Addons::parseCatalog(R"({"format":"something-else","addons":[]})", &error).isEmpty() && !error.isEmpty(),
           QStringLiteral("a file that isn't a catalog is refused"), error);
    error.clear();
    report(Addons::parseCatalog(QByteArray(Addons::kMaxDownload + 1, ' '), &error).isEmpty() && error.contains(QLatin1String("big")),
           QStringLiteral("an oversized list is refused"), error);

    const QString installed = data.filePath(QStringLiteral("diskforge/addons/hello/addon.json"));
    CatalogEntry hello = entries.value(0);
    error.clear();
    CatalogEntry wrong = hello;
    wrong.sha256 = QString(64, QLatin1Char('0'));
    report(!Addons::installVerified(manifest, wrong, &error) && !QFile::exists(installed), QStringLiteral("a checksum mismatch isn't installed"), error);
    error.clear();
    CatalogEntry renamed = hello;
    renamed.id = QStringLiteral("not-hello");
    report(!Addons::installVerified(manifest, renamed, &error) && error.contains(QLatin1String("calls itself")),
           QStringLiteral("an add-on whose id doesn't match isn't installed"), error);
    error.clear();
    const QByteArray huge = QByteArray(Addons::kMaxDownload + 10, ' ');
    CatalogEntry big = hello;
    big.sha256 = sha(huge);
    report(!Addons::installVerified(huge, big, &error), QStringLiteral("an oversized download isn't installed"), error);
    error.clear();
    const QByteArray broken = R"({"id":"hello","actions":[{"label":"X","command":["rm","{nope}"]}]})";
    CatalogEntry brokenEntry = hello;
    brokenEntry.sha256 = sha(broken);
    report(!Addons::installVerified(broken, brokenEntry, &error) && error.contains(QLatin1String("placeholder")),
           QStringLiteral("a broken add-on isn't installed"), error);
    error.clear();
    report(Addons::installVerified(manifest, hello, &error) && Addons::parse(installed).error.isEmpty(),
           QStringLiteral("a matching add-on installs and parses"), error);

    // The real catalog, if the add-ons repository sits next to this one: every entry has to
    // go through the same checks, using the file exactly as it is in the pinned commit.
    const QString repo = QStringLiteral(SOURCE_DIR "/../diskforge-addons");
    QFile real(repo + QStringLiteral("/catalog.json"));
    if (!real.open(QIODevice::ReadOnly)) {
        out << "SKIP  the diskforge-addons repository isn't next to this one" << Qt::endl;
        return;
    }
    error.clear();
    const QVector<CatalogEntry> listed = Addons::parseCatalog(real.readAll(), &error);
    report(!listed.isEmpty() && error.isEmpty(), QStringLiteral("the real catalog is accepted whole"), QStringLiteral("%1 add-on(s) %2").arg(listed.size()).arg(error));
    for (const CatalogEntry &e : listed) {
        const QString commitAndPath = e.url.section(QLatin1Char('/'), 5, 5) + QLatin1Char(':') + e.url.section(QLatin1Char('/'), 6);
        QProcess git;
        git.start(QStringLiteral("git"), {QStringLiteral("-C"), repo, QStringLiteral("show"), commitAndPath});
        git.waitForFinished();
        error.clear();
        report(Addons::installVerified(git.readAllStandardOutput(), e, &error), QStringLiteral("catalog entry %1 installs").arg(e.id), error);
    }
}
