// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "udisks.h"

#include <QDialog>

class QLabel;
class QLineEdit;
class QPushButton;
class QRadioButton;

// Secure Erase: asks the drive's firmware to erase itself. Shows the frozen state and
// how to get past it (sleep and wake), and needs the drive's name typed to start.
class SecureEraseDialog : public QDialog
{
    Q_OBJECT
public:
    SecureEraseDialog(UDisks *udisks, const QString &blockPath, int diskNumber, QWidget *parent = nullptr);
    UDisks::EraseMethod method() const;

private:
    void updateState();

    UDisks *m_udisks;
    QString m_blockPath;
    QLabel *m_intro;
    QRadioButton *m_first;
    QRadioButton *m_second;
    QLabel *m_frozen;
    QPushButton *m_sleep;
    QPushButton *m_checkAgain;
    QLabel *m_warning;
    QLineEdit *m_confirm;
    QPushButton *m_erase;
    bool m_ata = true;
};
