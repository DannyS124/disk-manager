// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#include "rescuestick.h"

#include "applog.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QStorageInfo>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>

namespace {

constexpr qsizetype kChunk = 4 * 1024 * 1024;
constexpr quint64 kMaxSmallFile = 4 * 1024 * 1024; // sha256sum.txt and the info file

const QString kInfoPath = QStringLiteral(".disk/diskforge-rescue");
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

bool writeAll(int fd, const char *data, qsizetype size)
{
    while (size > 0) {
        const ssize_t n = ::write(fd, data, size_t(size));
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            return false;
        data += n;
        size -= n;
    }
    return true;
}

} // namespace

rescue::Info rescue::parseInfo(const QByteArray &text)
{
    Info info;
    const QList<QByteArray> lines = text.split('\n');
    if (lines.value(0).trimmed() != "DiskForge Rescue")
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
    const isofs::Entry *info = isofs::find(image.listing, kInfoPath);
    const isofs::Entry *sums = isofs::find(image.listing, kSumsPath);
    if (info && !info->isDir)
        image.info = parseInfo(readEntry(fd, *info));
    ::close(fd);
    if (!image.info.valid() || !sums) {
        image.error = QObject::tr("This isn't a DiskForge Rescue image. For other ISOs, use Write Image to USB.");
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
    QFile file(root + QLatin1Char('/') + kInfoPath);
    if (!file.open(QIODevice::ReadOnly))
        return {};
    return parseInfo(file.read(4096));
}

int rescue::logFolders(const QString &root)
{
    return int(QDir(root + QStringLiteral("/logs")).entryList(QDir::Dirs | QDir::NoDotAndDotDot).size());
}

rescue::StickWriter::StickWriter(const QString &isoPath, const QString &stickRoot)
    : m_iso(isoPath)
    , m_root(stickRoot)
{
}

void rescue::StickWriter::finish(bool ok, const QString &message)
{
    qCInfo(lcOps).noquote() << "Make a Rescue USB" << (ok ? "finished:" : "failed:") << message;
    emit finished(ok, message);
}

bool rescue::StickWriter::copy(int iso, const isofs::Entry &entry, const QByteArray &expected, quint64 &done, quint64 total)
{
    const QString target = m_root + QLatin1Char('/') + entry.path;
    const int out = ::open(QFile::encodeName(target).constData(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (out < 0) {
        m_error = tr("Couldn't write %1 to the stick: %2").arg(entry.path, QString::fromLocal8Bit(strerror(errno)));
        return false;
    }
    QCryptographicHash hash(QCryptographicHash::Sha256);
    QByteArray buffer(kChunk, Qt::Uninitialized);
    quint64 copied = 0;
    bool ok = true;
    while (ok && copied < entry.size) {
        if (m_cancel) {
            ok = false;
            break;
        }
        const qsizetype want = qsizetype(qMin<quint64>(quint64(kChunk), entry.size - copied));
        const ssize_t n = ::pread(iso, buffer.data(), size_t(want), off_t(entry.offset + copied));
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0) {
            m_error = tr("Couldn't read %1 from the image.").arg(entry.path);
            ok = false;
            break;
        }
        hash.addData(QByteArrayView(buffer.constData(), n));
        if (!writeAll(out, buffer.constData(), n)) {
            m_error = tr("Couldn't write %1 to the stick: %2").arg(entry.path, QString::fromLocal8Bit(strerror(errno)));
            ok = false;
            break;
        }
        copied += quint64(n);
        done += quint64(n);
        emit progress(tr("Copying"), done, total);
    }
    // On the stick for real, and out of the page cache, so the check reads the stick itself.
    if (ok && ::fsync(out) != 0) {
        m_error = tr("Couldn't write %1 to the stick: %2").arg(entry.path, QString::fromLocal8Bit(strerror(errno)));
        ok = false;
    }
    ::posix_fadvise(out, 0, 0, POSIX_FADV_DONTNEED);
    ::close(out);
    if (ok && !expected.isEmpty() && hash.result().toHex() != expected) {
        m_error = tr("The image is damaged: %1 doesn't match its checksum. Download it again.").arg(entry.path);
        ok = false;
    }
    return ok;
}

bool rescue::StickWriter::check(const QString &path, const QByteArray &expected, quint64 &done, quint64 total)
{
    const int in = ::open(QFile::encodeName(m_root + QLatin1Char('/') + path).constData(), O_RDONLY | O_CLOEXEC);
    if (in < 0) {
        m_error = tr("%1 isn't on the stick after writing it. The stick may be failing; try another one.").arg(path);
        return false;
    }
    ::posix_fadvise(in, 0, 0, POSIX_FADV_DONTNEED);
    QCryptographicHash hash(QCryptographicHash::Sha256);
    QByteArray buffer(kChunk, Qt::Uninitialized);
    bool ok = true;
    for (;;) {
        if (m_cancel) {
            ok = false;
            break;
        }
        const ssize_t n = ::read(in, buffer.data(), size_t(buffer.size()));
        if (n < 0 && errno == EINTR)
            continue;
        if (n < 0) {
            m_error = tr("Couldn't read %1 back from the stick. It may be failing; try another one.").arg(path);
            ok = false;
            break;
        }
        if (n == 0)
            break;
        hash.addData(QByteArrayView(buffer.constData(), n));
        done += quint64(n);
        emit progress(tr("Checking"), done, total);
    }
    ::close(in);
    if (ok && hash.result().toHex() != expected) {
        m_error = tr("The stick gave back something different from what was written to %1. "
                     "It may be failing; try another one.").arg(path);
        ok = false;
    }
    return ok;
}

void rescue::StickWriter::run()
{
    const int iso = ::open(QFile::encodeName(m_iso).constData(), O_RDONLY | O_CLOEXEC);
    if (iso < 0)
        return finish(false, tr("Couldn't open %1.").arg(QFileInfo(m_iso).fileName()));
    const isofs::Listing listing = isofs::list(iso);
    const isofs::Entry *sumsEntry = listing.ok() ? isofs::find(listing, kSumsPath) : nullptr;
    const QByteArray sumsText = sumsEntry && !sumsEntry->isDir ? readEntry(iso, *sumsEntry) : QByteArray();
    const QHash<QString, QByteArray> sums = parseSums(sumsText);
    if (!listing.ok() || sums.isEmpty() || !isofs::find(listing, kInfoPath)) {
        ::close(iso);
        return finish(false, listing.ok() ? tr("This isn't a DiskForge Rescue image.") : listing.error);
    }

    // What goes onto the stick: everything sha256sum.txt lists, plus that file itself. The
    // boot catalog xorriso adds is only for CDs, so it's left out.
    QVector<const isofs::Entry *> files;
    quint64 total = 0;
    for (auto it = sums.cbegin(); it != sums.cend(); ++it) {
        const isofs::Entry *e = isofs::find(listing, it.key());
        if (!e || e->isDir) {
            ::close(iso);
            return finish(false, tr("The image is damaged: %1 is missing. Download it again.").arg(it.key()));
        }
        files.push_back(e);
        total += e->size;
    }
    files.push_back(sumsEntry);
    total += sumsEntry->size;
    std::sort(files.begin(), files.end(), [](const isofs::Entry *a, const isofs::Entry *b) { return a->path < b->path; });

    const QStorageInfo storage(m_root);
    if (storage.isValid() && quint64(storage.bytesAvailable()) < total + 16 * 1024 * 1024) {
        ::close(iso);
        return finish(false, tr("The stick is too small: DiskForge Rescue needs %1 MB.").arg(total / 1000000 + 16));
    }

    QDir root(m_root);
    for (const isofs::Entry *e : std::as_const(files)) {
        const QString folder = QFileInfo(e->path).path();
        if (folder != QLatin1String(".") && !root.mkpath(folder)) {
            ::close(iso);
            return finish(false, tr("Couldn't make the folder %1 on the stick.").arg(folder));
        }
    }

    qCInfo(lcOps).noquote() << "Make a Rescue USB: copying" << files.size() << "files," << total << "bytes, from" << m_iso << "to" << m_root;
    quint64 done = 0;
    for (const isofs::Entry *e : std::as_const(files)) {
        if (!copy(iso, *e, e == sumsEntry ? QByteArray() : sums.value(e->path), done, total)) {
            ::close(iso);
            return finish(false, m_cancel ? tr("Stopped. The stick is only half done; make it again before using it.") : m_error);
        }
    }
    ::close(iso);

    done = 0;
    const QByteArray sumsHash = QCryptographicHash::hash(sumsText, QCryptographicHash::Sha256).toHex();
    for (const isofs::Entry *e : std::as_const(files)) {
        if (!check(e->path, e == sumsEntry ? sumsHash : sums.value(e->path), done, total))
            return finish(false, m_cancel ? tr("Stopped. The stick is only half done; make it again before using it.") : m_error);
    }

    // Where the rescue system leaves its notes.
    root.mkpath(QStringLiteral("logs"));
    QFile note(m_root + QStringLiteral("/logs/README.txt"));
    if (note.open(QIODevice::WriteOnly | QIODevice::Text)) {
        note.write("DiskForge Rescue keeps its notes here: one folder for each time this stick started a PC,\n"
                   "named by the date and the PC's model. summary.txt in each one is the place to start.\n");
        note.close();
    }
    const int rootFd = ::open(QFile::encodeName(m_root).constData(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (rootFd >= 0) {
        ::syncfs(rootFd);
        ::close(rootFd);
    }
    finish(true, tr("Copied and checked %1 files.").arg(files.size()));
}
