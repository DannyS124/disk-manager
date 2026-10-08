// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#include "erasedialog.h"

#include "dialogs.h"
#include "format.h"
#include "jobui.h"

#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusPendingCall>
#include <QDialogButtonBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QRadioButton>
#include <QVBoxLayout>

SecureEraseDialog::SecureEraseDialog(UDisks *udisks, const QString &blockPath, int diskNumber, QWidget *parent)
    : QDialog(parent)
    , m_udisks(udisks)
    , m_blockPath(blockPath)
    , m_intro(new QLabel)
    , m_first(new QRadioButton)
    , m_second(new QRadioButton)
    , m_frozen(new QLabel)
    , m_sleep(new QPushButton(tr("Sleep Now")))
    , m_checkAgain(new QPushButton(tr("Check Again")))
    , m_warning(new QLabel)
    , m_confirm(new QLineEdit)
{
    setWindowTitle(tr("Secure Erase Disk %1").arg(diskNumber));
    const Disk *d = udisks->diskByPath(blockPath);
    m_ata = !d || !d->nvmeNamespace;
    for (QLabel *l : {m_intro, m_frozen, m_warning}) {
        l->setWordWrap(true);
        l->setTextFormat(Qt::RichText);
    }
    m_first->setChecked(true);
    if (d) {
        m_intro->setText(tr("<p><b>%1</b></p><p>Tells the drive itself to erase everything on it, including spare areas that "
                            "Wipe Disk can't reach. It's the right way to clear an SSD before selling or giving it away.</p>")
                             .arg(diskTitle(*d).toHtmlEscaped()));
        if (m_ata) {
            m_first->setText(tr("Normal erase (%1)").arg(durationText(d->ataEraseMinutes * 60.0)));
            m_second->setText(tr("Enhanced erase (%1): also overwrites areas the drive set aside").arg(durationText(d->ataEnhancedEraseMinutes * 60.0)));
            m_second->setVisible(d->ataEnhancedEraseMinutes > 0);
            if (d->ataEraseMinutes <= 0 && d->ataEnhancedEraseMinutes > 0)
                m_second->setChecked(true);
            m_first->setVisible(d->ataEraseMinutes > 0);
        } else {
            m_first->setText(tr("Erase all data"));
            m_second->setText(tr("Crypto erase: instant, if the drive encrypts what it stores (many do)"));
        }
    }
    m_frozen->setText(redText(tr("The drive is frozen: its firmware refuses erase commands until the PC has been to sleep and woken "
                                 "up once. Put the PC to sleep, wake it, then check again.")));

    auto *frozenRow = new QHBoxLayout;
    frozenRow->addStretch();
    frozenRow->addWidget(m_sleep);
    frozenRow->addWidget(m_checkAgain);

    auto *layout = new QVBoxLayout(this);
    layout->addWidget(m_intro);
    layout->addWidget(m_first);
    layout->addWidget(m_second);
    layout->addWidget(m_frozen);
    layout->addLayout(frozenRow);
    layout->addWidget(m_warning);
    layout->addWidget(m_confirm);
    layout->addWidget(dialogButtons(this, tr("Erase"), &m_erase));

    connect(m_confirm, &QLineEdit::textChanged, this, &SecureEraseDialog::updateState);
    connect(m_sleep, &QPushButton::clicked, this, [] {
        QDBusMessage sleep = QDBusMessage::createMethodCall(QStringLiteral("org.freedesktop.login1"), QStringLiteral("/org/freedesktop/login1"),
                                                            QStringLiteral("org.freedesktop.login1.Manager"), QStringLiteral("Suspend"));
        sleep << true; // let polkit ask if it needs to
        QDBusConnection::systemBus().asyncCall(sleep);
    });
    // The frozen flag is read when the drive is probed; re-reading the drive refreshes it.
    connect(m_checkAgain, &QPushButton::clicked, this, [this] {
        m_udisks->rescan(m_blockPath);
        m_udisks->refresh();
        updateState();
    });
    connect(m_udisks, &UDisks::changed, this, &SecureEraseDialog::updateState);
    updateState();
    resize(560, sizeHint().height());
}

UDisks::EraseMethod SecureEraseDialog::method() const
{
    if (m_ata)
        return m_second->isChecked() ? UDisks::EraseMethod::AtaEnhanced : UDisks::EraseMethod::AtaNormal;
    return m_second->isChecked() ? UDisks::EraseMethod::NvmeCrypto : UDisks::EraseMethod::NvmeUserData;
}

void SecureEraseDialog::updateState()
{
    const Disk *d = m_udisks->diskByPath(m_blockPath);
    const bool frozen = d && m_ata && d->ataFrozen;
    m_frozen->setVisible(frozen);
    m_sleep->setVisible(frozen);
    m_checkAgain->setVisible(frozen);
    bool ok = d && !frozen && !d->isSystem;
    if (d && d->isSystem) {
        m_warning->setText(redText(tr("%1 runs this system, so it can't be erased while it's in use. Erase it from a live USB.")
                                       .arg(shortDevice(d->device))));
    } else if (d) {
        QString text = redText(tr("Everything on %1 will be erased, for good.").arg(diskTitle(*d)));
        if (!diskWarning(*d).isEmpty())
            text += QStringLiteral("<br>") + redText(diskWarning(*d));
        // UDisks sets the ATA password "xxxx" for the erase; an interrupted erase leaves it set.
        text += QStringLiteral("<br>")
              + (m_ata ? tr("Keep the PC on and the drive plugged in until it's done. If the erase is cut off, the drive stays locked "
                            "with the password <b>xxxx</b> until it's unlocked (see Help).")
                       : tr("Keep the PC on until it's done."));
        const QString name = shortDevice(d->device);
        text += QStringLiteral("<br>") + tr("Type <b>%1</b> to confirm:").arg(name);
        m_confirm->setPlaceholderText(name);
        ok = ok && m_confirm->text().trimmed() == name;
        m_warning->setText(text);
    }
    m_warning->setVisible(d && !frozen);
    m_confirm->setVisible(d && !frozen && !d->isSystem);
    m_erase->setEnabled(ok);
    fitHeight(this);
}
