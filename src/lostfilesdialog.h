// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "jobui.h"
#include "lostscan.h"

#include <QAbstractListModel>
#include <QDialog>
#include <QHash>
#include <QPixmap>
#include <QSet>

#include <memory>

class QCheckBox;
class QLabel;
class QLineEdit;
class QListView;
class QListWidget;
class QPlainTextEdit;
class QProgressBar;
class QPushButton;
class QStackedWidget;
class QThread;
class QTreeWidget;
class ThumbnailLoader;
class UDisks;

// What Find Lost Files found, filtered by kind and by what's typed, with a tick box each and a
// thumbnail for pictures (asked for only when a picture is on screen).
class LostFilesModel : public QAbstractListModel
{
    Q_OBJECT
public:
    explicit LostFilesModel(QObject *parent = nullptr);

    void append(const QVector<lost::Found> &files);
    void clear();
    void setFilter(int category, bool hideDamaged, const QString &text); // category -1: all
    const lost::Found &file(int index) const { return m_files[index]; }
    int fileCount() const { return int(m_files.size()); }
    int fileIndex(int row) const { return m_shown.value(row, -1); }
    int count(int category) const; // how many of a kind were found
    void setThumbnail(int index, const QPixmap &thumbnail);
    QVector<lost::Found> ticked() const;
    int tickedCount() const { return int(m_ticked.size()); }
    quint64 tickedSize() const;
    void tickShown(bool tick);

    int rowCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    bool setData(const QModelIndex &index, const QVariant &value, int role) override;
    Qt::ItemFlags flags(const QModelIndex &index) const override;

Q_SIGNALS:
    void wantThumbnail(int index) const;
    void tickedChanged();

private:
    bool passes(const lost::Found &f) const;

    QVector<lost::Found> m_files;
    QVector<int> m_shown; // file indexes in the list, in order
    QSet<int> m_ticked;
    QHash<int, QPixmap> m_thumbnails;
    mutable QSet<int> m_asked;
    int m_category = -1;
    bool m_hideDamaged = false;
    QString m_text;
};

// Find Lost Files: pick a drive, a partition or a disk image, look through it for files that
// were deleted or that its file system lost, look at them, and save the ones you want onto
// another drive. Only ever reads the drive it looks through.
class LostFilesDialog : public QDialog
{
    Q_OBJECT
public:
    // preferredPath: a drive or partition (UDisks object path) to have picked already.
    LostFilesDialog(UDisks *udisks, const QString &preferredPath, QWidget *parent = nullptr);
    ~LostFilesDialog() override;

    // Looks through a disk image file straight away.
    void scanImage(const QString &path);
    // Looks through a source that's already open (and the tests' way in). diskDevice is the
    // drive it's on (/dev/sdb), so saving there can be refused; empty for an image.
    void scanSource(std::shared_ptr<lost::Source> source, const QString &title, const QString &diskDevice);
    // Saves the ticked files into `folder`, after the checks (the tests call this directly).
    void saveTo(const QString &folder);

protected:
    void reject() override;

private:
    void fillDrives(const QString &preferredPath);
    void driveChosen();
    void start();
    void chooseImage();
    void filterChanged();
    void refreshCategories();
    int chosenCategory() const;
    void showPreview();
    void updateTicked();
    void saveTicked();
    void stopWorkers();

    UDisks *m_udisks;
    QStackedWidget *m_pages;
    // Picking
    QTreeWidget *m_drives;
    QLabel *m_driveNote;
    QPushButton *m_look;
    // Results
    QLabel *m_status;
    QProgressBar *m_bar;
    QLabel *m_phase;
    PhaseProgress m_meter;
    QPushButton *m_stop;
    QListWidget *m_categories;
    QLineEdit *m_search;
    QCheckBox *m_hideDamaged;
    QPushButton *m_grid;
    QListView *m_view;
    QLabel *m_previewImage;
    QLabel *m_details;
    QPlainTextEdit *m_hex;
    QLabel *m_tickedInfo;
    QPushButton *m_save;
    // Saving
    QLabel *m_saveStatus;
    QProgressBar *m_saveBar;
    QLabel *m_savePhase;
    PhaseProgress m_saveMeter;
    QPushButton *m_openFolder;
    QPushButton *m_backToFiles;
    QString m_savedTo;

    LostFilesModel *m_model;
    std::shared_ptr<lost::Source> m_source;
    QString m_diskDevice;
    QString m_title;
    lost::DeepScan *m_scan = nullptr;
    QThread *m_scanThread = nullptr;
    lost::Saver *m_saver = nullptr;
    QThread *m_saveThread = nullptr;
    ThumbnailLoader *m_thumbs = nullptr;
    QThread *m_thumbThread = nullptr;
    int m_previewIndex = -1;
    bool m_scanning = false;
    bool m_saving = false;
};
