// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "filecopy.h"
#include "isofs.h"

#include <QHash>
#include <QObject>

#include <atomic>

// DiskForge Rescue images (the ISO rescue/build.sh makes) and the USB sticks made from them.
namespace rescue {

// /.disk/diskforge-rescue, on the image and on a stick:
//   DiskForge Rescue
//   version=0.5.0
//   built=2026-10-09
//   id=<build id>
struct Info {
    QString version;
    QString built;
    QString id;
    bool valid() const { return !version.isEmpty() && !id.isEmpty(); }
};
Info parseInfo(const QByteArray &text);

// sha256sum.txt: "<hash>  ./path" on each line. Paths come back without the "./".
QHash<QString, QByteArray> parseSums(const QByteArray &text);

// What's in an image file, read without mounting it.
struct Image {
    isofs::Listing listing;
    Info info;
    quint64 bytes = 0; // the files that go onto the stick
    QString error;     // set when it isn't a DiskForge Rescue image (or can't be read)
};
Image inspect(const QString &isoPath);

// A mounted stick (or any folder) with DiskForge Rescue on it: its info, and how many boots
// have left logs there.
Info stickInfo(const QString &root);
int logFolders(const QString &root);

// Copies a rescue image's files onto a mounted, empty FAT32 stick, with filecopy: each file
// is checked against the image's sha256sum.txt while it's copied, then read back from the
// stick and checked again. The source is the ISO file, or (from inside DiskForge Rescue) the
// folder of a running rescue stick. Runs in a worker thread.
class StickWriter : public QObject
{
    Q_OBJECT
public:
    // `source`: an ISO file, or a folder holding a rescue stick's files.
    StickWriter(const QString &source, const QString &stickRoot);
    void cancel();

public slots:
    void run();

signals:
    void progress(const QString &phase, quint64 done, quint64 total);
    void finished(bool ok, const QString &message);

private:
    void finish(bool ok, const QString &message);

    QString m_source;
    QString m_root;
    std::atomic<bool> m_cancel{false};
    std::atomic<filecopy::Copier *> m_copier{nullptr};
};

} // namespace rescue
