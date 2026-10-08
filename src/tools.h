// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "udisks.h"

#include <QDialog>

class QCheckBox;
class QComboBox;
class QFormLayout;
class QLabel;
class QLineEdit;
class QProgressBar;
class QPushButton;
class QThread;
class QTreeWidget;

// "Encrypt with a passphrase" checkbox plus passphrase and confirm fields.
class EncryptionFields : public QWidget
{
    Q_OBJECT
public:
    explicit EncryptionFields(QWidget *parent = nullptr);
    QString passphrase() const; // empty when not encrypting
    bool isValid() const;

signals:
    void changed();

private:
    QCheckBox *m_box;
    QWidget *m_fields;
    QLineEdit *m_pass;
    QLineEdit *m_confirm;
    QLabel *m_note;
};

class ChangePassphraseDialog : public QDialog
{
    Q_OBJECT
public:
    explicit ChangePassphraseDialog(const QString &device, QWidget *parent = nullptr);
    QString oldPassphrase() const;
    QString newPassphrase() const;

private:
    QLineEdit *m_old;
    QLineEdit *m_new;
    QLineEdit *m_confirm;
};

// Overwrites a whole disk with zeros. Type-to-confirm, like a new partition table.
class WipeDialog : public QDialog
{
    Q_OBJECT
public:
    WipeDialog(const Disk &disk, int diskNumber, QWidget *parent = nullptr);
};

class HealthDialog : public QDialog
{
    Q_OBJECT
public:
    HealthDialog(UDisks *udisks, const QString &blockPath, QWidget *parent = nullptr);

private:
    void reload();

    UDisks *m_udisks;
    QString m_blockPath;
    QLabel *m_state;
    QLabel *m_explain;
    QFormLayout *m_form;
    QTreeWidget *m_attributes;
    QPushButton *m_selftest;
};

class BenchmarkDialog : public QDialog
{
    Q_OBJECT
public:
    BenchmarkDialog(UDisks *udisks, const Disk &disk, QWidget *parent = nullptr);
    ~BenchmarkDialog() override;

protected:
    void reject() override;

private:
    void start();
    void stop();

    UDisks *m_udisks;
    Disk m_disk;
    QString m_writeDir;
    QCheckBox *m_writeTest;
    QPushButton *m_start;
    QProgressBar *m_progress;
    QLabel *m_phase;
    QLabel *m_results;
    QThread *m_thread = nullptr;
    QObject *m_worker = nullptr;
    QMetaObject::Connection m_openConn;
};

class WriteImageDialog : public QDialog
{
    Q_OBJECT
public:
    WriteImageDialog(UDisks *udisks, const QString &preferredDisk, QWidget *parent = nullptr);
    ~WriteImageDialog() override;

protected:
    void reject() override;

private:
    void fillTargets(const QString &preferred);
    void updateState();
    void start();
    const Disk *target() const;

    UDisks *m_udisks;
    QLineEdit *m_image;
    QLabel *m_imageInfo;
    QComboBox *m_targets;
    QLineEdit *m_sha;
    QCheckBox *m_verify;
    QLabel *m_warning;
    QLineEdit *m_confirm;
    QPushButton *m_write;
    QProgressBar *m_progress;
    QLabel *m_phase;
    QThread *m_thread = nullptr;
    QObject *m_worker = nullptr;
    QMetaObject::Connection m_openConn;
    bool m_running = false;
};
