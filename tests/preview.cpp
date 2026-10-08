// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

// Renders the dialogs to PNGs: QT_QPA_PLATFORM=offscreen diskforge-preview <dir>

#include "../src/about.h"
#include "../src/addons.h"
#include "../src/addonsdialog.h"
#include "../src/blockmapwidget.h"
#include "../src/copydialogs.h"
#include "../src/imagebackup.h"
#include "../src/rescuecopy.h"
#include "../src/usagedialog.h"
#include "../src/systemtools.h"
#include "../src/dialogs.h"
#include "../src/tools.h"
#include "../src/translations.h"
#include "../src/udisks.h"

#include <QApplication>
#include <QDir>
#include <QIcon>
#include <QCheckBox>
#include <QTabWidget>
#include <QLineEdit>
#include <QSpinBox>
#include <QTextStream>
#include <QFile>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <QElapsedTimer>
#include <QProgressBar>
#include <QLabel>

void previewTools(UDisks &udisks, const QDir &out);
void previewCopyTools(UDisks &udisks, const QDir &out);

namespace {

void save(QWidget &widget, const QString &path)
{
    widget.show();
    QApplication::processEvents();
    widget.grab().save(path);
    QTextStream(stdout) << path << Qt::endl;
    widget.hide();
}

} // namespace

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    installTranslations();
    QApplication::setApplicationVersion(QStringLiteral(APP_VERSION));
    QApplication::setWindowIcon(QIcon(QStringLiteral(":/data/" APP_ID ".svg")));
    const QDir out(app.arguments().value(1, QStringLiteral(".")));
    UDisks udisks;

    AboutDialog about(udisks.daemonVersion());
    save(about, out.filePath(QStringLiteral("about.png")));
    if (auto *tabs = about.findChild<QTabWidget *>()) {
        tabs->setCurrentIndex(3);
        save(about, out.filePath(QStringLiteral("about-components.png")));
    }
    HelpWindow help;
    save(help, out.filePath(QStringLiteral("handbook.png")));
    Addons addons;
    addons.load();
    AddonsDialog addonsDialog(&addons);
    save(addonsDialog, out.filePath(QStringLiteral("addons.png")));
    previewTools(udisks, out);

    // prefer Ventoy so the warning shows up
    const Disk *disk = nullptr;
    int number = -1;
    for (int i = 0; i < udisks.disks().size(); ++i) {
        const Disk &d = udisks.disks()[i];
        if (!d.isSystem && !d.volumes.isEmpty() && (!disk || d.isVentoy)) {
            disk = &d;
            number = i;
        }
    }
    if (!disk)
        return 1;

    FormatDialog format(*disk, disk->volumes.first(), udisks.filesystems());
    save(format, out.filePath(QStringLiteral("format.png")));

    for (const Disk &d : udisks.disks()) {
        if (d.isSystem)
            continue;
        for (const Span &s : diskSpans(d)) {
            if (s.isFree()) {
                NewPartitionDialog dialog(d, s, udisks.filesystems());
                save(dialog, out.filePath(QStringLiteral("new-partition.png")));
                goto table;
            }
        }
    }
table:
    PartitionTableDialog table(*disk, number);
    save(table, out.filePath(QStringLiteral("table.png")));
    if (auto *confirm = table.findChild<QLineEdit *>()) {
        confirm->setText(disk->device.section(QLatin1Char('/'), -1));
        save(table, out.filePath(QStringLiteral("table-confirmed.png")));
    }

    for (const Disk &d : udisks.disks()) {
        for (const Volume &v : d.volumes) {
            const ResizeLimits limits = udisks.resizeLimits(v);
            if (limits.possible) {
                ResizeDialog dialog(d, v, limits, udisks.resizeNeedsRemount(v, true), udisks.resizeNeedsRemount(v, false));
                save(dialog, out.filePath(QStringLiteral("resize.png")));
                if (auto *size = dialog.findChild<QSpinBox *>()) {
                    size->setValue(size->minimum() + (size->maximum() - size->minimum()) / 3);
                    save(dialog, out.filePath(QStringLiteral("resize-shrink.png")));
                }
                return 0;
            }
        }
    }
    QTextStream(stdout) << "no resizable volume to preview" << Qt::endl;
    return 0;
}

// Second pass: the tools dialogs. Called from main before the resize preview returns.
void previewTools(UDisks &udisks, const QDir &out)
{
    for (const Disk &d : udisks.disks()) {
        if (d.health.state == Health::State::Warning || d.health.state == Health::State::Failing) {
            HealthDialog health(&udisks, d.blockPath);
            save(health, out.filePath(QStringLiteral("health.png")));
            BadSectorsDialog scan(&udisks, d);
            save(scan, out.filePath(QStringLiteral("badsectors.png")));
            // Same dialog partway through a scan: mostly fine, a few slow spots, one bad.
            if (auto *map = scan.findChild<BlockMapWidget *>()) {
                QVector<ReadSample> samples;
                const quint64 step = d.size / 1000;
                for (quint64 off = 0; off < d.size * 7 / 10; off += step)
                    samples.append({off, quint32(step), off % (step * 97) == 0 ? 400u : 12u, off / step != 311});
                map->addSamples(samples);
                save(scan, out.filePath(QStringLiteral("badsectors-map.png")));
            }
            break;
        }
    }
    for (const Disk &d : udisks.disks()) {
        if (d.isSystem || d.isLoop || d.volumes.isEmpty())
            continue;
        BenchmarkDialog bench(&udisks, d);
        save(bench, out.filePath(QStringLiteral("benchmark.png")));
        WipeDialog wipe(d, 2);
        save(wipe, out.filePath(QStringLiteral("wipe.png")));
        WriteImageDialog write(&udisks, QString());
        save(write, out.filePath(QStringLiteral("write-image.png")));
        FormatDialog format(d, d.volumes.first(), udisks.filesystems());
        if (auto *box = format.findChild<QCheckBox *>())
            box->setChecked(true);
        save(format, out.filePath(QStringLiteral("format-encrypted.png")));
        break;
    }
    previewCopyTools(udisks, out);

    // Disk usage of this source tree (small and always there), once the scan is done.
    UsageDialog usage(QStringLiteral(SOURCE_DIR), QStringLiteral("diskforge"));
    usage.show();
    if (auto *busy = usage.findChild<QProgressBar *>()) {
        QElapsedTimer waited;
        waited.start();
        while (busy->isVisible() && waited.elapsed() < 30000)
            QApplication::processEvents(QEventLoop::AllEvents, 50);
    }
    save(usage, out.filePath(QStringLiteral("usage.png")));

    OptimizeDialog optimize(&udisks);
    save(optimize, out.filePath(QStringLiteral("optimize.png")));
    SnapshotsDialog snapshots;
    save(snapshots, out.filePath(QStringLiteral("snapshots.png")));
    // Cleanup works out its sizes in the background first.
    CleanupDialog clean(&udisks);
    clean.show();
    QElapsedTimer waited;
    waited.start();
    auto measuring = [&clean] {
        for (QLabel *l : clean.findChildren<QLabel *>()) {
            if (l->text() == QStringLiteral("…"))
                return true;
        }
        return false;
    };
    while (measuring() && waited.elapsed() < 60000)
        QApplication::processEvents(QEventLoop::AllEvents, 50);
    save(clean, out.filePath(QStringLiteral("cleanup.png")));
}

// Clone, Back Up, Restore and Rescue Copy, with sample files in a temporary folder.
void previewCopyTools(UDisks &udisks, const QDir &out)
{
    const Disk *source = nullptr;
    for (const Disk &d : udisks.disks()) {
        if (!d.isSystem && !d.isLoop && !d.volumes.isEmpty() && (!source || d.rotationRate > 0))
            source = &d;
    }
    if (!source)
        return;
    const Disk disk = *source;
    const Volume volume = disk.volumes.first();
    QTemporaryDir tmp;

    CloneDialog clone(&udisks, disk.blockPath);
    save(clone, out.filePath(QStringLiteral("clone.png")));

    BackupDialog backup(&udisks, volume.objectPath);
    if (auto *file = backup.findChild<QLineEdit *>())
        file->setText(tmp.filePath(QStringLiteral("SATA500-2026-10-08.img.zst")));
    save(backup, out.filePath(QStringLiteral("backup.png")));

    // A description file is all Restore reads until it starts.
    const QString image = tmp.filePath(QStringLiteral("SATA500-2026-10-08.img.zst"));
    BackupInfo info = imagebackup::infoFor(disk, &volume);
    info.size = volume.size / 2;
    info.sha256 = QString(64, QLatin1Char('a'));
    QFile json(imagebackup::infoPath(image));
    if (json.open(QIODevice::WriteOnly))
        json.write(QJsonDocument(info.toJson()).toJson());
    json.close();
    {
        QFile empty(image);
        if (!empty.open(QIODevice::WriteOnly))
            return;
    }
    RestoreDialog restore(&udisks, volume.objectPath);
    if (auto *file = restore.findChild<QLineEdit *>())
        file->setText(image);
    save(restore, out.filePath(QStringLiteral("restore.png")));

    // A rescue that was stopped partway: the first third done, a few bad spots, some to retry.
    RescueMap map(disk.size);
    const quint64 sector = 512;
    map.set(0, disk.size / 3 / sector * sector, RescueMap::Finished);
    for (int i = 1; i < 6; ++i) {
        const quint64 at = disk.size / 3 / 6 * i / sector * sector;
        map.set(at, 64 * 1024 * 1024, i % 2 ? RescueMap::NonTrimmed : RescueMap::Bad);
    }
    const QString rescued = tmp.filePath(QStringLiteral("HGST.img"));
    QString error;
    map.save(rescued + QStringLiteral(".map"), &error);
    {
        QFile empty(rescued);
        if (!empty.open(QIODevice::WriteOnly))
            return;
    }
    RescueDialog rescue(&udisks, disk.blockPath);
    if (auto *file = rescue.findChild<QLineEdit *>())
        file->setText(rescued);
    save(rescue, out.filePath(QStringLiteral("rescue.png")));
}
