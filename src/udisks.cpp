// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#include "udisks.h"

#include "dbusnames.h"
#include "format.h"

#include <QCollator>
#include <QDBusArgument>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusObjectPath>
#include <QDBusPendingCallWatcher>
#include <QDBusVariant>
#include <QMap>
#include <QStorageInfo>
#include <QVariantMap>

#include <algorithm>
#include <memory>

namespace {

using InterfaceMap = QMap<QString, QVariantMap>;
using ObjectMap = QMap<QDBusObjectPath, InterfaceMap>;

// "ay" byte strings, NUL-terminated
QString byteString(const QVariant &value)
{
    QByteArray bytes = value.toByteArray();
    if (bytes.endsWith('\0'))
        bytes.chop(1);
    return QString::fromLocal8Bit(bytes);
}

QStringList byteStringList(const QVariant &value)
{
    QStringList out;
    if (!value.canConvert<QDBusArgument>())
        return out;
    const QDBusArgument arg = value.value<QDBusArgument>();
    arg.beginArray();
    while (!arg.atEnd()) {
        QByteArray bytes;
        arg >> bytes;
        if (bytes.endsWith('\0'))
            bytes.chop(1);
        out << QString::fromLocal8Bit(bytes);
    }
    arg.endArray();
    // shortest first so "/" leads for btrfs subvolumes
    std::sort(out.begin(), out.end(), [](const QString &a, const QString &b) {
        return a.size() != b.size() ? a.size() < b.size() : a < b;
    });
    return out;
}

QString objectPath(const QVariant &value)
{
    return value.value<QDBusObjectPath>().path();
}

// Anything mounted outside these counts as the running system.
bool isUserMount(const QString &mountPoint)
{
    return mountPoint.startsWith(QLatin1String("/run/media/"))
        || mountPoint.startsWith(QLatin1String("/media/"))
        || mountPoint.startsWith(QLatin1String("/mnt/"));
}

Volume makeVolume(const QString &path, const InterfaceMap &ifaces)
{
    const QVariantMap block = ifaces.value(kBlock);
    Volume v;
    v.objectPath = path;
    v.device = byteString(block.value(QStringLiteral("PreferredDevice")));
    if (v.device.isEmpty())
        v.device = byteString(block.value(QStringLiteral("Device")));
    v.size = block.value(QStringLiteral("Size")).toULongLong();
    v.label = block.value(QStringLiteral("IdLabel")).toString();
    v.fsType = block.value(QStringLiteral("IdType")).toString();
    v.fsUsage = block.value(QStringLiteral("IdUsage")).toString();
    v.uuid = block.value(QStringLiteral("IdUUID")).toString();

    if (ifaces.contains(kPartition)) {
        const QVariantMap part = ifaces.value(kPartition);
        v.number = part.value(QStringLiteral("Number")).toInt();
        v.offset = part.value(QStringLiteral("Offset")).toULongLong();
        v.size = part.value(QStringLiteral("Size")).toULongLong();
        v.partType = part.value(QStringLiteral("Type")).toString();
        v.partName = part.value(QStringLiteral("Name")).toString();
        v.isContainer = part.value(QStringLiteral("IsContainer")).toBool();
        v.isContained = part.value(QStringLiteral("IsContained")).toBool();
    }
    v.isEfi = v.partType.compare(QLatin1String("c12a7328-f81f-11d2-ba4b-00a0c93ec93b"), Qt::CaseInsensitive) == 0
        || v.partType == QLatin1String("0xef");

    if (ifaces.contains(kFilesystem)) {
        v.hasFilesystem = true;
        v.mountPoints = byteStringList(ifaces.value(kFilesystem).value(QStringLiteral("MountPoints")));
    }
    // Configuration is a(sa{sv}); keep the fstab entry so it can be removed again verbatim.
    const QVariant config = block.value(QStringLiteral("Configuration"));
    if (config.canConvert<QDBusArgument>()) {
        const QDBusArgument arg = config.value<QDBusArgument>();
        arg.beginArray();
        while (!arg.atEnd()) {
            QString type;
            QVariantMap details;
            arg.beginStructure();
            arg >> type >> details;
            arg.endStructure();
            if (type == QLatin1String("fstab"))
                v.fstab = details;
        }
        arg.endArray();
    }
    if (ifaces.contains(kSwapspace))
        v.swapActive = ifaces.value(kSwapspace).value(QStringLiteral("Active")).toBool();
    v.encrypted = ifaces.contains(kEncrypted);
    return v;
}

} // namespace

QVector<Span> diskSpans(const Disk &disk)
{
    // skip alignment gaps
    constexpr quint64 kMinFree = 2ull * 1024 * 1024;

    QVector<Span> spans;
    quint64 cursor = 0;
    for (int i = 0; i < disk.volumes.size(); ++i) {
        const Volume &v = disk.volumes[i];
        if (v.isContainer)
            continue; // logicals are drawn instead
        if (v.offset > cursor && v.offset - cursor >= kMinFree)
            spans.push_back({-1, cursor, v.offset - cursor});
        spans.push_back({i, v.offset, v.size});
        cursor = std::max(cursor, v.offset + v.size);
    }
    if (disk.size > cursor && disk.size - cursor >= kMinFree)
        spans.push_back({-1, cursor, disk.size - cursor});
    return spans;
}

UDisks::UDisks(QObject *parent)
    : QObject(parent)
{
    // coalesce signal bursts (hotplug, mount)
    m_debounce.setSingleShot(true);
    m_debounce.setInterval(250);
    connect(&m_debounce, &QTimer::timeout, this, &UDisks::refresh);

    QDBusConnection bus = QDBusConnection::systemBus();
    const QString objectManager = QStringLiteral("org.freedesktop.DBus.ObjectManager");
    bus.connect(kService, kRoot, objectManager, QStringLiteral("InterfacesAdded"),
                this, SLOT(onDBusSignal(QDBusMessage)));
    bus.connect(kService, kRoot, objectManager, QStringLiteral("InterfacesRemoved"),
                this, SLOT(onDBusSignal(QDBusMessage)));
    bus.connect(kService, QString(), QStringLiteral("org.freedesktop.DBus.Properties"),
                QStringLiteral("PropertiesChanged"), this, SLOT(onDBusSignal(QDBusMessage)));

    detectFilesystems();
    refresh();
}

void UDisks::onDBusSignal(const QDBusMessage &)
{
    m_debounce.start();
}

void UDisks::refresh()
{
    QDBusMessage call = QDBusMessage::createMethodCall(kService, kRoot,
        QStringLiteral("org.freedesktop.DBus.ObjectManager"), QStringLiteral("GetManagedObjects"));
    const QDBusMessage reply = QDBusConnection::systemBus().call(call, QDBus::Block, 5000);
    if (reply.type() != QDBusMessage::ReplyMessage || reply.arguments().isEmpty()) {
        m_error = reply.errorMessage().isEmpty() ? tr("No reply from UDisks2") : reply.errorMessage();
        m_disks.clear();
        emit changed();
        return;
    }
    m_error.clear();

    ObjectMap objects;
    reply.arguments().constFirst().value<QDBusArgument>() >> objects;

    QMap<QString, InterfaceMap> drives;
    m_jobs.clear();
    for (auto it = objects.cbegin(); it != objects.cend(); ++it) {
        if (it->contains(kDrive))
            drives.insert(it.key().path(), it.value());
        if (it->contains(kJob)) {
            const QVariantMap j = it->value(kJob);
            Job job;
            job.operation = j.value(QStringLiteral("Operation")).toString();
            job.progress = j.value(QStringLiteral("Progress")).toDouble();
            job.progressValid = j.value(QStringLiteral("ProgressValid")).toBool();
            job.rate = j.value(QStringLiteral("Rate")).toULongLong();
            const QVariant objs = j.value(QStringLiteral("Objects"));
            if (objs.canConvert<QDBusArgument>()) {
                const QDBusArgument arg = objs.value<QDBusArgument>();
                arg.beginArray();
                while (!arg.atEnd()) {
                    QDBusObjectPath o;
                    arg >> o;
                    job.objects << o.path();
                }
                arg.endArray();
            }
            m_jobs.push_back(job);
        }
    }

    // whole disks
    QMap<QString, Disk> disks;
    for (auto it = objects.cbegin(); it != objects.cend(); ++it) {
        const InterfaceMap &ifaces = it.value();
        if (!ifaces.contains(kBlock) || ifaces.contains(kPartition))
            continue;
        const QVariantMap block = ifaces.value(kBlock);
        if (objectPath(block.value(QStringLiteral("CryptoBackingDevice"))) != QLatin1String("/"))
            continue;

        Disk d;
        d.blockPath = it.key().path();
        d.drivePath = objectPath(block.value(QStringLiteral("Drive")));
        d.isLoop = ifaces.contains(kLoop);
        if (d.isLoop)
            d.backingFile = byteString(ifaces.value(kLoop).value(QStringLiteral("BackingFile")));
        // skip zram/dm/md (no drive) and unused loop devices
        if (d.isLoop ? d.backingFile.isEmpty() : d.drivePath == QLatin1String("/"))
            continue;
        d.size = block.value(QStringLiteral("Size")).toULongLong();
        if (d.size == 0)
            continue; // no media

        d.device = byteString(block.value(QStringLiteral("PreferredDevice")));
        if (d.device.isEmpty())
            d.device = byteString(block.value(QStringLiteral("Device")));

        const InterfaceMap driveIfaces = drives.value(d.drivePath);
        const QVariantMap drive = driveIfaces.value(kDrive);
        const QString vendor = drive.value(QStringLiteral("Vendor")).toString().trimmed();
        d.model = drive.value(QStringLiteral("Model")).toString().trimmed();
        if (!vendor.isEmpty() && !d.model.startsWith(vendor))
            d.model = vendor + QLatin1Char(' ') + d.model;
        if (d.isLoop)
            d.model = d.backingFile.section(QLatin1Char('/'), -1);
        d.serial = drive.value(QStringLiteral("Serial")).toString();
        d.bus = drive.value(QStringLiteral("ConnectionBus")).toString();
        d.removable = drive.value(QStringLiteral("Removable")).toBool();
        d.rotationRate = drive.value(QStringLiteral("RotationRate"), -1).toInt();
        d.canPowerOff = drive.value(QStringLiteral("CanPowerOff")).toBool();
        d.readOnly = block.value(QStringLiteral("ReadOnly")).toBool();
        if (!d.isLoop)
            d.health = readHealth(d.drivePath, driveIfaces.value(kAta), driveIfaces.value(kNvme));

        if (ifaces.contains(kPartitionTable))
            d.tableType = ifaces.value(kPartitionTable).value(QStringLiteral("Type")).toString();
        else if (!block.value(QStringLiteral("IdUsage")).toString().isEmpty())
            d.volumes.push_back(makeVolume(d.blockPath, ifaces)); // fs on the bare disk

        disks.insert(d.blockPath, d);
    }

    for (auto it = objects.cbegin(); it != objects.cend(); ++it) {
        if (!it->contains(kPartition))
            continue;
        const QString table = objectPath(it->value(kPartition).value(QStringLiteral("Table")));
        auto disk = disks.find(table);
        if (disk != disks.end())
            disk->volumes.push_back(makeVolume(it.key().path(), it.value()));
    }

    // unlocked LUKS: where the cleartext device is mounted
    for (auto it = objects.cbegin(); it != objects.cend(); ++it) {
        const QString backing = objectPath(it->value(kBlock).value(QStringLiteral("CryptoBackingDevice")));
        if (!it->contains(kBlock) || backing.isEmpty() || backing == QLatin1String("/"))
            continue;
        const QStringList mounts = byteStringList(it->value(kFilesystem).value(QStringLiteral("MountPoints")));
        for (Disk &d : disks) {
            for (Volume &v : d.volumes) {
                if (v.objectPath == backing) {
                    v.cleartextPath = it.key().path();
                    v.cleartextMountPoints = mounts;
                    v.cleartextHasFilesystem = it->contains(kFilesystem);
                    v.cleartextFsType = it->value(kBlock).value(QStringLiteral("IdType")).toString();
                }
            }
        }
    }

    m_disks.clear();
    for (Disk &d : disks) {
        std::sort(d.volumes.begin(), d.volumes.end(),
                  [](const Volume &a, const Volume &b) { return a.offset < b.offset; });
        for (Volume &v : d.volumes) {
            for (const QString &mp : v.mountPoints + v.cleartextMountPoints) {
                if (!isUserMount(mp)) {
                    v.isSystem = true;
                    if (d.systemReason.isEmpty())
                        d.systemReason = tr("Holds %1").arg(mp);
                }
            }
            if (v.swapActive) {
                v.isSystem = true;
                if (d.systemReason.isEmpty())
                    d.systemReason = tr("Swap in use");
            }
            d.isSystem = d.isSystem || v.isSystem;
            if (v.label == QLatin1String("VTOYEFI"))
                d.isVentoy = true;

            const QString mp = v.mountPoints.value(0, v.cleartextMountPoints.value(0));
            if (!mp.isEmpty()) {
                const QStorageInfo info(mp);
                if (info.isValid()) {
                    v.fsTotal = info.bytesTotal();
                    v.fsFree = info.bytesAvailable();
                }
            }
        }
        m_disks.push_back(d);
    }

    // internal, then removable, then images
    QCollator collator;
    collator.setNumericMode(true);
    auto rank = [](const Disk &d) { return d.isLoop ? 2 : (d.removable || d.bus == QLatin1String("usb")) ? 1 : 0; };
    std::sort(m_disks.begin(), m_disks.end(), [&](const Disk &a, const Disk &b) {
        if (rank(a) != rank(b))
            return rank(a) < rank(b);
        return collator.compare(a.device, b.device) < 0;
    });

    emit changed();
}

const Disk *UDisks::diskOf(const Volume &volume) const
{
    for (const Disk &d : m_disks) {
        for (const Volume &v : d.volumes) {
            if (v.objectPath == volume.objectPath)
                return &d;
        }
    }
    return nullptr;
}

QVariantMap UDisks::options(QVariantMap extra) const
{
    if (!m_interactive)
        extra.insert(QStringLiteral("auth.no_user_interaction"), true);
    return extra;
}

bool UDisks::refuseSystem(const Disk *disk, const QString &failure)
{
    if (disk && !disk->isSystem)
        return false;
    emit operationFinished(false, failure + QStringLiteral(": ")
                                      + (disk ? tr("%1 holds the running system").arg(disk->device) : tr("disk not found")));
    return true;
}

void UDisks::callThen(const QString &path, const QString &interface, const QString &method, const QVariantList &args,
                      const QString &failure, const std::function<void(const QDBusMessage &)> &next)
{
    QDBusMessage message = QDBusMessage::createMethodCall(kService, path, interface, method);
    message.setArguments(args);
    message.setInteractiveAuthorizationAllowed(m_interactive);

    // long timeout for polkit prompts and slow formats
    ++m_pending;
    auto *watcher = new QDBusPendingCallWatcher(QDBusConnection::systemBus().asyncCall(message, 10 * 60 * 1000), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this, [this, failure, next](QDBusPendingCallWatcher *w) {
        w->deleteLater();
        --m_pending;
        if (w->isError())
            emit operationFinished(false, failure + QStringLiteral(": ") + w->error().message());
        else
            next(w->reply());
    });
}

void UDisks::call(const QString &path, const QString &interface, const QString &method, const QVariantList &args,
                  const SuccessText &success, const QString &failure)
{
    callThen(path, interface, method, args, failure, [this, success](const QDBusMessage &reply) {
        emit operationFinished(true, success(reply));
    });
}

void UDisks::unmountThen(const QVector<Volume> &volumes, const QString &failure, const std::function<void()> &then,
                         bool lockEncrypted)
{
    // "tear-down" doesn't unmount the device itself, so do it first, one step at a time.
    struct Step { QString path, interface, method; };
    QVector<Step> steps;
    for (const Volume &v : volumes) {
        if (v.canMount() && !v.mounts().isEmpty())
            steps.push_back({v.filesystemPath(), kFilesystem, QStringLiteral("Unmount")});
        if (lockEncrypted && v.encrypted && !v.cleartextPath.isEmpty())
            steps.push_back({v.objectPath, kEncrypted, QStringLiteral("Lock")});
    }
    auto run = std::make_shared<std::function<void(int)>>();
    std::weak_ptr<std::function<void(int)>> weak = run;
    *run = [this, steps, failure, then, weak](int i) {
        if (i == steps.size()) {
            then();
            return;
        }
        auto self = weak.lock();
        callThen(steps[i].path, steps[i].interface, steps[i].method, {options()}, failure,
                 [self, i](const QDBusMessage &) { (*self)(i + 1); });
    };
    (*run)(0);
}

void UDisks::mount(const Volume &volume)
{
    const QString name = shortDevice(volume.device);
    if (volume.encrypted && volume.cleartextPath.isEmpty()) {
        emit operationFinished(false, tr("Couldn't mount %1: unlock it first").arg(name));
        return;
    }
    call(volume.filesystemPath(), kFilesystem, QStringLiteral("Mount"), {options()},
         [name](const QDBusMessage &reply) { return tr("Mounted %1 at %2").arg(name, reply.arguments().value(0).toString()); },
         tr("Couldn't mount %1").arg(name));
}

void UDisks::unmount(const Volume &volume)
{
    const QString name = shortDevice(volume.device);
    if (refuseSystem(diskOf(volume), tr("Couldn't unmount %1").arg(name)))
        return;
    call(volume.filesystemPath(), kFilesystem, QStringLiteral("Unmount"), {options()},
         [name](const QDBusMessage &) { return tr("Unmounted %1").arg(name); },
         tr("Couldn't unmount %1").arg(name));
}

void UDisks::setLabel(const Volume &volume, const QString &label)
{
    const QString name = shortDevice(volume.device);
    if (refuseSystem(diskOf(volume), tr("Couldn't rename %1").arg(name)))
        return;
    call(volume.filesystemPath(), kFilesystem, QStringLiteral("SetLabel"), {label, options()},
         [name, label](const QDBusMessage &) { return tr("Renamed %1 to \"%2\"").arg(name, label); },
         tr("Couldn't rename %1").arg(name));
}

namespace {

QVariantMap formatOptions(const QString &fsType, const QString &label, const QString &passphrase = {})
{
    QVariantMap o;
    if (!passphrase.isEmpty()) {
        o.insert(QStringLiteral("encrypt.passphrase"), passphrase);
        o.insert(QStringLiteral("encrypt.type"), QStringLiteral("luks2"));
    }
    if (!label.isEmpty())
        o.insert(QStringLiteral("label"), label);
    // root dir owned by the user instead of root
    if (fsType == QLatin1String("ext4") || fsType == QLatin1String("btrfs") || fsType == QLatin1String("xfs"))
        o.insert(QStringLiteral("take-ownership"), true);
    return o;
}

QString partitionTypeFor(const QString &tableType, const QString &fsType, bool encrypted = false)
{
    if (encrypted)
        return tableType == QLatin1String("gpt") ? QStringLiteral("ca7d7ccb-63ed-4c53-861c-1742536059cc") : QStringLiteral("0x83");
    const bool windows = fsType == QLatin1String("vfat") || fsType == QLatin1String("exfat") || fsType == QLatin1String("ntfs");
    if (tableType == QLatin1String("gpt"))
        return windows ? QStringLiteral("ebd0a0a2-b9e5-4433-87c0-68b6b72699c7") : QStringLiteral("0fc63daf-8483-4772-8e79-3d69d8477de4");
    if (fsType == QLatin1String("vfat"))
        return QStringLiteral("0x0c");
    return windows ? QStringLiteral("0x07") : QStringLiteral("0x83");
}

} // namespace

void UDisks::format(const Volume &volume, const QString &fsType, const QString &label, const QString &passphrase)
{
    const QString name = shortDevice(volume.device);
    if (refuseSystem(diskOf(volume), tr("Couldn't format %1").arg(name)))
        return;
    QVariantMap o = formatOptions(fsType, label, passphrase);
    o.insert(QStringLiteral("tear-down"), true);
    o.insert(QStringLiteral("update-partition-type"), true);
    const QString failure = tr("Couldn't format %1").arg(name);
    const QString path = volume.objectPath;
    const bool encrypted = !passphrase.isEmpty();
    unmountThen({volume}, failure, [this, path, fsType, o, name, failure, encrypted] {
        call(path, kBlock, QStringLiteral("Format"), {fsType, options(o)},
             [name, fsType, encrypted](const QDBusMessage &) {
                 return encrypted ? tr("Formatted %1 as encrypted %2").arg(name, fsType) : tr("Formatted %1 as %2").arg(name, fsType);
             },
             failure);
    }, true);
}

void UDisks::deletePartition(const Volume &volume)
{
    const QString name = shortDevice(volume.device);
    if (refuseSystem(diskOf(volume), tr("Couldn't delete %1").arg(name)))
        return;
    const QString failure = tr("Couldn't delete %1").arg(name);
    const QString path = volume.objectPath;
    unmountThen({volume}, failure, [this, path, name, failure] {
        call(path, kPartition, QStringLiteral("Delete"), {options({{QStringLiteral("tear-down"), true}})},
             [name](const QDBusMessage &) { return tr("Deleted %1").arg(name); }, failure);
    }, true);
}

void UDisks::createPartition(const Disk &disk, quint64 offset, quint64 size, const QString &fsType, const QString &label,
                             const QString &passphrase)
{
    const QString name = shortDevice(disk.device);
    const QString failure = tr("Couldn't create a partition on %1").arg(name);
    if (refuseSystem(&disk, failure))
        return;

    // 1 MiB aligned; keep the last MiB free for the GPT backup header
    constexpr quint64 MiB = 1024 * 1024;
    quint64 freeEnd = 0;
    for (const Span &s : diskSpans(disk)) {
        if (s.isFree() && offset >= s.offset && offset < s.offset + s.size)
            freeEnd = s.offset + s.size;
    }
    freeEnd = std::min<quint64>(freeEnd, disk.size > MiB ? disk.size - MiB : 0) / MiB * MiB;
    const quint64 start = std::max<quint64>((offset + MiB - 1) / MiB * MiB, MiB);
    const quint64 end = std::min<quint64>(start + (size + MiB - 1) / MiB * MiB, freeEnd);
    if (end <= start) {
        emit operationFinished(false, failure + QStringLiteral(": ") + tr("not enough space"));
        return;
    }

    const QString partName = disk.tableType == QLatin1String("gpt") ? label : QString(); // no names on MBR
    call(disk.blockPath, kPartitionTable, QStringLiteral("CreatePartitionAndFormat"),
         {start, end - start, partitionTypeFor(disk.tableType, fsType, !passphrase.isEmpty()), partName, options(),
          fsType, options(formatOptions(fsType, label, passphrase))},
         [name, fsType, start, end](const QDBusMessage &) {
             return tr("Created a %1 %2 partition on %3").arg(formatSize(end - start), fsType, name);
         },
         failure);
}

void UDisks::createPartitionTable(const Disk &disk, const QString &tableType)
{
    const QString name = shortDevice(disk.device);
    const QString failure = tr("Couldn't initialize %1").arg(name);
    if (refuseSystem(&disk, failure))
        return;
    const QString path = disk.blockPath;
    unmountThen(disk.volumes, failure, [this, path, tableType, name, failure] {
        call(path, kBlock, QStringLiteral("Format"), {tableType, options({{QStringLiteral("tear-down"), true}})},
             [name, tableType](const QDBusMessage &) {
                 return tr("%1 now has an empty %2 partition table").arg(name, tableType == QLatin1String("gpt") ? QStringLiteral("GPT") : QStringLiteral("MBR"));
             },
             failure);
    }, true);
}

void UDisks::detectFilesystems()
{
    m_filesystems = {
        {QStringLiteral("exfat"), tr("exFAT"), tr("USB drives, works everywhere"), 11, QStringLiteral("exfatprogs")},
        {QStringLiteral("vfat"), tr("FAT32"), tr("old devices, max 4 GB per file"), 11, QStringLiteral("dosfstools")},
        {QStringLiteral("ntfs"), tr("NTFS"), tr("Windows"), 32, QStringLiteral("ntfs-3g")},
        {QStringLiteral("ext4"), tr("ext4"), tr("Linux"), 16, QStringLiteral("e2fsprogs")},
        {QStringLiteral("btrfs"), tr("Btrfs"), tr("Linux, snapshots"), 255, QStringLiteral("btrfs-progs")},
        {QStringLiteral("xfs"), tr("XFS"), tr("Linux, big files"), 12, QStringLiteral("xfsprogs")},
    };
    for (FsType &fs : m_filesystems) {
        QDBusMessage message = QDBusMessage::createMethodCall(kService, QStringLiteral("/org/freedesktop/UDisks2/Manager"),
                                                              QStringLiteral("org.freedesktop.UDisks2.Manager"), QStringLiteral("CanFormat"));
        message << fs.id;
        const QDBusMessage reply = QDBusConnection::systemBus().call(message, QDBus::Block, 3000);
        if (reply.type() != QDBusMessage::ReplyMessage || reply.arguments().isEmpty())
            continue;
        const QDBusArgument arg = reply.arguments().constFirst().value<QDBusArgument>();
        QString utility;
        arg.beginStructure();
        arg >> fs.available >> utility;
        arg.endStructure();

        for (const auto &[method, flag] : {std::pair{QStringLiteral("CanCheck"), &fs.canCheck}, std::pair{QStringLiteral("CanRepair"), &fs.canRepair}}) {
            QDBusMessage can = QDBusMessage::createMethodCall(kService, kManagerPath, kManager, method);
            can << fs.id;
            const QDBusMessage canReply = QDBusConnection::systemBus().call(can, QDBus::Block, 3000);
            if (canReply.type() == QDBusMessage::ReplyMessage && !canReply.arguments().isEmpty()) {
                const QDBusArgument c = canReply.arguments().constFirst().value<QDBusArgument>();
                QString missing;
                c.beginStructure();
                c >> *flag >> missing;
                c.endStructure();
            }
        }
        // errors for filesystems that can't resize at all (exfat)
        QDBusMessage resize = QDBusMessage::createMethodCall(kService, QStringLiteral("/org/freedesktop/UDisks2/Manager"),
                                                             QStringLiteral("org.freedesktop.UDisks2.Manager"), QStringLiteral("CanResize"));
        resize << fs.id;
        const QDBusMessage resizeReply = QDBusConnection::systemBus().call(resize, QDBus::Block, 3000);
        if (resizeReply.type() == QDBusMessage::ReplyMessage && !resizeReply.arguments().isEmpty()) {
            const QDBusArgument r = resizeReply.arguments().constFirst().value<QDBusArgument>();
            quint64 modes = 0;
            r.beginStructure();
            r >> fs.resizeAvailable >> modes >> utility;
            r.endStructure();
            fs.resizeModes = int(modes);
        }
    }
}

const FsType *UDisks::filesystem(const QString &id) const
{
    for (const FsType &fs : m_filesystems) {
        if (fs.id == id)
            return &fs;
    }
    return nullptr;
}

ResizeLimits UDisks::resizeLimits(const Volume &v) const
{
    constexpr quint64 MiB = 1024 * 1024;
    ResizeLimits limits;
    limits.minSize = limits.maxSize = v.size;

    const Disk *disk = diskOf(v);
    const FsType *fs = filesystem(v.fsType);
    if (!disk || disk->isSystem) {
        limits.reason = tr("Disk holds the running system");
        return limits;
    }
    if (v.number == 0 || v.isContainer) {
        limits.reason = tr("Only partitions can be resized");
        return limits;
    }
    if (!fs || fs->resizeModes == 0) {
        limits.reason = v.fsType.isEmpty() ? tr("No file system to resize")
                                           : tr("%1 can't be resized").arg(fs ? fs->name : v.fsType);
        return limits;
    }
    if (!fs->resizeAvailable) {
        limits.reason = tr("Install %1 to resize %2").arg(fs->package, fs->name);
        return limits;
    }

    const bool canShrink = fs->resizeModes & (OfflineShrink | OnlineShrink);
    const bool canGrow = fs->resizeModes & (OfflineGrow | OnlineGrow);
    if (v.fsTotal)
        limits.used = v.fsTotal - v.fsFree;

    if (canGrow) {
        // only into free space directly after it
        const QVector<Span> spans = diskSpans(*disk);
        for (int i = 0; i + 1 < spans.size(); ++i) {
            if (!spans[i].isFree() && disk->volumes[spans[i].volume].objectPath == v.objectPath && spans[i + 1].isFree()) {
                const quint64 end = std::min<quint64>(spans[i + 1].offset + spans[i + 1].size, disk->size - MiB);
                limits.maxSize = std::max<quint64>(v.size, (end - v.offset) / MiB * MiB);
            }
        }
    }
    if (canShrink) {
        // unmounted: usage unknown, the resize tool refuses if too small
        quint64 floor = limits.used ? limits.used + std::max<quint64>(limits.used / 10, 32 * MiB) : 32 * MiB;
        if (v.fsType == QLatin1String("btrfs"))
            floor = std::max<quint64>(floor, 256 * MiB); // kernel minimum
        limits.minSize = std::min<quint64>(v.size, (floor + MiB - 1) / MiB * MiB);
    }

    limits.possible = limits.minSize < v.size || limits.maxSize > v.size;
    if (!limits.possible)
        limits.reason = canGrow ? tr("No unallocated space right after it, and %1 can't shrink").arg(fs->name)
                                : tr("%1 can only grow, and there's no unallocated space after it").arg(fs->name);
    return limits;
}

bool UDisks::resizeNeedsRemount(const Volume &v, bool shrink) const
{
    const FsType *fs = filesystem(v.fsType);
    const int modes = fs ? fs->resizeModes : 0;
    const bool mounted = !v.mountPoints.isEmpty();
    const bool online = modes & (shrink ? OnlineShrink : OnlineGrow);
    const bool offline = modes & (shrink ? OfflineShrink : OfflineGrow);
    return mounted ? !online : !offline;
}

void UDisks::resize(const Volume &volume, quint64 newSize)
{
    constexpr quint64 MiB = 1024 * 1024;
    const QString name = shortDevice(volume.device);
    const QString failure = tr("Couldn't resize %1").arg(name);
    if (refuseSystem(diskOf(volume), failure))
        return;
    const ResizeLimits limits = resizeLimits(volume);
    newSize = newSize / MiB * MiB;
    if (!limits.possible || newSize < limits.minSize || newSize > limits.maxSize || newSize == volume.size) {
        emit operationFinished(false, failure + QStringLiteral(": ")
                                          + (limits.possible ? tr("size out of range") : limits.reason));
        return;
    }

    const bool shrink = newSize < volume.size;
    const QString path = volume.objectPath;
    const QString done = tr("Resized %1 to %2").arg(name, formatSize(newSize));
    auto resizeNow = [this, path, newSize, shrink, failure, done] {
        if (shrink) {
            // fs first so the partition never cuts into data
            callThen(path, kFilesystem, QStringLiteral("Resize"), {newSize, options()}, failure, [this, path, newSize, failure, done](const QDBusMessage &) {
                call(path, kPartition, QStringLiteral("Resize"), {newSize, options()},
                     [done](const QDBusMessage &) { return done; }, failure);
            });
        } else {
            // size 0 = fill the partition
            callThen(path, kPartition, QStringLiteral("Resize"), {newSize, options()}, failure, [this, path, newSize, failure, done](const QDBusMessage &) {
                call(path, kFilesystem, QStringLiteral("Resize"), {quint64(0), options()},
                     [done](const QDBusMessage &) { return done; }, failure);
            });
        }
    };

    if (!resizeNeedsRemount(volume, shrink))
        resizeNow();
    else if (volume.mountPoints.isEmpty())
        callThen(path, kFilesystem, QStringLiteral("Mount"), {options()}, failure, [resizeNow](const QDBusMessage &) { resizeNow(); });
    else
        unmountThen({volume}, failure, resizeNow);
}

QString UDisks::daemonVersion() const
{
    QDBusMessage message = QDBusMessage::createMethodCall(kService, QStringLiteral("/org/freedesktop/UDisks2/Manager"),
                                                          QStringLiteral("org.freedesktop.DBus.Properties"), QStringLiteral("Get"));
    message << QStringLiteral("org.freedesktop.UDisks2.Manager") << QStringLiteral("Version");
    const QDBusMessage reply = QDBusConnection::systemBus().call(message, QDBus::Block, 3000);
    return reply.arguments().value(0).value<QDBusVariant>().variant().toString();
}
