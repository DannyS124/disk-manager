// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "addons.h"

#include <QMainWindow>

class DiskMap;
class HelpWindow;
class UpdateChecker;
class QAction;
class QMenu;
class QProgressBar;
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

    bool selectDevice(const QString &device); // e.g. /dev/sdb1, or /dev/sdb for the whole drive
    void buildContextMenu(QMenu *menu); // the right-click menu for the current selection

private:
    void createActions();
    void rebuild();
    void fillTable();
    void syncTableToMap();
    void onTableSelection();
    void updateActions();
    void updateProgress();
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
    bool addAddonActions(QMenu *menu); // false if none apply
    void runAddon(const Addon &addon, const AddonAction &action);
    int selectedDiskNumber() const;

    const Disk *selectedDisk() const;
    const Volume *selectedVolume() const;
    const Volume *volumeByPath(const QString &objectPath) const;
    void gone();

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
    QAction *m_safelyRemove = nullptr;
    QAction *m_check = nullptr;
    QAction *m_startup = nullptr;
    QAction *m_unlock = nullptr;
    QAction *m_lock = nullptr;
    QAction *m_changePass = nullptr;
    QAction *m_openImage = nullptr;
    QAction *m_detachImage = nullptr;
    QAction *m_writeImage = nullptr;
    QAction *m_wipe = nullptr;
    QAction *m_health = nullptr;
    QAction *m_benchmark = nullptr;
    QAction *m_badSectors = nullptr;
    QAction *m_clone = nullptr;
    QAction *m_backup = nullptr;
    QAction *m_restore = nullptr;
    QAction *m_rescue = nullptr;
    QAction *m_usage = nullptr;
    QAction *m_optimize = nullptr;
    QAction *m_cleanup = nullptr;
    QAction *m_snapshots = nullptr;
    QAction *m_secureErase = nullptr;
    QProgressBar *m_progress = nullptr;
    Addons m_addons;
    HelpWindow *m_help = nullptr;
    UpdateChecker *m_updates = nullptr;
};
