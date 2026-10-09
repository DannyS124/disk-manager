// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "lostsource.h"

#include <QObject>
#include <QVector>

#include <atomic>
#include <memory>

// Find Lost Files: what it found, how it looks for it, and how it saves it.
namespace lost {

enum class Condition : quint8 {
    Good,         // all there, as far as can be told
    MaybeDamaged, // its structure breaks off or doesn't add up: it may open with parts missing
    Overwritten,  // its space has been used for something else since
    Unreadable,   // part of it is on spots the drive couldn't read; those parts are blank
};

enum class Origin : quint8 {
    FileSystem, // the file system still lists it: name and folder are known
    Contents,   // found by what's inside it: the name is made up
};

struct Found {
    QString name;
    QString folder; // where it was, when the file system says
    int type = -1;  // filetypes number, -1 when it's not one of those
    quint64 size = 0;
    QVector<Extent> extents; // where its bytes are, in order
    Condition condition = Condition::Good;
    Origin origin = Origin::Contents;
    bool deleted = true;
    qint64 modified = 0; // ms since 1970, 0 when not known
};

// A found file's bytes, read from where they are on the source.
class FileReader
{
public:
    FileReader(std::shared_ptr<Source> source, const Found &file);
    quint64 size() const { return m_size; }
    qint64 read(quint64 at, char *buf, qint64 len);
    QByteArray head(qint64 len); // the first len bytes (fewer for a smaller file)

private:
    std::shared_ptr<Source> m_source;
    QVector<Extent> m_extents;
    quint64 m_size;
};

// Looks through the source for files by their contents (carving): every sector is checked for
// the start of a file it knows, and each start is followed to that file's end. Reads the
// source once, start to end; slow on a big drive, but it finds files no file system remembers.
class DeepScan : public QObject
{
    Q_OBJECT
public:
    explicit DeepScan(std::shared_ptr<Source> source, quint64 start = 0, quint64 end = 0);
    void cancel() { m_cancel = true; }

    static constexpr int kPerTypeLimit = 250000; // past that, more of one type isn't listed

public Q_SLOTS:
    void run();

Q_SIGNALS:
    void found(const QVector<lost::Found> &files); // a batch every quarter second or so
    void progress(quint64 done, quint64 total);
    void finished(bool stopped);

private:
    std::shared_ptr<Source> m_source;
    quint64 m_start, m_end;
    std::atomic<bool> m_cancel{false};
};

// Saves found files into `folder`/Recovered <date and time>/, never over anything that's there,
// keeping going past files that fail. Writes "What was saved.txt" next to them.
class Saver : public QObject
{
    Q_OBJECT
public:
    Saver(std::shared_ptr<Source> source, QVector<Found> files, QString folder);
    void cancel() { m_cancel = true; }

public Q_SLOTS:
    void run();

Q_SIGNALS:
    void progress(quint64 done, quint64 total);
    void finished(int saved, int failed, const QString &folder, const QString &message);

private:
    std::shared_ptr<Source> m_source;
    QVector<Found> m_files;
    QString m_folder;
    std::atomic<bool> m_cancel{false};
};

// A file name any file system takes (FAT and NTFS included), from whatever a drive said.
QString cleanName(const QString &name);
// The made-up name for a file found by its contents: "Picture 000042.jpg".
QString carvedName(int type, int number);

} // namespace lost
