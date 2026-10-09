// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// Hard drive power settings, as UDisks keeps them (Drive.Configuration): when to spin down,
// how hard to save power (APM), and the write cache. UDisks saves them in /etc/udisks2 and
// sets them again whenever the drive shows up.

#include <QString>
#include <QVariantMap>
#include <QVector>

namespace drivepower {

constexpr int kDriveDefault = -2; // no setting: the drive does what it does by itself
constexpr int kKeep = -1;         // leave this one as it is

struct Choice {
    int value;
    QString label;
};
QVector<Choice> standbyChoices(); // hdparm -S values: 0 never, 1-240 five-second steps, 241-251 half hours
QVector<Choice> apmChoices();     // 1 most saving ... 254 most performance
QString standbyText(int value);   // "20 minutes", for any value

// The full configuration to set: the current one with these changed. kKeep leaves a key
// alone, kDriveDefault removes it; writeCache is kKeep, 0 or 1.
QVariantMap configuration(const QVariantMap &current, int standby, int apm, int writeCache);

QString stateText(int state); // from PmGetState: "Spinning", "Asleep (spun down)", ...

} // namespace drivepower
