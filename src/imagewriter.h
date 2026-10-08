// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QObject>

#include <atomic>

// Writes an image file (an ISO) to a device and reads it back to verify. Runs in a
// worker thread; the fd comes from UDisks::openDevice and is closed when done.
class ImageWriter : public QObject
{
    Q_OBJECT
public:
    ImageWriter(const QString &imagePath, int fd, bool verify, const QString &expectedSha256 = {});
    ~ImageWriter() override;

    void cancel() { m_cancel = true; }

public slots:
    void run();

signals:
    void progress(const QString &phase, quint64 done, quint64 total);
    void finished(bool ok, const QString &message);

private:
    bool fail(const QString &message);

    QString m_imagePath;
    int m_fd;
    bool m_verify;
    QString m_expected;
    std::atomic<bool> m_cancel{false};
};
