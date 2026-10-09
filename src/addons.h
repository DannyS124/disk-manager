// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// Add-ons are JSON manifests that add menu actions running a command (no shell) with
// placeholders like {device} or {mountpoint}. See docs/ADDONS.md.

#include "udisks.h"

#include <QByteArray>
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
    bool systemDisks = false; // also offered on the system disk, if it's look-only too
    bool lookOnly = false; // runs in a read-only sandbox: no changing files, no network
    int index = 0; // its place in the add-on's list of actions
};

// What an action's command could do, judging by the programs in it. Look-only actions
// run in the sandbox, so none of this applies to them.
struct AddonRisks {
    QString admin;    // pkexec, sudo, ...: runs as root
    QString anything; // a shell or script interpreter: can do whatever you can
    QString network;  // curl, ssh, ...: can send things off this PC or fetch them
    QString deletes;  // rm, dd, ...: can delete or overwrite files
    bool any() const { return !admin.isEmpty() || !anything.isEmpty() || !network.isEmpty() || !deletes.isEmpty(); }
    // Admin power or a shell: never run without asking first.
    bool alwaysAsk() const { return !admin.isEmpty() || !anything.isEmpty(); }
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
    QByteArray fileHash; // SHA-256 of addon.json; trust is tied to it
    bool systemWide = false; // installed by a package, in /usr/share/diskforge/addons
    // In your add-on folder, but not put there by DiskForge (or changed since).
    bool outside = false;
    QVector<AddonAction> actions;
};

// One add-on offered by the online catalog. The add-on itself is only ever fetched from a
// URL pinned to a commit of the catalog repository, and must match its checksum.
struct CatalogEntry {
    QString id;
    QString name;
    QString version;
    QString author;
    QString description;
    QString url;
    QString sha256;
};

class Addons
{
public:
    static constexpr qint64 kMaxDownload = 64 * 1024; // the catalog, its signature and each add-on
    static QString catalogUrl();
    static QString catalogSignatureUrl();
    // The keys that may sign the catalog ("ssh-ed25519 AAAA..."), and the signature namespace.
    static QStringList catalogKeys();
    static constexpr const char *kCatalogNamespace = "diskforge-addons";
    // https://raw.githubusercontent.com/DannyS124/diskforge-addons/<40-hex commit>/...
    static bool isPinnedUrl(const QString &url);
    // Checks the signature first; the list is only read if it's signed by one of `keys`.
    static QVector<CatalogEntry> parseSignedCatalog(const QByteArray &json, const QByteArray &signature,
                                                    const QStringList &keys, QString *error);
    static QVector<CatalogEntry> parseCatalog(const QByteArray &json, QString *error);
    // Checks size, checksum and id, then saves it into the user's add-on folder.
    static bool installVerified(const QByteArray &data, const CatalogEntry &entry, QString *error);

    static QString userDir(); // ~/.local/share/diskforge/addons
    static QString systemDir(); // /usr/share/diskforge/addons
    static QStringList searchDirs();
    static Addon parse(const QString &file);
    static Addon parseData(const QByteArray &json, const QString &file);

    void load();
    const QVector<Addon> &all() const { return m_addons; }
    void setEnabled(const QString &id, bool enabled);
    // "I put it there myself": clears the added-outside flag for this exact file.
    void accept(const QString &id);

    // Add-on actions that fit the selection (disk always set; volume null for a disk or free space).
    QVector<QPair<const Addon *, const AddonAction *>> actionsFor(const Disk &disk, const Volume *volume, bool freeSpace) const;
    static bool applies(const AddonAction &action, const Disk &disk, const Volume *volume, bool freeSpace);
    // Fills in the placeholders. Values that come from the drive can't start an argument
    // with "-", be "." or "..", hold "/" or hidden characters, so a drive's name can't turn
    // into an option or a path somewhere else. error says why it can't run.
    static QStringList expand(const QStringList &args, const Disk &disk, const Volume *volume, QString *error);
    // The same for the "confirm" question, which is only shown, so "-" is fine there.
    static QString expandText(const QString &text, const Disk &disk, const Volume *volume, QString *error);
    static AddonRisks risks(const AddonAction &action);

    // Trust is remembered per add-on file, so any change to the file asks again.
    static bool isTrusted(const Addon &addon, const AddonAction &action);
    static void trust(const Addon &addon, const AddonAction &action);
    // The command as it really runs: in the sandbox for look-only actions, in a terminal
    // if asked for. Empty with error set if it can't run.
    static QStringList commandLine(const AddonAction &action, const QStringList &argv, QString *error);
    static QStringList sandboxed(const QStringList &argv);
    static bool run(const AddonAction &action, const QStringList &argv, QString *error);

    // Saves exactly these bytes (after parsing them) into the user's add-on folder and notes
    // their checksum, so the add-on doesn't show up as added from outside.
    static bool install(const QByteArray &data, QString *error);
    static bool remove(const Addon &addon, QString *error);

private:
    QVector<Addon> m_addons;
};
