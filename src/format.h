// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "udisks.h"

#include <QString>

QString formatSize(quint64 bytes);
QString shortDevice(const QString &device);   // /dev/sda1 -> sda1
QString volumeTitle(const Volume &v); // "LABEL (sda1)"
QString volumeStatus(const Volume &v, bool brief = false);
QString diskKind(const Disk &d); // "NVMe SSD", "USB", ...
QString tableName(const Disk &d);
QString partitionTypeName(const QString &type);

// Drive names (labels, models, partition names) come from the drive, so whoever made it
// picked them. Control characters become spaces, and invisible ones (zero-width, or those
// that flip the text direction) are dropped, so a name can't hide or fake anything.
QString cleanName(const QString &name);
bool hasHiddenCharacters(const QString &text); // something cleanName() would change
