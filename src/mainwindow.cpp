#include "mainwindow.h"

#include "about.h"
#include "dialogs.h"
#include "diskmap.h"
#include "format.h"
#include "updates.h"
#include "udisks.h"

#include <QAction>
#include <QApplication>
#include <QClipboard>
#include <QDesktopServices>
#include <QDialog>
#include <QDialogButtonBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QLabel>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QPushButton>
#include <QScrollArea>
#include <QSplitter>
#include <QStatusBar>
#include <QToolBar>
#include <QTreeWidget>
#include <QUrl>
#include <QVBoxLayout>

namespace {

enum Column { ColVolume, ColDevice, ColFileSystem, ColStatus, ColCapacity, ColFree, ColPercentFree, ColumnCount };

QIcon themeIcon(const char *name, const char *fallback)
{
    return QIcon::fromTheme(QLatin1String(name), QIcon::fromTheme(QLatin1String(fallback)));
}

QWidget *legendItem(const QPixmap &swatch, const QString &text)
{
    auto *item = new QWidget;
    auto *layout = new QHBoxLayout(item);
    layout->setContentsMargins(6, 0, 6, 0);
    layout->setSpacing(4);
    auto *icon = new QLabel;
    icon->setPixmap(swatch);
    layout->addWidget(icon);
    layout->addWidget(new QLabel(text));
    return item;
}

QPixmap colorSwatch(const QColor &color)
{
    QPixmap pixmap(12, 12);
    pixmap.fill(color);
    return pixmap;
}

} // namespace

MainWindow::MainWindow(UDisks *udisks, QWidget *parent)
    : QMainWindow(parent)
    , m_udisks(udisks)
    , m_table(new QTreeWidget)
    , m_map(new DiskMap)
{
    setWindowTitle(tr("DiskForge"));

    m_table->setColumnCount(ColumnCount);
    m_table->setHeaderLabels({tr("Volume"), tr("Device"), tr("File System"), tr("Status"),
                              tr("Capacity"), tr("Free Space"), tr("% Free")});
    m_table->setRootIsDecorated(false);
    m_table->setUniformRowHeights(true);
    m_table->setAlternatingRowColors(true);
    m_table->setSelectionMode(QAbstractItemView::SingleSelection);
    m_table->setContextMenuPolicy(Qt::CustomContextMenu);
    m_table->header()->setStretchLastSection(false);
    m_table->header()->setSectionResizeMode(ColStatus, QHeaderView::Stretch);

    auto *scroll = new QScrollArea;
    scroll->setWidget(m_map);
    scroll->setWidgetResizable(true);

    auto *splitter = new QSplitter(Qt::Vertical);
    splitter->addWidget(m_table);
    splitter->addWidget(scroll);
    splitter->setChildrenCollapsible(false);
    splitter->setStretchFactor(0, 2);
    splitter->setStretchFactor(1, 3);
    splitter->setSizes({300, 500});
    setCentralWidget(splitter);

    createActions();

    statusBar()->addPermanentWidget(legendItem(colorSwatch(DiskMap::freeColor()), tr("Unallocated")));
    statusBar()->addPermanentWidget(legendItem(colorSwatch(DiskMap::partitionColor(palette())), tr("Partition")));
    statusBar()->addPermanentWidget(legendItem(QIcon::fromTheme(QStringLiteral("object-locked")).pixmap(12, 12),
                                               tr("System disk (read-only)")));

    connect(m_udisks, &UDisks::changed, this, &MainWindow::rebuild);
    connect(m_udisks, &UDisks::operationFinished, this, [this](bool ok, const QString &message) {
        updateActions();
        if (ok)
            statusBar()->showMessage(message, 8000);
        else {
            statusBar()->clearMessage();
            QMessageBox::warning(this, windowTitle(), message);
        }
    });
    connect(m_table, &QTreeWidget::itemSelectionChanged, this, &MainWindow::onTableSelection);
    connect(m_table, &QTreeWidget::itemDoubleClicked, this, &MainWindow::activate);
    connect(m_table, &QTreeWidget::customContextMenuRequested, this, [this](const QPoint &pos) {
        if (m_table->itemAt(pos))
            showContextMenu(m_table->viewport()->mapToGlobal(pos));
    });
    connect(m_map, &DiskMap::selectionChanged, this, [this] {
        syncTableToMap();
        updateActions();
    });
    connect(m_map, &DiskMap::contextMenuRequested, this, &MainWindow::showContextMenu);
    connect(m_map, &DiskMap::activated, this, &MainWindow::activate);

    rebuild();
}

void MainWindow::createActions()
{
    m_refresh = new QAction(themeIcon("view-refresh", "reload"), tr("&Refresh"), this);
    m_refresh->setShortcut(QKeySequence::Refresh);
    connect(m_refresh, &QAction::triggered, m_udisks, &UDisks::refresh);

    m_open = new QAction(themeIcon("document-open-folder", "folder-open"), tr("&Open in File Manager"), this);
    connect(m_open, &QAction::triggered, this, &MainWindow::openInFileManager);

    m_mount = new QAction(themeIcon("media-mount", "drive-harddisk"), tr("&Mount"), this);
    connect(m_mount, &QAction::triggered, this, [this] {
        if (const Volume *v = selectedVolume()) {
            statusBar()->showMessage(tr("Mounting %1…").arg(shortDevice(v->device)));
            m_udisks->mount(*v);
        }
    });

    m_unmount = new QAction(themeIcon("media-eject", "media-eject"), tr("&Unmount"), this);
    connect(m_unmount, &QAction::triggered, this, [this] {
        if (const Volume *v = selectedVolume()) {
            statusBar()->showMessage(tr("Unmounting %1…").arg(shortDevice(v->device)));
            m_udisks->unmount(*v);
        }
    });

    m_copy = new QAction(themeIcon("edit-copy", "edit-copy"), tr("&Copy Device Path"), this);
    connect(m_copy, &QAction::triggered, this, &MainWindow::copyDevicePath);

    m_properties = new QAction(themeIcon("document-properties", "configure"), tr("&Properties"), this);
    m_properties->setShortcut(QKeySequence(Qt::ALT | Qt::Key_Return));
    connect(m_properties, &QAction::triggered, this, &MainWindow::showProperties);

    m_newPartition = new QAction(themeIcon("list-add", "document-new"), tr("&New Partition…"), this);
    connect(m_newPartition, &QAction::triggered, this, &MainWindow::newPartition);

    m_format = new QAction(themeIcon("tools-wizard", "edit-clear"), tr("&Format…"), this);
    connect(m_format, &QAction::triggered, this, &MainWindow::formatVolume);

    m_rename = new QAction(themeIcon("edit-rename", "document-edit"), tr("Change &Label…"), this);
    m_rename->setShortcut(QKeySequence(Qt::Key_F2));
    connect(m_rename, &QAction::triggered, this, &MainWindow::changeLabel);

    m_resize = new QAction(themeIcon("transform-scale", "zoom-fit-best"), tr("&Resize…"), this);
    connect(m_resize, &QAction::triggered, this, &MainWindow::resizeVolume);

    m_delete = new QAction(themeIcon("edit-delete", "list-remove"), tr("&Delete Partition…"), this);
    m_delete->setShortcut(QKeySequence::Delete);
    connect(m_delete, &QAction::triggered, this, &MainWindow::deletePartition);

    m_newTable = new QAction(themeIcon("document-new", "list-add"), tr("New Partition &Table…"), this);
    connect(m_newTable, &QAction::triggered, this, &MainWindow::newPartitionTable);

    auto *quit = new QAction(themeIcon("application-exit", "window-close"), tr("&Quit"), this);
    quit->setShortcut(QKeySequence::Quit);
    connect(quit, &QAction::triggered, qApp, &QApplication::quit);

    QMenu *file = menuBar()->addMenu(tr("&File"));
    file->addAction(m_refresh);
    file->addSeparator();
    file->addAction(quit);

    QMenu *action = menuBar()->addMenu(tr("&Action"));
    action->addActions({m_open, m_mount, m_unmount});
    action->addSeparator();
    action->addActions({m_newPartition, m_format, m_resize, m_rename, m_delete});
    action->addSeparator();
    action->addAction(m_newTable);
    action->addSeparator();
    action->addActions({m_copy, m_properties});

    QMenu *help = menuBar()->addMenu(tr("&Help"));
    QAction *handbook = help->addAction(themeIcon("help-contents", "help-browser"), tr("DiskForge &Handbook"), this, [this] {
        if (!m_help)
            m_help = new HelpWindow(this);
        m_help->show();
        m_help->raise();
        m_help->activateWindow();
    });
    handbook->setShortcut(QKeySequence::HelpContents);
    help->addSeparator();
    help->addAction(themeIcon("update-none", "system-software-update"), tr("Check for &Updates…"), this, &MainWindow::checkForUpdates);
    help->addAction(themeIcon("tools-report-bug", "dialog-warning"), tr("Report a &Bug…"), this, [] {
        QDesktopServices::openUrl(QUrl(QStringLiteral(APP_HOMEPAGE "/issues/new/choose")));
    });
    help->addAction(themeIcon("internet-services", "applications-internet"), tr("Project &Website"), this, [] {
        QDesktopServices::openUrl(QUrl(QStringLiteral(APP_HOMEPAGE)));
    });
    help->addSeparator();
    help->addAction(QApplication::windowIcon(), tr("&About DiskForge"), this, [this] {
        AboutDialog(m_udisks->daemonVersion(), this).exec();
    });
    help->addAction(themeIcon("qtcreator", "help-about"), tr("About &Qt"), qApp, &QApplication::aboutQt);

    QToolBar *toolbar = addToolBar(tr("Main"));
    toolbar->setObjectName(QStringLiteral("mainToolbar"));
    toolbar->setMovable(false);
    toolbar->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    toolbar->addActions({m_refresh});
    toolbar->addSeparator();
    toolbar->addActions({m_open, m_mount, m_unmount, m_properties});
}

void MainWindow::rebuild()
{
    m_map->setDisks(m_udisks->disks());
    fillTable();
    syncTableToMap();
    updateActions();
    if (!m_udisks->isAvailable())
        statusBar()->showMessage(tr("UDisks2 is not available: %1").arg(m_udisks->lastError()));
}

bool MainWindow::selectDevice(const QString &device)
{
    for (const Disk &d : m_map->disks()) {
        for (const Volume &v : d.volumes) {
            if (v.device == device) {
                m_map->selectVolume(v.objectPath);
                syncTableToMap();
                updateActions();
                return true;
            }
        }
    }
    return false;
}

void MainWindow::fillTable()
{
    m_syncing = true;
    m_table->clear();
    for (const Disk &d : m_map->disks()) {
        for (const Volume &v : d.volumes) {
            if (v.isContainer)
                continue;
            auto *item = new QTreeWidgetItem(m_table);
            item->setData(ColVolume, Qt::UserRole, v.objectPath);
            item->setText(ColVolume, !v.label.isEmpty() ? v.label : !v.partName.isEmpty() ? v.partName : shortDevice(v.device));
            item->setIcon(ColVolume, d.isSystem ? themeIcon("drive-harddisk-root", "drive-harddisk")
                                     : (d.removable || d.bus == QLatin1String("usb")) ? themeIcon("drive-removable-media-usb", "drive-harddisk")
                                                                                     : themeIcon("drive-harddisk", "drive-harddisk"));
            item->setText(ColDevice, v.device);
            item->setText(ColFileSystem, v.fsType);
            item->setText(ColStatus, volumeStatus(v));
            item->setText(ColCapacity, formatSize(v.size));
            if (v.fsTotal) {
                item->setText(ColFree, formatSize(v.fsFree));
                item->setText(ColPercentFree, QStringLiteral("%1 %").arg(qRound(100.0 * double(v.fsFree) / double(v.fsTotal))));
            }
            for (int col : {ColCapacity, ColFree, ColPercentFree})
                item->setTextAlignment(col, Qt::AlignRight | Qt::AlignVCenter);
        }
    }
    for (int col = 0; col < ColumnCount; ++col) {
        if (col != ColStatus)
            m_table->resizeColumnToContents(col);
    }
    m_syncing = false;
}

void MainWindow::syncTableToMap()
{
    m_syncing = true;
    const Volume *v = selectedVolume();
    m_table->clearSelection();
    for (int i = 0; v && i < m_table->topLevelItemCount(); ++i) {
        QTreeWidgetItem *item = m_table->topLevelItem(i);
        if (item->data(ColVolume, Qt::UserRole).toString() == v->objectPath) {
            m_table->setCurrentItem(item);
            m_table->scrollToItem(item);
        }
    }
    m_syncing = false;
}

void MainWindow::onTableSelection()
{
    if (m_syncing)
        return;
    const QList<QTreeWidgetItem *> items = m_table->selectedItems();
    if (!items.isEmpty())
        m_map->selectVolume(items.first()->data(ColVolume, Qt::UserRole).toString());
    updateActions();
}

const Disk *MainWindow::selectedDisk() const
{
    const DiskMap::Selection sel = m_map->selection();
    return sel.kind == DiskMap::Selection::Kind::None ? nullptr : &m_map->disks()[sel.disk];
}

const Volume *MainWindow::selectedVolume() const
{
    const DiskMap::Selection sel = m_map->selection();
    return sel.kind == DiskMap::Selection::Kind::Volume ? &m_map->disks()[sel.disk].volumes[sel.volume] : nullptr;
}

void MainWindow::updateActions()
{
    const Disk *d = selectedDisk();
    const Volume *v = selectedVolume();
    const auto kind = m_map->selection().kind;
    const bool mounted = v && !(v->mountPoints.isEmpty() && v->cleartextMountPoints.isEmpty());
    const bool busy = m_udisks->isBusy();
    const bool changeable = d && !d->isSystem && !busy;

    m_open->setEnabled(mounted);
    m_mount->setEnabled(v && v->hasFilesystem && v->mountPoints.isEmpty() && !busy);
    m_unmount->setEnabled(v && v->hasFilesystem && !v->mountPoints.isEmpty() && !v->isSystem && !busy);
    m_newPartition->setEnabled(changeable && kind == DiskMap::Selection::Kind::Free && !d->tableType.isEmpty());
    m_format->setEnabled(changeable && v && !v->isContainer);
    m_rename->setEnabled(changeable && v && v->hasFilesystem);
    m_delete->setEnabled(changeable && v && v->number > 0);
    const ResizeLimits limits = v ? m_udisks->resizeLimits(*v) : ResizeLimits{};
    m_resize->setEnabled(changeable && limits.possible);
    m_newTable->setEnabled(changeable);
    m_copy->setEnabled(d && kind != DiskMap::Selection::Kind::Free);
    m_properties->setEnabled(d != nullptr);

    const QString why = d && d->isSystem ? tr("Disk holds the running system (%1)").arg(d->systemReason)
                      : busy             ? tr("Wait for the current operation to finish")
                                         : QString();
    for (QAction *a : {m_unmount, m_newPartition, m_format, m_rename, m_delete, m_newTable})
        a->setToolTip(why.isEmpty() || a->isEnabled() ? a->text().remove(QLatin1Char('&')) : why);
    m_resize->setToolTip(m_resize->isEnabled() ? tr("Resize") : !why.isEmpty() ? why : limits.reason);
}

void MainWindow::showContextMenu(const QPoint &globalPos)
{
    QMenu menu(this);
    switch (m_map->selection().kind) {
    case DiskMap::Selection::Kind::Volume:
        menu.addActions({m_open, m_mount, m_unmount});
        menu.addSeparator();
        menu.addActions({m_format, m_resize, m_rename, m_delete});
        menu.addSeparator();
        menu.addActions({m_copy, m_properties});
        break;
    case DiskMap::Selection::Kind::Free:
        menu.addAction(m_newPartition);
        if (selectedDisk() && selectedDisk()->tableType.isEmpty())
            menu.addAction(m_newTable);
        menu.addSeparator();
        menu.addAction(m_properties);
        break;
    case DiskMap::Selection::Kind::Disk:
        menu.addAction(m_newTable);
        menu.addSeparator();
        menu.addActions({m_copy, m_properties});
        break;
    case DiskMap::Selection::Kind::None:
        return;
    }
    menu.exec(globalPos);
}

void MainWindow::activate()
{
    if (m_open->isEnabled())
        openInFileManager();
    else if (m_properties->isEnabled())
        showProperties();
}

void MainWindow::openInFileManager()
{
    if (const Volume *v = selectedVolume()) {
        const QString mp = v->mountPoints.value(0, v->cleartextMountPoints.value(0));
        if (!mp.isEmpty())
            QDesktopServices::openUrl(QUrl::fromLocalFile(mp));
    }
}

void MainWindow::copyDevicePath()
{
    const Volume *v = selectedVolume();
    const Disk *d = selectedDisk();
    if (v || d)
        QApplication::clipboard()->setText(v ? v->device : d->device);
}

void MainWindow::showProperties()
{
    const Disk *d = selectedDisk();
    if (!d)
        return;
    const DiskMap::Selection sel = m_map->selection();
    const Volume *v = selectedVolume();

    QList<QPair<QString, QString>> rows;
    auto add = [&rows](const QString &name, const QString &value) {
        if (!value.isEmpty())
            rows.append({name, value});
    };
    auto bytes = [](quint64 n) { return QStringLiteral("%1 (%2 bytes)").arg(formatSize(n)).arg(n); };

    QString title;
    if (v) {
        title = volumeTitle(*v);
        add(tr("Device"), v->device);
        add(tr("Label"), v->label);
        add(tr("Partition name"), v->partName);
        add(tr("File system"), v->fsType);
        add(tr("UUID"), v->uuid);
        add(tr("Status"), volumeStatus(*v));
        add(tr("Capacity"), bytes(v->size));
        if (v->fsTotal)
            add(tr("Free space"), tr("%1 of %2").arg(formatSize(v->fsFree), formatSize(v->fsTotal)));
        if (v->number)
            add(tr("Partition number"), QString::number(v->number));
        add(tr("Partition type"), partitionTypeName(v->partType));
        add(tr("Starts at"), bytes(v->offset));
        add(tr("Disk"), d->device);
        add(tr("UDisks2 object"), v->objectPath);
    } else if (sel.kind == DiskMap::Selection::Kind::Free) {
        title = tr("Unallocated space on %1").arg(shortDevice(d->device));
        add(tr("Disk"), d->device);
        add(tr("Size"), bytes(sel.size));
        add(tr("Starts at"), bytes(sel.offset));
    } else {
        title = d->model.isEmpty() ? d->device : d->model;
        add(tr("Device"), d->device);
        add(tr("Model"), d->model);
        add(tr("Serial"), d->serial);
        add(tr("Type"), diskKind(*d));
        add(tr("Partition table"), tableName(*d));
        add(tr("Capacity"), bytes(d->size));
        if (d->rotationRate > 0)
            add(tr("Rotation rate"), tr("%1 rpm").arg(d->rotationRate));
        add(tr("Image file"), d->backingFile);
        add(tr("Removable"), d->removable ? tr("Yes") : tr("No"));
        if (d->isSystem)
            add(tr("System disk"), tr("%1. This app won't modify it.").arg(d->systemReason));
        add(tr("UDisks2 object"), d->blockPath);
    }

    QDialog dialog(this);
    dialog.setWindowTitle(tr("%1 Properties").arg(title));
    auto *tree = new QTreeWidget;
    tree->setColumnCount(2);
    tree->setHeaderLabels({tr("Property"), tr("Value")});
    tree->setRootIsDecorated(false);
    tree->setSelectionMode(QAbstractItemView::ExtendedSelection);
    for (const auto &[name, value] : rows)
        new QTreeWidgetItem(tree, {name, value});
    tree->resizeColumnToContents(0);
    tree->resizeColumnToContents(1);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    auto *layout = new QVBoxLayout(&dialog);
    layout->addWidget(tree);
    layout->addWidget(buttons);
    dialog.resize(560, 380);
    dialog.exec();
}

int MainWindow::selectedDiskNumber() const
{
    return m_map->selection().disk;
}

void MainWindow::newPartition()
{
    const Disk *d = selectedDisk();
    const DiskMap::Selection sel = m_map->selection();
    if (!d || sel.kind != DiskMap::Selection::Kind::Free)
        return;
    NewPartitionDialog dialog(*d, Span{-1, sel.offset, sel.size}, m_udisks->filesystems(), this);
    if (dialog.exec() != QDialog::Accepted)
        return;
    statusBar()->showMessage(tr("Creating a partition on %1…").arg(shortDevice(d->device)));
    m_udisks->createPartition(*d, sel.offset, dialog.sizeBytes(), dialog.fsType(), dialog.label());
    updateActions();
}

void MainWindow::formatVolume()
{
    const Disk *d = selectedDisk();
    const Volume *v = selectedVolume();
    if (!d || !v)
        return;
    FormatDialog dialog(*d, *v, m_udisks->filesystems(), this);
    if (dialog.exec() != QDialog::Accepted)
        return;

    QMessageBox confirm(QMessageBox::Warning, tr("Format %1").arg(shortDevice(v->device)),
                        tr("Formatting will erase ALL data on %1. This can't be undone.\n\nFormat it as %2?")
                            .arg(describeVolume(*v), dialog.fsType()),
                        QMessageBox::Cancel, this);
    QAbstractButton *format = confirm.addButton(tr("Format"), QMessageBox::DestructiveRole);
    confirm.setDefaultButton(QMessageBox::Cancel);
    confirm.exec();
    if (confirm.clickedButton() != format)
        return;

    statusBar()->showMessage(tr("Formatting %1…").arg(shortDevice(v->device)));
    m_udisks->format(*v, dialog.fsType(), dialog.label());
    updateActions();
}

void MainWindow::deletePartition()
{
    const Disk *d = selectedDisk();
    const Volume *v = selectedVolume();
    if (!d || !v || !m_delete->isEnabled())
        return;
    QString text = tr("Delete %1?\n\nEverything on it will be lost. This can't be undone.").arg(describeVolume(*v));
    if (!diskWarning(*d).isEmpty())
        text += QStringLiteral("\n\n") + diskWarning(*d);
    QMessageBox confirm(QMessageBox::Warning, tr("Delete Partition"), text, QMessageBox::Cancel, this);
    QAbstractButton *del = confirm.addButton(tr("Delete"), QMessageBox::DestructiveRole);
    confirm.setDefaultButton(QMessageBox::Cancel);
    confirm.exec();
    if (confirm.clickedButton() != del)
        return;

    statusBar()->showMessage(tr("Deleting %1…").arg(shortDevice(v->device)));
    m_udisks->deletePartition(*v);
    updateActions();
}

void MainWindow::changeLabel()
{
    const Volume *v = selectedVolume();
    if (!v || !m_rename->isEnabled())
        return;
    int maxLength = 255;
    for (const FsType &fs : m_udisks->filesystems()) {
        if (fs.id == v->fsType)
            maxLength = fs.maxLabel;
    }
    bool ok = false;
    QInputDialog input(this);
    input.setWindowTitle(tr("Change Label"));
    input.setLabelText(tr("New label for %1 (up to %2 characters):").arg(shortDevice(v->device)).arg(maxLength));
    input.setTextValue(v->label);
    if (auto *edit = input.findChild<QLineEdit *>())
        edit->setMaxLength(maxLength);
    ok = input.exec() == QDialog::Accepted;
    const QString label = input.textValue().trimmed();
    if (!ok || label == v->label)
        return;
    statusBar()->showMessage(tr("Renaming %1…").arg(shortDevice(v->device)));
    m_udisks->setLabel(*v, label);
    updateActions();
}

void MainWindow::newPartitionTable()
{
    const Disk *d = selectedDisk();
    if (!d || !m_newTable->isEnabled())
        return;
    PartitionTableDialog dialog(*d, selectedDiskNumber(), this);
    if (dialog.exec() != QDialog::Accepted)
        return;
    statusBar()->showMessage(tr("Writing a new partition table to %1…").arg(shortDevice(d->device)));
    m_udisks->createPartitionTable(*d, dialog.tableType());
    updateActions();
}

void MainWindow::resizeVolume()
{
    const Disk *d = selectedDisk();
    const Volume *v = selectedVolume();
    if (!d || !v || !m_resize->isEnabled())
        return;
    ResizeDialog dialog(*d, *v, m_udisks->resizeLimits(*v), m_udisks->resizeNeedsRemount(*v, true),
                        m_udisks->resizeNeedsRemount(*v, false), this);
    if (dialog.exec() != QDialog::Accepted)
        return;
    statusBar()->showMessage(tr("Resizing %1…").arg(shortDevice(v->device)));
    m_udisks->resize(*v, dialog.newSize());
    updateActions();
}

void MainWindow::checkForUpdates()
{
    if (!m_updates) {
        m_updates = new UpdateChecker(this);
        connect(m_updates, &UpdateChecker::finished, this, [this](const QString &latest, const QString &url, const QString &error) {
            statusBar()->clearMessage();
            const QString current = QApplication::applicationVersion();
            if (!error.isEmpty()) {
                QMessageBox::warning(this, tr("Check for Updates"), tr("Couldn't check for updates:\n%1").arg(error));
                return;
            }
            if (!isNewerVersion(latest, current)) {
                QMessageBox::information(this, tr("Check for Updates"), tr("You have the latest version (%1).").arg(current));
                return;
            }
            QMessageBox box(QMessageBox::Information, tr("Update Available"),
                            tr("DiskForge %1 is available. You have %2.").arg(latest, current), QMessageBox::Close, this);
            box.setInformativeText(tr("It installs over this version, so there's no need to uninstall first.\n\n"
                                      "Installed from the AUR:\n    yay -Syu\n\n"
                                      "Installed from the GitHub repo:\n    cd diskforge && git pull\n    cd packaging/arch && makepkg -si"));
            QAbstractButton *open = box.addButton(tr("Open Release Page"), QMessageBox::ActionRole);
            box.exec();
            if (box.clickedButton() == open && !url.isEmpty())
                QDesktopServices::openUrl(QUrl(url));
        });
    }
    statusBar()->showMessage(tr("Checking for updates…"));
    m_updates->check();
}
