// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// Starts fixed system units (fstrim, paccache, journal cleanup) and switches timers on
// or off through systemd's D-Bus API. polkit asks for the password; the app never runs
// commands as root itself, and it only ever names units, never command lines.

#include <QMap>
#include <QObject>
#include <QSet>

#include <functional>

class QDBusMessage;
class QDBusObjectPath;

class Systemd : public QObject
{
    Q_OBJECT
public:
    using Done = std::function<void(bool ok, const QString &message)>;

    explicit Systemd(QObject *parent = nullptr);
    void setInteractive(bool interactive) { m_interactive = interactive; }

    // Starts a unit and calls `done` when its job finishes (ok when systemd says "done").
    void startUnit(const QString &unit, const Done &done);
    // Enables and starts, or stops and disables, a timer.
    void setTimerEnabled(const QString &timer, bool enabled, const Done &done);

    // Blocking reads; empty/0 when the unit doesn't exist.
    bool unitExists(const QString &unit) const;
    QString unitFileState(const QString &unit) const; // "enabled", "disabled", ...
    quint64 timerLastTrigger(const QString &timer) const; // microseconds since the epoch, 0 = never
    QString serviceResult(const QString &service) const; // "success", "exit-code", ...
    quint64 serviceLastRun(const QString &service) const; // when it last finished, microseconds; 0 = never

private slots:
    void onJobRemoved(uint id, const QDBusObjectPath &job, const QString &unit, const QString &result);

private:
    void callThen(const QString &method, const QVariantList &args, const Done &failed,
                  const std::function<void(const QDBusMessage &)> &next);
    QVariant unitProperty(const QString &unit, const QString &interface, const QString &name) const;

    bool m_interactive = true;
    QMap<QString, Done> m_waiting;       // job path -> callback
    QMap<QString, QString> m_finished;   // job path -> result, if it ended before we knew the path
};
