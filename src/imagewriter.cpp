// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#include "imagewriter.h"

#include "blockio.h"

#include <QCryptographicHash>
#include <QFile>

#include <cerrno>
#include <fcntl.h>
#include <unistd.h>

namespace {

constexpr qint64 kChunk = 4 * 1024 * 1024;
using blockio::readFull;
using blockio::writeAll;

} // namespace

ImageWriter::ImageWriter(const QString &imagePath, int fd, bool verify, const QString &expectedSha256)
    : m_imagePath(imagePath)
    , m_fd(fd)
    , m_verify(verify)
    , m_expected(expectedSha256.trimmed().toLower())
{
}

ImageWriter::~ImageWriter()
{
    if (m_fd >= 0)
        ::close(m_fd);
}

bool ImageWriter::fail(const QString &message)
{
    emit finished(false, message);
    return false;
}

void ImageWriter::run()
{
    QFile image(m_imagePath);
    if (!image.open(QIODevice::ReadOnly)) {
        fail(tr("Couldn't open the image: %1").arg(image.errorString()));
        return;
    }
    const quint64 size = quint64(image.size());
    const off_t deviceSize = ::lseek(m_fd, 0, SEEK_END);
    if (deviceSize < 0 || quint64(deviceSize) < size) {
        fail(tr("The image is bigger than the drive"));
        return;
    }
    QByteArray buffer(kChunk, Qt::Uninitialized);

    // A wrong or corrupt download is caught before anything is written.
    if (!m_expected.isEmpty()) {
        QCryptographicHash hash(QCryptographicHash::Sha256);
        quint64 done = 0;
        while (!image.atEnd()) {
            if (m_cancel) {
                fail(tr("Cancelled. Nothing was written."));
                return;
            }
            const qint64 n = image.read(buffer.data(), kChunk);
            if (n < 0) {
                fail(tr("Couldn't read the image"));
                return;
            }
            hash.addData(QByteArrayView(buffer.constData(), n));
            done += quint64(n);
            emit progress(tr("Checking the image"), done, size);
        }
        if (QString::fromLatin1(hash.result().toHex()) != m_expected) {
            fail(tr("The image's SHA-256 doesn't match the one you entered. The download may be corrupt. Nothing was written."));
            return;
        }
        image.seek(0);
    }

    ::lseek(m_fd, 0, SEEK_SET);
    QCryptographicHash written(QCryptographicHash::Sha256);
    quint64 done = 0;
    while (!image.atEnd()) {
        if (m_cancel) {
            fail(tr("Cancelled. The drive won't work until you format it."));
            return;
        }
        const qint64 n = image.read(buffer.data(), kChunk);
        if (n < 0 || !writeAll(m_fd, buffer.constData(), n)) {
            fail(tr("Writing failed: %1").arg(n < 0 ? image.errorString() : QString::fromLocal8Bit(strerror(errno))));
            return;
        }
        written.addData(QByteArrayView(buffer.constData(), n));
        done += quint64(n);
        emit progress(tr("Writing"), done, size);
    }

    emit progress(tr("Finishing (flushing to the drive)"), size, size);
    if (::fdatasync(m_fd) != 0) {
        fail(tr("Writing failed: %1").arg(QString::fromLocal8Bit(strerror(errno))));
        return;
    }

    if (m_verify) {
        // Drop the cached copy so the check reads what actually landed on the drive.
        ::posix_fadvise(m_fd, 0, 0, POSIX_FADV_DONTNEED);
        ::lseek(m_fd, 0, SEEK_SET);
        QCryptographicHash readBack(QCryptographicHash::Sha256);
        quint64 checked = 0;
        while (checked < size) {
            if (m_cancel) {
                fail(tr("Verification cancelled. The image was written but not checked."));
                return;
            }
            const qint64 want = qint64(std::min<quint64>(kChunk, size - checked));
            const qint64 n = readFull(m_fd, buffer.data(), want);
            if (n != want) {
                fail(tr("Couldn't read the drive back to verify it"));
                return;
            }
            readBack.addData(QByteArrayView(buffer.constData(), n));
            checked += quint64(n);
            emit progress(tr("Verifying"), checked, size);
        }
        if (readBack.result() != written.result()) {
            fail(tr("Verification failed: what's on the drive doesn't match the image. The drive may be faulty."));
            return;
        }
    }

    ::close(m_fd);
    m_fd = -1;
    emit finished(true, m_verify ? tr("Done. The image was written and verified.") : tr("Done. The image was written."));
}
