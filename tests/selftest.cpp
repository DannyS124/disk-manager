// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

// Runs every operation against an image file attached as a loop device.
// Operations need root (no polkit agent); --guard runs as the user.

#include "../src/format.h"
#include "../src/udisks.h"

#include <QCoreApplication>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusObjectPath>
#include <QDBusUnixFileDescriptor>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QTemporaryDir>
#include <QTextStream>
#include <QThread>

#include <functional>
#include <unistd.h>

namespace {

QTextStream out(stdout);
int failures = 0;
QString imagePath;

void report(bool ok, const QString &step, const QString &detail = {})
{
    out << (ok ? "PASS  " : "FAIL  ") << step;
    if (!detail.isEmpty())
        out << "  (" << detail << ")";
    out << Qt::endl;
    if (!ok)
        ++failures;
}

bool run(UDisks &udisks, const QString &step, const std::function<void()> &op, QString *message = nullptr)
{
    bool done = false, ok = false;
    QString text;
    QEventLoop loop;
    auto conn = QObject::connect(&udisks, &UDisks::operationFinished, [&](bool success, const QString &m) {
        done = true;
        ok = success;
        text = m;
        loop.quit();
    });
    op();
    if (!done)
        loop.exec();
    QObject::disconnect(conn);
    if (message)
        *message = text;
    else
        report(ok, step, text);
    return ok;
}

const Disk *testDisk(UDisks &udisks)
{
    for (const Disk &d : udisks.disks()) {
        if (d.isLoop && d.backingFile == imagePath)
            return &d;
    }
    return nullptr;
}

// UDisks2 updates objects a bit after the job finishes
bool waitFor(UDisks &udisks, const QString &step, const std::function<bool(const Disk &)> &check)
{
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < 15000) {
        udisks.refresh();
        const Disk *d = testDisk(udisks);
        if (d && check(*d)) {
            report(true, step);
            return true;
        }
        QCoreApplication::processEvents();
        QThread::msleep(200);
    }
    report(false, step, QStringLiteral("state never matched"));
    return false;
}

QString loopSetup(const QString &path, QString *error)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadWrite)) {
        *error = file.errorString();
        return {};
    }
    QDBusMessage call = QDBusMessage::createMethodCall(QStringLiteral("org.freedesktop.UDisks2"),
        QStringLiteral("/org/freedesktop/UDisks2/Manager"), QStringLiteral("org.freedesktop.UDisks2.Manager"),
        QStringLiteral("LoopSetup"));
    call << QVariant::fromValue(QDBusUnixFileDescriptor(file.handle()))
         << QVariantMap{{QStringLiteral("auth.no_user_interaction"), true}};
    const QDBusMessage reply = QDBusConnection::systemBus().call(call, QDBus::Block, 30000);
    if (reply.type() != QDBusMessage::ReplyMessage) {
        *error = reply.errorMessage();
        return {};
    }
    return reply.arguments().value(0).value<QDBusObjectPath>().path();
}

void loopDelete(const QString &objectPath)
{
    QDBusMessage call = QDBusMessage::createMethodCall(QStringLiteral("org.freedesktop.UDisks2"), objectPath,
        QStringLiteral("org.freedesktop.UDisks2.Loop"), QStringLiteral("Delete"));
    call << QVariantMap{{QStringLiteral("auth.no_user_interaction"), true}};
    QDBusConnection::systemBus().call(call, QDBus::Block, 30000);
}

Span firstFree(const Disk &d)
{
    for (const Span &s : diskSpans(d)) {
        if (s.isFree())
            return s;
    }
    return {};
}

bool fsAvailable(const UDisks &udisks, const QString &id)
{
    for (const FsType &fs : udisks.filesystems()) {
        if (fs.id == id)
            return fs.available;
    }
    return false;
}

// returns false so the && chain stops at the first failure
bool step(UDisks &udisks, const QString &name, const std::function<void()> &op,
          const QString &expect, const std::function<bool(const Disk &)> &check)
{
    return run(udisks, name, op) && waitFor(udisks, expect, check);
}

bool resizing(UDisks &udisks);

bool operations(UDisks &udisks)
{
    const QString winFs = fsAvailable(udisks, QStringLiteral("ntfs")) ? QStringLiteral("ntfs") : QStringLiteral("vfat");
    auto disk = [&]() -> const Disk & { return *testDisk(udisks); };
    constexpr quint64 MiB = 1024 * 1024;

    return waitFor(udisks, QStringLiteral("image shows up as an empty disk"),
                   [](const Disk &d) { return d.volumes.isEmpty() && d.tableType.isEmpty(); })
        && step(udisks, QStringLiteral("create GPT partition table"),
                [&] { udisks.createPartitionTable(disk(), QStringLiteral("gpt")); },
                QStringLiteral("disk is GPT"), [](const Disk &d) { return d.tableType == QLatin1String("gpt"); })
        && step(udisks, QStringLiteral("create 100 MB ext4 partition"),
                [&] { udisks.createPartition(disk(), firstFree(disk()).offset, 100 * MiB, QStringLiteral("ext4"), QStringLiteral("WKTEST")); },
                QStringLiteral("ext4 partition labelled WKTEST"), [](const Disk &d) {
                    return d.volumes.size() == 1 && d.volumes[0].fsType == QLatin1String("ext4") && d.volumes[0].label == QLatin1String("WKTEST");
                })
        && step(udisks, QStringLiteral("create FAT32 partition in the rest"),
                [&] { const Span f = firstFree(disk()); udisks.createPartition(disk(), f.offset, f.size, QStringLiteral("vfat"), QStringLiteral("DATA")); },
                QStringLiteral("second partition is FAT32"), [](const Disk &d) {
                    return d.volumes.size() == 2 && d.volumes[1].fsType == QLatin1String("vfat");
                })
        && step(udisks, QStringLiteral("mount FAT32 partition"), [&] { udisks.mount(disk().volumes[1]); },
                QStringLiteral("it is mounted"), [](const Disk &d) { return !d.volumes[1].mountPoints.isEmpty(); })
        && step(udisks, QStringLiteral("rename ext4 partition"), [&] { udisks.setLabel(disk().volumes[0], QStringLiteral("RENAMED")); },
                QStringLiteral("label is RENAMED"), [](const Disk &d) { return d.volumes[0].label == QLatin1String("RENAMED"); })
        && step(udisks, QStringLiteral("format the mounted FAT32 partition as %1").arg(winFs),
                [&] { udisks.format(disk().volumes[1], winFs, QStringLiteral("WINDOWS")); },
                QStringLiteral("it is %1, unmounted, with the Microsoft partition type").arg(winFs), [&](const Disk &d) {
                    const Volume &v = d.volumes.value(1);
                    return v.fsType == winFs && v.mountPoints.isEmpty() && v.partType == QLatin1String("ebd0a0a2-b9e5-4433-87c0-68b6b72699c7");
                })
        && step(udisks, QStringLiteral("delete ext4 partition"), [&] { udisks.deletePartition(disk().volumes[0]); },
                QStringLiteral("one partition left, free space before it"), [](const Disk &d) {
                    const QVector<Span> spans = diskSpans(d);
                    return d.volumes.size() == 1 && !spans.isEmpty() && spans.first().isFree();
                })
        && step(udisks, QStringLiteral("replace with an MBR partition table"),
                [&] { udisks.createPartitionTable(disk(), QStringLiteral("dos")); },
                QStringLiteral("disk is MBR and empty"), [](const Disk &d) { return d.tableType == QLatin1String("dos") && d.volumes.isEmpty(); })
        && step(udisks, QStringLiteral("create FAT32 partition on MBR"),
                [&] { const Span f = firstFree(disk()); udisks.createPartition(disk(), f.offset, f.size, QStringLiteral("vfat"), QStringLiteral("MBRFAT")); },
                QStringLiteral("MBR partition is FAT32 with type 0x0c"), [](const Disk &d) {
                    return d.volumes.size() == 1 && d.volumes[0].fsType == QLatin1String("vfat") && d.volumes[0].partType == QLatin1String("0x0c");
                })
        && resizing(udisks);
}

bool resizing(UDisks &udisks)
{
    auto disk = [&]() -> const Disk & { return *testDisk(udisks); };
    constexpr quint64 MiB = 1024 * 1024;
    auto vol = [&](int i) -> const Volume & { return disk().volumes[i]; };

    return step(udisks, QStringLiteral("new GPT table for the resize tests"),
                [&] { udisks.createPartitionTable(disk(), QStringLiteral("gpt")); },
                QStringLiteral("disk is GPT and empty"), [](const Disk &d) { return d.tableType == QLatin1String("gpt") && d.volumes.isEmpty(); })
        && step(udisks, QStringLiteral("create 64 MB ext4 partition"),
                [&] { udisks.createPartition(disk(), firstFree(disk()).offset, 64 * MiB, QStringLiteral("ext4"), QStringLiteral("GROW")); },
                QStringLiteral("it is 64 MB"), [](const Disk &d) { return d.volumes.size() == 1 && d.volumes[0].size == 64 * MiB; })
        && step(udisks, QStringLiteral("grow unmounted ext4 to 160 MB"), [&] { udisks.resize(vol(0), 160 * MiB); },
                QStringLiteral("partition is 160 MB"), [](const Disk &d) { return d.volumes[0].size == 160 * MiB; })
        && step(udisks, QStringLiteral("mount it"), [&] { udisks.mount(vol(0)); },
                QStringLiteral("file system grew with it (over 136 MB)"), [](const Disk &d) {
                    return !d.volumes[0].mountPoints.isEmpty() && d.volumes[0].fsTotal > 136 * MiB;
                })
        && step(udisks, QStringLiteral("grow mounted ext4 to 200 MB"), [&] { udisks.resize(vol(0), 200 * MiB); },
                QStringLiteral("partition is 200 MB, file system over 170 MB, still mounted"), [](const Disk &d) {
                    const Volume &v = d.volumes[0];
                    return v.size == 200 * MiB && !v.mountPoints.isEmpty() && v.fsTotal > 170 * MiB;
                })
        && step(udisks, QStringLiteral("shrink mounted ext4 to 96 MB (needs unmounting)"), [&] { udisks.resize(vol(0), 96 * MiB); },
                QStringLiteral("partition is 96 MB and unmounted"), [](const Disk &d) {
                    return d.volumes[0].size == 96 * MiB && d.volumes[0].mountPoints.isEmpty();
                })
        && step(udisks, QStringLiteral("create 400 MB Btrfs partition after it"),
                [&] { const Span f = firstFree(disk()); udisks.createPartition(disk(), f.offset, 400 * MiB, QStringLiteral("btrfs"), QStringLiteral("SHRINK")); },
                QStringLiteral("second partition is Btrfs"), [](const Disk &d) {
                    return d.volumes.size() == 2 && d.volumes[1].fsType == QLatin1String("btrfs");
                })
        && [&] {
               QString message;
               const bool ok = run(udisks, {}, [&] { udisks.resize(vol(1), 200 * MiB); }, &message);
               report(!ok && message.contains(QLatin1String("out of range")), QStringLiteral("Btrfs below 256 MB is refused"), message);
               return !ok;
           }()
        && step(udisks, QStringLiteral("shrink unmounted Btrfs to 300 MB (needs mounting)"), [&] { udisks.resize(vol(1), 300 * MiB); },
                QStringLiteral("partition is 300 MB, mounted, file system no bigger"), [](const Disk &d) {
                    const Volume &v = d.volumes[1];
                    return v.size == 300 * MiB && !v.mountPoints.isEmpty() && v.fsTotal > 0 && v.fsTotal <= 300 * MiB;
                })
        && [&] {
               QString message;
                  const bool ok = run(udisks, {}, [&] { udisks.resize(vol(1), 4096 * MiB); }, &message);
               report(!ok && message.contains(QLatin1String("out of range")), QStringLiteral("a size past the free space is refused"), message);
               return !ok;
           }();
}

void guard(UDisks &udisks)
{
    for (const Disk &d : udisks.disks()) {
        if (!d.isSystem || d.volumes.isEmpty())
            continue;
        const Volume &v = d.volumes.last();
        const QString name = shortDevice(v.device);
        QString message;
        bool ok = run(udisks, {}, [&] { udisks.format(v, QStringLiteral("ext4"), {}); }, &message);
        report(!ok && message.contains(QLatin1String("running system")), QStringLiteral("format %1 is refused").arg(name), message);
        ok = run(udisks, {}, [&] { udisks.deletePartition(v); }, &message);
        report(!ok && message.contains(QLatin1String("running system")), QStringLiteral("delete %1 is refused").arg(name), message);
        ok = run(udisks, {}, [&] { udisks.createPartitionTable(d, QStringLiteral("gpt")); }, &message);
        report(!ok && message.contains(QLatin1String("running system")), QStringLiteral("new partition table on %1 is refused").arg(shortDevice(d.device)), message);
        return;
    }
    report(false, QStringLiteral("find the system disk"));
}

} // namespace

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);
    const bool root = geteuid() == 0;

    if (app.arguments().contains(QStringLiteral("--guard"))) {
        // root bypasses polkit, so test the guard as a normal user
        if (root) {
            out << "Run --guard as your normal user, not root." << Qt::endl;
            return 2;
        }
        UDisks udisks;
        udisks.setInteractive(false);
        guard(udisks);
    } else {
        if (!root) {
            out << "Disk changes need a password, so the unattended run uses root:\n"
                   "  sudo ./build/diskforge-selftest      operations on a throwaway 512 MB image\n"
                   "  ./build/diskforge-selftest --guard   system disk is refused (as you)" << Qt::endl;
            return 2;
        }
        QTemporaryDir dir;
        imagePath = dir.filePath(QStringLiteral("selftest.img"));
        QFile image(imagePath);
        if (!image.open(QIODevice::WriteOnly) || !image.resize(512ll * 1024 * 1024)) {
            out << "Couldn't create " << imagePath << Qt::endl;
            return 2;
        }
        image.close();

        QString error;
        const QString loop = loopSetup(imagePath, &error);
        report(!loop.isEmpty(), QStringLiteral("attach image as a loop device"), loop.isEmpty() ? error : loop);
        if (loop.isEmpty())
            return 1;
        UDisks udisks;
        udisks.setInteractive(false);
        operations(udisks);
        if (const Disk *d = testDisk(udisks)) {
            for (const Volume &v : d->volumes) {
                if (!v.mountPoints.isEmpty()) {
                    QString ignored;
                    run(udisks, {}, [&] { udisks.unmount(v); }, &ignored);
                }
            }
        }
        loopDelete(loop);
    }

    out << (failures ? QStringLiteral("%1 step(s) failed").arg(failures) : QStringLiteral("All steps passed")) << Qt::endl;
    return failures ? 1 : 0;
}
