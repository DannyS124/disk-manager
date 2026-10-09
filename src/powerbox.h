// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// The Power part of a hard drive's Properties: what it's doing now, Sleep Now, when to spin
// down, how hard to save power, and the write cache.

#include <QGroupBox>
#include <QVariantMap>

class QCheckBox;
class QComboBox;
class QLabel;
class QPushButton;
class UDisks;
struct Disk;

class PowerBox : public QGroupBox
{
    Q_OBJECT
public:
    PowerBox(UDisks *udisks, const Disk &disk, QWidget *parent = nullptr);
    static bool applies(const Disk &disk); // a hard drive (ATA, spinning) that has any of these
    QVariantMap configuration() const;      // what Save would set

private:
    void showState();
    void changed();

    UDisks *m_udisks;
    QString m_blockPath;
    QVariantMap m_current;
    bool m_cacheWas = false;
    QLabel *m_state;
    QPushButton *m_sleep;
    QComboBox *m_standby = nullptr;
    QComboBox *m_apm = nullptr;
    QCheckBox *m_cache = nullptr;
    QPushButton *m_save;
};
