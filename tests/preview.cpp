// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

// Renders the dialogs to PNGs: QT_QPA_PLATFORM=offscreen diskforge-preview <dir>

#include "../src/about.h"
#include "../src/dialogs.h"
#include "../src/tools.h"
#include "../src/udisks.h"

#include <QApplication>
#include <QDir>
#include <QIcon>
#include <QCheckBox>
#include <QTabWidget>
#include <QLineEdit>
#include <QSpinBox>
#include <QTextStream>

void previewTools(UDisks &udisks, const QDir &out);

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
}
