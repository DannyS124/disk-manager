// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#include "tools.h"

#include "blockmapwidget.h"
#include "jobui.h"

#include "benchmark.h"
#include "dialogs.h"
#include "format.h"
#include "imagewriter.h"
#include "surfacescan.h"
#include "theme.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDir>
#include <QDialogButtonBox>
#include <QElapsedTimer>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QProgressBar>
#include <QPushButton>
#include <QThread>
#include <QTreeWidget>
#include <QVBoxLayout>

namespace {

QString healthColor(Health::State s)
{
    const Theme &theme = Theme::instance();
    switch (s) {
    case Health::State::Healthy: return theme.html(Theme::Role::Good);
    case Health::State::Warning: return theme.html(Theme::Role::Warning);
    case Health::State::Failing: return theme.html(Theme::Role::Danger);
    case Health::State::Unknown: break;
    }
    return theme.html(Theme::Role::Muted);
}

QString selftestText(const Health &h)
{
    const QString &s = h.selftestStatus;
    if (s.isEmpty())
        return QObject::tr("Never run");
    if (s == QLatin1String("success"))
        return QObject::tr("Last one passed");
    if (s == QLatin1String("inprogress"))
        return h.selftestPercentRemaining >= 0 ? QObject::tr("Running, %1% left").arg(h.selftestPercentRemaining)
                                               : QObject::tr("Running");
    if (s.startsWith(QLatin1String("abort")) || s == QLatin1String("interrupted"))
        return QObject::tr("Last one was stopped before it finished");
    return QObject::tr("Last one failed (%1)").arg(s);
}

QString sectors(qint64 n)
{
    return QObject::tr("%n sector(s)", nullptr, int(n));
}

} // namespace

// --- Encryption fields --------------------------------------------------------

EncryptionFields::EncryptionFields(QWidget *parent)
    : QWidget(parent)
    , m_box(new QCheckBox(tr("Encrypt with a passphrase (LUKS2)")))
    , m_fields(new QWidget)
    , m_pass(new QLineEdit)
    , m_confirm(new QLineEdit)
    , m_note(new QLabel)
{
    m_pass->setEchoMode(QLineEdit::Password);
    m_confirm->setEchoMode(QLineEdit::Password);
    m_pass->setPlaceholderText(tr("Passphrase"));
    m_confirm->setPlaceholderText(tr("Type it again"));
    m_note->setWordWrap(true);

    auto *fields = new QVBoxLayout(m_fields);
    fields->setContentsMargins(0, 0, 0, 0);
    fields->addWidget(m_pass);
    fields->addWidget(m_confirm);
    fields->addWidget(m_note);
    m_fields->setVisible(false);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(m_box);
    layout->addWidget(m_fields);

    auto update = [this] {
        const bool mismatch = !m_confirm->text().isEmpty() && m_pass->text() != m_confirm->text();
        m_note->setText(mismatch ? redText(tr("The passphrases don't match."))
                                 : tr("If you forget it, nothing on the drive can be recovered."));
        emit changed();
    };
    connect(m_box, &QCheckBox::toggled, this, [this, update](bool on) {
        m_fields->setVisible(on);
        update();
        // Grow the dialog to fit, keeping its width (adjustSize would shrink it).
        QWidget *w = window();
        w->layout()->activate();
        const int h = w->layout()->hasHeightForWidth() ? w->layout()->totalHeightForWidth(w->width()) : w->sizeHint().height();
        w->resize(w->width(), std::max(h, w->minimumSizeHint().height()));
    });
    connect(m_pass, &QLineEdit::textChanged, this, update);
    connect(m_confirm, &QLineEdit::textChanged, this, update);
    update();
}

QString EncryptionFields::passphrase() const
{
    return m_box->isChecked() ? m_pass->text() : QString();
}

bool EncryptionFields::isValid() const
{
    return !m_box->isChecked() || (!m_pass->text().isEmpty() && m_pass->text() == m_confirm->text());
}

// --- Change passphrase --------------------------------------------------------

ChangePassphraseDialog::ChangePassphraseDialog(const QString &device, QWidget *parent)
    : QDialog(parent)
    , m_old(new QLineEdit)
    , m_new(new QLineEdit)
    , m_confirm(new QLineEdit)
{
    setWindowTitle(tr("Change Passphrase of %1").arg(device));
    for (QLineEdit *e : {m_old, m_new, m_confirm})
        e->setEchoMode(QLineEdit::Password);
    auto *form = new QFormLayout;
    form->addRow(tr("Current passphrase:"), m_old);
    form->addRow(tr("New passphrase:"), m_new);
    form->addRow(tr("New passphrase again:"), m_confirm);

    QPushButton *ok;
    QDialogButtonBox *box = dialogButtons(this, tr("Change"), &ok);
    auto update = [this, ok] {
        ok->setEnabled(!m_old->text().isEmpty() && !m_new->text().isEmpty() && m_new->text() == m_confirm->text());
    };
    for (QLineEdit *e : {m_old, m_new, m_confirm})
        connect(e, &QLineEdit::textChanged, this, update);
    update();

    auto *layout = new QVBoxLayout(this);
    layout->addLayout(form);
    layout->addWidget(box);
}

QString ChangePassphraseDialog::oldPassphrase() const { return m_old->text(); }
QString ChangePassphraseDialog::newPassphrase() const { return m_new->text(); }

// --- Wipe ---------------------------------------------------------------------

WipeDialog::WipeDialog(const Disk &disk, int diskNumber, QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle(tr("Wipe Disk %1").arg(diskNumber));
    const bool slow = disk.rotationRate > 0 || disk.bus == QLatin1String("usb") || disk.removable;
    // Measured: a 5400 rpm laptop drive zero-fills at about 50 MB/s.
    const double seconds = double(disk.size) / (slow ? 50e6 : 400e6);

    QStringList lost;
    for (const Volume &v : disk.volumes) {
        if (!v.isContainer)
            lost << QStringLiteral("• ") + describeVolume(v).toHtmlEscaped();
    }
    QString text = tr("<p><b>Disk %1: %2</b></p>"
                      "<p>Every byte on the disk is overwritten with zeros, so nothing on it can be recovered. "
                      "Afterwards it has no partitions; use New Partition Table to use it again.</p>"
                      "<p>This takes %3.</p>")
                       .arg(diskNumber).arg(diskTitle(disk).toHtmlEscaped(), durationText(seconds));
    if (!lost.isEmpty())
        text += redText(tr("This erases:")) + QStringLiteral("<br>") + lost.join(QStringLiteral("<br>"));
    if (!diskWarning(disk).isEmpty())
        text += QStringLiteral("<br>") + redText(diskWarning(disk));

    auto *layout = new QVBoxLayout(this);
    layout->addWidget(wrappingLabel(text));
    const QString name = shortDevice(disk.device);
    layout->addWidget(new QLabel(tr("Type <b>%1</b> to confirm:").arg(name)));
    auto *confirm = new QLineEdit;
    confirm->setPlaceholderText(name);
    layout->addWidget(confirm);
    QPushButton *ok;
    layout->addWidget(dialogButtons(this, tr("Wipe Disk"), &ok));
    ok->setEnabled(false);
    connect(confirm, &QLineEdit::textChanged, this, [ok, name](const QString &t) { ok->setEnabled(t.trimmed() == name); });
    resize(520, sizeHint().height());
}

// --- Health -------------------------------------------------------------------

HealthDialog::HealthDialog(UDisks *udisks, const QString &blockPath, QWidget *parent)
    : QDialog(parent)
    , m_udisks(udisks)
    , m_blockPath(blockPath)
    , m_state(new QLabel)
    , m_explain(new QLabel)
    , m_form(new QFormLayout)
    , m_attributes(new QTreeWidget)
    , m_selftest(new QPushButton(tr("Run Short Self-Test")))
{
    m_explain->setWordWrap(true);
    m_attributes->setRootIsDecorated(false);
    m_attributes->setAlternatingRowColors(true);

    auto *refresh = new QPushButton(tr("Refresh"));
    auto *close = new QPushButton(tr("Close"));
    connect(close, &QPushButton::clicked, this, &QDialog::accept);
    connect(refresh, &QPushButton::clicked, this, [this] {
        if (const Disk *d = m_udisks->diskByPath(m_blockPath))
            m_udisks->smartUpdate(*d);
    });
    connect(m_selftest, &QPushButton::clicked, this, [this] {
        if (const Disk *d = m_udisks->diskByPath(m_blockPath))
            m_udisks->smartSelftest(*d, QStringLiteral("short"));
    });
    auto *longTest = new QPushButton(tr("Run Long Self-Test"));
    connect(longTest, &QPushButton::clicked, this, [this] {
        if (const Disk *d = m_udisks->diskByPath(m_blockPath))
            m_udisks->smartSelftest(*d, QStringLiteral("extended"));
    });
    auto *scan = new QPushButton(tr("Scan for Bad Sectors…"));
    connect(scan, &QPushButton::clicked, this, [this] {
        if (const Disk *d = m_udisks->diskByPath(m_blockPath))
            BadSectorsDialog(m_udisks, *d, this).exec();
    });
    auto *buttons = new QHBoxLayout;
    buttons->addWidget(m_selftest);
    buttons->addWidget(longTest);
    buttons->addWidget(scan);
    buttons->addWidget(refresh);
    buttons->addStretch();
    buttons->addWidget(close);

    auto *layout = new QVBoxLayout(this);
    layout->addWidget(m_state);
    layout->addWidget(m_explain);
    layout->addLayout(m_form);
    layout->addWidget(m_attributes, 1);
    layout->addLayout(buttons);

    connect(m_udisks, &UDisks::changed, this, &HealthDialog::reload);
    reload();
    resize(640, 560);
}

void HealthDialog::reload()
{
    const Disk *disk = m_udisks->diskByPath(m_blockPath);
    if (!disk) {
        m_state->setText(tr("The disk is gone."));
        return;
    }
    const Health &h = disk->health;
    setWindowTitle(tr("Health of %1").arg(disk->model));
    m_state->setText(QStringLiteral("<span style=\"font-size:16pt; font-weight:bold; color:%1\">%2</span><br>%3")
                         .arg(healthColor(h.state), h.summary.toHtmlEscaped(), diskTitle(*disk).toHtmlEscaped()));

    QString explain;
    if (h.state == Health::State::Failing)
        explain = tr("The drive itself reports that it's failing. Copy anything you want to keep to another drive now.");
    else if (h.state == Health::State::Warning && h.pendingSectors > 0)
        explain = tr("The drive can't read some sectors. Scan for Bad Sectors finds them and can repair them; whatever "
                     "was stored there is already lost. Keep backups of anything important on this drive.");
    else if (h.state == Health::State::Warning && h.reallocatedSectors > 0)
        explain = tr("The drive has swapped some weak sectors for spares. That's what drives are built to do and it still "
                     "works fine, but keep backups and watch whether the number grows.");
    else if (h.state == Health::State::Warning)
        explain = tr("Something isn't quite right. Keep backups of anything important on this drive.");
    else if (h.state == Health::State::Healthy)
        explain = tr("No problems reported.");
    m_explain->setText(explain);
    m_explain->setVisible(!explain.isEmpty());

    while (m_form->rowCount() > 0)
        m_form->removeRow(0);
    if (h.temperatureC > 0)
        m_form->addRow(tr("Temperature:"), new QLabel(QStringLiteral("%1 °C").arg(qRound(h.temperatureC))));
    if (h.powerOnHours > 0)
        m_form->addRow(tr("Powered on:"), new QLabel(tr("%L1 hours (%L2 days)").arg(h.powerOnHours).arg(h.powerOnHours / 24)));
    if (h.reallocatedSectors >= 0)
        m_form->addRow(tr("Replaced sectors:"), new QLabel(QString::number(h.reallocatedSectors)));
    if (h.pendingSectors >= 0)
        m_form->addRow(tr("Unreadable sectors:"), new QLabel(QString::number(h.pendingSectors)));
    if (h.percentUsed >= 0)
        m_form->addRow(tr("Life used:"), new QLabel(QStringLiteral("%1%").arg(h.percentUsed)));
    if (!h.criticalWarnings.isEmpty())
        m_form->addRow(tr("Warnings:"), new QLabel(h.criticalWarnings.join(QStringLiteral(", "))));
    m_form->addRow(tr("Self-test:"), new QLabel(selftestText(h)));
    m_selftest->setEnabled(h.selftestStatus != QLatin1String("inprogress"));

    const QVector<SmartAttribute> attrs = m_udisks->smartAttributes(*disk);
    m_attributes->clear();
    const bool ata = !h.nvme;
    m_attributes->setHeaderLabels(ata ? QStringList{tr("ID"), tr("Attribute"), tr("Value"), tr("Worst"), tr("Threshold"), tr("Raw")}
                                      : QStringList{tr("Attribute"), tr("Value")});
    for (const SmartAttribute &a : attrs) {
        auto *item = ata ? new QTreeWidgetItem(m_attributes, {QString::number(a.id), a.name, QString::number(a.value),
                                                               QString::number(a.worst), QString::number(a.threshold), a.raw})
                         : new QTreeWidgetItem(m_attributes, {a.name, a.raw});
        if (a.failing) {
            for (int c = 0; c < item->columnCount(); ++c)
                item->setForeground(c, Theme::instance().color(Theme::Role::Danger));
        }
    }
    for (int c = 0; c < m_attributes->columnCount(); ++c)
        m_attributes->resizeColumnToContents(c);
}

// --- Benchmark ----------------------------------------------------------------

BenchmarkDialog::BenchmarkDialog(UDisks *udisks, const Disk &disk, QWidget *parent)
    : QDialog(parent)
    , m_udisks(udisks)
    , m_disk(disk)
    , m_writeTest(new QCheckBox)
    , m_start(new QPushButton(tr("Start")))
    , m_progress(new QProgressBar)
    , m_phase(new QLabel)
    , m_results(new QLabel)
{
    setWindowTitle(tr("Benchmark %1").arg(disk.model));
    // A writable mount on this disk, or your home folder if it lives on this disk.
    const QString home = QDir::homePath();
    QString homeMount;
    for (const Volume &v : disk.volumes) {
        for (const QString &mp : v.mounts()) {
            if (m_writeDir.isEmpty() && QFileInfo(mp).isWritable())
                m_writeDir = mp;
            if ((home == mp || home.startsWith(mp == QLatin1String("/") ? mp : mp + QLatin1Char('/'))) && mp.size() > homeMount.size())
                homeMount = mp;
        }
    }
    if (m_writeDir.isEmpty() && !homeMount.isEmpty())
        m_writeDir = home;
    m_writeTest->setText(m_writeDir.isEmpty() ? tr("Write speed (mount a partition on this disk to test it)")
                                              : tr("Also test write speed (a 256 MB file in %1, deleted afterwards)").arg(m_writeDir));
    m_writeTest->setEnabled(!m_writeDir.isEmpty());
    m_progress->setRange(0, 100);
    m_progress->setVisible(false);
    m_results->setTextFormat(Qt::RichText);

    auto *close = new QPushButton(tr("Close"));
    connect(close, &QPushButton::clicked, this, &BenchmarkDialog::reject);
    connect(m_start, &QPushButton::clicked, this, &BenchmarkDialog::start);
    auto *buttons = new QHBoxLayout;
    buttons->addStretch();
    buttons->addWidget(m_start);
    buttons->addWidget(close);

    auto *layout = new QVBoxLayout(this);
    layout->addWidget(wrappingLabel(tr("<b>%1</b><br>Measures how fast the drive reads and how quickly it finds data. "
                                       "Nothing on the drive is changed.").arg(diskTitle(disk).toHtmlEscaped())));
    layout->addWidget(m_writeTest);
    layout->addWidget(m_phase);
    layout->addWidget(m_progress);
    layout->addWidget(m_results);
    layout->addStretch();
    layout->addLayout(buttons);
    resize(520, 320);
}

BenchmarkDialog::~BenchmarkDialog()
{
    stop();
}

void BenchmarkDialog::stop()
{
    disconnect(m_openConn);
    if (m_thread) {
        if (auto *b = qobject_cast<Benchmark *>(m_worker))
            b->cancel();
        m_thread->quit();
        m_thread->wait();
        m_thread = nullptr;
    }
}

void BenchmarkDialog::reject()
{
    stop();
    QDialog::reject();
}

void BenchmarkDialog::start()
{
    m_start->setEnabled(false);
    m_writeTest->setEnabled(false);
    m_results->clear();
    m_phase->setText(tr("Waiting for permission…"));
    m_openConn = connect(m_udisks, &UDisks::deviceOpened, this, [this](const QString &path, int fd) {
        if (path != m_disk.blockPath)
            return;
        disconnect(m_openConn);
        if (fd < 0) {
            m_phase->setText(tr("Couldn't open the drive."));
            m_start->setEnabled(true);
            return;
        }
        auto *bench = new Benchmark(fd, m_disk.size, m_writeTest->isChecked() ? m_writeDir : QString());
        m_worker = bench;
        m_thread = new QThread(this);
        bench->moveToThread(m_thread);
        connect(m_thread, &QThread::started, bench, &Benchmark::run);
        connect(m_thread, &QThread::finished, bench, &QObject::deleteLater);
        connect(bench, &Benchmark::progress, this, [this](const QString &phase, int percent) {
            m_phase->setText(phase);
            m_progress->setValue(percent);
        });
        connect(bench, &Benchmark::finished, this, [this](bool ok, const BenchmarkResult &r, const QString &error) {
            m_thread->quit();
            m_thread->wait();
            m_thread = nullptr;
            m_worker = nullptr;
            m_progress->setVisible(false);
            m_start->setEnabled(true);
            m_start->setText(tr("Run Again"));
            m_writeTest->setEnabled(!m_writeDir.isEmpty());
            if (!ok) {
                m_phase->setText(error);
                return;
            }
            m_phase->clear();
            QString text = tr("<p><b>Read speed:</b> %1 MB/s<br><small>slowest part %2 MB/s, fastest %3 MB/s</small></p>"
                              "<p><b>Access time:</b> %4 ms</p>")
                               .arg(qRound(r.readMBps)).arg(qRound(r.minReadMBps)).arg(qRound(r.maxReadMBps))
                               .arg(r.accessMs, 0, 'f', 2);
            if (r.writeMBps > 0)
                text += tr("<p><b>Write speed:</b> %1 MB/s</p>").arg(qRound(r.writeMBps));
            m_results->setText(text);
        });
        m_progress->setValue(0);
        m_progress->setVisible(true);
        m_thread->start();
    });
    m_udisks->openDevice(m_disk, false, true);
}

// --- Write image --------------------------------------------------------------

WriteImageDialog::WriteImageDialog(UDisks *udisks, const QString &preferredDisk, QWidget *parent)
    : QDialog(parent)
    , m_udisks(udisks)
    , m_image(new QLineEdit)
    , m_imageInfo(new QLabel)
    , m_targets(new QComboBox)
    , m_sha(new QLineEdit)
    , m_verify(new QCheckBox(tr("Check the drive after writing (recommended)")))
    , m_warning(new QLabel)
    , m_confirm(new QLineEdit)
    , m_progress(new QProgressBar)
    , m_phase(new QLabel)
{
    setWindowTitle(tr("Write Image to USB"));
    auto *browse = new QPushButton(tr("Browse…"));
    connect(browse, &QPushButton::clicked, this, [this] {
        const QString file = QFileDialog::getOpenFileName(this, tr("Choose an Image"), QDir::homePath() + QStringLiteral("/Downloads"),
                                                          tr("Disk images (*.iso *.img *.raw);;All files (*)"));
        if (!file.isEmpty())
            m_image->setText(file);
    });
    auto *imageRow = new QHBoxLayout;
    imageRow->addWidget(m_image, 1);
    imageRow->addWidget(browse);
    m_image->setPlaceholderText(tr("archlinux-x86_64.iso"));
    m_sha->setPlaceholderText(tr("Optional: paste it from the download page to check the file first"));
    m_verify->setChecked(true);
    m_warning->setWordWrap(true);
    m_warning->setTextFormat(Qt::RichText);
    m_progress->setRange(0, 1000);
    m_progress->setVisible(false);

    auto *form = new QFormLayout;
    form->addRow(tr("Image:"), imageRow);
    form->addRow(QString(), m_imageInfo);
    form->addRow(tr("Drive:"), m_targets);
    form->addRow(tr("SHA-256:"), m_sha);
    form->addRow(QString(), m_verify);

    auto *layout = new QVBoxLayout(this);
    layout->addLayout(form);
    layout->addWidget(wrappingLabel(tr("<small>Works for Linux ISOs (Arch, Ubuntu, Fedora...) and other bootable images. "
                                       "Windows ISOs need a different tool, like WoeUSB.</small>")));
    layout->addWidget(m_warning);
    layout->addWidget(m_confirm);
    layout->addWidget(m_phase);
    layout->addWidget(m_progress);
    auto *box = new QDialogButtonBox(QDialogButtonBox::Cancel);
    m_write = box->addButton(tr("Write"), QDialogButtonBox::ActionRole);
    m_write->setAutoDefault(false);
    box->button(QDialogButtonBox::Cancel)->setDefault(true);
    connect(m_write, &QPushButton::clicked, this, &WriteImageDialog::start);
    connect(box, &QDialogButtonBox::rejected, this, &WriteImageDialog::reject);
    layout->addWidget(box);

    fillTargets(preferredDisk);
    connect(m_image, &QLineEdit::textChanged, this, &WriteImageDialog::updateState);
    connect(m_targets, &QComboBox::currentIndexChanged, this, &WriteImageDialog::updateState);
    connect(m_confirm, &QLineEdit::textChanged, this, &WriteImageDialog::updateState);
    updateState();
    resize(600, sizeHint().height());
}

WriteImageDialog::~WriteImageDialog()
{
    disconnect(m_openConn);
    if (m_thread) {
        if (auto *w = qobject_cast<ImageWriter *>(m_worker))
            w->cancel();
        m_thread->quit();
        m_thread->wait();
    }
}

void WriteImageDialog::fillTargets(const QString &preferred)
{
    m_targets->clear();
    const QVector<Disk> &disks = m_udisks->disks();
    for (int i = 0; i < disks.size(); ++i) {
        const Disk &d = disks[i];
        // USB and removable drives only: writing an image over an internal disk is almost always a mistake.
        if (d.isSystem || d.isLoop || !(d.removable || d.bus == QLatin1String("usb")))
            continue;
        m_targets->addItem(tr("Disk %1: %2").arg(i).arg(diskTitle(d)), d.blockPath);
        if (d.blockPath == preferred)
            m_targets->setCurrentIndex(m_targets->count() - 1);
    }
    if (m_targets->count() == 0)
        m_targets->addItem(tr("No drive to write to (plug in a USB stick)"));
}

const Disk *WriteImageDialog::target() const
{
    return m_udisks->diskByPath(m_targets->currentData().toString());
}

void WriteImageDialog::updateState()
{
    if (m_running)
        return;
    const QFileInfo image(m_image->text().trimmed());
    const Disk *d = target();
    bool ok = image.isFile() && d;
    if (!image.isFile())
        m_imageInfo->setText(m_image->text().isEmpty() ? QString() : tr("File not found"));
    else
        m_imageInfo->setText(formatSize(quint64(image.size())));

    QString warning;
    if (d) {
        if (image.isFile() && quint64(image.size()) > d->size) {
            warning = redText(tr("The image (%1) is bigger than this drive.").arg(formatSize(quint64(image.size()))));
            ok = false;
        } else {
            warning = redText(tr("Everything on %1 will be erased.").arg(diskTitle(*d)));
            if (!diskWarning(*d).isEmpty())
                warning += QStringLiteral("<br>") + redText(diskWarning(*d));
            const QString name = shortDevice(d->device);
            warning += QStringLiteral("<br>") + tr("Type <b>%1</b> to confirm:").arg(name);
            m_confirm->setPlaceholderText(name);
            ok = ok && m_confirm->text().trimmed() == name;
        }
    }
    m_warning->setText(warning);
    m_confirm->setVisible(d != nullptr);
    m_write->setEnabled(ok);
}

void WriteImageDialog::reject()
{
    if (m_running) {
        const auto answer = QMessageBox::warning(this, windowTitle(),
                                                 tr("Stop writing? The drive won't be usable until you format it again."),
                                                 QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
        if (answer != QMessageBox::Yes)
            return;
        if (auto *w = qobject_cast<ImageWriter *>(m_worker))
            w->cancel();
        return; // finished() closes the dialog
    }
    disconnect(m_openConn);
    QDialog::reject();
}

void WriteImageDialog::start()
{
    const Disk *d = target();
    if (!d)
        return;
    const Disk disk = *d;
    const QString image = m_image->text().trimmed();
    const QString sha = m_sha->text().trimmed();
    const bool verify = m_verify->isChecked();

    m_running = true;
    for (QWidget *w : std::initializer_list<QWidget *>{m_image, m_targets, m_sha, m_verify, m_confirm, m_write})
        w->setEnabled(false);
    m_phase->setText(tr("Waiting for permission…"));

    m_openConn = connect(m_udisks, &UDisks::deviceOpened, this, [this, disk, image, sha, verify](const QString &path, int fd) {
        if (path != disk.blockPath)
            return;
        disconnect(m_openConn);
        if (fd < 0) {
            m_running = false;
            for (QWidget *w : std::initializer_list<QWidget *>{m_image, m_targets, m_sha, m_verify, m_confirm})
                w->setEnabled(true);
            m_phase->setText(tr("Couldn't open the drive."));
            updateState();
            return;
        }
        auto *writer = new ImageWriter(image, fd, verify, sha);
        m_worker = writer;
        m_thread = new QThread(this);
        writer->moveToThread(m_thread);
        connect(m_thread, &QThread::started, writer, &ImageWriter::run);
        connect(m_thread, &QThread::finished, writer, &QObject::deleteLater);

        // Speed and time left are per phase (checking, writing, verifying).
        struct Clock { QElapsedTimer timer; QString phase; };
        auto *clock = new Clock;
        connect(writer, &ImageWriter::progress, this, [this, clock](const QString &phase, quint64 done, quint64 total) {
            if (phase != clock->phase) {
                clock->phase = phase;
                clock->timer.start();
            }
            m_progress->setValue(total ? int(done * 1000 / total) : 0);
            const double seconds = clock->timer.nsecsElapsed() / 1e9;
            QString text = phase;
            if (seconds > 2 && done < total) {
                const double rate = done / seconds;
                text += tr(", %1/s, %2 left").arg(formatSize(quint64(rate)), durationText((total - done) / std::max(rate, 1.0)));
            }
            m_phase->setText(text);
        });
        connect(writer, &ImageWriter::finished, this, [this, clock](bool ok, const QString &message) {
            delete clock;
            m_thread->quit();
            m_thread->wait();
            m_thread = nullptr;
            m_worker = nullptr;
            m_running = false;
            m_udisks->refresh();
            if (ok) {
                QMessageBox::information(this, windowTitle(), message);
                QDialog::accept();
            } else {
                QMessageBox::warning(this, windowTitle(), message);
                QDialog::reject();
            }
        });
        m_progress->setValue(0);
        m_progress->setVisible(true);
        m_thread->start();
    });
    m_udisks->openDevice(disk, true, false);
}

// --- Bad sectors --------------------------------------------------------------

BadSectorsDialog::BadSectorsDialog(UDisks *udisks, const Disk &disk, QWidget *parent)
    : QDialog(parent)
    , m_udisks(udisks)
    , m_disk(disk)
    , m_status(new QLabel)
    , m_progress(new QProgressBar)
    , m_map(new BlockMapWidget)
    , m_found(new QTreeWidget)
    , m_repairNote(new QLabel)
    , m_scan(new QPushButton(tr("Start Scan")))
    , m_repair(new QPushButton(tr("Repair…")))
{
    setWindowTitle(tr("Bad Sectors on %1").arg(disk.model));
    const bool slow = disk.rotationRate > 0 || disk.bus == QLatin1String("usb") || disk.removable;
    QString intro = tr("<p><b>%1</b></p><p>Reads every sector of the drive to find ones that can't be read. "
                       "Scanning doesn't change anything and takes %2. You can keep using the PC meanwhile.</p>")
                        .arg(diskTitle(disk).toHtmlEscaped(), durationText(double(disk.size) / (slow ? 70e6 : 450e6)));
    if (disk.health.pendingSectors > 0)
        intro += tr("<p>The drive reports %1 that it can't read. This scan finds them so they can be repaired.</p>")
                     .arg(sectors(disk.health.pendingSectors));
    else if (disk.health.reallocatedSectors > 0)
        intro += tr("<p>The drive has already swapped %1 for spares; those are handled. It doesn't report any unreadable "
                    "sectors right now, but a scan checks every sector to be sure.</p>")
                     .arg(sectors(disk.health.reallocatedSectors));

    m_progress->setRange(0, 1000);
    m_progress->setVisible(false);
    m_status->setWordWrap(true);
    m_map->reset(disk.size, disk.rotationRate > 0);
    QVector<BlockMapWidget::Area> areas;
    for (const Volume &v : disk.volumes) {
        if (!v.isContainer)
            areas.append({v.offset, v.offset + v.size, volumeTitle(v)});
    }
    m_map->setAreas(areas);
    m_map->setToolTip(QString()); // per-cell tooltips come from the widget
    m_found->setHeaderLabels({tr("Sector"), tr("Position"), tr("Partition")});
    m_found->setRootIsDecorated(false);
    m_found->setVisible(false);
    m_repairNote->setWordWrap(true);
    m_repairNote->setText(tr("Repair rewrites only these sectors. Anything the drive can still read is kept; the rest was "
                             "already lost and becomes zeros. The drive then reuses each spot or swaps in a spare. "
                             "If new bad sectors keep showing up, the drive is wearing out: plan to replace it."));
    m_repairNote->setVisible(false);
    m_repair->setVisible(false);

    auto *close = new QPushButton(tr("Close"));
    connect(close, &QPushButton::clicked, this, &BadSectorsDialog::reject);
    connect(m_scan, &QPushButton::clicked, this, [this] {
        if (m_running)
            stop();
        else
            startScan();
    });
    connect(m_repair, &QPushButton::clicked, this, &BadSectorsDialog::startRepair);
    auto *buttons = new QHBoxLayout;
    buttons->addWidget(m_repair);
    buttons->addStretch();
    buttons->addWidget(m_scan);
    buttons->addWidget(close);

    auto *layout = new QVBoxLayout(this);
    layout->addWidget(wrappingLabel(intro));
    layout->addWidget(m_progress);
    layout->addWidget(m_status);
    layout->addWidget(m_map, 2);
    layout->addWidget(m_found, 1);
    layout->addWidget(m_repairNote);
    layout->addLayout(buttons);
    resize(680, 620);
}

BadSectorsDialog::~BadSectorsDialog()
{
    stop();
}

void BadSectorsDialog::stop()
{
    disconnect(m_openConn);
    if (auto *s = qobject_cast<SurfaceScan *>(m_worker))
        s->cancel(); // finished() then reports how far it got
}

void BadSectorsDialog::reject()
{
    if (m_running && qobject_cast<SectorRepair *>(m_worker))
        return; // a repair is a few writes; let it finish
    stop();
    if (m_thread) {
        m_thread->quit();
        m_thread->wait();
    }
    QDialog::reject();
}

void BadSectorsDialog::setRunning(bool running)
{
    m_running = running;
    m_scan->setText(running ? tr("Stop") : tr("Scan Again"));
    m_repair->setEnabled(!running && !m_bad.isEmpty() && !m_disk.isSystem);
}

void BadSectorsDialog::showFound()
{
    m_found->clear();
    for (quint64 offset : m_bad) {
        QString where = tr("Outside any partition");
        for (const Volume &v : m_disk.volumes) {
            if (!v.isContainer && offset >= v.offset && offset < v.offset + v.size)
                where = volumeTitle(v);
        }
        new QTreeWidgetItem(m_found, {QString::number(offset / quint64(m_logical)), formatSize(offset), where});
    }
    for (int c = 0; c < m_found->columnCount(); ++c)
        m_found->resizeColumnToContents(c);
    const bool any = !m_bad.isEmpty();
    m_found->setVisible(any);
    m_repairNote->setVisible(any);
    m_repair->setVisible(any);
    if (any && m_disk.isSystem)
        m_repairNote->setText(m_repairNote->text() + QStringLiteral("\n\n")
                              + tr("This is your system drive, so DiskForge won't write to it. Repair it from a live USB instead."));
}

void BadSectorsDialog::startScan()
{
    m_bad.clear();
    showFound();
    setRunning(true);
    m_status->setText(tr("Waiting for permission…"));
    m_openConn = connect(m_udisks, &UDisks::deviceOpened, this, [this](const QString &path, int fd) {
        if (path != m_disk.blockPath)
            return;
        disconnect(m_openConn);
        if (fd < 0) {
            m_status->setText(tr("Couldn't open the drive."));
            setRunning(false);
            return;
        }
        auto *scan = new SurfaceScan(fd, m_disk.size);
        m_worker = scan;
        m_thread = new QThread(this);
        scan->moveToThread(m_thread);
        connect(m_thread, &QThread::started, scan, &SurfaceScan::run);
        connect(m_thread, &QThread::finished, scan, &QObject::deleteLater);
        auto *clock = new QElapsedTimer;
        clock->start();
        m_map->reset(m_disk.size, m_disk.rotationRate > 0);
        connect(scan, &SurfaceScan::samples, m_map, &BlockMapWidget::addSamples);
        connect(scan, &SurfaceScan::progress, this, [this, clock](quint64 done, quint64 total, int bad) {
            m_progress->setValue(int(done * 1000 / std::max<quint64>(total, 1)));
            const double rate = done / std::max(clock->nsecsElapsed() / 1e9, 0.001);
            m_status->setText(tr("Scanned %1 of %2, %3/s, %4 left. Bad sectors so far: %5")
                                  .arg(formatSize(done), formatSize(total), formatSize(quint64(rate)),
                                       durationText((total - done) / std::max(rate, 1.0)))
                                  .arg(bad));
        });
        connect(scan, &SurfaceScan::finished, this, [this, clock](bool completed, const QVector<quint64> &bad, int logical) {
            delete clock;
            m_thread->quit();
            m_thread->wait();
            m_thread = nullptr;
            m_worker = nullptr;
            m_bad = bad;
            m_logical = logical;
            m_progress->setVisible(false);
            const int slow = m_map->data().slowAreas();
            const QString slowNote = slow == 0 ? QString()
                : QStringLiteral(" ") + tr("%n area(s) read slowly. Slow spots often turn into bad sectors later, so keep a backup.", nullptr, slow);
            if (!completed)
                m_status->setText(tr("Stopped. Bad sectors found so far: %1").arg(bad.size()) + slowNote);
            else if (bad.isEmpty() && slow == 0)
                m_status->setText(tr("Done. Every sector reads fine."));
            else if (bad.isEmpty())
                m_status->setText(tr("Done. Every sector can be read.") + slowNote);
            else
                m_status->setText(tr("Done. %1 can't be read.").arg(sectors(bad.size())) + slowNote);
            showFound();
            setRunning(false);
        });
        m_progress->setValue(0);
        m_progress->setVisible(true);
        m_thread->start();
    });
    m_udisks->openDevice(m_disk, false, true);
}

void BadSectorsDialog::startRepair()
{
    if (m_bad.isEmpty() || m_disk.isSystem)
        return;
    QStringList mounted;
    for (const Volume &v : m_disk.volumes) {
        if (!v.mounts().isEmpty())
            mounted << volumeTitle(v);
    }
    QString question = tr("Repair %n bad sector(s) on %1?", nullptr, int(m_bad.size())).arg(diskTitle(m_disk));
    if (!mounted.isEmpty())
        question += QStringLiteral("\n\n") + tr("These get unmounted first: %1").arg(mounted.join(QStringLiteral(", ")));
    QMessageBox confirm(QMessageBox::Warning, tr("Repair Bad Sectors"), question, QMessageBox::Cancel, this);
    confirm.setInformativeText(m_repairNote->text());
    QAbstractButton *go = confirm.addButton(tr("Repair"), QMessageBox::AcceptRole);
    confirm.setDefaultButton(QMessageBox::Cancel);
    confirm.exec();
    if (confirm.clickedButton() != go)
        return;

    setRunning(true);
    m_scan->setEnabled(false);
    m_status->setText(tr("Waiting for permission…"));
    const QVector<quint64> bad = m_bad;
    m_openConn = connect(m_udisks, &UDisks::deviceOpened, this, [this, bad](const QString &path, int fd) {
        if (path != m_disk.blockPath)
            return;
        disconnect(m_openConn);
        if (fd < 0) {
            m_status->setText(tr("Couldn't open the drive for writing."));
            m_scan->setEnabled(true);
            setRunning(false);
            return;
        }
        auto *repair = new SectorRepair(fd, bad);
        m_worker = repair;
        m_thread = new QThread(this);
        repair->moveToThread(m_thread);
        connect(m_thread, &QThread::started, repair, &SectorRepair::run);
        connect(m_thread, &QThread::finished, repair, &QObject::deleteLater);
        connect(repair, &SectorRepair::progress, this, [this](int done, int total) {
            m_status->setText(tr("Rewriting block %1 of %2…").arg(done).arg(total));
        });
        connect(repair, &SectorRepair::finished, this, [this](const RepairResult &r) {
            m_thread->quit();
            m_thread->wait();
            m_thread = nullptr;
            m_worker = nullptr;
            m_scan->setEnabled(true);
            QString text;
            if (!r.error.isEmpty()) {
                text = r.error;
            } else {
                text = tr("Rewrote %1. Still readable and kept: %2. Already lost, now zeros: %3.")
                           .arg(tr("%n block(s)", nullptr, r.blocks), sectors(r.recovered), sectors(r.zeroed));
                if (r.stillBad > 0)
                    text += QStringLiteral("\n\n") + tr("%1 still can't be read: the drive has run out of ways to fix them. "
                                                        "Copy off anything you need and replace it.").arg(sectors(r.stillBad));
                else
                    text += QStringLiteral("\n\n") + tr("Run Check for Errors on the partition so the file system catches up, then scan again to confirm.");
            }
            m_status->setText(text);
            m_bad.clear();
            showFound();
            setRunning(false);
            if (const Disk *d = m_udisks->diskByPath(m_disk.blockPath))
                m_udisks->smartUpdate(*d); // pending count should drop
            QMessageBox::information(this, tr("Repair Bad Sectors"), text);
        });
        m_thread->start();
    });
    m_udisks->openDevice(m_disk, true, false, true);
}
