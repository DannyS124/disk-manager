// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

// UDisks operations beyond partitioning: safe removal, check/repair, fstab, disk images,
// LUKS, wiping, SMART and raw device access.

#include "dbusnames.h"
#include "format.h"
#include "udisks.h"

#include <QDBusArgument>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusObjectPath>
#include <QDBusPendingCallWatcher>
#include <QDBusUnixFileDescriptor>
#include <QFile>
#include <QRegularExpression>

#include <fcntl.h>
#include <unistd.h>

namespace {

// Sector counts are sometimes packed with other fields in the raw value (HGST/Hitachi
// put extra data in the upper bytes); the count is the low 16 bits.
qint64 sectorCount(qint64 raw)
{
    return raw > 0xFFFF ? (raw & 0xFFFF) : raw;
}

QDBusMessage blockingCall(const QString &path, const QString &interface, const QString &method, const QVariantList &args)
{
    QDBusMessage message = QDBusMessage::createMethodCall(kService, path, interface, method);
    message.setArguments(args);
    return QDBusConnection::systemBus().call(message, QDBus::Block, 15000);
}

QVector<SmartAttribute> decodeAta(const QDBusMessage &reply)
{
    QVector<SmartAttribute> out;
    if (reply.type() != QDBusMessage::ReplyMessage || reply.arguments().isEmpty())
        return out;
    const QDBusArgument arg = reply.arguments().constFirst().value<QDBusArgument>();
    arg.beginArray();
    while (!arg.atEnd()) {
        uchar id = 0;
        QString name;
        ushort flags = 0;
        int value = 0, worst = 0, threshold = 0, unit = 0;
        qlonglong pretty = 0;
        QVariantMap expansion;
        arg.beginStructure();
        arg >> id >> name >> flags >> value >> worst >> threshold >> pretty >> unit >> expansion;
        arg.endStructure();

        SmartAttribute a;
        a.id = id;
        a.name = name;
        a.value = value;
        a.worst = worst;
        a.threshold = threshold;
        a.failing = threshold > 0 && value > 0 && value <= threshold;
        switch (unit) {
        case 2: // milliseconds
            a.raw = UDisks::tr("%L1 hours").arg(pretty / 3600000);
            break;
        case 3: // sectors
            a.rawValue = sectorCount(pretty);
            a.raw = UDisks::tr("%L1 sectors").arg(a.rawValue);
            break;
        case 4: // millikelvin
            a.raw = QStringLiteral("%1 °C").arg(pretty / 1000.0 - 273.15, 0, 'f', 0);
            break;
        default:
            a.raw = QString::number(pretty);
        }
        out.push_back(a);
    }
    arg.endArray();
    return out;
}

QString startupFsType(const QString &fsType)
{
    return fsType == QLatin1String("ntfs") ? QStringLiteral("ntfs3") : fsType; // the kernel's NTFS driver
}

QByteArray byteString(const QString &s)
{
    QByteArray b = s.toLocal8Bit();
    b.append('\0'); // UDisks reads these with g_variant_get_bytestring, which wants the NUL
    return b;
}

} // namespace

const Disk *UDisks::diskByPath(const QString &blockPath) const
{
    for (const Disk &d : m_disks) {
        if (d.blockPath == blockPath)
            return &d;
    }
    return nullptr;
}

Health UDisks::readHealth(const QString &drivePath, const QVariantMap &ata, const QVariantMap &nvme)
{
    Health h;
    HealthCache &cache = m_healthCache[drivePath];

    if (!ata.isEmpty() && ata.value(QStringLiteral("SmartSupported")).toBool()) {
        if (!ata.value(QStringLiteral("SmartEnabled")).toBool()) {
            h.summary = tr("SMART is turned off");
            return h;
        }
        h.updated = ata.value(QStringLiteral("SmartUpdated")).toULongLong();
        const double kelvin = ata.value(QStringLiteral("SmartTemperature")).toDouble();
        h.temperatureC = kelvin > 0 ? kelvin - 273.15 : -1;
        h.powerOnHours = ata.value(QStringLiteral("SmartPowerOnSeconds")).toULongLong() / 3600;
        h.selftestStatus = ata.value(QStringLiteral("SmartSelftestStatus")).toString();
        h.selftestPercentRemaining = ata.value(QStringLiteral("SmartSelftestPercentRemaining"), -1).toInt();
        if (h.updated == 0) {
            h.summary = tr("No health data yet");
            return h;
        }
        if (cache.updated != h.updated) {
            cache = {};
            cache.updated = h.updated;
            const QVector<SmartAttribute> attrs = decodeAta(blockingCall(drivePath, kAta, QStringLiteral("SmartGetAttributes"), {QVariantMap()}));
            qint64 bad = 0;
            for (const SmartAttribute &a : attrs) {
                if ((a.id == 5 || a.id == 197) && a.rawValue > 0)
                    bad += a.rawValue;
                cache.attrFailing = cache.attrFailing || a.failing;
            }
            cache.badSectors = attrs.isEmpty() ? -1 : bad;
        }
        h.badSectors = cache.badSectors;
        const bool failing = ata.value(QStringLiteral("SmartFailing")).toBool()
            || ata.value(QStringLiteral("SmartNumAttributesFailing")).toInt() > 0 || cache.attrFailing;
        if (failing) {
            h.state = Health::State::Failing;
            h.summary = tr("Failing, back up now");
        } else if (h.badSectors > 0) {
            h.state = Health::State::Warning;
            h.summary = tr("%n bad sector(s)", nullptr, int(h.badSectors));
        } else if (ata.value(QStringLiteral("SmartNumAttributesFailedInThePast")).toInt() > 0) {
            h.state = Health::State::Warning;
            h.summary = tr("Had problems in the past");
        } else {
            h.state = Health::State::Healthy;
            h.summary = tr("Healthy");
        }
        return h;
    }

    if (!nvme.isEmpty()) {
        h.nvme = true;
        h.updated = nvme.value(QStringLiteral("SmartUpdated")).toULongLong();
        const double kelvin = nvme.value(QStringLiteral("SmartTemperature")).toDouble();
        h.temperatureC = kelvin > 0 ? kelvin - 273.15 : -1;
        h.powerOnHours = nvme.value(QStringLiteral("SmartPowerOnHours")).toULongLong();
        h.criticalWarnings = nvme.value(QStringLiteral("SmartCriticalWarning")).toStringList();
        h.selftestStatus = nvme.value(QStringLiteral("SmartSelftestStatus")).toString();
        h.selftestPercentRemaining = nvme.value(QStringLiteral("SmartSelftestPercentRemaining"), -1).toInt();
        if (h.updated == 0) {
            h.summary = tr("No health data yet");
            return h;
        }
        if (cache.updated != h.updated) {
            cache = {};
            cache.updated = h.updated;
            const QDBusMessage reply = blockingCall(drivePath, kNvme, QStringLiteral("SmartGetAttributes"), {QVariantMap()});
            if (reply.type() == QDBusMessage::ReplyMessage && !reply.arguments().isEmpty()) {
                QVariantMap attrs;
                reply.arguments().constFirst().value<QDBusArgument>() >> attrs;
                cache.percentUsed = attrs.value(QStringLiteral("percent_used"), -1).toInt();
                cache.mediaErrors = attrs.value(QStringLiteral("media_errors")).toLongLong();
                cache.attrFailing = attrs.contains(QStringLiteral("avail_spare"))
                    && attrs.value(QStringLiteral("avail_spare")).toInt() < attrs.value(QStringLiteral("spare_thresh")).toInt();
            }
        }
        h.percentUsed = cache.percentUsed;
        static const QStringList serious = {QStringLiteral("spare"), QStringLiteral("degraded"), QStringLiteral("readonly"),
                                            QStringLiteral("volatile_mem"), QStringLiteral("pmr_readonly")};
        bool failing = cache.attrFailing;
        for (const QString &w : h.criticalWarnings)
            failing = failing || serious.contains(w);
        if (failing) {
            h.state = Health::State::Failing;
            h.summary = tr("Failing, back up now");
        } else if (h.criticalWarnings.contains(QStringLiteral("temperature"))) {
            h.state = Health::State::Warning;
            h.summary = tr("Running too hot");
        } else if (h.percentUsed >= 100) {
            h.state = Health::State::Warning;
            h.summary = tr("Worn out (%1% used)").arg(h.percentUsed);
        } else if (cache.mediaErrors > 0) {
            h.state = Health::State::Warning;
            h.summary = tr("%n media error(s)", nullptr, int(cache.mediaErrors));
        } else {
            h.state = Health::State::Healthy;
            h.summary = tr("Healthy");
        }
    }
    return h;
}

QVector<SmartAttribute> UDisks::smartAttributes(const Disk &disk)
{
    if (disk.health.state == Health::State::Unknown && disk.health.updated == 0)
        return {};
    if (!disk.health.nvme)
        return decodeAta(blockingCall(disk.drivePath, kAta, QStringLiteral("SmartGetAttributes"), {QVariantMap()}));

    QVector<SmartAttribute> out;
    const QDBusMessage reply = blockingCall(disk.drivePath, kNvme, QStringLiteral("SmartGetAttributes"), {QVariantMap()});
    if (reply.type() != QDBusMessage::ReplyMessage || reply.arguments().isEmpty())
        return out;
    QVariantMap a;
    reply.arguments().constFirst().value<QDBusArgument>() >> a;
    auto add = [&out, &a](const char *key, const QString &name, const std::function<QString(const QVariant &)> &fmt) {
        if (a.contains(QLatin1String(key)))
            out.push_back({0, name, -1, -1, -1, fmt(a.value(QLatin1String(key))), false});
    };
    auto number = [](const QVariant &v) { return QStringLiteral("%L1").arg(v.toLongLong()); };
    auto percent = [](const QVariant &v) { return QStringLiteral("%1%").arg(v.toInt()); };
    auto bytes = [](const QVariant &v) { return formatSize(v.toULongLong()); };
    add("avail_spare", tr("Available spare"), percent);
    add("spare_thresh", tr("Spare threshold"), percent);
    add("percent_used", tr("Life used"), percent);
    add("total_data_read", tr("Data read"), bytes);
    add("total_data_written", tr("Data written"), bytes);
    add("power_cycles", tr("Power cycles"), number);
    add("unsafe_shutdowns", tr("Unsafe shutdowns"), number);
    add("media_errors", tr("Media errors"), number);
    add("num_err_log_entries", tr("Error log entries"), number);
    add("warning_temp_time", tr("Minutes above warning temperature"), number);
    add("critical_temp_time", tr("Minutes above critical temperature"), number);
    return out;
}

void UDisks::smartUpdate(const Disk &disk)
{
    const QString name = shortDevice(disk.device);
    call(disk.drivePath, disk.health.nvme ? kNvme : kAta, QStringLiteral("SmartUpdate"), {options()},
         [name](const QDBusMessage &) { return tr("Read fresh health data from %1").arg(name); },
         tr("Couldn't read health data from %1").arg(name));
}

void UDisks::smartSelftest(const Disk &disk, const QString &type)
{
    const QString name = shortDevice(disk.device);
    call(disk.drivePath, disk.health.nvme ? kNvme : kAta, QStringLiteral("SmartSelftestStart"), {type, options()},
         [name](const QDBusMessage &) { return tr("Self-test started on %1").arg(name); },
         tr("Couldn't start a self-test on %1").arg(name));
}

void UDisks::powerOff(const Disk &disk)
{
    const QString name = shortDevice(disk.device);
    const QString failure = tr("Couldn't safely remove %1").arg(name);
    if (refuseSystem(&disk, failure))
        return;
    if (!disk.canPowerOff) {
        emit operationFinished(false, failure + QStringLiteral(": ") + tr("this drive can't be powered off"));
        return;
    }
    const QString drive = disk.drivePath;
    unmountThen(disk.volumes, failure, [this, drive, name, failure] {
        call(drive, kDrive, QStringLiteral("PowerOff"), {options()},
             [name](const QDBusMessage &) { return tr("%1 can be unplugged now").arg(name); }, failure);
    }, true);
}

void UDisks::check(const Volume &volume)
{
    const QString name = shortDevice(volume.device);
    const QString failure = tr("Couldn't check %1").arg(name);
    if (refuseSystem(diskOf(volume), failure))
        return;
    if (!volume.canMount()) {
        emit operationFinished(false, failure + QStringLiteral(": ") + tr("no file system to check"));
        return;
    }
    const QString fs = volume.filesystemPath();
    const QString id = volume.objectPath;
    unmountThen({volume}, failure, [this, fs, id, name, failure] {
        callThen(fs, kFilesystem, QStringLiteral("Check"), {options()}, failure, [this, id, name](const QDBusMessage &reply) {
            const bool clean = reply.arguments().value(0).toBool();
            emit checkFinished(id, clean);
            emit operationFinished(true, clean ? tr("No problems found on %1").arg(name) : tr("%1 has errors").arg(name));
        });
    });
}

void UDisks::repair(const Volume &volume)
{
    const QString name = shortDevice(volume.device);
    const QString failure = tr("Couldn't repair %1").arg(name);
    if (refuseSystem(diskOf(volume), failure))
        return;
    const QString fs = volume.filesystemPath();
    unmountThen({volume}, failure, [this, fs, name, failure] {
        callThen(fs, kFilesystem, QStringLiteral("Repair"), {options()}, failure, [this, name](const QDBusMessage &reply) {
            const bool repaired = reply.arguments().value(0).toBool();
            emit operationFinished(repaired, repaired ? tr("Repaired %1").arg(name)
                                                      : tr("Some errors on %1 couldn't be repaired").arg(name));
        });
    });
}

QString UDisks::startupMountPoint(const Volume &volume)
{
    QString name = !volume.label.isEmpty() ? volume.label : volume.uuid.left(8);
    name.replace(QRegularExpression(QStringLiteral("[^A-Za-z0-9._-]")), QStringLiteral("_"));
    return QStringLiteral("/mnt/") + (name.isEmpty() ? shortDevice(volume.device) : name);
}

void UDisks::setMountAtStartup(const Volume &volume, bool enable)
{
    const QString name = shortDevice(volume.device);
    const QString failure = enable ? tr("Couldn't make %1 mount at startup").arg(name)
                                   : tr("Couldn't stop %1 mounting at startup").arg(name);
    if (refuseSystem(diskOf(volume), failure))
        return;
    if (volume.encrypted || !volume.hasFilesystem || volume.uuid.isEmpty()) {
        emit operationFinished(false, failure + QStringLiteral(": ") + tr("only unencrypted file systems can mount at startup"));
        return;
    }

    QVariantMap details = volume.fstab;
    if (enable) {
        if (!details.isEmpty()) {
            emit operationFinished(true, tr("%1 already mounts at startup").arg(name));
            return;
        }
        QString opts = QStringLiteral("nosuid,nodev,nofail,x-gvfs-show,x-mount.mkdir");
        // These have no Unix permissions; give the files to whoever set this up.
        static const QStringList noPermissions = {QStringLiteral("vfat"), QStringLiteral("exfat"), QStringLiteral("ntfs")};
        if (noPermissions.contains(volume.fsType))
            opts += QStringLiteral(",uid=%1,gid=%2").arg(getuid()).arg(getgid());
        details = {
            {QStringLiteral("fsname"), byteString(QStringLiteral("UUID=") + volume.uuid)},
            {QStringLiteral("dir"), byteString(startupMountPoint(volume))},
            {QStringLiteral("type"), byteString(startupFsType(volume.fsType))},
            {QStringLiteral("opts"), byteString(opts)},
            {QStringLiteral("freq"), 0},
            {QStringLiteral("passno"), 0},
        };
    } else if (details.isEmpty()) {
        emit operationFinished(true, tr("%1 doesn't mount at startup").arg(name));
        return;
    }

    QDBusArgument item;
    item.beginStructure();
    item << QStringLiteral("fstab") << details;
    item.endStructure();
    const QString dir = startupMountPoint(volume);
    call(volume.objectPath, kBlock, enable ? QStringLiteral("AddConfigurationItem") : QStringLiteral("RemoveConfigurationItem"),
         {QVariant::fromValue(item), options()},
         [name, dir, enable](const QDBusMessage &) {
             return enable ? tr("%1 will mount at %2 every time you start your PC").arg(name, dir)
                           : tr("%1 won't mount at startup anymore").arg(name);
         },
         failure);
}

void UDisks::openImage(const QString &path, bool readOnly)
{
    const QString name = path.section(QLatin1Char('/'), -1);
    const QString failure = tr("Couldn't open %1").arg(name);
    QFile file(path);
    if (!file.open(readOnly ? QIODevice::ReadOnly : QIODevice::ReadWrite)) {
        emit operationFinished(false, failure + QStringLiteral(": ") + file.errorString());
        return;
    }
    // QDBusUnixFileDescriptor keeps its own copy of the fd, so the file can close afterwards.
    call(kManagerPath, kManager, QStringLiteral("LoopSetup"),
         {QVariant::fromValue(QDBusUnixFileDescriptor(file.handle())), options({{QStringLiteral("read-only"), readOnly}})},
         [this, name](const QDBusMessage &reply) {
             emit imageOpened(reply.arguments().value(0).value<QDBusObjectPath>().path());
             return tr("Opened %1").arg(name);
         },
         failure);
}

void UDisks::detachImage(const Disk &disk)
{
    const QString name = disk.backingFile.section(QLatin1Char('/'), -1);
    const QString failure = tr("Couldn't close %1").arg(name);
    if (!disk.isLoop) {
        emit operationFinished(false, failure + QStringLiteral(": ") + tr("not a disk image"));
        return;
    }
    const QString path = disk.blockPath;
    unmountThen(disk.volumes, failure, [this, path, name, failure] {
        call(path, kLoop, QStringLiteral("Delete"), {options()},
             [name](const QDBusMessage &) { return tr("Closed %1").arg(name); }, failure);
    }, true);
}

void UDisks::unlock(const Volume &volume, const QString &passphrase)
{
    const QString name = shortDevice(volume.device);
    call(volume.objectPath, kEncrypted, QStringLiteral("Unlock"), {passphrase, options()},
         [name](const QDBusMessage &) { return tr("Unlocked %1").arg(name); },
         tr("Couldn't unlock %1. Is the passphrase right?").arg(name));
}

void UDisks::lock(const Volume &volume)
{
    const QString name = shortDevice(volume.device);
    if (refuseSystem(diskOf(volume), tr("Couldn't lock %1").arg(name)))
        return;
    unmountThen({volume}, tr("Couldn't lock %1").arg(name), [this, name] {
        emit operationFinished(true, tr("Locked %1").arg(name));
    }, true);
}

void UDisks::changePassphrase(const Volume &volume, const QString &oldPassphrase, const QString &newPassphrase)
{
    const QString name = shortDevice(volume.device);
    const QString failure = tr("Couldn't change the passphrase of %1").arg(name);
    if (refuseSystem(diskOf(volume), failure))
        return;
    call(volume.objectPath, kEncrypted, QStringLiteral("ChangePassphrase"), {oldPassphrase, newPassphrase, options()},
         [name](const QDBusMessage &) { return tr("Changed the passphrase of %1").arg(name); }, failure);
}

void UDisks::wipe(const Disk &disk)
{
    const QString name = shortDevice(disk.device);
    const QString failure = tr("Couldn't wipe %1").arg(name);
    if (refuseSystem(&disk, failure))
        return;
    const QString path = disk.blockPath;
    unmountThen(disk.volumes, failure, [this, path, name, failure] {
        call(path, kBlock, QStringLiteral("Format"),
             {QStringLiteral("empty"), options({{QStringLiteral("erase"), QStringLiteral("zero")}, {QStringLiteral("tear-down"), true}})},
             [name](const QDBusMessage &) { return tr("%1 has been wiped").arg(name); }, failure);
    }, true);
}

void UDisks::openDevice(const Disk &disk, bool writable, bool forBenchmark)
{
    const QString name = shortDevice(disk.device);
    const QString failure = tr("Couldn't open %1").arg(name);
    if (refuseSystem(&disk, failure)) {
        emit deviceOpened(disk.blockPath, -1);
        return;
    }
    const QString path = disk.blockPath;
    auto open = [this, path, writable, forBenchmark, failure] {
        QDBusMessage message = forBenchmark
            ? QDBusMessage::createMethodCall(kService, path, kBlock, QStringLiteral("OpenForBenchmark"))
            : QDBusMessage::createMethodCall(kService, path, kBlock, QStringLiteral("OpenDevice"));
        if (forBenchmark)
            message << options({{QStringLiteral("writable"), writable}});
        else
            message << (writable ? QStringLiteral("rw") : QStringLiteral("r")) << options({{QStringLiteral("flags"), O_EXCL | O_CLOEXEC}});
        message.setInteractiveAuthorizationAllowed(m_interactive);
        ++m_pending;
        auto *watcher = new QDBusPendingCallWatcher(QDBusConnection::systemBus().asyncCall(message, 10 * 60 * 1000), this);
        connect(watcher, &QDBusPendingCallWatcher::finished, this, [this, path, failure](QDBusPendingCallWatcher *w) {
            w->deleteLater();
            --m_pending;
            if (w->isError()) {
                emit operationFinished(false, failure + QStringLiteral(": ") + w->error().message());
                emit deviceOpened(path, -1);
                return;
            }
            const auto fd = w->reply().arguments().value(0).value<QDBusUnixFileDescriptor>();
            emit deviceOpened(path, fd.isValid() ? ::dup(fd.fileDescriptor()) : -1);
        });
    };
    // Reading for a benchmark works while mounted; anything that writes needs it unmounted.
    if (forBenchmark && !writable)
        open();
    else
        unmountThen(disk.volumes, failure, open, true);
}
