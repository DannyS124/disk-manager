// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// Wrapper around the UDisks2 D-Bus API. All writes go through UDisks2/polkit,
// nothing here opens block devices directly (raw access goes through UDisks2's
// OpenDevice/OpenForBenchmark, which also ask polkit).

#include <QObject>
#include <QString>
#include <QStringList>
#include <QTimer>
#include <QVariantMap>
#include <QVector>

#include <functional>

class QDBusMessage;

// Partition, or a filesystem directly on a disk.
struct Volume {
    QString objectPath;
    QString device;
    int number = 0; // 0 = fs on the whole disk
    quint64 offset = 0;
    quint64 size = 0;
    QString label;
    QString partName; // GPT only
    QString fsType;
    QString fsUsage;
    QString uuid;
    QString partType; // GUID (gpt) or "0x83" (dos)
    bool isContainer = false; // MBR extended
    bool isContained = false; // MBR logical
    bool isEfi = false;
    bool hasFilesystem = false;
    QStringList mountPoints;
    bool swapActive = false;
    bool encrypted = false;
    QString cleartextPath; // unlocked LUKS mapping
    bool cleartextHasFilesystem = false;
    QString cleartextFsType;
    QStringList cleartextMountPoints;
    QVariantMap fstab; // this volume's /etc/fstab entry, empty if none
    quint64 fsTotal = 0; // only known while mounted
    quint64 fsFree = 0;
    bool isSystem = false;

    // The object that holds the filesystem: the volume itself, or its unlocked cleartext side.
    QString filesystemPath() const { return encrypted ? cleartextPath : objectPath; }
    bool canMount() const { return encrypted ? cleartextHasFilesystem : hasFilesystem; }
    const QStringList &mounts() const { return encrypted ? cleartextMountPoints : mountPoints; }
    QString effectiveFsType() const { return encrypted && !cleartextFsType.isEmpty() ? cleartextFsType : fsType; }
};

struct Health {
    enum class State { Unknown, Healthy, Warning, Failing };
    State state = State::Unknown; // Unknown = the drive doesn't report SMART (USB sticks, images)
    QString summary;
    bool nvme = false;
    double temperatureC = -1;
    quint64 powerOnHours = 0;
    qint64 badSectors = -1; // ATA: reallocated + pending + uncorrectable, -1 if unknown
    int percentUsed = -1; // NVMe wear
    QStringList criticalWarnings; // NVMe
    QString selftestStatus;
    int selftestPercentRemaining = -1;
    quint64 updated = 0;
};

struct Disk {
    QString blockPath;
    QString drivePath; // "/" for loop devices
    QString device;
    QString model;
    QString serial;
    QString bus; // "usb" or empty
    QString tableType; // "gpt", "dos" or empty
    quint64 size = 0;
    int rotationRate = -1; // 0 = SSD, -1 = unknown
    bool removable = false;
    bool canPowerOff = false;
    bool readOnly = false;
    bool isLoop = false;
    QString backingFile; // loop devices
    bool isSystem = false;
    QString systemReason;
    bool isVentoy = false;
    Health health;
    QVector<Volume> volumes;   // sorted by offset
};

// A long-running UDisks2 operation (format, erase, check...).
struct Job {
    QString operation;
    double progress = 0;
    bool progressValid = false;
    QStringList objects;
    quint64 rate = 0; // bytes per second, 0 if unknown
};

struct FsType {
    QString id;
    QString name;
    QString hint;
    int maxLabel = 0;
    QString package; // provides the mkfs tool
    bool available = false;
    int resizeModes = 0; // ResizeMode flags, 0 = can't resize
    bool resizeAvailable = false;
    bool canCheck = false;
    bool canRepair = false;
};

// BDFSResizeFlags from libblockdev (Manager.CanResize)
enum ResizeMode {
    OfflineShrink = 2,
    OfflineGrow = 4,
    OnlineShrink = 8,
    OnlineGrow = 16,
};

struct ResizeLimits {
    bool possible = false;
    QString reason;
    quint64 minSize = 0;
    quint64 maxSize = 0; // up to the end of the free space after it
    quint64 used = 0; // 0 if not mounted
};

// One row of a drive's SMART attribute table.
struct SmartAttribute {
    int id = 0;
    QString name;
    int value = -1;
    int worst = -1;
    int threshold = -1;
    QString raw; // already decoded for display
    qint64 rawValue = -1; // sector counts, decoded
    bool failing = false;
};

// Volume or unallocated range, in disk order.
struct Span {
    int volume = -1; // -1 = unallocated
    quint64 offset = 0;
    quint64 size = 0;
    bool isFree() const { return volume < 0; }
};

QVector<Span> diskSpans(const Disk &disk);

class UDisks : public QObject
{
    Q_OBJECT
public:
    explicit UDisks(QObject *parent = nullptr);

    bool isAvailable() const { return m_error.isEmpty(); }
    QString lastError() const { return m_error; }
    const QVector<Disk> &disks() const { return m_disks; }
    const QVector<Job> &jobs() const { return m_jobs; }
    const QVector<FsType> &filesystems() const { return m_filesystems; }
    const Disk *diskOf(const Volume &volume) const;
    const Disk *diskByPath(const QString &blockPath) const;
    bool isBusy() const { return m_pending > 0; }
    QString daemonVersion() const;

    // false: fail instead of prompting (selftest)
    void setInteractive(bool interactive) { m_interactive = interactive; }

    void refresh();

    // Async, results via operationFinished. System disks are refused here as well.
    void mount(const Volume &volume);
    void unmount(const Volume &volume);
    void setLabel(const Volume &volume, const QString &label);
    // passphrase non-empty: LUKS2 container with the filesystem inside
    void format(const Volume &volume, const QString &fsType, const QString &label, const QString &passphrase = {});
    void deletePartition(const Volume &volume);
    // rounded to 1 MiB, clamped to the free range
    void createPartition(const Disk &disk, quint64 offset, quint64 size, const QString &fsType, const QString &label,
                         const QString &passphrase = {});
    void createPartitionTable(const Disk &disk, const QString &tableType); // "gpt" or "dos"
    // shrink: fs then partition; grow: partition then fs
    void resize(const Volume &volume, quint64 newSize);
    ResizeLimits resizeLimits(const Volume &volume) const;
    // Btrfs only resizes mounted, ext4 only shrinks unmounted
    bool resizeNeedsRemount(const Volume &volume, bool shrink) const;
    const FsType *filesystem(const QString &id) const;

    // Unmounts everything on the disk, locks encrypted volumes, then cuts power (USB drives).
    void powerOff(const Disk &disk);
    // Unmounts first; result also via checkFinished.
    void check(const Volume &volume);
    void repair(const Volume &volume);
    // /etc/fstab entry so the volume mounts at boot (under /mnt).
    void setMountAtStartup(const Volume &volume, bool enable);
    static QString startupMountPoint(const Volume &volume);
    // Disk images: attach as a loop device (read-only by default), detach again.
    void openImage(const QString &path, bool readOnly = true);
    void detachImage(const Disk &disk);
    // LUKS
    void unlock(const Volume &volume, const QString &passphrase);
    void lock(const Volume &volume);
    void changePassphrase(const Volume &volume, const QString &oldPassphrase, const QString &newPassphrase);
    // Overwrite the whole disk with zeros; leaves it without a partition table.
    void wipe(const Disk &disk);
    // SMART
    void smartUpdate(const Disk &disk);
    void smartSelftest(const Disk &disk, const QString &type); // "short" or "extended"
    QVector<SmartAttribute> smartAttributes(const Disk &disk); // blocking
    // Raw device access for the image writer and benchmark. Unmounts first; polkit asks
    // for a password. The fd arrives through deviceOpened; the caller closes it.
    void openDevice(const Disk &disk, bool writable, bool forBenchmark = false, bool direct = false);

signals:
    void changed();
    void operationFinished(bool ok, const QString &message);
    void checkFinished(const QString &objectPath, bool clean);
    void imageOpened(const QString &loopBlockPath);
    void deviceOpened(const QString &blockPath, int fd); // fd = -1 on failure (reported via operationFinished)

private slots:
    void onDBusSignal(const QDBusMessage &message);

private:
    using SuccessText = std::function<QString(const QDBusMessage &reply)>;
    void call(const QString &path, const QString &interface, const QString &method, const QVariantList &args,
              const SuccessText &success, const QString &failure);
    // call() that chains into `next` instead of reporting
    void callThen(const QString &path, const QString &interface, const QString &method, const QVariantList &args,
                  const QString &failure, const std::function<void(const QDBusMessage &reply)> &next);

    bool refuseSystem(const Disk *disk, const QString &failure);
    // Unmounts (and with lockEncrypted, locks) the given volumes one at a time, then runs `then`.
    void unmountThen(const QVector<Volume> &volumes, const QString &failure, const std::function<void()> &then,
                     bool lockEncrypted = false);
    QVariantMap options(QVariantMap extra = {}) const;
    void detectFilesystems();
    Health readHealth(const QString &drivePath, const QVariantMap &ata, const QVariantMap &nvme);

    QVector<Disk> m_disks;
    QVector<Job> m_jobs;
    QVector<FsType> m_filesystems;
    QString m_error;
    QTimer m_debounce;
    int m_pending = 0;
    bool m_interactive = true;
    // SMART attribute details, refetched only when the drive's SmartUpdated changes.
    struct HealthCache { quint64 updated = 0; qint64 badSectors = -1; qint64 mediaErrors = 0; int percentUsed = -1; bool attrFailing = false; };
    QMap<QString, HealthCache> m_healthCache;
};
