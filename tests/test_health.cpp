// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

// The health verdict, from recorded SMART values: what counts, what's only a note, and
// counters that never go down only warning when they grow.

#include "testkit.h"

#include "../src/health.h"

#include <QTemporaryDir>

namespace {

SmartAttribute attr(int id, qint64 raw, int value = 100, int threshold = 0)
{
    SmartAttribute a;
    a.id = id;
    a.rawValue = raw;
    a.value = value;
    a.worst = value;
    a.threshold = threshold;
    a.failing = threshold > 0 && value <= threshold;
    return a;
}

QStringList codes(const health::Verdict &v)
{
    QStringList out;
    for (const HealthReason &r : v.reasons)
        out << r.code + QLatin1Char(r.level == HealthReason::Level::Failing ? '!' : r.level == HealthReason::Level::Warning ? '?' : '.');
    return out;
}

} // namespace

void healthTests()
{
    QTemporaryDir config;
    qputenv("XDG_CONFIG_HOME", QFile::encodeName(config.path()));

    // This PC's HGST, as UDisks reported it: 16 replaced sectors, 4 command timeouts (packed).
    health::AtaInput hgst;
    hgst.attributes = {attr(5, 16), attr(10, 0), attr(184, 0), attr(187, 0), attr(188, 4), attr(196, 2), attr(197, 0),
                       attr(198, 0), attr(199, 0)};
    hgst.temperatureC = 26;
    health::Verdict v = health::ata(hgst);
    report(v.state == Health::State::Warning && codes(v) == QStringList{QStringLiteral("reallocated?"), QStringLiteral("timeouts.")}
               && v.summary.contains(QLatin1String("16")),
           QStringLiteral("the HGST: replaced sectors warn, old timeouts are only a note"), codes(v).join(QLatin1Char(' ')) + QStringLiteral(" | ") + v.summary);

    health::AtaInput clean;
    clean.attributes = {attr(5, 0), attr(197, 0), attr(198, 0), attr(187, 0), attr(199, 0)};
    v = health::ata(clean);
    report(v.state == Health::State::Healthy && v.reasons.isEmpty(), QStringLiteral("a clean drive is healthy"), codes(v).join(QLatin1Char(' ')));

    health::AtaInput uncorrectable = clean;
    uncorrectable.attributes[3] = attr(187, 3);
    v = health::ata(uncorrectable);
    report(v.state == Health::State::Warning && codes(v).contains(QLatin1String("uncorrectable?")),
           QStringLiteral("errors it couldn't fix (187) warn"), codes(v).join(QLatin1Char(' ')));

    health::AtaInput cable = clean;
    cable.attributes[4] = attr(199, 12);
    v = health::ata(cable);
    report(v.state == Health::State::Healthy && codes(v) == QStringList{QStringLiteral("crc.")},
           QStringLiteral("old connection errors are only a note"), codes(v).join(QLatin1Char(' ')));
    cable.seen.insert(199, 10);
    v = health::ata(cable);
    report(v.state == Health::State::Warning && codes(v) == QStringList{QStringLiteral("crc?")} && v.reasons[0].text.contains(QLatin1String("cable")),
           QStringLiteral("connection errors that went up say to check the cable"), v.reasons.value(0).text);

    health::AtaInput failing = clean;
    failing.attributes << attr(1, 0, 40, 62);
    failing.failingNow = 1;
    v = health::ata(failing);
    report(v.state == Health::State::Failing && v.summary.contains(QLatin1String("back up")),
           QStringLiteral("a number past its maker's limit is failing"), v.summary);
    health::AtaInput says = clean;
    says.driveSaysFailing = true;
    report(health::ata(says).state == Health::State::Failing, QStringLiteral("the drive saying it's failing is failing"));

    health::AtaInput hot = clean;
    hot.temperatureC = 58;
    report(codes(health::ata(hot)) == QStringList{QStringLiteral("hot?")}, QStringLiteral("a hard drive at 58 °C runs hot"));
    hot.ssd = true;
    report(health::ata(hot).reasons.isEmpty(), QStringLiteral("an SSD at 58 °C is fine"));

    health::AtaInput ssd = clean;
    ssd.ssd = true;
    ssd.attributes << attr(231, 0, 87);
    report(health::ata(ssd).lifeLeft == 87, QStringLiteral("SSD life left is read from its wear attribute"));

    // NVMe
    health::NvmeInput nvme;
    nvme.availableSpare = 100;
    nvme.spareThreshold = 10;
    nvme.percentUsed = 3;
    nvme.temperatureC = 41;
    nvme.warningTempC = 82;
    report(health::nvme(nvme).state == Health::State::Healthy, QStringLiteral("the Samsung 990 PRO is healthy"));
    health::NvmeInput worn = nvme;
    worn.percentUsed = 104;
    report(codes(health::nvme(worn)) == QStringList{QStringLiteral("worn?")}, QStringLiteral("an NVMe past its rated life warns"));
    health::NvmeInput low = nvme;
    low.availableSpare = 15;
    report(codes(health::nvme(low)) == QStringList{QStringLiteral("spare-low?")}, QStringLiteral("spare close to its threshold warns"));
    low.availableSpare = 5;
    report(health::nvme(low).state == Health::State::Failing, QStringLiteral("spare under its threshold is failing"));
    health::NvmeInput hotNvme = nvme;
    hotNvme.temperatureC = 83;
    report(codes(health::nvme(hotNvme)) == QStringList{QStringLiteral("hot?")}, QStringLiteral("an NVMe past its own warning temperature warns"));
    health::NvmeInput critical = nvme;
    critical.criticalWarnings = {QStringLiteral("readonly")};
    report(health::nvme(critical).state == Health::State::Failing, QStringLiteral("an NVMe that went read-only is failing"));
    health::NvmeInput errors = nvme;
    errors.mediaErrors = 2;
    report(codes(health::nvme(errors)) == QStringList{QStringLiteral("media-errors?")}, QStringLiteral("media errors warn"));

    // Remembered counters: first values stick until acknowledged.
    const QString key = QStringLiteral("Test_Drive_123");
    health::remember(key, {attr(199, 3), attr(188, 1)});
    health::remember(key, {attr(199, 9), attr(188, 1)});
    report(health::seen(key).value(199) == 3, QStringLiteral("the first value seen is kept"));
    health::acknowledge(key, {attr(199, 9), attr(188, 1)});
    report(health::seen(key).value(199) == 9, QStringLiteral("dismissing a warning takes the current values"));

    // The banner: dismissed until something new or a bigger number.
    Health shown;
    shown.reasons = {{HealthReason::Level::Warning, QStringLiteral("reallocated"), 16, QString()},
                     {HealthReason::Level::Note, QStringLiteral("timeouts"), 4, QString()}};
    const QStringList dismissed = health::signature(shown);
    report(dismissed == QStringList{QStringLiteral("reallocated=16")} && !health::worseThan(shown, dismissed),
           QStringLiteral("a dismissed warning stays dismissed"), dismissed.join(QLatin1Char(' ')));
    Health more = shown;
    more.reasons[0].number = 24;
    Health other = shown;
    other.reasons << HealthReason{HealthReason::Level::Warning, QStringLiteral("pending"), 2, QString()};
    Health better = shown;
    better.reasons[0].number = 8;
    report(health::worseThan(more, dismissed) && health::worseThan(other, dismissed) && !health::worseThan(better, dismissed)
               && health::worseThan(shown, {}),
           QStringLiteral("it comes back when a number goes up or something new appears, not when it gets better"));

    int described = 0;
    for (const int id : {1, 3, 4, 5, 7, 9, 10, 12, 177, 184, 187, 188, 190, 194, 196, 197, 198, 199, 231, 241})
        described += health::describe(id).name.isEmpty() ? 0 : 1;
    report(described == 20 && health::describe(5).counts && health::describe(199).counts && !health::describe(9).counts,
           QStringLiteral("the common attributes have plain names, and the ones that count are marked"));
}
