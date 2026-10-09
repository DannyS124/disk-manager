// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#include "rescuestick.h"

#include "applog.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>

#include <cerrno>
#include <fcntl.h>
#include <unistd.h>

namespace {

constexpr quint64 kMaxSmallFile = 4 * 1024 * 1024; // sha256sum.txt and the info file

// Sticks and images made before the rename to Bluespark have the old name. Both work.
const QStringList kInfoPaths = {QStringLiteral(".disk/bluespark"), QStringLiteral(".disk/diskforge-rescue")};
const QString kSumsPath = QStringLiteral("sha256sum.txt");

QByteArray readEntry(int fd, const isofs::Entry &entry)
{
    if (entry.size > kMaxSmallFile)
        return {};
    QByteArray data(qsizetype(entry.size), '\0');
    qsizetype done = 0;
    while (done < data.size()) {
        const ssize_t n = ::pread(fd, data.data() + done, size_t(data.size() - done), off_t(entry.offset + done));
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            return {};
        done += n;
    }
    return data;
}

} // namespace

QString rescue::rescueConfPath = QStringLiteral("/etc/bluespark.conf");

bool rescue::runningInRescue()
{
    return QFileInfo::exists(rescueConfPath);
}

QString rescue::notInRescueReason()
{
    return QObject::tr("Bluespark starts fresh from the stick every time, so this would only change the copy in memory.");
}

QString rescue::runningBuildId()
{
    QFile conf(rescueConfPath);
    if (!conf.open(QIODevice::ReadOnly))
        return {};
    for (const QByteArray &line : conf.read(4096).split('\n')) {
        if (line.startsWith("BUILD_ID="))
            return QString::fromUtf8(line.mid(9).trimmed());
    }
    return {};
}

rescue::Info rescue::parseInfo(const QByteArray &text)
{
    Info info;
    const QList<QByteArray> lines = text.split('\n');
    const QByteArray header = lines.value(0).trimmed();
    if (header != "Bluespark" && header != "DiskForge Rescue")
        return info;
    for (const QByteArray &line : lines) {
        const qsizetype eq = line.indexOf('=');
        if (eq <= 0)
            continue;
        const QByteArray key = line.left(eq).trimmed();
        // Shown in the dialog, so only plain characters, and not too many.
        QString value = QString::fromUtf8(line.mid(eq + 1).trimmed()).left(64);
        value.removeIf([](QChar c) { return !(c.isLetterOrNumber() || c == QLatin1Char('.') || c == QLatin1Char('-')); });
        if (key == "version")
            info.version = value;
        else if (key == "built")
            info.built = value;
        else if (key == "id")
            info.id = value;
    }
    return info;
}

QHash<QString, QByteArray> rescue::parseSums(const QByteArray &text)
{
    QHash<QString, QByteArray> sums;
    for (const QByteArray &line : text.split('\n')) {
        // "<64 hex digits>  ./path" (two spaces, or a space and a '*' for binary mode)
        if (line.size() < 67 || line.at(64) != ' ' || (line.at(65) != ' ' && line.at(65) != '*'))
            continue;
        const QByteArray hash = line.left(64).toLower();
        if (QByteArray::fromHex(hash).toHex() != hash) // fromHex skips what isn't hex
            continue;
        QString path = QString::fromUtf8(line.mid(66).trimmed());
        if (path.startsWith(QLatin1String("./")))
            path.remove(0, 2);
        if (path.isEmpty() || path.startsWith(QLatin1Char('/')) || path.split(QLatin1Char('/')).contains(QLatin1String("..")))
            continue;
        sums.insert(path, hash);
    }
    return sums;
}

rescue::Image rescue::inspect(const QString &isoPath)
{
    Image image;
    // A running rescue stick's own folder (copying the stick DiskForge runs from).
    if (QFileInfo(isoPath).isDir()) {
        image.info = stickInfo(isoPath);
        QFile sums(isoPath + QLatin1Char('/') + kSumsPath);
        if (!image.info.valid() || !sums.open(QIODevice::ReadOnly)) {
            image.error = QObject::tr("This folder doesn't hold Bluespark.");
            return image;
        }
        const QHash<QString, QByteArray> listed = parseSums(sums.read(kMaxSmallFile));
        for (auto it = listed.cbegin(); it != listed.cend(); ++it)
            image.bytes += quint64(QFileInfo(isoPath + QLatin1Char('/') + it.key()).size());
        return image;
    }
    const int fd = ::open(QFile::encodeName(isoPath).constData(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        image.error = QObject::tr("Couldn't open %1.").arg(QFileInfo(isoPath).fileName());
        return image;
    }
    image.listing = isofs::list(fd);
    if (!image.listing.ok()) {
        image.error = image.listing.error;
        ::close(fd);
        return image;
    }
    const isofs::Entry *info = nullptr;
    for (const QString &path : kInfoPaths)
        info = info ? info : isofs::find(image.listing, path);
    const isofs::Entry *sums = isofs::find(image.listing, kSumsPath);
    if (info && !info->isDir)
        image.info = parseInfo(readEntry(fd, *info));
    ::close(fd);
    if (!image.info.valid() || !sums) {
        image.error = QObject::tr("This isn't a Bluespark image. For other ISOs, use Write Image to USB.");
        return image;
    }
    for (const isofs::Entry &e : image.listing.entries) {
        if (!e.isDir)
            image.bytes += e.size;
    }
    return image;
}

rescue::Info rescue::stickInfo(const QString &root)
{
    for (const QString &path : kInfoPaths) {
        QFile file(root + QLatin1Char('/') + path);
        if (file.open(QIODevice::ReadOnly))
            return parseInfo(file.read(4096));
    }
    return {};
}

int rescue::logFolders(const QString &root)
{
    return int(QDir(root + QStringLiteral("/logs")).entryList(QDir::Dirs | QDir::NoDotAndDotDot).size());
}

rescue::StickWriter::StickWriter(const QString &source, const QString &stickRoot)
    : m_source(source)
    , m_root(stickRoot)
{
}

void rescue::StickWriter::cancel()
{
    m_cancel = true;
    if (filecopy::Copier *copier = m_copier)
        copier->cancel();
}

void rescue::StickWriter::finish(bool ok, const QString &message)
{
    qCInfo(lcOps).noquote() << "Make a Bluespark USB" << (ok ? "finished:" : "failed:") << message;
    emit finished(ok, message);
}

void rescue::StickWriter::run()
{
    const std::unique_ptr<filecopy::Source> source =
        QFileInfo(m_source).isDir() ? filecopy::openFolder(m_source) : filecopy::openIso(m_source);
    if (!source->error().isEmpty())
        return finish(false, source->error());
    auto find = [&source](const QString &path) -> const filecopy::Entry * {
        for (const filecopy::Entry &e : source->entries()) {
            if (e.path == path && !e.isDir)
                return &e;
        }
        return nullptr;
    };
    auto readSmall = [&source](const filecopy::Entry *e) {
        QByteArray data;
        if (e && e->size <= kMaxSmallFile) {
            data.resize(qsizetype(e->size));
            if (!source->read(*e, 0, data.data(), data.size()))
                data.clear();
        }
        return data;
    };
    const QHash<QString, QByteArray> sums = parseSums(readSmall(find(kSumsPath)));
    if (sums.isEmpty() || (!find(kInfoPaths[0]) && !find(kInfoPaths[1])))
        return finish(false, tr("This isn't a Bluespark image."));
    for (auto it = sums.cbegin(); it != sums.cend(); ++it) {
        if (!find(it.key()))
            return finish(false, tr("The image is damaged: %1 is missing. Download it again.").arg(it.key()));
    }

    // What goes onto the stick: everything sha256sum.txt lists, plus that file itself. The
    // boot catalog xorriso adds is only for CDs, so it's left out.
    filecopy::Options options;
    options.expected = sums;
    options.skip = [&sums](const QString &path) { return path != kSumsPath && !sums.contains(path); };
    filecopy::Copier copier(source.get(), m_root, options);
    connect(&copier, &filecopy::Copier::progress, this, &StickWriter::progress);
    bool ok = false;
    QString message;
    connect(&copier, &filecopy::Copier::finished, this, [&](bool copied, const QString &text) {
        ok = copied;
        message = text;
    });
    qCInfo(lcOps).noquote() << "Make a Bluespark USB: copying from" << m_source << "to" << m_root;
    m_copier = &copier;
    if (m_cancel)
        copier.cancel();
    copier.run();
    m_copier = nullptr;
    if (!ok)
        return finish(false, message);

    // Where the rescue system leaves its notes.
    QDir(m_root).mkpath(QStringLiteral("logs"));
    QFile note(m_root + QStringLiteral("/logs/README.txt"));
    if (note.open(QIODevice::WriteOnly | QIODevice::Text)) {
        note.write("Bluespark keeps its notes here: one folder for each time this stick started a PC,\n"
                   "named by the date and the PC's model. summary.txt in each one is the place to start.\n");
        note.close();
    }
    const int rootFd = ::open(QFile::encodeName(m_root).constData(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (rootFd >= 0) {
        ::syncfs(rootFd);
        ::close(rootFd);
    }
    finish(true, message);
}
