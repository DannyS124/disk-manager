// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#include "blockmapwidget.h"

#include "format.h"

#include <QHelpEvent>
#include <QPainter>
#include <QToolTip>

#include <cmath>

BlockMapWidget::BlockMapWidget(QWidget *parent)
    : QWidget(parent)
{
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
}

void BlockMapWidget::reset(quint64 size, bool rotational)
{
    m_data = BlockMapData(size, rotational);
    update();
}

void BlockMapWidget::addSamples(const QVector<ReadSample> &samples)
{
    m_data.add(samples);
    update();
}

QColor BlockMapWidget::colour(BlockMapData::State state, const QPalette &palette)
{
    switch (state) {
    case BlockMapData::State::Good:
        return QColor(0x3f, 0xae, 0x5a);
    case BlockMapData::State::Slow:
        return QColor(0xe8, 0x9a, 0x2f);
    case BlockMapData::State::Pending:
        return QColor(0xd8, 0xc8, 0x3a);
    case BlockMapData::State::Bad:
        return QColor(0xd9, 0x3a, 0x34);
    case BlockMapData::State::Unread:
        break;
    }
    return palette.color(QPalette::Mid);
}

int BlockMapWidget::legendHeight() const
{
    return fontMetrics().height() + 10;
}

// The biggest squares that still fit every cell in the space above the legend.
BlockMapWidget::Grid BlockMapWidget::grid() const
{
    Grid g;
    const QRect space = rect().adjusted(0, 0, 0, -legendHeight());
    const int n = std::max(m_data.cells(), 1);
    g.cell = std::max(2, int(std::sqrt(double(space.width()) * space.height() / n)));
    while (g.cell > 2 && ((n + space.width() / g.cell - 1) / std::max(space.width() / g.cell, 1)) * g.cell > space.height())
        --g.cell;
    g.columns = std::max(space.width() / g.cell, 1);
    const int rows = (n + g.columns - 1) / g.columns;
    g.area = QRect(space.left(), space.top(), g.columns * g.cell, rows * g.cell);
    return g;
}

void BlockMapWidget::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    const Grid g = grid();
    const int gap = g.cell >= 5 ? 1 : 0;
    QColor colours[5];
    for (int s = 0; s < 5; ++s)
        colours[s] = colour(BlockMapData::State(s), palette());
    for (int c = 0; c < m_data.cells(); ++c) {
        const int x = g.area.left() + (c % g.columns) * g.cell;
        const int y = g.area.top() + (c / g.columns) * g.cell;
        p.fillRect(x, y, g.cell - gap, g.cell - gap, colours[int(m_data.state(c))]);
    }

    // Legend: a swatch and a count for each kind of cell.
    struct Entry {
        BlockMapData::State state;
        QString text;
    };
    QVector<Entry> legend = {
        {BlockMapData::State::Good, tr("Reads fine")},
        {BlockMapData::State::Slow, tr("Slow (%n area(s))", nullptr, m_data.slowAreas())},
        {BlockMapData::State::Bad, tr("Can't be read")},
        {BlockMapData::State::Unread, tr("Not read yet")},
    };
    if (m_showPending)
        legend.insert(2, {BlockMapData::State::Pending, tr("To retry")});
    int x = 0;
    const int y = height() - legendHeight() + 5;
    const int box = fontMetrics().ascent();
    for (const Entry &e : legend) {
        p.fillRect(x, y + (fontMetrics().height() - box) / 2, box, box, colour(e.state, palette()));
        x += box + 5;
        p.setPen(palette().color(QPalette::WindowText));
        p.drawText(x, y + fontMetrics().ascent(), e.text);
        x += fontMetrics().horizontalAdvance(e.text) + 16;
    }
}

int BlockMapWidget::cellAt(const QPoint &pos) const
{
    const Grid g = grid();
    if (!g.area.contains(pos))
        return -1;
    const int c = (pos.y() - g.area.top()) / g.cell * g.columns + (pos.x() - g.area.left()) / g.cell;
    return c < m_data.cells() ? c : -1;
}

bool BlockMapWidget::event(QEvent *event)
{
    if (event->type() != QEvent::ToolTip)
        return QWidget::event(event);
    auto *help = static_cast<QHelpEvent *>(event);
    const int c = cellAt(help->pos());
    if (c < 0) {
        QToolTip::hideText();
        event->ignore();
        return true;
    }
    const quint64 start = m_data.cellStart(c), end = m_data.cellEnd(c);
    QStringList lines = {tr("%1 to %2").arg(formatSize(start), formatSize(end))};
    for (const Area &a : m_areas) {
        if (a.start < end && a.end > start)
            lines << a.name;
    }
    switch (m_data.state(c)) {
    case BlockMapData::State::Unread:
        lines << tr("Not read yet");
        break;
    case BlockMapData::State::Bad:
        lines << tr("Has sectors that can't be read");
        break;
    case BlockMapData::State::Pending:
        lines << tr("Parts still to retry");
        break;
    default:
        lines << tr("Slowest read: %1 ms").arg(m_data.slowestMs(c));
    }
    QToolTip::showText(help->globalPos(), lines.join(QLatin1Char('\n')), this);
    return true;
}
