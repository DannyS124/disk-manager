// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// Add-ons are JSON manifests that add menu actions running a command (no shell) with
// placeholders like {device} or {mountpoint}. See docs/ADDONS.md.

#include "udisks.h"

#include <QString>
#include <QStringList>
#include <QVector>

struct AddonAction {
    QString label;
    QString icon;
    QString appliesTo = QStringLiteral("volume"); // volume, disk, free or any
    QStringList when; // conditions, all must hold
    QStringList command;
    bool terminal = false;
    QString confirm;
    bool systemDisks = false; // also offered on the system disk (read-only tools)
};

struct Addon {
    QString id;
    QString name;
    QString version;
    QString author;
    QString description;
    QString file; // path of addon.json
    QString error; // set if the manifest is broken; such add-ons have no actions
    bool enabled = true;
    QVector<AddonAction> actions;
};

class Addons
{
public:
    static QString userDir(); // ~/.local/share/diskforge/addons
    static QStringList searchDirs();
    static Addon parse(const QString &file);

    void load();
    const QVector<Addon> &all() const { return m_addons; }
    void setEnabled(const QString &id, bool enabled);

    // Add-on actions that fit the selection (disk always set; volume null for a disk or free space).
    QVector<QPair<const Addon *, const AddonAction *>> actionsFor(const Disk &disk, const Volume *volume, bool freeSpace) const;
    static bool applies(const AddonAction &action, const Disk &disk, const Volume *volume, bool freeSpace);
    // Fills in the placeholders; error says which one isn't available (e.g. not mounted).
    static QStringList expand(const QStringList &args, const Disk &disk, const Volume *volume, QString *error);

    // Trust is remembered per exact command, so editing an add-on asks again.
    static bool isTrusted(const Addon &addon, const AddonAction &action);
    static void trust(const Addon &addon, const AddonAction &action);
    static bool run(const AddonAction &action, const QStringList &argv, QString *error);

    // Copies a manifest (after parsing it) into the user's add-on folder.
    static bool install(const QString &file, QString *error);
    static bool remove(const Addon &addon, QString *error);

private:
    QVector<Addon> m_addons;
};
