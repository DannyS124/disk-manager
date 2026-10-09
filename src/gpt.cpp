// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#include "gpt.h"

#include "blockio.h"

#include <QObject>
#include <QRandomGenerator>

#include <cstring>
#include <unistd.h>

namespace gpt {

namespace {

// All GPT fields are little-endian.
quint32 le32(const char *p)
{
    quint32 v;
    std::memcpy(&v, p, 4);
    return v;
}
quint64 le64(const char *p)
{
    quint64 v;
    std::memcpy(&v, p, 8);
    return v;
}
void put32(char *p, quint32 v) { std::memcpy(p, &v, 4); }
void put64(char *p, quint64 v) { std::memcpy(p, &v, 8); }

quint32 crc32(const char *data, size_t len)
{
    static quint32 table[256] = {};
    static bool ready = false;
    if (!ready) {
        for (quint32 i = 0; i < 256; ++i) {
            quint32 c = i;
            for (int k = 0; k < 8; ++k)
                c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            table[i] = c;
        }
        ready = true;
    }
    quint32 c = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; ++i)
        c = table[(c ^ quint8(data[i])) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

// Header field offsets (UEFI spec, table 5-5).
constexpr int kHeaderSize = 12, kHeaderCrc = 16, kMyLba = 24, kAlternateLba = 32, kLastUsable = 48, kDiskGuid = 56, kEntriesLba = 72, kEntryCount = 80, kEntrySize = 84, kEntriesCrc = 88;

quint32 headerCrc(char *header)
{
    const quint32 size = le32(header + kHeaderSize);
    char copy[512];
    std::memcpy(copy, header, size);
    put32(copy + kHeaderCrc, 0);
    return crc32(copy, size);
}

void randomGuid(char *p)
{
    QRandomGenerator::system()->fillRange(reinterpret_cast<quint32 *>(p), 4);
    p[7] = char((quint8(p[7]) & 0x0F) | 0x40); // version 4 (mixed-endian field)
    p[8] = char((quint8(p[8]) & 0x3F) | 0x80); // variant 1
}

} // namespace

bool isGpt(int fd, int sectorSize)
{
    const int sector = sectorSize > 0 ? sectorSize : blockio::logicalSize(fd);
    blockio::Buffer buf = blockio::alignedBuffer(size_t(std::max(sector, int(blockio::kAlign))));
    return buf && blockio::readAt(fd, buf.get(), quint64(sector), quint64(sector))
        && std::memcmp(buf.get(), "EFI PART", 8) == 0;
}

Result relocateBackup(int fd, int sectorSize, bool newGuids)
{
    Result r;
    const quint64 sector = quint64(sectorSize > 0 ? sectorSize : blockio::logicalSize(fd));
    const quint64 lastLba = blockio::deviceSize(fd) / sector - 1;

    blockio::Buffer header = blockio::alignedBuffer(std::max<size_t>(sector, blockio::kAlign));
    if (!header || !blockio::readAt(fd, header.get(), sector, sector)) {
        r.error = QObject::tr("Couldn't read the partition table");
        return r;
    }
    char *h = header.get();
    if (std::memcmp(h, "EFI PART", 8) != 0) {
        r.error = QObject::tr("Not a GPT disk");
        return r;
    }
    const quint32 hsize = le32(h + kHeaderSize);
    if (hsize < 92 || hsize > 512 || hsize > sector || headerCrc(h) != le32(h + kHeaderCrc)) {
        r.error = QObject::tr("The partition table header is damaged, so it was left alone");
        return r;
    }

    const quint64 entriesLba = le64(h + kEntriesLba);
    const quint32 count = le32(h + kEntryCount);
    const quint32 entrySize = le32(h + kEntrySize);
    if (entrySize < 128 || count == 0 || quint64(count) * entrySize > 16 * 1024 * 1024) {
        r.error = QObject::tr("The partition table is damaged, so it was left alone");
        return r;
    }
    const quint64 entriesBytes = quint64(count) * entrySize;
    const quint64 entriesSectors = (entriesBytes + sector - 1) / sector;
    if (lastLba < entriesSectors + 34 || lastLba >= quint64(1) << 48) {
        r.error = QObject::tr("The disk is too small for its partitions");
        return r;
    }
    blockio::Buffer entries = blockio::alignedBuffer(size_t(entriesSectors * sector));
    if (!entries || !blockio::readAt(fd, entries.get(), entriesSectors * sector, entriesLba * sector)
        || crc32(entries.get(), entriesBytes) != le32(h + kEntriesCrc)) {
        r.error = QObject::tr("The partition list is damaged, so it was left alone");
        return r;
    }

    // Every partition has to end before the new backup area.
    const quint64 oldBackupLba = le64(h + kAlternateLba);
    const quint64 newLastUsable = lastLba - entriesSectors - 1;
    quint64 lastEnd = 0;
    for (quint32 i = 0; i < count; ++i) {
        char *e = entries.get() + quint64(i) * entrySize;
        static const char zero[16] = {};
        if (std::memcmp(e, zero, 16) == 0)
            continue; // unused slot
        lastEnd = std::max(lastEnd, le64(e + 40));
        if (le64(e + 40) > newLastUsable) {
            r.error = QObject::tr("The disk is too small for its partitions");
            return r;
        }
        if (newGuids)
            randomGuid(e + 16);
    }
    if (newGuids)
        randomGuid(h + kDiskGuid);

    const quint32 entriesCrc = crc32(entries.get(), entriesBytes);
    put64(h + kAlternateLba, lastLba);
    put64(h + kLastUsable, newLastUsable);
    put32(h + kEntriesCrc, entriesCrc);
    put32(h + kHeaderCrc, headerCrc(h));

    blockio::Buffer backup = blockio::alignedBuffer(std::max<size_t>(sector, blockio::kAlign));
    std::memcpy(backup.get(), h, sector);
    char *b = backup.get();
    put64(b + kMyLba, lastLba);
    put64(b + kAlternateLba, 1);
    put64(b + kEntriesLba, lastLba - entriesSectors);
    put32(b + kHeaderCrc, headerCrc(b));

    // Backup first: if anything stops halfway, the primary still describes a valid disk.
    if (!blockio::writeAt(fd, entries.get(), entriesSectors * sector, (lastLba - entriesSectors) * sector)
        || !blockio::writeAt(fd, b, sector, lastLba * sector) || ::fdatasync(fd) != 0) {
        r.error = QObject::tr("Couldn't write the backup partition table");
        return r;
    }
    if ((newGuids && !blockio::writeAt(fd, entries.get(), entriesSectors * sector, entriesLba * sector))
        || !blockio::writeAt(fd, h, sector, sector)) {
        r.error = QObject::tr("Couldn't write the partition table");
        return r;
    }

    // The old backup header now sits in free space in the middle of the disk; clear it
    // so nothing mistakes it for the real one later.
    if (oldBackupLba != lastLba && oldBackupLba > lastEnd && oldBackupLba < lastLba - entriesSectors) {
        blockio::Buffer zero = blockio::alignedBuffer(std::max<size_t>(sector, blockio::kAlign));
        std::memset(zero.get(), 0, sector);
        blockio::writeAt(fd, zero.get(), sector, oldBackupLba * sector);
    }

    // The protective MBR's single partition covers the whole disk (capped at 32 bits).
    blockio::Buffer mbr = blockio::alignedBuffer(std::max<size_t>(sector, blockio::kAlign));
    if (blockio::readAt(fd, mbr.get(), sector, 0) && quint8(mbr.get()[450]) == 0xEE) {
        put32(mbr.get() + 458, quint32(std::min<quint64>(lastLba, 0xFFFFFFFFu)));
        blockio::writeAt(fd, mbr.get(), sector, 0);
    }
    if (::fdatasync(fd) != 0) {
        r.error = QObject::tr("Couldn't write the partition table");
        return r;
    }
    r.ok = true;
    return r;
}

} // namespace gpt
