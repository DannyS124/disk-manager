// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

// Runs every operation against an image file attached as a loop device.
// Operations need root (no polkit agent); --guard runs as the user.

#include "testkit.h"

#include "../src/format.h"
#include "../src/udisks.h"

#include <QCoreApplication>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusObjectPath>
#include <QDBusUnixFileDescriptor>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QTextStream>
#include <QThread>

#include "../src/addons.h"
#include "../src/benchmark.h"
#include "../src/surfacescan.h"

#include <QProcess>
#include <fcntl.h>
#include "../src/imagewriter.h"

#include <QFileInfo>
#include <QRandomGenerator>

#include <functional>
#include <unistd.h>

namespace {

QString imagePath;

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
bool extras(UDisks &udisks);

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
        && resizing(udisks)
        && extras(udisks);
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

// Waits for UDisks::deviceOpened and returns the fd (-1 on failure).
int openFd(UDisks &udisks, const Disk &disk, bool writable, bool benchmark)
{
    int fd = -2;
    QEventLoop loop;
    auto conn = QObject::connect(&udisks, &UDisks::deviceOpened, [&](const QString &, int f) { fd = f; loop.quit(); });
    udisks.openDevice(disk, writable, benchmark);
    if (fd == -2)
        loop.exec();
    QObject::disconnect(conn);
    return fd;
}

const Disk *diskWithFile(UDisks &udisks, const QString &file)
{
    for (const Disk &d : udisks.disks()) {
        if (d.isLoop && d.backingFile == file)
            return &d;
    }
    return nullptr;
}

bool checkFails(UDisks &udisks, const QString &name, const std::function<void()> &op, const char *expect)
{
    QString message;
    const bool ok = run(udisks, {}, op, &message);
    report(!ok && message.contains(QLatin1String(expect)), name, message);
    return !ok && message.contains(QLatin1String(expect));
}

// Check/repair, mount at startup, LUKS, disk images, image writing, benchmark, wipe.
bool extras(UDisks &udisks)
{
    auto disk = [&]() -> const Disk & { return *testDisk(udisks); };
    auto vol = [&](int i) -> const Volume & { return disk().volumes[i]; };
    constexpr quint64 MiB = 1024 * 1024;

    QFile fstabFile(QStringLiteral("/etc/fstab"));
    if (!fstabFile.open(QIODevice::ReadOnly))
        return false;
    const QByteArray fstabBefore = fstabFile.readAll();
    fstabFile.close();

    bool ok = step(udisks, QStringLiteral("new GPT table for the extra tests"),
                   [&] { udisks.createPartitionTable(disk(), QStringLiteral("gpt")); },
                   QStringLiteral("disk is GPT and empty"), [](const Disk &d) { return d.tableType == QLatin1String("gpt") && d.volumes.isEmpty(); })
        && step(udisks, QStringLiteral("create 120 MB ext4 partition"),
                [&] { udisks.createPartition(disk(), firstFree(disk()).offset, 120 * MiB, QStringLiteral("ext4"), QStringLiteral("CHECKME")); },
                QStringLiteral("it exists"), [](const Disk &d) { return d.volumes.size() == 1 && d.volumes[0].fsType == QLatin1String("ext4"); })
        && run(udisks, QStringLiteral("check it"), [&] { udisks.check(vol(0)); })
        && run(udisks, QStringLiteral("repair it"), [&] { udisks.repair(vol(0)); })
        && step(udisks, QStringLiteral("make it mount at startup"), [&] { udisks.setMountAtStartup(vol(0), true); },
                QStringLiteral("fstab entry exists"), [](const Disk &d) { return !d.volumes[0].fstab.isEmpty(); })
        && step(udisks, QStringLiteral("mount it (uses the fstab entry)"), [&] { udisks.mount(vol(0)); },
                QStringLiteral("mounted at /mnt/CHECKME"), [](const Disk &d) { return d.volumes[0].mountPoints.contains(QStringLiteral("/mnt/CHECKME")); })
        && step(udisks, QStringLiteral("unmount it"), [&] { udisks.unmount(vol(0)); },
                QStringLiteral("unmounted"), [](const Disk &d) { return d.volumes[0].mountPoints.isEmpty(); })
        && step(udisks, QStringLiteral("stop it mounting at startup"), [&] { udisks.setMountAtStartup(vol(0), false); },
                QStringLiteral("fstab entry gone"), [](const Disk &d) { return d.volumes[0].fstab.isEmpty(); });

    if (!fstabFile.open(QIODevice::ReadOnly))
        return false;
    const QByteArray fstabAfter = fstabFile.readAll();
    fstabFile.close();
    report(fstabAfter == fstabBefore, QStringLiteral("/etc/fstab is back to how it was"));
    if (fstabAfter != fstabBefore) {
        QFile restore(QStringLiteral("/etc/fstab"));
        if (restore.open(QIODevice::WriteOnly | QIODevice::Truncate))
            restore.write(fstabBefore);
        return false;
    }
    QDir(QStringLiteral("/mnt/CHECKME")).removeRecursively();
    if (!ok)
        return false;

    ok = step(udisks, QStringLiteral("create 150 MB encrypted ext4 partition"),
              [&] { const Span f = firstFree(disk()); udisks.createPartition(disk(), f.offset, 150 * MiB, QStringLiteral("ext4"), QStringLiteral("SECRET"), QStringLiteral("first-passphrase")); },
              QStringLiteral("it is LUKS"), [](const Disk &d) { return d.volumes.size() == 2 && d.volumes[1].fsType == QLatin1String("crypto_LUKS"); })
        && step(udisks, QStringLiteral("lock it"), [&] { udisks.lock(vol(1)); },
                QStringLiteral("locked"), [](const Disk &d) { return d.volumes[1].cleartextPath.isEmpty(); })
        && checkFails(udisks, QStringLiteral("a wrong passphrase is refused"), [&] { udisks.unlock(vol(1), QStringLiteral("wrong")); }, "passphrase")
        && step(udisks, QStringLiteral("unlock it"), [&] { udisks.unlock(vol(1), QStringLiteral("first-passphrase")); },
                QStringLiteral("unlocked, ext4 inside"), [](const Disk &d) { return d.volumes[1].cleartextHasFilesystem && d.volumes[1].cleartextFsType == QLatin1String("ext4"); })
        && step(udisks, QStringLiteral("mount the unlocked side"), [&] { udisks.mount(vol(1)); },
                QStringLiteral("mounted"), [](const Disk &d) { return !d.volumes[1].cleartextMountPoints.isEmpty(); })
        && step(udisks, QStringLiteral("lock it while mounted (unmounts first)"), [&] { udisks.lock(vol(1)); },
                QStringLiteral("locked"), [](const Disk &d) { return d.volumes[1].cleartextPath.isEmpty(); })
        && run(udisks, QStringLiteral("change the passphrase"), [&] { udisks.changePassphrase(vol(1), QStringLiteral("first-passphrase"), QStringLiteral("second-passphrase")); })
        && step(udisks, QStringLiteral("unlock with the new passphrase"), [&] { udisks.unlock(vol(1), QStringLiteral("second-passphrase")); },
                QStringLiteral("unlocked"), [](const Disk &d) { return !d.volumes[1].cleartextPath.isEmpty(); })
        && step(udisks, QStringLiteral("lock it again"), [&] { udisks.lock(vol(1)); },
                QStringLiteral("locked"), [](const Disk &d) { return d.volumes[1].cleartextPath.isEmpty(); });
    if (!ok)
        return false;

    // A second, smaller image, attached the way the app does it.
    const QString second = QFileInfo(imagePath).dir().filePath(QStringLiteral("second.img"));
    {
        QFile f(second);
        if (!f.open(QIODevice::WriteOnly) || !f.resize(64ll * 1024 * 1024))
            return false;
    }
    if (!run(udisks, QStringLiteral("open a disk image (read-write)"), [&] { udisks.openImage(second, false); }))
        return false;
    if (!waitFor(udisks, QStringLiteral("it shows up as a disk"), [&](const Disk &) { return diskWithFile(udisks, second) != nullptr; }))
        return false;

    const QString iso = QFileInfo(imagePath).dir().filePath(QStringLiteral("test.iso"));
    QByteArray isoData(16 * 1024 * 1024, Qt::Uninitialized);
    QRandomGenerator::global()->fillRange(reinterpret_cast<quint32 *>(isoData.data()), isoData.size() / 4);
    {
        QFile f(iso);
        if (!f.open(QIODevice::WriteOnly))
            return false;
        f.write(isoData);
    }

    int fd = openFd(udisks, *diskWithFile(udisks, second), true, false);
    report(fd >= 0, QStringLiteral("open the image disk for writing"));
    if (fd < 0)
        return false;
    bool written = false;
    QString writeMessage;
    {
        ImageWriter writer(iso, fd, true, QString::fromLatin1(QCryptographicHash::hash(isoData, QCryptographicHash::Sha256).toHex()));
        QObject::connect(&writer, &ImageWriter::finished, [&](bool k, const QString &m) { written = k; writeMessage = m; });
        writer.run();
    }
    report(written, QStringLiteral("write a 16 MB image and verify it"), writeMessage);
    {
        QFile dev(diskWithFile(udisks, second)->device);
        if (!dev.open(QIODevice::ReadOnly))
            return false;
        report(dev.read(isoData.size()) == isoData, QStringLiteral("the disk really holds the image"));
    }

    fd = openFd(udisks, *diskWithFile(udisks, second), false, true);
    bool benchOk = false;
    BenchmarkResult result;
    if (fd >= 0) {
        Benchmark bench(fd, diskWithFile(udisks, second)->size);
        QObject::connect(&bench, &Benchmark::finished, [&](bool k, const BenchmarkResult &r, const QString &) { benchOk = k; result = r; });
        bench.run();
    }
    report(benchOk && result.readMBps > 0 && result.accessMs >= 0, QStringLiteral("benchmark it"),
           QStringLiteral("%1 MB/s, %2 ms").arg(result.readMBps, 0, 'f', 0).arg(result.accessMs, 0, 'f', 2));

    const bool wiped = run(udisks, QStringLiteral("securely wipe it"), [&] { udisks.wipe(*diskWithFile(udisks, second)); });
    {
        QFile dev(diskWithFile(udisks, second)->device);
        if (!dev.open(QIODevice::ReadOnly))
            return false;
        const QByteArray head = dev.read(isoData.size());
        report(wiped && head == QByteArray(isoData.size(), '\0'), QStringLiteral("it is all zeros now"));
    }

    return run(udisks, QStringLiteral("close the disk image"), [&] { udisks.detachImage(*diskWithFile(udisks, second)); })
        && waitFor(udisks, QStringLiteral("it is gone"), [&](const Disk &) { return diskWithFile(udisks, second) == nullptr; });
}

// Scan and repair against dm-dust, a kernel test target whose bad blocks fail on read
// and are cleared by a write, the way a drive's pending sectors behave.
void badSectorTests()
{
    QTemporaryDir dir;
    const QString image = dir.filePath(QStringLiteral("dust.img"));
    QByteArray original(64 * 1024 * 1024, Qt::Uninitialized);
    QRandomGenerator::global()->fillRange(reinterpret_cast<quint32 *>(original.data()), original.size() / 4);
    {
        QFile f(image);
        if (!f.open(QIODevice::WriteOnly))
            return report(false, QStringLiteral("create test image"));
        f.write(original);
    }
    const QString loop = sh(QStringLiteral("losetup"), {QStringLiteral("-f"), QStringLiteral("--show"), image});
    report(loop.startsWith(QLatin1String("/dev/loop")), QStringLiteral("attach test image"), loop);
    const QString name = QStringLiteral("diskforge-dust-test");
    const QString dev = QStringLiteral("/dev/mapper/") + name;
    sh(QStringLiteral("modprobe"), {QStringLiteral("dm-dust")});
    sh(QStringLiteral("dmsetup"), {QStringLiteral("create"), name, QStringLiteral("--table"),
                                   QStringLiteral("0 131072 dust %1 0 512").arg(loop)});
    // Sector 1000 starts a 4 KiB block; 50003 sits in the middle of one.
    for (const char *block : {"1000", "50003"})
        sh(QStringLiteral("dmsetup"), {QStringLiteral("message"), name, QStringLiteral("0"), QStringLiteral("addbadblock"), QLatin1String(block)});
    sh(QStringLiteral("dmsetup"), {QStringLiteral("message"), name, QStringLiteral("0"), QStringLiteral("enable")});

    auto scan = [&](QVector<quint64> *bad) {
        const int fd = ::open(dev.toLocal8Bit().constData(), O_RDONLY | O_DIRECT | O_CLOEXEC);
        if (fd < 0)
            return false;
        bool completed = false;
        SurfaceScan s(fd, quint64(original.size()));
        QObject::connect(&s, &SurfaceScan::finished, [&](bool c, const QVector<quint64> &b, int) { completed = c; *bad = b; });
        s.run();
        return completed;
    };

    QVector<quint64> bad;
    const bool scanned = scan(&bad);
    report(scanned && bad == QVector<quint64>{1000 * 512ull, 50003 * 512ull}, QStringLiteral("scan finds exactly the two bad sectors"),
           QStringLiteral("%1 found").arg(bad.size()));

    RepairResult result;
    {
        const int fd = ::open(dev.toLocal8Bit().constData(), O_RDWR | O_DIRECT | O_CLOEXEC);
        SectorRepair repair(fd, bad, 4096); // as on a drive with 4 KiB physical sectors, like the HGST
        QObject::connect(&repair, &SectorRepair::finished, [&](const RepairResult &r) { result = r; });
        repair.run();
    }
    report(result.error.isEmpty() && result.blocks == 2 && result.zeroed == 2 && result.stillBad == 0,
           QStringLiteral("repair rewrites both blocks"),
           QStringLiteral("blocks %1, zeroed %2, still bad %3 %4").arg(result.blocks).arg(result.zeroed).arg(result.stillBad).arg(result.error));

    QVector<quint64> after;
    report(scan(&after) && after.isEmpty(), QStringLiteral("a second scan finds nothing"));

    {
        QFile f(dev);
        if (f.open(QIODevice::ReadOnly)) {
            const QByteArray now = f.readAll();
            auto sector = [](const QByteArray &d, int s) { return d.mid(s * 512, 512); };
            bool neighboursKept = true;
            for (int s : {1001, 1002, 1007, 50000, 50002, 50004, 50007, 999})
                neighboursKept = neighboursKept && sector(now, s) == sector(original, s);
            report(neighboursKept, QStringLiteral("good sectors next to the bad ones keep their data"));
            report(sector(now, 1000) == QByteArray(512, '\0') && sector(now, 50003) == QByteArray(512, '\0'),
                   QStringLiteral("the unreadable sectors are zeros now"));
            report(now.left(1000 * 512) == original.left(1000 * 512), QStringLiteral("nothing else changed"));
        }
    }
    sh(QStringLiteral("dmsetup"), {QStringLiteral("remove"), name});
    sh(QStringLiteral("losetup"), {QStringLiteral("-d"), loop});
}

// Add-on parsing, matching and placeholder filling. Touches no disks.
void addonTests()
{
    const QString examples = QStringLiteral(SOURCE_DIR "/examples/addons");
    QMap<QString, Addon> byId;
    for (const QString &name : QDir(examples).entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
        const Addon a = Addons::parse(examples + QLatin1Char('/') + name + QStringLiteral("/addon.json"));
        report(a.error.isEmpty(), QStringLiteral("example add-on %1 parses").arg(name), a.error);
        byId.insert(a.id, a);
    }

    QTemporaryDir tmp;
    auto broken = [&](const QByteArray &json, const char *expect, const QString &what) {
        const QString file = tmp.filePath(QStringLiteral("addon.json"));
        QFile f(file);
        if (f.open(QIODevice::WriteOnly | QIODevice::Truncate))
            f.write(json);
        f.close();
        const Addon a = Addons::parse(file);
        report(a.error.contains(QLatin1String(expect)), what, a.error);
    };
    broken(R"({"id":"x","actions":[{"label":"L","command":["echo","{nope}"]}]})", "Unknown placeholder", QStringLiteral("unknown placeholder is rejected"));
    broken(R"({"id":"Bad Id","actions":[{"label":"L","command":["echo"]}]})", "id", QStringLiteral("bad id is rejected"));
    broken(R"({"id":"x","actions":[{"label":"L","when":["sometimes"],"command":["echo"]}]})", "Unknown condition", QStringLiteral("unknown condition is rejected"));
    broken(R"({"id":"x","actions":[]})", "No actions", QStringLiteral("add-on without actions is rejected"));
    broken("not json", "JSON", QStringLiteral("invalid JSON is rejected"));

    Disk usb;
    usb.device = QStringLiteral("/dev/sdz");
    usb.removable = true;
    usb.model = QStringLiteral("Test Stick");
    Volume vol;
    vol.device = QStringLiteral("/dev/sdz1");
    vol.label = QStringLiteral("My Stuff; rm -rf ~");
    vol.fsType = QStringLiteral("vfat");
    vol.hasFilesystem = true;
    vol.mountPoints = {QStringLiteral("/run/media/me/My Stuff")};
    usb.volumes = {vol};

    const AddonAction &terminal = byId.value(QStringLiteral("open-terminal")).actions.value(0);
    const AddonAction &backup = byId.value(QStringLiteral("backup-rsync")).actions.value(0);
    const AddonAction &smart = byId.value(QStringLiteral("smart-report")).actions.value(0);
    report(Addons::applies(terminal, usb, &usb.volumes[0], false), QStringLiteral("open-terminal is offered on a mounted volume"));
    Volume unmounted = vol;
    unmounted.mountPoints.clear();
    report(!Addons::applies(terminal, usb, &unmounted, false), QStringLiteral("but not on an unmounted one"));
    report(!Addons::applies(terminal, usb, nullptr, false), QStringLiteral("nor on the disk itself"));
    report(!Addons::applies(smart, usb, nullptr, false), QStringLiteral("smart-report isn't offered without health data"));
    Disk system = usb;
    system.isSystem = true;
    system.health.state = Health::State::Healthy;
    report(Addons::applies(smart, system, nullptr, false), QStringLiteral("smart-report is offered on the system disk (read-only)"));
    report(!Addons::applies(backup, system, &system.volumes[0], false), QStringLiteral("other add-ons aren't offered on the system disk"));

    QString error;
    const QStringList argv = Addons::expand(backup.command, usb, &usb.volumes[0], &error);
    const QStringList expected = {QStringLiteral("rsync"), QStringLiteral("-a"), QStringLiteral("--info=progress2"), QStringLiteral("--mkpath"),
                                  QStringLiteral("/run/media/me/My Stuff/"), QDir::homePath() + QStringLiteral("/Backups/My Stuff; rm -rf ~/")};
    report(argv == expected, QStringLiteral("placeholders fill in, and a nasty label stays one plain argument"), argv.join(QStringLiteral(" | ")));
    error.clear();
    report(Addons::expand(backup.command, usb, &unmounted, &error).isEmpty() && error.contains(QLatin1String("Mount")),
           QStringLiteral("{mountpoint} on an unmounted volume asks to mount first"), error);
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
        ok = run(udisks, {}, [&] { udisks.wipe(d); }, &message);
        report(!ok && message.contains(QLatin1String("running system")), QStringLiteral("wiping %1 is refused").arg(shortDevice(d.device)), message);
        ok = run(udisks, {}, [&] { udisks.powerOff(d); }, &message);
        report(!ok && message.contains(QLatin1String("running system")), QStringLiteral("powering off %1 is refused").arg(shortDevice(d.device)), message);
        ok = run(udisks, {}, [&] { udisks.setMountAtStartup(v, true); }, &message);
        report(!ok && message.contains(QLatin1String("running system")), QStringLiteral("startup mount on %1 is refused").arg(name), message);
        const int fd = openFd(udisks, d, true, false);
        report(fd < 0, QStringLiteral("raw write access to %1 is refused").arg(shortDevice(d.device)));
        return;
    }
    report(false, QStringLiteral("find the system disk"));
}

} // namespace

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);
    const bool root = geteuid() == 0;

    const QStringList args = app.arguments();
    auto needsRoot = [&](const char *suite) {
        if (!root)
            out << suite << " needs root (it sets up test devices)" << Qt::endl;
        return !root;
    };
    if (args.contains(QStringLiteral("--addons"))) {
        addonTests();
    } else if (args.contains(QStringLiteral("--gpt"))) {
        gptTests();
    } else if (args.contains(QStringLiteral("--copy"))) {
        copyTests();
    } else if (args.contains(QStringLiteral("--usage"))) {
        usageTests();
    } else if (args.contains(QStringLiteral("--blockmap"))) {
        if (needsRoot("--blockmap"))
            return 2;
        blockMapTests();
    } else if (app.arguments().contains(QStringLiteral("--badsectors"))) {
        if (!root) {
            out << "--badsectors needs root (it creates a test device with dmsetup)" << Qt::endl;
            return 2;
        }
        badSectorTests();
    } else if (app.arguments().contains(QStringLiteral("--guard"))) {
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
        for (const Disk &d : udisks.disks()) {
            if (d.isLoop && d.backingFile.startsWith(dir.path()) && d.blockPath != loop)
                loopDelete(d.blockPath);
        }
        loopDelete(loop);
    }

    out << (failures ? QStringLiteral("%1 step(s) failed").arg(failures) : QStringLiteral("All steps passed")) << Qt::endl;
    return failures ? 1 : 0;
}
