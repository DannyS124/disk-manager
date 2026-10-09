// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// DiskForge's own colors: the disk map, the bad sector map, the usage map, health and
// warnings. Everything that draws asks here instead of picking colors itself, so a theme
// can change them in one place.

#include <QColor>
#include <QHash>
#include <QObject>
#include <QString>
#include <QVector>

class Theme : public QObject
{
    Q_OBJECT
public:
    enum class Role {
        Partition, // the strip on top of a partition
        Free,      // unallocated space
        Selection,
        Good,      // healthy, worked
        Warning,
        Danger,    // failing, will erase
        Muted,     // no health data and the like
        MapGood,   // bad sector and rescue maps
        MapSlow,
        MapRetry,
        MapBad,
        MapUnread,
        UsageSmallFiles, // usage map: the "n smaller files" box
        UsageFile,       // usage map: a single file
        Encrypted,       // stripe on encrypted partitions
    };

    static Theme &instance();

    QColor color(Role role) const;
    // For rich text, e.g. "#e05050". Always QColor::name(), never text from a theme file.
    QString html(Role role) const { return color(role).name(); }
    QColor partitionColor(const QString &fsType) const;
    QColor usageColor(int index) const; // folders on the usage map
    // Dark or light text, whichever reads better on `fill`.
    static QColor textOn(const QColor &fill);

signals:
    void changed();

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    Theme();

    QHash<int, QColor> m_colors; // set by a theme; missing ones use the defaults
    QHash<QString, QColor> m_filesystems;
    QVector<QColor> m_usage;
};
