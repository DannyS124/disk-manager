// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#include "addons.h"

#include "format.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QSaveFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QRegularExpression>
#include <QSettings>
#include <QStandardPaths>

namespace {

const QStringList kConditions = {
    QStringLiteral("mounted"), QStringLiteral("unmounted"), QStringLiteral("removable"), QStringLiteral("internal"),
    QStringLiteral("encrypted"), QStringLiteral("unlocked"), QStringLiteral("locked"), QStringLiteral("has-health"),
};
const QStringList kPlaceholders = {
    QStringLiteral("device"), QStringLiteral("disk"), QStringLiteral("mountpoint"), QStringLiteral("label"),
    QStringLiteral("uuid"), QStringLiteral("fstype"), QStringLiteral("size"), QStringLiteral("model"), QStringLiteral("home"),
};

QSettings settings()
{
    return QSettings(QStringLiteral("diskforge"), QStringLiteral("addons"));
}

QString firstMount(const Disk &disk, const Volume *volume)
{
    if (volume)
        return volume->mounts().value(0);
    for (const Volume &v : disk.volumes) {
        if (!v.mounts().isEmpty())
            return v.mounts().first();
    }
    return {};
}

bool conditionHolds(const QString &c, const Disk &disk, const Volume *v)
{
    const bool removable = disk.removable || disk.bus == QLatin1String("usb");
    if (c == QLatin1String("mounted"))
        return !firstMount(disk, v).isEmpty();
    if (c == QLatin1String("unmounted"))
        return firstMount(disk, v).isEmpty();
    if (c == QLatin1String("removable"))
        return removable;
    if (c == QLatin1String("internal"))
        return !removable && !disk.isLoop;
    if (c == QLatin1String("encrypted"))
        return v && v->encrypted;
    if (c == QLatin1String("unlocked"))
        return v && v->encrypted && !v->cleartextPath.isEmpty();
    if (c == QLatin1String("locked"))
        return v && v->encrypted && v->cleartextPath.isEmpty();
    if (c == QLatin1String("has-health"))
        return disk.health.state != Health::State::Unknown;
    if (c.startsWith(QLatin1String("filesystem:")))
        return v && c.mid(11).split(QLatin1Char('|')).contains(v->effectiveFsType());
    return false;
}

// Wraps the command so it runs in a terminal window the user can watch.
// In a Flatpak, add-on commands run on the host through flatpak-spawn. That needs the
// user's OK: flatpak override --user --talk-name=org.freedesktop.Flatpak <app id>
bool inFlatpak()
{
    static const bool yes = QFileInfo::exists(QStringLiteral("/.flatpak-info"));
    return yes;
}

bool programExists(const QString &name)
{
    if (!inFlatpak())
        return !QStandardPaths::findExecutable(name).isEmpty() || QFileInfo(name).isExecutable();
    const QString path = name.startsWith(QLatin1Char('/')) ? name : QStringLiteral("/usr/bin/") + name;
    return QProcess::execute(QStringLiteral("flatpak-spawn"), {QStringLiteral("--host"), QStringLiteral("test"), QStringLiteral("-x"), path}) == 0;
}

QStringList inTerminal(const QStringList &argv)
{
    const QString preferred = qEnvironmentVariable("TERMINAL");
    const QList<QPair<QString, QStringList>> terminals = {
        {QStringLiteral("konsole"), {QStringLiteral("--hold"), QStringLiteral("-e")}},
        {QStringLiteral("kitty"), {QStringLiteral("--hold")}},
        {QStringLiteral("alacritty"), {QStringLiteral("--hold"), QStringLiteral("-e")}},
        {QStringLiteral("foot"), {QStringLiteral("--hold")}},
        {QStringLiteral("gnome-terminal"), {QStringLiteral("--")}},
        {QStringLiteral("xterm"), {QStringLiteral("-hold"), QStringLiteral("-e")}},
    };
    for (const auto &[name, args] : terminals) {
        if (!preferred.isEmpty() && preferred.section(QLatin1Char('/'), -1) != name)
            continue;
        if (programExists(name))
            return QStringList{name} + args + argv;
    }
    if (!preferred.isEmpty())
        return QStringList{preferred, QStringLiteral("-e")} + argv;
    return {};
}

} // namespace

QString Addons::userDir()
{
    return QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) + QStringLiteral("/diskforge/addons");
}

QStringList Addons::searchDirs()
{
    return {userDir(), QStringLiteral("/usr/share/diskforge/addons")};
}

Addon Addons::parse(const QString &file)
{
    QFile f(file);
    if (!f.open(QIODevice::ReadOnly)) {
        Addon a;
        a.file = file;
        a.error = f.errorString();
        return a;
    }
    return parseData(f.read(kMaxDownload * 4), file);
}

Addon Addons::parseData(const QByteArray &json, const QString &file)
{
    Addon a;
    a.file = file;
    QJsonParseError parseError;
    const QJsonObject o = QJsonDocument::fromJson(json, &parseError).object();
    if (parseError.error != QJsonParseError::NoError) {
        a.error = QObject::tr("Not valid JSON: %1").arg(parseError.errorString());
        return a;
    }
    a.id = o.value(QStringLiteral("id")).toString();
    a.name = o.value(QStringLiteral("name")).toString(a.id);
    a.version = o.value(QStringLiteral("version")).toString();
    a.author = o.value(QStringLiteral("author")).toString();
    a.description = o.value(QStringLiteral("description")).toString();
    if (!QRegularExpression(QStringLiteral("^[a-z0-9][a-z0-9-]*$")).match(a.id).hasMatch()) {
        a.error = QObject::tr("\"id\" must be lowercase letters, digits and dashes");
        return a;
    }

    QVector<AddonAction> actions;
    for (const QJsonValue &value : o.value(QStringLiteral("actions")).toArray()) {
        const QJsonObject j = value.toObject();
        AddonAction act;
        act.label = j.value(QStringLiteral("label")).toString();
        act.icon = j.value(QStringLiteral("icon")).toString();
        act.appliesTo = j.value(QStringLiteral("applies_to")).toString(QStringLiteral("volume"));
        act.terminal = j.value(QStringLiteral("terminal")).toBool();
        act.confirm = j.value(QStringLiteral("confirm")).toString();
        act.systemDisks = j.value(QStringLiteral("system_disks")).toBool();
        for (const QJsonValue &w : j.value(QStringLiteral("when")).toArray())
            act.when << w.toString();
        for (const QJsonValue &c : j.value(QStringLiteral("command")).toArray())
            act.command << c.toString();

        if (act.label.isEmpty() || act.command.isEmpty()) {
            a.error = QObject::tr("Every action needs a \"label\" and a \"command\"");
            return a;
        }
        if (!QStringList{QStringLiteral("volume"), QStringLiteral("disk"), QStringLiteral("free"), QStringLiteral("any")}.contains(act.appliesTo)) {
            a.error = QObject::tr("\"applies_to\" must be volume, disk, free or any");
            return a;
        }
        for (const QString &w : act.when) {
            if (!kConditions.contains(w) && !w.startsWith(QLatin1String("filesystem:"))) {
                a.error = QObject::tr("Unknown condition \"%1\"").arg(w);
                return a;
            }
        }
        static const QRegularExpression placeholder(QStringLiteral("\\{([a-z]+)\\}"));
        for (const QString &arg : act.command) {
            for (auto it = placeholder.globalMatch(arg); it.hasNext();) {
                const QString name = it.next().captured(1);
                if (!kPlaceholders.contains(name)) {
                    a.error = QObject::tr("Unknown placeholder {%1}").arg(name);
                    return a;
                }
            }
        }
        actions << act;
    }
    if (actions.isEmpty()) {
        a.error = QObject::tr("No actions");
        return a;
    }
    a.actions = actions;
    return a;
}

void Addons::load()
{
    m_addons.clear();
    QStringList seen;
    const QSettings s = settings();
    for (const QString &dir : searchDirs()) {
        for (const QFileInfo &sub : QDir(dir).entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name)) {
            const QString file = sub.filePath() + QStringLiteral("/addon.json");
            if (!QFileInfo::exists(file))
                continue;
            Addon a = parse(file);
            if (!a.id.isEmpty() && seen.contains(a.id))
                continue; // the user's copy wins over a system-wide one
            seen << a.id;
            a.enabled = s.value(QStringLiteral("enabled/") + a.id, true).toBool();
            m_addons << a;
        }
    }
}

void Addons::setEnabled(const QString &id, bool enabled)
{
    QSettings s = settings();
    s.setValue(QStringLiteral("enabled/") + id, enabled);
    for (Addon &a : m_addons) {
        if (a.id == id)
            a.enabled = enabled;
    }
}

bool Addons::applies(const AddonAction &action, const Disk &disk, const Volume *volume, bool freeSpace)
{
    if (disk.isSystem && !action.systemDisks)
        return false;
    const QString &to = action.appliesTo;
    const bool target = to == QLatin1String("any") || (to == QLatin1String("volume") && volume)
        || (to == QLatin1String("disk") && !volume && !freeSpace) || (to == QLatin1String("free") && freeSpace);
    if (!target)
        return false;
    for (const QString &c : action.when) {
        if (!conditionHolds(c, disk, volume))
            return false;
    }
    return true;
}

QVector<QPair<const Addon *, const AddonAction *>> Addons::actionsFor(const Disk &disk, const Volume *volume, bool freeSpace) const
{
    QVector<QPair<const Addon *, const AddonAction *>> out;
    for (const Addon &a : m_addons) {
        if (!a.enabled || !a.error.isEmpty())
            continue;
        for (const AddonAction &act : a.actions) {
            if (applies(act, disk, volume, freeSpace))
                out.push_back({&a, &act});
        }
    }
    return out;
}

QStringList Addons::expand(const QStringList &args, const Disk &disk, const Volume *v, QString *error)
{
    auto value = [&](const QString &name) -> QString {
        if (name == QLatin1String("device"))
            return v ? v->device : disk.device;
        if (name == QLatin1String("disk"))
            return disk.device;
        if (name == QLatin1String("mountpoint"))
            return firstMount(disk, v);
        if (name == QLatin1String("label"))
            return v ? (!v->label.isEmpty() ? v->label : !v->partName.isEmpty() ? v->partName : shortDevice(v->device)) : disk.model;
        if (name == QLatin1String("uuid"))
            return v ? v->uuid : QString();
        if (name == QLatin1String("fstype"))
            return v ? v->effectiveFsType() : QString();
        if (name == QLatin1String("size"))
            return QString::number(v ? v->size : disk.size);
        if (name == QLatin1String("model"))
            return disk.model;
        if (name == QLatin1String("home"))
            return QDir::homePath();
        return {};
    };
    static const QRegularExpression placeholder(QStringLiteral("\\{([a-z]+)\\}"));
    QStringList out;
    for (const QString &arg : args) {
        QString result;
        qsizetype last = 0;
        for (auto it = placeholder.globalMatch(arg); it.hasNext();) {
            const QRegularExpressionMatch m = it.next();
            const QString v = value(m.captured(1));
            if (v.isEmpty()) {
                if (error)
                    *error = m.captured(1) == QLatin1String("mountpoint") ? QObject::tr("Mount it first")
                                                                          : QObject::tr("{%1} isn't available here").arg(m.captured(1));
                return {};
            }
            result += arg.mid(last, m.capturedStart() - last) + v;
            last = m.capturedEnd();
        }
        out << result + arg.mid(last);
    }
    return out;
}

namespace {
QString trustKey(const Addon &addon, const AddonAction &action)
{
    const QByteArray what = (addon.id + QLatin1Char('\n') + action.label + QLatin1Char('\n') + action.command.join(QChar(0))
                             + (action.terminal ? QStringLiteral("\nterminal") : QString())).toUtf8();
    return QStringLiteral("trusted/") + QString::fromLatin1(QCryptographicHash::hash(what, QCryptographicHash::Sha256).toHex());
}
} // namespace

bool Addons::isTrusted(const Addon &addon, const AddonAction &action)
{
    return settings().value(trustKey(addon, action)).toBool();
}

void Addons::trust(const Addon &addon, const AddonAction &action)
{
    QSettings s = settings();
    s.setValue(trustKey(addon, action), true);
}

bool Addons::run(const AddonAction &action, const QStringList &argv, QString *error)
{
    if (inFlatpak() && QProcess::execute(QStringLiteral("flatpak-spawn"), {QStringLiteral("--host"), QStringLiteral("true")}) != 0) {
        if (error)
            *error = QObject::tr("Add-ons run programs outside the Flatpak, which it isn't allowed to do yet. To allow it, run:\n"
                                 "flatpak override --user --talk-name=org.freedesktop.Flatpak " APP_ID);
        return false;
    }
    QStringList full = action.terminal ? inTerminal(argv) : argv;
    if (full.isEmpty()) {
        if (error)
            *error = QObject::tr("No terminal program found (install konsole, kitty or xterm)");
        return false;
    }
    if (!programExists(argv.first())) {
        if (error)
            *error = QObject::tr("%1 isn't installed").arg(argv.first());
        return false;
    }
    if (inFlatpak())
        full.prepend(QStringLiteral("--host"));
    const QString program = inFlatpak() ? QStringLiteral("flatpak-spawn") : full.takeFirst();
    if (!QProcess::startDetached(program, full)) {
        if (error)
            *error = QObject::tr("Couldn't start %1").arg(program);
        return false;
    }
    return true;
}

bool Addons::install(const QString &file, QString *error)
{
    const Addon a = parse(file);
    if (!a.error.isEmpty()) {
        if (error)
            *error = a.error;
        return false;
    }
    const QString dir = userDir() + QLatin1Char('/') + a.id;
    if (!QDir().mkpath(dir)) {
        if (error)
            *error = QObject::tr("Couldn't create %1").arg(dir);
        return false;
    }
    const QString target = dir + QStringLiteral("/addon.json");
    QFile::remove(target);
    if (!QFile::copy(file, target)) {
        if (error)
            *error = QObject::tr("Couldn't copy it to %1").arg(dir);
        return false;
    }
    return true;
}

QString Addons::catalogUrl()
{
    return QStringLiteral("https://raw.githubusercontent.com/DannyS124/diskforge-addons/main/catalog.json");
}

bool Addons::isPinnedUrl(const QString &url)
{
    static const QRegularExpression pinned(
        QStringLiteral("^https://raw\\.githubusercontent\\.com/DannyS124/diskforge-addons/[0-9a-f]{40}/[A-Za-z0-9._/-]+\\.json$"));
    // No "..": the path has to stay inside that commit of the repository.
    return pinned.match(url).hasMatch() && !url.contains(QLatin1String(".."));
}

QVector<CatalogEntry> Addons::parseCatalog(const QByteArray &json, QString *error)
{
    QVector<CatalogEntry> entries;
    if (json.size() > kMaxDownload) {
        *error = QObject::tr("The add-on list is too big");
        return entries;
    }
    QJsonParseError parseError;
    const QJsonObject o = QJsonDocument::fromJson(json, &parseError).object();
    if (parseError.error != QJsonParseError::NoError || o.value(QStringLiteral("format")).toString() != QLatin1String("diskforge-addon-catalog")) {
        *error = QObject::tr("The add-on list isn't in a format DiskForge knows");
        return entries;
    }
    if (o.value(QStringLiteral("version")).toInt() > 1) {
        *error = QObject::tr("The add-on list needs a newer DiskForge");
        return entries;
    }
    static const QRegularExpression id(QStringLiteral("^[a-z0-9][a-z0-9-]*$"));
    static const QRegularExpression sha(QStringLiteral("^[0-9a-f]{64}$"));
    int skipped = 0;
    for (const QJsonValue &v : o.value(QStringLiteral("addons")).toArray()) {
        const QJsonObject j = v.toObject();
        CatalogEntry e;
        e.id = j.value(QStringLiteral("id")).toString();
        e.name = j.value(QStringLiteral("name")).toString(e.id);
        e.version = j.value(QStringLiteral("version")).toString();
        e.author = j.value(QStringLiteral("author")).toString();
        e.description = j.value(QStringLiteral("description")).toString();
        e.url = j.value(QStringLiteral("url")).toString();
        e.sha256 = j.value(QStringLiteral("sha256")).toString();
        // Anything not pinned to a commit of the catalog repository is left out.
        if (!id.match(e.id).hasMatch() || !sha.match(e.sha256).hasMatch() || !isPinnedUrl(e.url)) {
            ++skipped;
            continue;
        }
        entries << e;
    }
    if (skipped)
        *error = QObject::tr("%n entry(s) in the list were left out because they didn't check out", nullptr, skipped);
    return entries;
}

bool Addons::installVerified(const QByteArray &data, const CatalogEntry &entry, QString *error)
{
    if (data.size() > kMaxDownload) {
        *error = QObject::tr("The download is bigger than an add-on can be");
        return false;
    }
    if (QString::fromLatin1(QCryptographicHash::hash(data, QCryptographicHash::Sha256).toHex()) != entry.sha256) {
        *error = QObject::tr("The download doesn't match the list's checksum, so it wasn't installed");
        return false;
    }
    const QString dir = userDir() + QLatin1Char('/') + entry.id;
    const Addon a = parseData(data, dir + QStringLiteral("/addon.json"));
    if (!a.error.isEmpty()) {
        *error = a.error;
        return false;
    }
    if (a.id != entry.id) {
        *error = QObject::tr("The add-on calls itself \"%1\", not \"%2\"").arg(a.id, entry.id);
        return false;
    }
    QSaveFile f(dir + QStringLiteral("/addon.json"));
    if (!QDir().mkpath(dir) || !f.open(QIODevice::WriteOnly) || f.write(data) != data.size() || !f.commit()) {
        *error = QObject::tr("Couldn't save it in %1").arg(dir);
        return false;
    }
    return true;
}

bool Addons::remove(const Addon &addon, QString *error)
{
    if (!addon.file.startsWith(userDir())) {
        if (error)
            *error = QObject::tr("This add-on was installed by a package; remove it with pacman");
        return false;
    }
    if (!QDir(QFileInfo(addon.file).path()).removeRecursively()) {
        if (error)
            *error = QObject::tr("Couldn't delete %1").arg(QFileInfo(addon.file).path());
        return false;
    }
    return true;
}
