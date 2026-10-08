// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// Bits shared by the dialogs that run long jobs (scan, clone, backup, rescue...).

#include "udisks.h"

#include <QElapsedTimer>
#include <QThread>

#include <functional>

class QLabel;
class QProgressBar;

QString durationText(double seconds); // "about 5 minutes"
QString diskTitle(const Disk &d);     // "SanDisk Ultra (sdb, 32.00 GB)"

// Fills a progress bar and a "Phase, 120 MB/s, about 5 minutes left" label. Speed and
// time left are worked out per phase, since checking and writing go at different speeds.
class PhaseProgress
{
public:
    PhaseProgress(QProgressBar *bar, QLabel *label);
    void update(const QString &phase, quint64 done, quint64 total);

private:
    QProgressBar *m_bar;
    QLabel *m_label;
    QElapsedTimer m_timer;
    QString m_phase;
};

// Runs worker->run() on a new thread owned by `owner`; the worker is deleted when the
// thread stops. Stop it with quit() and wait() once the worker reports it's finished.
template <typename Worker>
QThread *startOnThread(QObject *owner, Worker *worker)
{
    auto *thread = new QThread(owner);
    worker->moveToThread(thread);
    QObject::connect(thread, &QThread::started, worker, &Worker::run);
    QObject::connect(thread, &QThread::finished, worker, &QObject::deleteLater);
    thread->start();
    return thread;
}

// UDisks::openBlock, then `then(fd)` (fd -1 if it failed). If `context` is gone by
// the time the fd arrives, the fd is closed.
void openBlockThen(UDisks *udisks, QObject *context, const QString &objectPath, UDisks::OpenMode mode,
                   const std::function<void(int fd)> &then);
