// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#include "jobui.h"

#include "format.h"

#include <QLabel>
#include <QPointer>
#include <QProgressBar>

#include <memory>
#include <unistd.h>

QString durationText(double seconds)
{
    if (seconds < 90)
        return QObject::tr("about a minute");
    if (seconds < 90 * 60)
        return QObject::tr("about %n minute(s)", nullptr, qRound(seconds / 60));
    return QObject::tr("about %n hour(s)", nullptr, qRound(seconds / 3600));
}

QString diskTitle(const Disk &d)
{
    return QStringLiteral("%1 (%2, %3)").arg(d.model, shortDevice(d.device), formatSize(d.size));
}

PhaseProgress::PhaseProgress(QProgressBar *bar, QLabel *label)
    : m_bar(bar)
    , m_label(label)
{
}

void PhaseProgress::update(const QString &phase, quint64 done, quint64 total)
{
    if (phase != m_phase) {
        m_phase = phase;
        m_timer.start();
    }
    m_bar->setRange(0, 1000);
    m_bar->setValue(total ? int(done * 1000 / total) : 0);
    m_bar->setVisible(true);
    const double seconds = m_timer.nsecsElapsed() / 1e9;
    QString text = phase;
    if (seconds > 2 && done < total) {
        const double rate = done / seconds;
        text += QObject::tr(", %1/s, %2 left").arg(formatSize(quint64(rate)), durationText((total - done) / std::max(rate, 1.0)));
    }
    m_label->setText(text);
}

void openBlockThen(UDisks *udisks, QObject *context, const QString &objectPath, UDisks::OpenMode mode,
                   const std::function<void(int fd)> &then)
{
    auto conn = std::make_shared<QMetaObject::Connection>();
    QPointer<QObject> guard(context);
    *conn = QObject::connect(udisks, &UDisks::deviceOpened, udisks, [conn, guard, objectPath, then](const QString &path, int fd) {
        if (path != objectPath)
            return;
        QObject::disconnect(*conn);
        if (!guard) {
            if (fd >= 0)
                ::close(fd);
            return;
        }
        then(fd);
    });
    udisks->openBlock(objectPath, mode);
}
