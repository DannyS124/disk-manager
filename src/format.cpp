// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#include "format.h"

#include <QCoreApplication>
#include <QHash>
#include <QLocale>
#include <QStringList>

namespace {

QString tr(const char *text)
{
    return QCoreApplication::translate("format", text);
}

} // namespace

QString formatSize(quint64 bytes)
{
    // 1024-based, like Windows
    return QLocale().formattedDataSize(qint64(bytes), 2, QLocale::DataSizeTraditionalFormat);
}

QString shortDevice(const QString &device)
{
    return device.section(QLatin1Char('/'), -1);
}

QString volumeTitle(const Volume &v)
{
    const QString name = !v.label.isEmpty() ? v.label : v.partName;
    const QString dev = shortDevice(v.device);
    return name.isEmpty() ? dev : QStringLiteral("%1 (%2)").arg(name, dev);
}

QString volumeStatus(const Volume &v, bool brief)
{
    auto mounted = [brief](const QStringList &mps) {
        if (brief && mps.size() > 1)
            return tr("Mounted at %1 (+%2)").arg(mps.first()).arg(mps.size() - 1);
        return tr("Mounted at %1").arg(mps.join(QStringLiteral(", ")));
    };

    QStringList parts;
    if (v.isEfi)
        parts << tr("EFI System");
    if (v.isSystem)
        parts << tr("System");

    if (v.isContainer)
        parts << tr("Extended partition");
    else if (v.fsType == QLatin1String("swap"))
        parts << (v.swapActive ? tr("Swap, in use") : tr("Swap, not in use"));
    else if (v.encrypted) {
        parts << tr("Encrypted");
        if (v.cleartextPath.isEmpty())
            parts << tr("Locked");
        else if (v.cleartextMountPoints.isEmpty())
            parts << tr("Unlocked");
        else
            parts << mounted(v.cleartextMountPoints);
    } else if (!v.mountPoints.isEmpty())
        parts << mounted(v.mountPoints);
    else if (v.hasFilesystem)
        parts << tr("Not mounted");
    else if (v.fsType == QLatin1String("LVM2_member"))
        parts << tr("LVM member");
    else if (v.fsUsage == QLatin1String("raid"))
        parts << tr("RAID member");
    else if (v.fsType.isEmpty())
        parts << tr("No file system");
    else
        parts << v.fsType;

    return parts.join(QStringLiteral(" · "));
}

QString diskKind(const Disk &d)
{
    if (d.isLoop)
        return tr("Disk image");
    if (d.bus == QLatin1String("usb"))
        return tr("USB");
    if (d.device.startsWith(QLatin1String("/dev/nvme")))
        return tr("NVMe SSD");
    if (d.device.startsWith(QLatin1String("/dev/mmcblk")))
        return tr("SD card");
    if (d.rotationRate > 0)
        return tr("HDD");
    if (d.rotationRate == 0)
        return tr("SSD");
    return tr("Disk");
}

QString tableName(const Disk &d)
{
    if (d.tableType == QLatin1String("gpt"))
        return tr("GPT");
    if (d.tableType == QLatin1String("dos"))
        return tr("MBR");
    if (!d.tableType.isEmpty())
        return d.tableType.toUpper();
    return d.volumes.isEmpty() ? tr("Not initialized") : tr("No partition table");
}

QString partitionTypeName(const QString &type)
{
    static const QHash<QString, const char *> names = {
        {QStringLiteral("c12a7328-f81f-11d2-ba4b-00a0c93ec93b"), "EFI System"},
        {QStringLiteral("21686148-6449-6e6f-744e-656564454649"), "BIOS boot"},
        {QStringLiteral("bc13c2ff-59e6-4262-a352-b275fd6f7172"), "Linux extended boot"},
        {QStringLiteral("0fc63daf-8483-4772-8e79-3d69d8477de4"), "Linux filesystem"},
        {QStringLiteral("4f68bce3-e8cd-4db1-96e7-fbcaf984b709"), "Linux root (x86-64)"},
        {QStringLiteral("933ac7e1-2eb4-4f13-b844-0e14e2aef915"), "Linux /home"},
        {QStringLiteral("0657fd6d-a4ab-43c4-84e5-0933c84b4f4f"), "Linux swap"},
        {QStringLiteral("e6d6d379-f507-44c2-a23c-238f2a3df928"), "Linux LVM"},
        {QStringLiteral("ca7d7ccb-63ed-4c53-861c-1742536059cc"), "Linux LUKS"},
        {QStringLiteral("ebd0a0a2-b9e5-4433-87c0-68b6b72699c7"), "Microsoft basic data"},
        {QStringLiteral("e3c9e316-0b5c-4db8-817d-f92df00215ae"), "Microsoft reserved"},
        {QStringLiteral("de94bba4-06d1-4d40-a16a-bfd50179d6ac"), "Windows recovery"},
        {QStringLiteral("0x07"), "NTFS / exFAT"},
        {QStringLiteral("0x0b"), "FAT32"},
        {QStringLiteral("0x0c"), "FAT32 (LBA)"},
        {QStringLiteral("0x05"), "Extended"},
        {QStringLiteral("0x0f"), "Extended (LBA)"},
        {QStringLiteral("0x82"), "Linux swap"},
        {QStringLiteral("0x83"), "Linux"},
        {QStringLiteral("0x8e"), "Linux LVM"},
        {QStringLiteral("0xef"), "EFI System"},
    };
    const auto it = names.constFind(type.toLower());
    return it == names.constEnd() ? type : QStringLiteral("%1 (%2)").arg(tr(*it), type);
}
