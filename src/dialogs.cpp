// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#include "dialogs.h"

#include "format.h"

#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QRadioButton>
#include <QSlider>
#include <QSpinBox>
#include <QStandardItemModel>
#include <QVBoxLayout>

namespace {

constexpr quint64 kMiB = 1024 * 1024;

QLabel *warningLabel(const QString &html)
{
    auto *label = new QLabel(html);
    label->setWordWrap(true);
    label->setTextFormat(Qt::RichText);
    return label;
}

// Cancel stays the default button.
QDialogButtonBox *buttons(QDialog *dialog, const QString &actionText, QPushButton **action)
{
    auto *box = new QDialogButtonBox(QDialogButtonBox::Cancel);
    *action = box->addButton(actionText, QDialogButtonBox::AcceptRole);
    (*action)->setAutoDefault(false);
    box->button(QDialogButtonBox::Cancel)->setDefault(true);
    QObject::connect(box, &QDialogButtonBox::accepted, dialog, &QDialog::accept);
    QObject::connect(box, &QDialogButtonBox::rejected, dialog, &QDialog::reject);
    return box;
}

QString red(const QString &text)
{
    return QStringLiteral("<span style=\"color:#e05050\"><b>%1</b></span>").arg(text.toHtmlEscaped());
}

} // namespace

QString describeVolume(const Volume &v)
{
    QString text = QStringLiteral("%1, %2").arg(volumeTitle(v), formatSize(v.size));
    if (!v.fsType.isEmpty())
        text += QLatin1Char(' ') + v.fsType;
    return text;
}

QString diskWarning(const Disk &d)
{
    if (d.isVentoy)
        return QObject::tr("This is a Ventoy boot USB. Changing its partitions will stop it from booting.");
    return {};
}

FsPicker::FsPicker(const QVector<FsType> &filesystems, const Disk &disk, QComboBox *combo, QLineEdit *label)
    : m_filesystems(filesystems)
    , m_combo(combo)
    , m_label(label)
{
    // exFAT/FAT for USB sticks, ext4 for internal disks
    const bool portable = disk.removable || disk.bus == QLatin1String("usb");
    int preferred = -1;
    auto *model = qobject_cast<QStandardItemModel *>(combo->model());
    for (int i = 0; i < m_filesystems.size(); ++i) {
        const FsType &fs = m_filesystems[i];
        QString text = QStringLiteral("%1 — %2").arg(fs.name, fs.hint);
        if (!fs.available)
            text += QObject::tr(" (install %1)").arg(fs.package);
        combo->addItem(text, fs.id);
        if (!fs.available && model)
            model->item(i)->setEnabled(false);
        if (fs.available && preferred < 0) {
            if (portable ? (fs.id == QLatin1String("exfat") || fs.id == QLatin1String("vfat")) : fs.id == QLatin1String("ext4"))
                preferred = i;
        }
    }
    combo->setCurrentIndex(std::max(preferred, 0));
    QObject::connect(combo, &QComboBox::currentIndexChanged, combo, [this] { updateLabelLimit(); });
    updateLabelLimit();
}

QString FsPicker::fsType() const
{
    return m_combo->currentData().toString();
}

void FsPicker::updateLabelLimit()
{
    const int i = m_combo->currentIndex();
    if (i >= 0 && i < m_filesystems.size())
        m_label->setMaxLength(m_filesystems[i].maxLabel);
}

FormatDialog::FormatDialog(const Disk &disk, const Volume &volume, const QVector<FsType> &filesystems, QWidget *parent)
    : QDialog(parent)
    , m_fs(new QComboBox)
    , m_label(new QLineEdit(volume.label))
    , m_picker(filesystems, disk, m_fs, m_label)
{
    setWindowTitle(tr("Format %1").arg(shortDevice(volume.device)));

    auto *form = new QFormLayout;
    form->addRow(tr("Volume label:"), m_label);
    form->addRow(tr("File system:"), m_fs);

    QString warning = red(tr("Formatting erases everything on %1.").arg(describeVolume(volume)));
    if (!volume.mountPoints.isEmpty())
        warning += QStringLiteral("<br>") + tr("It will be unmounted first.").toHtmlEscaped();
    if (!diskWarning(disk).isEmpty())
        warning += QStringLiteral("<br>") + red(diskWarning(disk));

    QPushButton *action;
    auto *layout = new QVBoxLayout(this);
    layout->addLayout(form);
    layout->addWidget(warningLabel(warning));
    layout->addWidget(buttons(this, tr("Format"), &action));
    resize(520, sizeHint().height());
}

QString FormatDialog::fsType() const { return m_picker.fsType(); }
QString FormatDialog::label() const { return m_label->text().trimmed(); }

NewPartitionDialog::NewPartitionDialog(const Disk &disk, const Span &free, const QVector<FsType> &filesystems, QWidget *parent)
    : QDialog(parent)
    , m_size(new QSpinBox)
    , m_fs(new QComboBox)
    , m_label(new QLineEdit)
    , m_picker(filesystems, disk, m_fs, m_label)
{
    setWindowTitle(tr("New Partition on %1").arg(shortDevice(disk.device)));

    // leave room for alignment and the GPT backup header
    const int maxMiB = int(std::max<qint64>(1, qint64(free.size / kMiB) - 2));
    m_size->setRange(1, maxMiB);
    m_size->setValue(maxMiB);
    m_size->setSuffix(tr(" MB"));
    m_size->setGroupSeparatorShown(true);

    auto *form = new QFormLayout;
    form->addRow(tr("Free space:"), new QLabel(formatSize(free.size)));
    form->addRow(tr("Size:"), m_size);
    form->addRow(tr("Volume label:"), m_label);
    form->addRow(tr("File system:"), m_fs);

    QPushButton *action;
    auto *layout = new QVBoxLayout(this);
    layout->addLayout(form);
    if (!diskWarning(disk).isEmpty())
        layout->addWidget(warningLabel(red(diskWarning(disk))));
    layout->addWidget(buttons(this, tr("Create"), &action));
    resize(520, sizeHint().height());
}

quint64 NewPartitionDialog::sizeBytes() const { return quint64(m_size->value()) * kMiB; }
QString NewPartitionDialog::fsType() const { return m_picker.fsType(); }
QString NewPartitionDialog::label() const { return m_label->text().trimmed(); }

PartitionTableDialog::PartitionTableDialog(const Disk &disk, int diskNumber, QWidget *parent)
    : QDialog(parent)
    , m_gpt(new QRadioButton(tr("GPT (GUID Partition Table): recommended")))
{
    setWindowTitle(tr("New Partition Table on Disk %1").arg(diskNumber));
    auto *mbr = new QRadioButton(tr("MBR (Master Boot Record): for very old computers and devices"));
    m_gpt->setChecked(disk.tableType != QLatin1String("dos"));
    mbr->setChecked(disk.tableType == QLatin1String("dos"));

    auto *layout = new QVBoxLayout(this);
    layout->addWidget(new QLabel(tr("Disk %1: %2, %3").arg(diskNumber).arg(disk.model, formatSize(disk.size))));
    layout->addWidget(m_gpt);
    layout->addWidget(mbr);

    QPushButton *action;
    QDialogButtonBox *box = buttons(this, disk.volumes.isEmpty() ? tr("Initialize") : tr("Erase Disk"), &action);
    m_ok = action;

    if (!disk.volumes.isEmpty()) {
        QStringList lost;
        for (const Volume &v : disk.volumes) {
            if (!v.isContainer)
                lost << QStringLiteral("• ") + describeVolume(v).toHtmlEscaped();
        }
        QString warning = red(tr("This erases every partition on the disk:")) + QStringLiteral("<br>") + lost.join(QStringLiteral("<br>"));
        if (!diskWarning(disk).isEmpty())
            warning += QStringLiteral("<br>") + red(diskWarning(disk));
        layout->addWidget(warningLabel(warning));

        const QString name = shortDevice(disk.device);
        layout->addWidget(new QLabel(tr("Type <b>%1</b> to confirm:").arg(name)));
        m_confirm = new QLineEdit;
        m_confirm->setPlaceholderText(name);
        layout->addWidget(m_confirm);
        m_ok->setEnabled(false);
        connect(m_confirm, &QLineEdit::textChanged, this, [this, name](const QString &text) { m_ok->setEnabled(text.trimmed() == name); });
    }
    layout->addWidget(box);
    resize(520, sizeHint().height());
}

QString PartitionTableDialog::tableType() const
{
    return m_gpt->isChecked() ? QStringLiteral("gpt") : QStringLiteral("dos");
}

ResizeDialog::ResizeDialog(const Disk &disk, const Volume &volume, const ResizeLimits &limits,
                           bool remountShrink, bool remountGrow, QWidget *parent)
    : QDialog(parent)
    , m_volume(volume)
    , m_remountShrink(remountShrink)
    , m_remountGrow(remountGrow)
    , m_size(new QSpinBox)
    , m_slider(new QSlider(Qt::Horizontal))
    , m_effect(new QLabel)
{
    setWindowTitle(tr("Resize %1").arg(volumeTitle(volume)));
    const int current = int(volume.size / kMiB);
    const int minimum = int(limits.minSize / kMiB);
    const int maximum = int(limits.maxSize / kMiB);

    m_size->setRange(minimum, maximum);
    m_size->setValue(current);
    m_size->setSuffix(tr(" MB"));
    m_size->setGroupSeparatorShown(true);
    m_slider->setRange(minimum, maximum);
    m_slider->setValue(current);
    connect(m_slider, &QSlider::valueChanged, m_size, &QSpinBox::setValue);
    connect(m_size, &QSpinBox::valueChanged, this, [this](int value) {
        m_slider->setValue(value);
        update();
    });

    auto mb = [](quint64 bytes) { return tr("%L1 MB").arg(bytes / kMiB); };
    auto *form = new QFormLayout;
    form->addRow(tr("Current size:"), new QLabel(mb(volume.size)));
    form->addRow(tr("Space in use:"), new QLabel(limits.used ? mb(limits.used) : tr("Unknown while not mounted")));
    form->addRow(tr("Can shrink to:"), new QLabel(minimum < current ? mb(limits.minSize) : tr("Can't shrink")));
    form->addRow(tr("Can grow to:"), new QLabel(maximum > current ? mb(limits.maxSize) : tr("No unallocated space right after it")));
    form->addRow(tr("New size:"), m_size);
    form->addRow(QString(), m_slider);

    m_effect->setWordWrap(true);
    QString note = tr("<b>Back up anything important first.</b> Resizing rewrites the file system.");
    if (!limits.used && minimum < current)
        note += QStringLiteral("<br>") + tr("If you shrink below what's stored on it, nothing is changed.");
    if (!diskWarning(disk).isEmpty())
        note += QStringLiteral("<br>") + red(diskWarning(disk));

    QPushButton *action;
    auto *layout = new QVBoxLayout(this);
    layout->addLayout(form);
    layout->addWidget(m_effect);
    layout->addWidget(warningLabel(note));
    layout->addWidget(buttons(this, tr("Resize"), &action));
    m_ok = action;
    update();
    resize(520, sizeHint().height());
}

quint64 ResizeDialog::newSize() const
{
    return quint64(m_size->value()) * kMiB;
}

void ResizeDialog::update()
{
    const qint64 delta = qint64(newSize()) - qint64(m_volume.size / kMiB * kMiB);
    m_ok->setEnabled(delta != 0);
    QString text;
    if (delta < 0)
        text = tr("Shrinking frees %1 of unallocated space after it.").arg(formatSize(quint64(-delta)));
    else if (delta > 0)
        text = tr("Growing uses %1 of the unallocated space after it.").arg(formatSize(quint64(delta)));
    const bool remount = delta < 0 ? m_remountShrink : delta > 0 && m_remountGrow;
    if (remount)
        text += QLatin1Char(' ') + (m_volume.mountPoints.isEmpty() ? tr("It will be mounted while it's resized.")
                                                                   : tr("It will be unmounted first."));
    m_effect->setText(text);
}
