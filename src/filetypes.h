// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QString>
#include <QVector>

#include <functional>

// The kinds of file Find Lost Files can find by their contents alone, when there's no file
// system left to say where they are. Each one starts with something recognisable, and each
// one says (somewhere in itself) how long it is or where it ends: a PNG's chunks end with IEND,
// a ZIP ends with its central directory, an MP4 is a row of boxes that each give their size.
//
// Everything here reads data from a drive that may be damaged or may hold anything at all, so
// every number read is checked before it's used, and every walk has a limit.
namespace filetypes {

enum class Category : quint8 { Picture, Document, Video, Music, Archive, Other };

struct Type {
    QString id;        // also the extension: "jpg", "docx"
    Category category;
    QString name;      // "JPEG picture"
};

// Every type there is. A type's number is its place in this list.
const QVector<Type> &types();
int typeNumber(const QString &id); // -1 for an unknown one
QString categoryName(Category category);

// Reads a candidate file's bytes: `at` counts from its first byte. Returns how many it got,
// fewer at the end of the drive.
using Reader = std::function<qint64(quint64 at, char *buf, qint64 len)>;

// The types whose start this could be: `head` is the first bytes of a sector (at least 32).
QVector<int> candidates(const uchar *head, int len);

struct Measured {
    int type = -1;         // can be more exact than what was asked (a .zip that's a .docx)
    quint64 size = 0;      // 0: not one after all
    bool complete = false; // its structure checks out from start to end
};
// Works out where a file of this type that starts at `read`'s byte 0 ends.
Measured measure(int type, const Reader &read);

// The largest file of this type looked for.
quint64 sizeLimit(int type);

// CRC-32 as PNG, ZIP and 7z use it.
quint32 crc32(const uchar *data, qint64 len, quint32 crc = 0);

} // namespace filetypes
