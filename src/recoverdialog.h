// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// Recover Partitions: put a partition table back from the drive's own backup copy or from a
// layout DiskForge remembered. Only the table is written; what's in the partitions isn't
// touched. The table that's there now is remembered first, so this can be undone too.

#include "partrecover.h"
#include "udisks.h"

#include <QDialog>

class DiskMap;
class QLabel;
class QLineEdit;
class QListWidget;
class QPushButton;

class RecoverDialog : public QDialog
{
    Q_OBJECT
public:
    RecoverDialog(UDisks *udisks, const Disk &disk, QWidget *parent = nullptr);
    // With the table already read (previews and tests): nothing is opened to look.
    RecoverDialog(UDisks *udisks, const Disk &disk, const gpt::Report &report, QWidget *parent = nullptr);
    // Why a drive can't have its table put back right now, or empty.
    static QString refusal(const Disk &disk);
    int sourceCount() const;

signals:
    void ready(); // the drive was read and the choices are listed
    void done(bool ok, const QString &message);

private:
    void build();
    const Disk *disk() const; // the drive as UDisks has it now, or as it was given
    struct Source {
        QString title;
        bool backup = false; // copy the backup over the main table, exactly
        recover::Layout layout;
    };
    void listSources(const gpt::Report &report);
    void choose(int row);
    void refresh();
    void write();
    void waitForPartitions(int expected);

    UDisks *m_udisks;
    Disk m_disk;
    QString m_blockPath;
    QString m_device;
    QString m_driveKey;
    QVector<Source> m_sources;
    QLabel *m_intro;
    QListWidget *m_list;
    DiskMap *m_before;
    DiskMap *m_after;
    QLabel *m_problem;
    QLineEdit *m_confirm;
    QPushButton *m_write = nullptr;
    bool m_busy = false;
};
