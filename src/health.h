// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// What a drive's SMART data means, in plain words, and the overall verdict. The attributes
// that count are the ones Backblaze's drive statistics show going with failures (5, 187,
// 188, 197, 198), plus a few that point at the hardware (10, 184) or the cable (199).
// Counters that never go down are only warned about when they've grown since DiskForge
// last saw them (or since the warning was dismissed).

#include "udisks.h"

#include <QMap>
#include <QString>
#include <QVector>

namespace health {

struct AttributeInfo {
    QString name;    // plain name, like "Replaced sectors"
    QString meaning; // a sentence or two for the Health window
    bool counts = false; // goes into the verdict
};
AttributeInfo describe(int id);

struct AtaInput {
    bool driveSaysFailing = false;
    int failingNow = 0;   // attributes at or under their threshold
    int failedBefore = 0; // attributes that were, at some point
    QVector<SmartAttribute> attributes;
    QMap<int, qint64> seen; // raw values DiskForge saw before (counters that never go down)
    double temperatureC = -1;
    bool ssd = false;
};

struct NvmeInput {
    QStringList criticalWarnings; // UDisks' names: spare, temperature, degraded, readonly, ...
    int availableSpare = -1;
    int spareThreshold = -1;
    int percentUsed = -1;
    qint64 mediaErrors = 0;
    double temperatureC = -1;
    double warningTempC = -1;
};

struct Verdict {
    Health::State state = Health::State::Healthy;
    QString summary;
    QVector<HealthReason> reasons; // worst first
    int lifeLeft = -1;             // SATA SSDs that report it
};

Verdict ata(const AtaInput &in);
Verdict nvme(const NvmeInput &in);

// Where a drive's remembered values and dismissals are kept: its UDisks Drive.Id (the same
// drive keeps it on any port), or its object name when it has none.
QString keyFor(const QString &driveId, const QString &drivePath);

// A dismissed warning banner: "code=number" for each warning or failure. The banner comes
// back when there's one that wasn't there then, or a number that went up.
QStringList signature(const Health &h);
bool worseThan(const Health &h, const QStringList &dismissed);

// The counters whose growth matters (184, 188, 199), with the values DiskForge remembers
// for a drive. remember() stores values it hasn't seen yet; acknowledge() takes the
// current ones as the new starting point (a dismissed warning).
QList<int> watchedCounters();
QMap<int, qint64> seen(const QString &driveKey);
void remember(const QString &driveKey, const QVector<SmartAttribute> &attributes);
void acknowledge(const QString &driveKey, const QVector<SmartAttribute> &attributes);

} // namespace health
