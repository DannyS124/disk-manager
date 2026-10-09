// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#include "powerbox.h"

#include "drivepower.h"
#include "udisks.h"

#include <QCheckBox>
#include <QComboBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QTimer>

bool PowerBox::applies(const Disk &disk)
{
    return !disk.isLoop && !disk.nvmeNamespace && disk.rotationRate != 0 && (disk.ataPm || disk.ataApm || disk.ataWriteCache);
}

PowerBox::PowerBox(UDisks *udisks, const Disk &disk, QWidget *parent)
    : QGroupBox(tr("Power"), parent)
    , m_udisks(udisks)
    , m_blockPath(disk.blockPath)
    , m_current(disk.driveConfiguration)
    , m_cacheWas(disk.ataWriteCacheEnabled)
    , m_state(new QLabel)
    , m_sleep(new QPushButton(tr("Sleep Now")))
    , m_save(new QPushButton(tr("Save")))
{
    auto *form = new QFormLayout(this);

    auto *now = new QHBoxLayout;
    now->addWidget(m_state, 1);
    now->addWidget(m_sleep);
    form->addRow(tr("Now:"), now);
    bool inUse = false;
    for (const Volume &v : disk.volumes)
        inUse = inUse || !v.mounts().isEmpty() || v.swapActive;
    m_sleep->setEnabled(disk.ataPm && !inUse);
    m_sleep->setToolTip(inUse ? tr("Something on it is mounted. Unmount it first.") : tr("Spins it down now. It wakes up by itself when it's used."));
    connect(m_sleep, &QPushButton::clicked, this, [this] {
        if (const Disk *d = m_udisks->diskByPath(m_blockPath))
            m_udisks->sleepNow(*d);
        QTimer::singleShot(1500, this, &PowerBox::showState);
    });

    // Each starts at what UDisks keeps for the drive, or "the drive's own" when it keeps nothing.
    auto combo = [this](const QVector<drivepower::Choice> &choices, const QString &key) {
        auto *box = new QComboBox;
        box->addItem(tr("The drive's own setting"), drivepower::kDriveDefault);
        for (const drivepower::Choice &c : choices)
            box->addItem(c.label, c.value);
        if (m_current.contains(key)) {
            const int value = m_current.value(key).toInt();
            int index = box->findData(value);
            if (index < 0) {
                box->addItem(key == QLatin1String("ata-pm-standby") ? tr("After %1").arg(drivepower::standbyText(value)) : tr("Level %1").arg(value), value);
                index = box->count() - 1;
            }
            box->setCurrentIndex(index);
        }
        connect(box, &QComboBox::currentIndexChanged, this, &PowerBox::changed);
        return box;
    };
    if (disk.ataPm) {
        m_standby = combo(drivepower::standbyChoices(), QStringLiteral("ata-pm-standby"));
        form->addRow(tr("Spin down:"), m_standby);
    }
    if (disk.ataApm) {
        m_apm = combo(drivepower::apmChoices(), QStringLiteral("ata-apm-level"));
        form->addRow(tr("Power saving:"), m_apm);
    }
    if (disk.ataWriteCache) {
        m_cache = new QCheckBox(tr("Write cache (faster; what's in it is lost if the power goes off)"));
        m_cache->setChecked(m_current.contains(QStringLiteral("ata-write-cache-enabled"))
                                ? m_current.value(QStringLiteral("ata-write-cache-enabled")).toBool()
                                : m_cacheWas);
        connect(m_cache, &QCheckBox::toggled, this, &PowerBox::changed);
        form->addRow(QString(), m_cache);
    }
    auto *note = new QLabel(tr("Saved settings are set again every time the drive shows up. Spinning down often wears a drive "
                               "faster, and so do the lowest power-saving levels on laptop drives."));
    note->setWordWrap(true);
    form->addRow(note);
    auto *saveRow = new QHBoxLayout;
    saveRow->addStretch();
    saveRow->addWidget(m_save);
    form->addRow(saveRow);
    m_save->setEnabled(false);
    connect(m_save, &QPushButton::clicked, this, [this] {
        const QVariantMap next = configuration();
        if (const Disk *d = m_udisks->diskByPath(m_blockPath))
            m_udisks->setPowerSettings(*d, next);
        m_current = next;
        changed();
    });
    showState();
}

QVariantMap PowerBox::configuration() const
{
    const int cache = !m_cache ? drivepower::kKeep
        : m_cache->isChecked() == (m_current.contains(QStringLiteral("ata-write-cache-enabled"))
                                       ? m_current.value(QStringLiteral("ata-write-cache-enabled")).toBool()
                                       : m_cacheWas)
        ? drivepower::kKeep // unchanged: no new setting
        : int(m_cache->isChecked());
    return drivepower::configuration(m_current, m_standby ? m_standby->currentData().toInt() : drivepower::kKeep,
                                     m_apm ? m_apm->currentData().toInt() : drivepower::kKeep, cache);
}

void PowerBox::changed()
{
    m_save->setEnabled(configuration() != m_current);
}

void PowerBox::showState()
{
    const Disk *d = m_udisks->diskByPath(m_blockPath);
    const int state = d ? m_udisks->powerState(*d) : -1;
    m_state->setText(state < 0 ? tr("Unknown") : drivepower::stateText(state));
}
