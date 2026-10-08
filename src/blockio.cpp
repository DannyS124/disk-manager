// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#include "blockio.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QStringList>

#include <cerrno>
#include <linux/fs.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace blockio {

Buffer alignedBuffer(size_t size)
{
    void *raw = nullptr;
    if (posix_memalign(&raw, kAlign, size) != 0)
        return Buffer(nullptr, &free);
    return Buffer(static_cast<char *>(raw), &free);
}

bool readAt(int fd, char *buf, quint64 len, quint64 offset)
{
    while (len > 0) {
        const ssize_t n = ::pread(fd, buf, len, off_t(offset));
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            return false;
        buf += n;
        len -= quint64(n);
        offset += quint64(n);
    }
    return true;
}

bool writeAt(int fd, const char *buf, quint64 len, quint64 offset)
{
    while (len > 0) {
        const ssize_t n = ::pwrite(fd, buf, len, off_t(offset));
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            return false;
        buf += n;
        len -= quint64(n);
        offset += quint64(n);
    }
    return true;
}

bool writeAll(int fd, const char *data, qint64 size)
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

qint64 readFull(int fd, char *data, qint64 size)
{
    qint64 total = 0;
    while (total < size) {
        const ssize_t n = ::read(fd, data + total, size_t(size - total));
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            break;
        total += n;
    }
    return total;
}

int logicalSize(int fd)
{
    int size = 0;
    return ::ioctl(fd, BLKSSZGET, &size) == 0 && size > 0 ? size : 512;
}

int physicalSize(int fd)
{
    unsigned int size = 0;
    return ::ioctl(fd, BLKPBSZGET, &size) == 0 && size > 0 ? int(size) : logicalSize(fd);
}

quint64 deviceSize(int fd)
{
    quint64 size = 0;
    if (::ioctl(fd, BLKGETSIZE64, &size) == 0)
        return size;
    struct stat st {};
    return ::fstat(fd, &st) == 0 ? quint64(st.st_size) : 0;
}

namespace {

// The /dev node holding the file system that `path` is on, from /proc/self/mountinfo
// (st_dev doesn't help for Btrfs subvolumes, which get anonymous device numbers).
QString mountSource(const QString &path)
{
    const QString target = QFileInfo(path).canonicalFilePath().isEmpty() ? QFileInfo(QFileInfo(path).absolutePath()).canonicalFilePath()
                                                                          : QFileInfo(path).canonicalFilePath();
    QFile f(QStringLiteral("/proc/self/mountinfo"));
    if (!f.open(QIODevice::ReadOnly))
        return {};
    QString best, source;
    for (const QByteArray &line : f.readAll().split('\n')) {
        const QList<QByteArray> fields = line.split(' ');
        const int dash = fields.indexOf("-");
        if (fields.size() < 5 || dash < 0 || dash + 2 >= fields.size())
            continue;
        const QString mp = QString::fromLocal8Bit(fields[4]).replace(QStringLiteral("\\040"), QStringLiteral(" "));
        const bool inside = target == mp || target.startsWith(mp == QLatin1String("/") ? mp : mp + QLatin1Char('/'));
        if (inside && mp.size() >= best.size()) {
            best = mp;
            source = QString::fromLocal8Bit(fields[dash + 2]);
        }
    }
    return source;
}

// Every kernel block device name under `name`: itself, its parent disk, and for
// device-mapper/md devices, the devices they sit on.
void underlying(const QString &name, QStringList *out, int depth = 0)
{
    if (name.isEmpty() || out->contains(name) || depth > 8)
        return;
    *out << name;
    const QString sys = QStringLiteral("/sys/class/block/") + name;
    const QString parent = QFileInfo(QFileInfo(sys).canonicalFilePath()).dir().dirName();
    if (QFileInfo::exists(sys + QStringLiteral("/partition")))
        *out << parent;
    for (const QString &slave : QDir(sys + QStringLiteral("/slaves")).entryList(QDir::Dirs | QDir::NoDotAndDotDot))
        underlying(slave, out, depth + 1);
}

} // namespace

bool pathIsOnDisk(const QString &path, const QString &diskDevice)
{
    const QString source = mountSource(path);
    if (!source.startsWith(QLatin1String("/dev/")))
        return false; // tmpfs, network shares and the like
    QStringList names;
    underlying(QFileInfo(source).canonicalFilePath().section(QLatin1Char('/'), -1), &names);
    return names.contains(diskDevice.section(QLatin1Char('/'), -1));
}

} // namespace blockio
