// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// GPT keeps a backup of its header and partition list in the disk's last sectors. After
// copying a disk onto a bigger one, that backup sits in the middle of the new disk;
// relocateBackup moves it to the real end and lets the partition list grow into the
// new space. Works on the fd a clone or restore already holds, so no second prompt.

#include <QString>
#include <QStringList>
#include <QVector>

namespace gpt {

struct Result {
    bool ok = false;
    QString error;
};

// sectorSize 0 = ask the device (regular files report 512).
bool isGpt(int fd, int sectorSize = 0);
// newGuids: give the disk and its partitions new unique IDs, so a clone kept next to
// its original doesn't share them.
Result relocateBackup(int fd, int sectorSize = 0, bool newGuids = false);

// --- Reading a partition table as it is on the disk (the inspector) -----------------
// Everything here only reads, and every size or position taken from the disk is checked
// before it's used: a damaged or hostile table can't make it read out of bounds.

struct Entry {
    int index = 0; // slot in the list, from 1
    QString type;  // GUID, lower case
    QString guid;
    quint64 firstLba = 0;
    quint64 lastLba = 0;
    quint64 attributes = 0;
    QString name;
};

struct Header {
    quint64 lba = 0;
    bool present = false;     // "EFI PART" is there
    bool sizeOk = false;      // a header size DiskForge can check
    bool headerCrcOk = false;
    quint32 revision = 0;
    quint32 headerSize = 0;
    quint64 myLba = 0;
    quint64 alternateLba = 0;
    quint64 firstUsable = 0;
    quint64 lastUsable = 0;
    QString diskGuid;
    quint64 entriesLba = 0;
    quint32 entryCount = 0;
    quint32 entrySize = 0;
    quint32 entriesCrc = 0;
    bool entriesRead = false; // the list fit on the disk and was read
    bool entriesCrcOk = false;
    QVector<Entry> entries;   // the used slots
    bool valid() const { return present && sizeOk && headerCrcOk && entriesCrcOk; }
};

struct MbrEntry {
    int index = 0; // 1-4
    bool bootable = false;
    quint8 type = 0;
    quint32 firstLba = 0;
    quint32 sectors = 0;
};

struct Mbr {
    bool signature = false; // 55 aa
    QVector<MbrEntry> entries; // the used ones
    bool protective() const { return entries.size() == 1 && entries.first().type == 0xEE; }
    bool hybrid() const; // GPT's 0xEE next to real MBR partitions
};

struct Report {
    int sectorSize = 512;
    quint64 lastLba = 0;
    Mbr mbr;
    Header primary;
    Header backup;           // at the last sector
    bool backupElsewhere = false; // the primary points somewhere else for its backup
    bool backupMatches = false;   // same disk ID and the same partition list
    QStringList problems;    // in plain words; empty when all is well
};

Mbr parseMbr(const char *sector0); // the first 512 bytes
// A header sector at `lba`; entries are read separately.
Header parseHeader(const char *sector, int sectorSize, quint64 lba);
// Fills in the header's entries from the raw list, if its size and checksum allow.
void parseEntries(Header &header, const char *list, quint64 listBytes);
Report inspect(int fd, int sectorSize = 0);
QString guidText(const char *p); // the mixed-endian GPT form
// "00000000  45 46 49 20 ...  |EFI PART...|", 16 bytes a line.
QString hexDump(const QByteArray &data, quint64 firstOffset);

} // namespace gpt
