// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QObject>
#include <QStringList>
#include <QVector>

#include <functional>
#include <memory>

class UDisks;

// Sets a USB stick up through UDisks, one step at a time: a new partition table, its
// partitions (each with its file system and label), their flags, then mounts the ones asked
// for. Every step waits until UDisks shows the result before the next one starts. Make a
// Rescue USB, the file copy mode of Write Image to USB and Make a Windows USB use it.
//
// It erases the stick: the dialogs ask first.
class UsbPrep : public QObject
{
    Q_OBJECT
public:
    struct Partition {
        QString fsType;     // "vfat", "ext4"...
        QString label;
        quint64 size = 0;   // 0: the rest of the stick
        quint64 flags = 0;  // MBR: 0x80 marks it bootable
        bool mount = false; // mounted once everything is made
    };

    UsbPrep(UDisks *udisks, const QString &diskPath, const QString &tableType, const QVector<Partition> &partitions,
            QObject *parent = nullptr);

    void start();
    // Unmounts what start() mounted, then emits done().
    void finish();

    // Once ready: the partitions' UDisks objects and mount points, in the order asked for.
    QStringList volumePaths() const { return m_volumes; }
    QStringList mountPoints() const { return m_mounts; }

signals:
    void phase(const QString &text);
    void ready(const QStringList &mountPoints);
    // A step failed. `shownAlready`: it was a UDisks error, which the main window shows itself.
    void failed(const QString &message, bool shownAlready);
    void done(bool ok, const QString &message);

private:
    void listen();
    void makeTable();
    void makePartition(int index);
    void setFlags(int index);
    void mountNext(int index);
    void unmountNext(int index);
    // The next UDisks result goes to `next`; a failure ends it, unless `failureIsFine`.
    void expect(const std::function<void()> &next, bool failureIsFine = false);
    void waitFor(const std::function<bool()> &ready, int seconds, const std::function<void()> &then,
                 const QString &timeoutMessage);
    void fail(const QString &message, bool shownAlready = false);
    QString mountPointOf(const QString &volumePath) const;

    UDisks *m_udisks;
    QString m_disk;
    QString m_table;
    QVector<Partition> m_partitions;
    QStringList m_volumes;
    QStringList m_mounts;
    std::function<void()> m_next;
    bool m_failureIsFine = false;
    bool m_running = false;
    QMetaObject::Connection m_opConn;
    std::shared_ptr<bool> m_waiting;
};
