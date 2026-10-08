#pragma once

// Wrapper around the UDisks2 D-Bus API. All writes go through UDisks2/polkit,
// nothing here opens block devices directly.

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
    QStringList cleartextMountPoints;
    quint64 fsTotal = 0; // only known while mounted
    quint64 fsFree = 0;
    bool isSystem = false;
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
    bool isLoop = false;
    QString backingFile; // loop devices
    bool isSystem = false;
    QString systemReason;
    bool isVentoy = false;
    QVector<Volume> volumes;   // sorted by offset
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
    const QVector<FsType> &filesystems() const { return m_filesystems; }
    const Disk *diskOf(const Volume &volume) const;
    bool isBusy() const { return m_pending > 0; }

    // false: fail instead of prompting (selftest)
    void setInteractive(bool interactive) { m_interactive = interactive; }

    void refresh();

    // Async, results via operationFinished. System disks are refused here as well.
    void mount(const Volume &volume);
    void unmount(const Volume &volume);
    void setLabel(const Volume &volume, const QString &label);
    void format(const Volume &volume, const QString &fsType, const QString &label);
    void deletePartition(const Volume &volume);
    // rounded to 1 MiB, clamped to the free range
    void createPartition(const Disk &disk, quint64 offset, quint64 size, const QString &fsType, const QString &label);
    void createPartitionTable(const Disk &disk, const QString &tableType); // "gpt" or "dos"
    // shrink: fs then partition; grow: partition then fs
    void resize(const Volume &volume, quint64 newSize);
    ResizeLimits resizeLimits(const Volume &volume) const;
    // Btrfs only resizes mounted, ext4 only shrinks unmounted
    bool resizeNeedsRemount(const Volume &volume, bool shrink) const;
    const FsType *filesystem(const QString &id) const;

signals:
    void changed();
    void operationFinished(bool ok, const QString &message);

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
    void unmountThen(const QVector<Volume> &volumes, const QString &failure, const std::function<void()> &then);
    QVariantMap options(QVariantMap extra = {}) const;
    void detectFilesystems();

    QVector<Disk> m_disks;
    QVector<FsType> m_filesystems;
    QString m_error;
    QTimer m_debounce;
    int m_pending = 0;
    bool m_interactive = true;
};
