// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#include "recoverdialog.h"

#include "blockio.h"
#include "dialogs.h"
#include "diskmap.h"
#include "format.h"
#include "jobui.h"

#include <QDialogButtonBox>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QLocale>
#include <QPushButton>
#include <QTimer>
#include <QVBoxLayout>

#include <memory>
#include <unistd.h>

QString RecoverDialog::refusal(const Disk &disk)
{
    if (disk.isSystem)
        return tr("%1 holds the running system, so its partition table can't be written while it runs.").arg(shortDevice(disk.device));
    for (const Volume &v : disk.volumes) {
        if (!v.mounts().isEmpty() || v.swapActive || !v.cleartextPath.isEmpty())
            return tr("%1 is in use. Unmount (and lock) everything on the drive first.").arg(shortDevice(v.device));
    }
    return {};
}

RecoverDialog::RecoverDialog(UDisks *udisks, const Disk &disk, const gpt::Report &report, QWidget *parent)
    : QDialog(parent)
    , m_udisks(udisks)
    , m_disk(disk)
    , m_blockPath(disk.blockPath)
    , m_device(shortDevice(disk.device))
    , m_driveKey(disk.health.key)
    , m_intro(new QLabel)
    , m_list(new QListWidget)
    , m_before(new DiskMap)
    , m_after(new DiskMap)
    , m_problem(new QLabel)
    , m_confirm(new QLineEdit)
{
    build();
    listSources(report);
}

const Disk *RecoverDialog::disk() const
{
    const Disk *live = m_udisks->diskByPath(m_blockPath);
    return live ? live : &m_disk;
}

RecoverDialog::RecoverDialog(UDisks *udisks, const Disk &disk, QWidget *parent)
    : QDialog(parent)
    , m_udisks(udisks)
    , m_disk(disk)
    , m_blockPath(disk.blockPath)
    , m_device(shortDevice(disk.device))
    , m_driveKey(disk.health.key)
    , m_intro(new QLabel)
    , m_list(new QListWidget)
    , m_before(new DiskMap)
    , m_after(new DiskMap)
    , m_problem(new QLabel)
    , m_confirm(new QLineEdit)
{
    build();
    const QString why = refusal(disk);
    if (!why.isEmpty()) {
        m_problem->setText(redText(why));
        m_list->setEnabled(false);
        m_confirm->setEnabled(false);
        refresh();
        return;
    }
    // Read-only first, like a benchmark: just to see what's there.
    openBlockThen(udisks, this, disk.blockPath, UDisks::OpenMode::Benchmark, [this](int fd) {
        gpt::Report report;
        if (fd >= 0) {
            report = gpt::inspect(fd);
            ::close(fd);
        }
        listSources(report);
        emit ready();
    });
    refresh();
}

void RecoverDialog::build()
{
    setWindowTitle(tr("Recover Partitions on %1").arg(diskTitle(m_disk)));
    m_intro->setWordWrap(true);
    m_intro->setText(tr("Puts a partition table back: from the copy GPT keeps at the end of the drive, or from a layout "
                        "DiskForge saw on this drive before. Only the table is written; what's in the partitions isn't "
                        "touched. The table there now is remembered first, so this can be undone the same way."));
    m_problem->setWordWrap(true);
    m_problem->setTextFormat(Qt::RichText);
    m_before->setDisks({m_disk});
    for (DiskMap *map : {m_before, m_after}) {
        map->setAttribute(Qt::WA_TransparentForMouseEvents); // pictures, not something to click
        map->setFocusPolicy(Qt::NoFocus);
    }
    m_confirm->setPlaceholderText(m_device);

    auto *layout = new QVBoxLayout(this);
    layout->addWidget(m_intro);
    layout->addWidget(new QLabel(tr("Put back:")));
    layout->addWidget(m_list, 1);
    layout->addWidget(new QLabel(tr("Now:")));
    layout->addWidget(m_before);
    layout->addWidget(new QLabel(tr("After:")));
    layout->addWidget(m_after);
    layout->addWidget(m_problem);
    auto *confirmLabel = new QLabel(tr("Type <b>%1</b> to confirm:").arg(m_device.toHtmlEscaped()));
    layout->addWidget(confirmLabel);
    layout->addWidget(m_confirm);
    // Writing happens here (the window stays open to show how it went), so not an accept button.
    auto *box = new QDialogButtonBox(QDialogButtonBox::Close);
    m_write = box->addButton(tr("Write Partition Table"), QDialogButtonBox::ActionRole);
    m_write->setAutoDefault(false);
    connect(box, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(box);
    connect(m_write, &QPushButton::clicked, this, &RecoverDialog::write);
    connect(m_list, &QListWidget::currentRowChanged, this, &RecoverDialog::choose);
    connect(m_confirm, &QLineEdit::textChanged, this, &RecoverDialog::refresh);
    resize(820, 640);
}

int RecoverDialog::sourceCount() const
{
    return int(m_sources.size());
}

void RecoverDialog::listSources(const gpt::Report &report)
{
    const QString now = recover::fromDisk(*disk()).fingerprint();
    if (report.backup.valid() && report.backup.lba == report.lastLba && !report.primary.valid()) {
        Source s;
        s.backup = true;
        s.layout = recover::fromReport(report, true);
        s.title = tr("The backup copy at the end of the drive: %n partition(s)", nullptr, int(s.layout.parts.size()));
        m_sources.push_back(s);
    }
    for (const recover::Layout &l : recover::saved(m_driveKey)) {
        if (l.fingerprint() == now)
            continue; // that's what's there already
        Source s;
        s.layout = l;
        QStringList names;
        for (const recover::Part &p : l.parts) {
            if (!p.container)
                names << (p.label.isEmpty() ? (p.fsType.isEmpty() ? formatSize(p.size) : p.fsType) : p.label);
        }
        s.title = tr("As it was on %1: %2").arg(QLocale().toString(l.saved.toLocalTime(), QLocale::ShortFormat), names.join(QStringLiteral(", ")));
        m_sources.push_back(s);
    }
    for (const Source &s : std::as_const(m_sources))
        m_list->addItem(s.title); // plain text: names and labels come from the drive
    if (m_sources.isEmpty())
        m_problem->setText(tr("There's nothing to put back: the table is intact, and DiskForge hasn't seen another layout on this drive."));
    else
        m_list->setCurrentRow(0);
    refresh();
}

void RecoverDialog::choose(int row)
{
    if (row < 0 || row >= m_sources.size())
        return;
    m_after->setDisks({recover::preview(*disk(), m_sources[row].layout)});
    refresh();
}

void RecoverDialog::refresh()
{
    const int row = m_list->currentRow();
    const Disk *d = disk();
    QString why;
    if (row >= 0 && row < m_sources.size() && !m_sources[row].backup) {
        // The sector size isn't known before the drive is open for writing; 512 is the
        // strictest for alignment, and write() checks again with the real one.
        why = recover::problem(m_sources[row].layout, d->size, 512);
        m_problem->setText(why.isEmpty() ? QString() : redText(why));
    }
    m_write->setEnabled(!m_busy && row >= 0 && why.isEmpty() && refusal(*d).isEmpty() && m_confirm->text().trimmed() == m_device);
}

void RecoverDialog::write()
{
    const Disk *disk = m_udisks->diskByPath(m_blockPath);
    const int row = m_list->currentRow();
    if (!disk || row < 0 || row >= m_sources.size() || !refusal(*disk).isEmpty())
        return;
    // What's there now goes on the list first, so this can be undone the same way.
    recover::remember(m_driveKey, recover::fromDisk(*disk));
    const Source source = m_sources[row];
    m_busy = true;
    refresh();
    m_problem->setText(tr("Writing the partition table…"));
    openBlockThen(m_udisks, this, m_blockPath, UDisks::OpenMode::ReadWrite, [this, source](int fd) {
        if (fd < 0) {
            m_busy = false;
            m_problem->setText(redText(tr("Couldn't open the drive for writing.")));
            refresh();
            return;
        }
        const gpt::Result result = source.backup ? gpt::restoreFromBackup(fd) : recover::write(fd, source.layout);
        ::close(fd);
        if (!result.ok) {
            m_busy = false;
            m_problem->setText(redText(result.error));
            refresh();
            emit done(false, result.error);
            return;
        }
        m_udisks->rescan(m_blockPath);
        waitForPartitions(int(source.layout.parts.size()));
    });
}

void RecoverDialog::waitForPartitions(int expected)
{
    // udev reads the new table when the drive is closed, and partitions can come and go for
    // a moment. Done once the count is right and nothing changed for 2 s (or after 20 s).
    m_problem->setText(tr("Waiting for the partitions to show up…"));
    auto *settled = new QTimer(this);
    settled->setSingleShot(true);
    settled->setInterval(2000);
    auto *timeout = new QTimer(this);
    timeout->setSingleShot(true);
    auto finish = [this, settled, timeout, expected](bool inTime) {
        settled->stop();
        timeout->stop();
        disconnect(m_udisks, &UDisks::changed, this, nullptr);
        const Disk *d = m_udisks->diskByPath(m_blockPath);
        const int found = d ? int(d->volumes.size()) : 0;
        m_busy = false;
        const QString message = found >= expected
            ? tr("The partition table of %1 is back: %n partition(s).", nullptr, found).arg(m_device)
            : tr("The table was written, but only %1 of %2 partitions showed up%3.").arg(found).arg(expected).arg(inTime ? QString() : tr(" in time"));
        m_problem->setText(message.toHtmlEscaped());
        refresh();
        emit done(found >= expected, message);
    };
    connect(m_udisks, &UDisks::changed, this, [this, settled, expected] {
        const Disk *d = m_udisks->diskByPath(m_blockPath);
        if (d && int(d->volumes.size()) >= expected)
            settled->start();
        else
            settled->stop();
    });
    connect(settled, &QTimer::timeout, this, [finish] { finish(true); });
    connect(timeout, &QTimer::timeout, this, [finish] { finish(false); });
    timeout->start(20000);
    m_udisks->refresh();
}
