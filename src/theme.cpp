// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#include "theme.h"

#include <QEvent>
#include <QGuiApplication>
#include <QPalette>

#include <cmath>

Theme &Theme::instance()
{
    static Theme theme;
    return theme;
}

Theme::Theme()
{
    // The default colors follow the desktop's (the highlight color, for one), so repaint
    // when the desktop's colors change.
    if (QCoreApplication *app = QCoreApplication::instance())
        app->installEventFilter(this);
}

bool Theme::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == QCoreApplication::instance() && event->type() == QEvent::ApplicationPaletteChange)
        emit changed();
    return false;
}

QColor Theme::color(Role role) const
{
    const auto set = m_colors.constFind(int(role));
    if (set != m_colors.constEnd())
        return *set;
    const QPalette palette = QGuiApplication::palette();
    switch (role) {
    case Role::Partition:
    case Role::Selection:
        return palette.color(QPalette::Highlight);
    case Role::Free:
        return QColor(0x80, 0x80, 0x80);
    case Role::Good:
        return QColor(0x2e, 0xcc, 0x71);
    case Role::Warning:
        return QColor(0xe8, 0x91, 0x2d);
    case Role::Danger:
        return QColor(0xe0, 0x50, 0x50);
    case Role::Muted:
        return QColor(0x88, 0x88, 0x88);
    case Role::MapGood:
        return QColor(0x3f, 0xae, 0x5a);
    case Role::MapSlow:
        return QColor(0xe8, 0x9a, 0x2f);
    case Role::MapRetry:
        return QColor(0xd8, 0xc8, 0x3a);
    case Role::MapBad:
        return QColor(0xd9, 0x3a, 0x34);
    case Role::MapUnread:
        return palette.color(QPalette::Mid);
    case Role::UsageSmallFiles:
        return QColor(0xb8, 0xbc, 0xc4);
    case Role::UsageFile:
        return QColor(0x9a, 0xb0, 0xc8);
    case Role::Encrypted:
        return QColor(0x8e, 0x6f, 0xd8);
    }
    return palette.color(QPalette::Text);
}

QColor Theme::partitionColor(const QString &fsType) const
{
    return m_filesystems.value(fsType, color(Role::Partition));
}

QColor Theme::usageColor(int index) const
{
    if (!m_usage.isEmpty())
        return m_usage[index % m_usage.size()];
    // Well-spread hues, so neighbours rarely look alike.
    return QColor::fromHsvF(std::fmod(0.58 + index * 0.618034, 1.0), 0.45, 0.88);
}

QColor Theme::textOn(const QColor &fill)
{
    // Relative luminance, as in the WCAG contrast formula.
    auto channel = [](double c) { return c <= 0.03928 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4); };
    const double l = 0.2126 * channel(fill.redF()) + 0.7152 * channel(fill.greenF()) + 0.0722 * channel(fill.blueF());
    return l > 0.18 ? QColor(0x20, 0x20, 0x20) : QColor(0xf2, 0xf2, 0xf2);
}
