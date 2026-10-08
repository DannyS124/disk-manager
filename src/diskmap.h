#pragma once

// Graphical view: one row per disk, partitions sized by capacity.

#include "udisks.h"

#include <QWidget>

class DiskMap : public QWidget
{
    Q_OBJECT
public:
    struct Selection {
        enum class Kind { None, Disk, Volume, Free };
        Kind kind = Kind::None;
        int disk = -1;
        int volume = -1; // Kind::Volume
        quint64 offset = 0; // Kind::Free
        quint64 size = 0;
    };

    explicit DiskMap(QWidget *parent = nullptr);

    void setDisks(const QVector<Disk> &disks); // keeps the selection
    const QVector<Disk> &disks() const { return m_disks; }
    Selection selection() const { return m_sel; }
    void selectVolume(const QString &objectPath); // doesn't emit selectionChanged

    static QColor partitionColor(const QPalette &palette);
    static QColor freeColor();

    QSize sizeHint() const override;

signals:
    void selectionChanged();
    void contextMenuRequested(const QPoint &globalPos);
    void activated();

protected:
    bool event(QEvent *event) override;
    void paintEvent(QPaintEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseDoubleClickEvent(QMouseEvent *event) override;
    void contextMenuEvent(QContextMenuEvent *event) override;

private:
    struct Segment {
        Span span;
        QRect rect;
    };
    struct Row {
        QRect header;
        QVector<Segment> segments;
    };

    void relayout();
    bool hitTest(const QPoint &pos, int *row, int *segment) const; // segment -1: header
    Selection selectionAt(int row, int segment) const;
    bool isSelected(int row, int segment) const;
    QString toolTipAt(int row, int segment) const;
    void paintHeader(QPainter &p, int row) const;
    void paintSegment(QPainter &p, int row, int segment) const;

    QVector<Disk> m_disks;
    QVector<Row> m_rows;
    Selection m_sel;
    int m_rowHeight = 86;
};
