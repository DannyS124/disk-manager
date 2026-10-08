// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QMainWindow>

class DiskMap;
class HelpWindow;
class UpdateChecker;
class QAction;
class QTreeWidget;
class QTreeWidgetItem;
class UDisks;
struct Disk;
struct Volume;

class MainWindow : public QMainWindow
{
    Q_OBJECT
public:
    explicit MainWindow(UDisks *udisks, QWidget *parent = nullptr);

    bool selectDevice(const QString &device); // e.g. /dev/sdb1

private:
    void createActions();
    void rebuild();
    void fillTable();
    void syncTableToMap();
    void onTableSelection();
    void updateActions();
    void showContextMenu(const QPoint &globalPos);
    void activate();
    void openInFileManager();
    void copyDevicePath();
    void showProperties();
    void newPartition();
    void formatVolume();
    void deletePartition();
    void resizeVolume();
    void changeLabel();
    void newPartitionTable();
    void checkForUpdates();
    int selectedDiskNumber() const;

    const Disk *selectedDisk() const;
    const Volume *selectedVolume() const;

    UDisks *m_udisks;
    QTreeWidget *m_table;
    DiskMap *m_map;
    bool m_syncing = false;

    QAction *m_refresh = nullptr;
    QAction *m_open = nullptr;
    QAction *m_mount = nullptr;
    QAction *m_unmount = nullptr;
    QAction *m_copy = nullptr;
    QAction *m_properties = nullptr;
    QAction *m_newPartition = nullptr;
    QAction *m_format = nullptr;
    QAction *m_delete = nullptr;
    QAction *m_resize = nullptr;
    QAction *m_rename = nullptr;
    QAction *m_newTable = nullptr;
    HelpWindow *m_help = nullptr;
    UpdateChecker *m_updates = nullptr;
};
