// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#include "systemd.h"

#include "dbusnames.h"

#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusObjectPath>
#include <QDBusPendingCallWatcher>
#include <QDBusVariant>

namespace {

const QString kSystemd = QStringLiteral("org.freedesktop.systemd1");
const QString kSystemdPath = QStringLiteral("/org/freedesktop/systemd1");
const QString kSystemdManager = QStringLiteral("org.freedesktop.systemd1.Manager");

} // namespace

Systemd::Systemd(QObject *parent)
    : QObject(parent)
{
    QDBusConnection bus = QDBusConnection::systemBus();
    bus.connect(kSystemd, kSystemdPath, kSystemdManager, QStringLiteral("JobRemoved"), this,
                SLOT(onJobRemoved(uint, QDBusObjectPath, QString, QString)));
    // JobRemoved is only sent to clients that subscribed.
    bus.call(QDBusMessage::createMethodCall(kSystemd, kSystemdPath, kSystemdManager, QStringLiteral("Subscribe")),
             QDBus::Block, 3000);
}

void Systemd::callThen(const QString &method, const QVariantList &args, const Done &failed,
                       const std::function<void(const QDBusMessage &)> &next)
{
    QDBusMessage message = QDBusMessage::createMethodCall(kSystemd, kSystemdPath, kSystemdManager, method);
    message.setArguments(args);
    message.setInteractiveAuthorizationAllowed(m_interactive);
    auto *watcher = new QDBusPendingCallWatcher(QDBusConnection::systemBus().asyncCall(message, kNoTimeout), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this, [failed, next](QDBusPendingCallWatcher *w) {
        w->deleteLater();
        if (w->isError())
            failed(false, w->error().message());
        else
            next(w->reply());
    });
}

void Systemd::startUnit(const QString &unit, const Done &done)
{
    callThen(QStringLiteral("StartUnit"), {unit, QStringLiteral("replace")}, done, [this, done](const QDBusMessage &reply) {
        const QString job = reply.arguments().value(0).value<QDBusObjectPath>().path();
        if (m_finished.contains(job)) {
            const QString result = m_finished.take(job);
            done(result == QLatin1String("done"), result);
        } else {
            m_waiting.insert(job, done);
        }
    });
}

void Systemd::onJobRemoved(uint, const QDBusObjectPath &job, const QString &, const QString &result)
{
    const auto it = m_waiting.find(job.path());
    if (it == m_waiting.end()) {
        m_finished.insert(job.path(), result);
        if (m_finished.size() > 64)
            m_finished.clear(); // jobs other programs started; nobody will ask for these
        return;
    }
    const Done done = it.value();
    m_waiting.erase(it);
    done(result == QLatin1String("done"), result);
}

void Systemd::setTimerEnabled(const QString &timer, bool enabled, const Done &done)
{
    const QStringList files = {timer};
    if (enabled) {
        callThen(QStringLiteral("EnableUnitFiles"), {files, false, false}, done, [this, timer, done](const QDBusMessage &) {
            callThen(QStringLiteral("Reload"), {}, done, [this, timer, done](const QDBusMessage &) { startUnit(timer, done); });
        });
    } else {
        callThen(QStringLiteral("StopUnit"), {timer, QStringLiteral("replace")}, done, [this, files, done](const QDBusMessage &) {
            callThen(QStringLiteral("DisableUnitFiles"), {files, false}, done, [this, done](const QDBusMessage &) {
                callThen(QStringLiteral("Reload"), {}, done, [done](const QDBusMessage &) { done(true, {}); });
            });
        });
    }
}

bool Systemd::unitExists(const QString &unit) const
{
    return !unitFileState(unit).isEmpty();
}

QString Systemd::unitFileState(const QString &unit) const
{
    QDBusMessage call = QDBusMessage::createMethodCall(kSystemd, kSystemdPath, kSystemdManager, QStringLiteral("GetUnitFileState"));
    call << unit;
    const QDBusMessage reply = QDBusConnection::systemBus().call(call, QDBus::Block, 5000);
    return reply.type() == QDBusMessage::ReplyMessage ? reply.arguments().value(0).toString() : QString();
}

QVariant Systemd::unitProperty(const QString &unit, const QString &interface, const QString &name) const
{
    QDBusMessage load = QDBusMessage::createMethodCall(kSystemd, kSystemdPath, kSystemdManager, QStringLiteral("LoadUnit"));
    load << unit;
    const QDBusMessage loaded = QDBusConnection::systemBus().call(load, QDBus::Block, 5000);
    if (loaded.type() != QDBusMessage::ReplyMessage)
        return {};
    const QString path = loaded.arguments().value(0).value<QDBusObjectPath>().path();
    QDBusMessage get = QDBusMessage::createMethodCall(kSystemd, path, QStringLiteral("org.freedesktop.DBus.Properties"), QStringLiteral("Get"));
    get << interface << name;
    const QDBusMessage reply = QDBusConnection::systemBus().call(get, QDBus::Block, 5000);
    return reply.type() == QDBusMessage::ReplyMessage ? reply.arguments().value(0).value<QDBusVariant>().variant() : QVariant();
}

quint64 Systemd::timerLastTrigger(const QString &timer) const
{
    return unitProperty(timer, QStringLiteral("org.freedesktop.systemd1.Timer"), QStringLiteral("LastTriggerUSec")).toULongLong();
}

quint64 Systemd::serviceLastRun(const QString &service) const
{
    return unitProperty(service, QStringLiteral("org.freedesktop.systemd1.Service"), QStringLiteral("ExecMainExitTimestamp")).toULongLong();
}

QString Systemd::serviceResult(const QString &service) const
{
    return unitProperty(service, QStringLiteral("org.freedesktop.systemd1.Service"), QStringLiteral("Result")).toString();
}
