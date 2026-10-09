// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

// Renders the dialogs to PNGs: QT_QPA_PLATFORM=offscreen diskforge-preview <dir>

#include "../src/about.h"
#include "../src/addonform.h"
#include "../src/addonmaker.h"
#include "../src/addonoutput.h"
#include "../src/addonprompt.h"
#include "../src/addons.h"
#include "../src/addonsdialog.h"
#include "../src/blockmapwidget.h"
#include "../src/copydialogs.h"
#include "../src/imagebackup.h"
#include "../src/mainwindow.h"
#include "../src/rescuecopy.h"
#include "../src/usagedialog.h"
#include "../src/systemtools.h"
#include "../src/erasedialog.h"
#include "../src/dialogs.h"
#include "../src/theme.h"
#include "../src/thememaker.h"
#include "../src/tools.h"
#include "../src/typedialog.h"
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
#include <QThread>
#include <QTimer>
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
    ThemeMaker themeMaker(&addons);
    save(themeMaker, out.filePath(QStringLiteral("theme-maker.png")));
    {
        // The main window (with the warning banner when a drive here has one), then in each
        // built-in theme.
        MainWindow window(&udisks);
        window.resize(1200, 760);
        save(window, out.filePath(QStringLiteral("main.png")));
        // A wipe running on the second drive (faked), with its Stop bar on top.
        if (udisks.disks().size() > 1) {
            const Disk &d = udisks.disks()[1];
            Job wipe;
            wipe.path = QStringLiteral("/preview/jobs/wipe");
            wipe.operation = QStringLiteral("format-erase");
            wipe.cancelable = true;
            wipe.objects = {d.blockPath};
            wipe.progress = 0.34;
            wipe.progressValid = true;
            wipe.rate = 49 * 1000 * 1000;
            wipe.bytes = d.size;
            udisks.setJobsForTest({wipe});
            udisks.refresh();
            save(window, out.filePath(QStringLiteral("main-stop.png")));
            udisks.setJobsForTest({});
            udisks.refresh();
        }
        for (const Disk &d : udisks.disks()) {
            if (!d.isSystem && !d.volumes.isEmpty() && d.tableType == QLatin1String("gpt")) {
                PartitionTypeDialog typeDialog(d, d.volumes.first());
                save(typeDialog, out.filePath(QStringLiteral("type-flags.png")));
                break;
            }
        }
        for (const char *id : {"classic", "deadshadow", "high-contrast"}) {
            Theme::instance().use(QLatin1String(id), addons);
            save(window, out.filePath(QStringLiteral("main-%1.png").arg(QLatin1String(id))));
        }
        Theme::instance().use(QStringLiteral("system"), addons);
    }
    {
        // The Add-on Maker, editing the backup example (its form fields and all).
        Addon backupExample = Addons::parse(QStringLiteral(SOURCE_DIR "/examples/addons/backup-rsync/addon.json"));
        AddonMaker maker(&addons, &backupExample, [](const Addon &, const AddonAction &) {});
        save(maker, out.filePath(QStringLiteral("addon-maker.png")));
        if (auto *tabs = maker.findChild<QTabWidget *>()) {
            tabs->setCurrentIndex(1);
            save(maker, out.filePath(QStringLiteral("addon-maker-where.png")));
        }
    }

    // The add-on questions open their own dialogs: grab each one while it's open.
    auto grabDialog = [](const QString &path) {
        QTimer::singleShot(100, [path] {
            if (QWidget *w = QApplication::activeModalWidget()) {
                w->grab().save(path);
                QTextStream(stdout) << path << Qt::endl;
                w->close();
            }
        });
    };
    const QString examples = QStringLiteral(SOURCE_DIR "/examples/addons/");
    const Addon smart = Addons::parse(examples + QStringLiteral("smart-report/addon.json"));
    const Addon sizes = Addons::parse(examples + QStringLiteral("folder-sizes/addon.json"));
    Addon backup = Addons::parse(examples + QStringLiteral("backup-rsync/addon.json"));
    backup.outside = true;
    grabDialog(out.filePath(QStringLiteral("addon-install.png")));
    askInstallAddon(nullptr, smart);
    grabDialog(out.filePath(QStringLiteral("addon-run-admin.png")));
    askRunAddon(nullptr, smart, smart.actions.value(0), {QStringLiteral("sudo"), QStringLiteral("smartctl"), QStringLiteral("-x"), QStringLiteral("/dev/sdb")}, nullptr);
    grabDialog(out.filePath(QStringLiteral("addon-run-outside.png")));
    askRunAddon(nullptr, backup, backup.actions.value(0),
                {QStringLiteral("rsync"), QStringLiteral("-a"), QStringLiteral("--info=progress2"), QStringLiteral("--mkpath"),
                 QStringLiteral("/run/media/me/My Stuff/"), QDir::homePath() + QStringLiteral("/Backups/My Stuff/")}, nullptr);
    {
        const Addon full = Addons::parseData(R"json({"id":"backup","name":"Back Up","actions":[{"label":"Back Up to a Folder","output":"window",
            "command":["rsync","-a","{ask:check}","{mountpoint}/","{ask:dest}/"],
            "ask":[{"id":"dest","type":"folder","label":"Back up to","default":"{home}/Backups"},
                   {"id":"check","type":"check","label":"Compare file contents (slower)","on":"--checksum","off":""},
                   {"id":"mode","type":"choice","label":"When a file exists","choices":[{"label":"Replace it","value":"--inplace"},{"label":"Keep both","value":"--backup"}]},
                   {"id":"limit","type":"number","label":"Speed limit (MB/s, 0 = none)","min":0,"max":1000}]}]})json", QString());
        QMap<QString, QString> values;
        for (const AddonField &f : full.actions.value(0).ask)
            values.insert(f.id, QString(f.defaultValue).replace(QStringLiteral("{home}"), QDir::homePath()));
        AddonFormDialog form(QStringLiteral("Back Up to a Folder"), QStringLiteral("\"Back Up to a Folder\" from the add-on \"Back Up\" needs a few things first:"),
                             full.actions.value(0).ask, values, QStringLiteral("Continue"));
        save(form, out.filePath(QStringLiteral("addon-form.png")));
        AddonOutputWindow output(QStringLiteral("Show Folder Sizes"),
                                 {QStringLiteral("sh"), QStringLiteral("-c"), QStringLiteral("printf '4.0K\\t/run/media/me/STICK/notes\\n1.2G\\t/run/media/me/STICK/photos\\n\\033[1;31m3.4G\\033[0m\\t/run/media/me/STICK/videos\\n'; exit 0")});
        output.setAttribute(Qt::WA_DeleteOnClose, false);
        QElapsedTimer wait;
        wait.start();
        while (output.isRunning() && wait.elapsed() < 3000)
            QApplication::processEvents(QEventLoop::AllEvents, 50);
        QThread::msleep(200);
        QApplication::processEvents();
        save(output, out.filePath(QStringLiteral("addon-output.png")));
    }
    grabDialog(out.filePath(QStringLiteral("addon-run-lookonly.png")));
    askRunAddon(nullptr, sizes, sizes.actions.value(0),
                {QStringLiteral("sh"), QStringLiteral("-c"), sizes.actions.value(0).command.value(2), QStringLiteral("sh"), QStringLiteral("/")}, nullptr);
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
        // fwupd answers in the background; wait for it (when it's running) before the picture.
        auto settle = [](QWidget &dialog) {
            QElapsedTimer waited;
            waited.start();
            auto asking = [&dialog] {
                for (QLabel *l : dialog.findChildren<QLabel *>()) {
                    if (l->text().contains(QLatin1String("Asking fwupd")))
                        return true;
                }
                return false;
            };
            do
                QApplication::processEvents(QEventLoop::AllEvents, 50);
            while (asking() && waited.elapsed() < 15000);
        };
        if (d.health.nvme) {
            HealthDialog nvme(&udisks, d.blockPath);
            settle(nvme);
            save(nvme, out.filePath(QStringLiteral("health-nvme.png")));
        }
        if (d.health.state == Health::State::Warning || d.health.state == Health::State::Failing) {
            HealthDialog health(&udisks, d.blockPath);
            settle(health);
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

    // Secure erase: a frozen ATA drive and an NVMe drive (the system one, so Erase stays off).
    for (const Disk &d : udisks.disks()) {
        if (d.ataEraseMinutes > 0 || d.ataEnhancedEraseMinutes > 0) {
            SecureEraseDialog erase(&udisks, d.blockPath, 1);
            save(erase, out.filePath(QStringLiteral("secure-erase-ata.png")));
        } else if (d.nvmeNamespace) {
            SecureEraseDialog erase(&udisks, d.blockPath, 0);
            save(erase, out.filePath(QStringLiteral("secure-erase-nvme.png")));
        }
    }

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
