// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

// Drives the real window, answering its dialogs the way a user would:
// - Check for Errors finds damage, the disk list refreshes while "Repair them now?" is
//   open, and the repair still has to reach the right partition.
// - The right-click menus of the system disk offer nothing that writes to it.
// - Clone Drive, with the disk list refreshed while its dialog is open.
// - Back Up a partition, damage it, Restore it, and it's back exactly.
// sudo QT_QPA_PLATFORM=offscreen build/diskforge-uitest

#include "../src/addonoutput.h"
#include "../src/addonform.h"
#include "../src/addonmaker.h"
#include "../src/diskmap.h"
#include "../src/noticebar.h"
#include "../src/typedialog.h"
#include "slowdisk.h"
#include "../src/addons.h"
#include "../src/addonsdialog.h"
#include "../src/copydialogs.h"
#include "../src/format.h"
#include "../src/mainwindow.h"
#include "../src/theme.h"
#include "../src/udisks.h"

#include <QAbstractButton>
#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QCryptographicHash>
#include <QFile>
#include <QStyle>
#include <QFileInfo>
#include <QDir>
#include <QKeySequenceEdit>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QToolBar>
#include <QInputDialog>
#include <QMouseEvent>
#include <QStatusBar>
#include <QShortcut>
#include <QMenuBar>
#include <QPushButton>
#include <QElapsedTimer>
#include <QMessageBox>
#include <QProcess>
#include <QTableWidget>
#include <QTemporaryDir>
#include <QTextStream>
#include <QThread>
#include <QTimer>

#include <memory>

#include <unistd.h>

namespace {

QTextStream out(stdout);
int failures = 0;

void report(bool ok, const QString &step, const QString &detail = {})
{
    out << (ok ? "PASS  " : "FAIL  ") << step;
    if (!detail.isEmpty())
        out << "  (" << detail << ")";
    out << Qt::endl;
    if (!ok)
        ++failures;
}

int sh(const QString &program, const QStringList &args, QString *output = nullptr)
{
    QProcess p;
    p.start(program, args);
    p.waitForFinished(60000);
    if (output)
        *output = QString::fromLocal8Bit(p.readAllStandardOutput() + p.readAllStandardError()).trimmed();
    return p.exitCode();
}

void waitUntil(const std::function<bool()> &done, int ms)
{
    QElapsedTimer t;
    t.start();
    while (!done() && t.elapsed() < ms) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 100);
        QThread::msleep(20);
    }
}


QAction *findAction(MainWindow &window, const QString &startsWith)
{
    for (QAction *a : window.findChildren<QAction *>()) {
        // Pinned add-on actions carry their add-on id and label; they're never DiskForge's own.
        if (a->data().toStringList().size() == 2)
            continue;
        if (a->text().remove(QLatin1Char('&')).startsWith(startsWith))
            return a;
    }
    return nullptr;
}

QPushButton *findButton(QWidget *dialog, const QString &text)
{
    for (QPushButton *b : dialog->findChildren<QPushButton *>()) {
        if (b->text().remove(QLatin1Char('&')) == text)
            return b;
    }
    return nullptr;
}

// Every 100 ms, hands whatever modal dialog is open to `answer`. Message boxes nobody
// expects are recorded and closed.
struct Answerer {
    QTimer timer;
    QStringList boxes;
    std::function<bool(QWidget *)> answer;
    Answerer()
    {
        QObject::connect(&timer, &QTimer::timeout, [this] {
            QWidget *modal = QApplication::activeModalWidget();
            if (!modal || (answer && answer(modal)))
                return;
            if (auto *box = qobject_cast<QMessageBox *>(modal)) {
                boxes << box->text();
                box->close();
            }
        });
        timer.start(100);
    }
};

QString partitionHash(const QString &device)
{
    QFile f(device);
    if (!f.open(QIODevice::ReadOnly))
        return {};
    QCryptographicHash hash(QCryptographicHash::Sha256);
    hash.addData(&f);
    return QString::fromLatin1(hash.result().toHex());
}

QString layout(const QString &device)
{
    QString dump;
    sh(QStringLiteral("sfdisk"), {QStringLiteral("--dump"), device}, &dump);
    QStringList lines;
    for (const QString &line : dump.split(QLatin1Char('\n'))) {
        if (line.startsWith(QLatin1String("/dev/")))
            lines << line.section(QLatin1Char(':'), 1);
    }
    return lines.join(QLatin1Char('\n'));
}

QString attach(const QString &image, const QString &script, qint64 megabytes)
{
    sh(QStringLiteral("truncate"), {QStringLiteral("-s"), QStringLiteral("%1M").arg(megabytes), image});
    if (!script.isEmpty()) {
        QProcess sfdisk;
        sfdisk.start(QStringLiteral("sfdisk"), {QStringLiteral("-q"), image});
        sfdisk.waitForStarted();
        sfdisk.write(script.toLatin1());
        sfdisk.closeWriteChannel();
        sfdisk.waitForFinished();
    }
    QString loop;
    sh(QStringLiteral("losetup"), {QStringLiteral("-fP"), QStringLiteral("--show"), image}, &loop);
    QThread::msleep(500);
    return loop;
}

// Nothing in the system disk's right-click menus may write to it.
void systemMenus(MainWindow &window, UDisks &udisks)
{
    static const QStringList writes = {QStringLiteral("Format"), QStringLiteral("Resize"), QStringLiteral("Delete Partition"),
                                       QStringLiteral("Change Label"), QStringLiteral("Mount at Startup"), QStringLiteral("Change Passphrase"),
                                       QStringLiteral("New Partition Table"), QStringLiteral("Wipe Disk"), QStringLiteral("Secure Erase"),
                                       QStringLiteral("Restore"), QStringLiteral("Clone Drive"), QStringLiteral("Rescue Copy"),
                                       QStringLiteral("Unmount"), QStringLiteral("Safely Remove")};
    QStringList devices;
    for (const Disk &d : udisks.disks()) {
        if (!d.isSystem)
            continue;
        devices << d.device;
        for (const Volume &v : d.volumes)
            devices << v.device;
    }
    report(!devices.isEmpty(), QStringLiteral("find the system disk"));
    QStringList offered;
    for (const QString &device : devices) {
        if (!window.selectDevice(device))
            continue;
        QMenu menu;
        window.buildContextMenu(&menu);
        for (QAction *a : menu.actions()) {
            const QString text = a->text().remove(QLatin1Char('&'));
            for (const QString &w : writes) {
                if (!a->isSeparator() && text.startsWith(w))
                    offered << QStringLiteral("%1: %2").arg(shortDevice(device), text);
            }
        }
    }
    report(offered.isEmpty(), QStringLiteral("the system disk's menus offer nothing that writes to it"), offered.join(QStringLiteral(", ")));
}

void cloneThroughWindow(MainWindow &window, UDisks &udisks, const QTemporaryDir &dir)
{
    const QString source = attach(dir.filePath(QStringLiteral("clone-source.img")), QStringLiteral("label: gpt\nsize=48MiB, type=L\nsize=24MiB, type=L\n"), 96);
    const QString target = attach(dir.filePath(QStringLiteral("clone-target.img")), QString(), 160);
    sh(QStringLiteral("mkfs.ext4"), {QStringLiteral("-q"), QStringLiteral("-L"), QStringLiteral("CLONEME"), source + QStringLiteral("p1")});
    sh(QStringLiteral("mkfs.vfat"), {QStringLiteral("-n"), QStringLiteral("CLONEFAT"), source + QStringLiteral("p2")});
    QString targetPath;
    waitUntil([&] {
        udisks.refresh();
        for (const Disk &d : udisks.disks()) {
            if (d.device == target)
                targetPath = d.blockPath;
        }
        return !targetPath.isEmpty() && window.selectDevice(source);
    }, 15000);
    report(window.selectDevice(source), QStringLiteral("select the drive to clone"), source);

    Answerer answerer;
    bool started = false, refreshed = false;
    answerer.answer = [&](QWidget *modal) {
        auto *dialog = qobject_cast<CloneDialog *>(modal);
        if (!dialog || started)
            return dialog != nullptr;
        auto *targets = dialog->findChild<QComboBox *>();
        const int index = targets ? targets->findData(targetPath) : -1;
        if (index < 0)
            return true; // not listed yet; try again next tick
        targets->setCurrentIndex(index);
        for (QLineEdit *e : dialog->findChildren<QLineEdit *>()) {
            if (e->placeholderText() == shortDevice(target))
                e->setText(shortDevice(target));
        }
        // Keep the layout identical so it can be compared (growing is tested in --clone).
        for (QCheckBox *box : dialog->findChildren<QCheckBox *>()) {
            if (box->text().startsWith(QLatin1String("Let the last partition")))
                box->setChecked(false);
        }
        // The list gets replaced while the dialog is open; the dialog has to cope.
        for (int i = 0; i < 3; ++i)
            udisks.refresh();
        refreshed = true;
        if (QPushButton *clone = findButton(dialog, QStringLiteral("Clone")); clone && clone->isEnabled()) {
            started = true;
            clone->click();
        }
        return true;
    };
    if (QAction *clone = findAction(window, QStringLiteral("Clone Drive")))
        clone->trigger(); // returns once the dialog has closed
    else
        report(false, QStringLiteral("Clone Drive is in the menus"));
    report(started && refreshed, QStringLiteral("start a clone after the disk list was replaced"));
    const bool copied = std::any_of(answerer.boxes.cbegin(), answerer.boxes.cend(), [](const QString &b) { return b.startsWith(QLatin1String("Copied")); });
    report(copied, QStringLiteral("the clone finished"), answerer.boxes.join(QStringLiteral(" | ")));
    sh(QStringLiteral("partprobe"), {target});
    const QString was = layout(source), now = layout(target);
    report(!was.isEmpty() && now == was && partitionHash(target + QStringLiteral("p1")) == partitionHash(source + QStringLiteral("p1")),
           QStringLiteral("the copy has the same partitions and data"), now == was ? QString() : was + QStringLiteral(" vs ") + now);
    sh(QStringLiteral("losetup"), {QStringLiteral("-d"), source});
    sh(QStringLiteral("losetup"), {QStringLiteral("-d"), target});
}

void backupAndRestore(MainWindow &window, UDisks &udisks, const QTemporaryDir &dir)
{
    const QString loop = attach(dir.filePath(QStringLiteral("backup-me.img")), QStringLiteral("label: gpt\nsize=64MiB, type=L\n"), 96);
    const QString part = loop + QStringLiteral("p1");
    sh(QStringLiteral("mkfs.ext4"), {QStringLiteral("-q"), QStringLiteral("-L"), QStringLiteral("KEEPME"), part});
    const QString before = partitionHash(part);
    waitUntil([&] {
        udisks.refresh();
        return window.selectDevice(part);
    }, 15000);
    const QString file = dir.filePath(QStringLiteral("backups/keepme.img.zst"));

    {
        Answerer answerer;
        bool started = false;
        answerer.answer = [&](QWidget *modal) {
            auto *dialog = qobject_cast<BackupDialog *>(modal);
            if (!dialog || started)
                return dialog != nullptr;
            if (auto *edit = dialog->findChild<QLineEdit *>())
                edit->setText(file);
            if (QPushButton *go = findButton(dialog, QStringLiteral("Back Up")); go && go->isEnabled()) {
                started = true;
                go->click();
            }
            return true;
        };
        if (QAction *backup = findAction(window, QStringLiteral("Back Up Partition")))
            backup->trigger();
        report(started && QFile::exists(file) && QFile::exists(file + QStringLiteral(".json")), QStringLiteral("back up a partition through the window"),
               answerer.boxes.join(QStringLiteral(" | ")));
    }

    // Damage it, then put the backup back.
    sh(QStringLiteral("mkfs.ext4"), {QStringLiteral("-q"), QStringLiteral("-F"), QStringLiteral("-L"), QStringLiteral("OOPS"), part});
    report(partitionHash(part) != before, QStringLiteral("the partition was changed"));
    waitUntil([&] {
        udisks.refresh();
        return window.selectDevice(part);
    }, 15000);
    {
        Answerer answerer;
        bool started = false;
        answerer.answer = [&](QWidget *modal) {
            auto *dialog = qobject_cast<RestoreDialog *>(modal);
            if (!dialog || started)
                return dialog != nullptr;
            for (QLineEdit *e : dialog->findChildren<QLineEdit *>()) {
                if (e->placeholderText() == shortDevice(part))
                    e->setText(shortDevice(part));
                else
                    e->setText(file);
            }
            for (int i = 0; i < 3; ++i)
                udisks.refresh();
            if (QPushButton *go = findButton(dialog, QStringLiteral("Restore")); go && go->isEnabled()) {
                started = true;
                go->click();
            }
            return true;
        };
        if (QAction *restore = findAction(window, QStringLiteral("Restore Partition Backup")))
            restore->trigger();
        report(started, QStringLiteral("restore it through the window"), answerer.boxes.join(QStringLiteral(" | ")));
    }
    report(partitionHash(part) == before, QStringLiteral("the partition is exactly as it was"));
    sh(QStringLiteral("losetup"), {QStringLiteral("-d"), loop});
}

// The add-on output window: real output and exit code, Stop ends everything the command
// started, and the command gets no open files besides its output.
void outputWindow()
{
    auto run = [](const QString &script) {
        auto *window = new AddonOutputWindow(QStringLiteral("test"), {QStringLiteral("sh"), QStringLiteral("-c"), script});
        window->setAttribute(Qt::WA_DeleteOnClose, false);
        window->show();
        return window;
    };
    {
        std::unique_ptr<AddonOutputWindow> w(run(QStringLiteral("printf 'one\\ntwo\\n'; ls /proc/$$/fd | tr '\\n' ' '; exit 3")));
        waitUntil([&] { return !w->isRunning(); }, 5000);
        QApplication::processEvents();
        QThread::msleep(150);
        QApplication::processEvents();
        const QString shown = w->text();
        report(shown.startsWith(QLatin1String("one\ntwo\n")) && w->findChild<QLabel *>()->text().contains(QLatin1String("3")),
               QStringLiteral("the output window shows what the command printed, and its exit code"), shown.simplified());
        report(shown.contains(QLatin1String("0 1 2")) && !shown.contains(QLatin1String("0 1 2 3")),
               QStringLiteral("the command only gets its own input and output"), shown.section(QLatin1Char('\n'), 2).simplified());
    }
    {
        // A background child too: Stop has to end the whole group.
        std::unique_ptr<AddonOutputWindow> w(run(QStringLiteral("sleep 60 & echo started; sleep 60")));
        waitUntil([&] {
            QThread::msleep(150);
            QApplication::processEvents();
            return w->text().contains(QLatin1String("started"));
        }, 5000);
        QProcess pids;
        QElapsedTimer timer;
        timer.start();
        w->stop();
        waitUntil([&] { return !w->isRunning(); }, 6000);
        QThread::msleep(300);
        pids.start(QStringLiteral("pgrep"), {QStringLiteral("-f"), QStringLiteral("^sleep 60$")});
        pids.waitForFinished();
        report(!w->isRunning() && timer.elapsed() < 5000 && pids.readAllStandardOutput().trimmed().isEmpty(),
               QStringLiteral("Stop ends the command and everything it started"), QString::number(timer.elapsed()) + QStringLiteral(" ms"));
    }
}

// Add-ons in the Tools menu, the right-click menu, the toolbar and on shortcuts, on the
// system disk (which every PC has, so this runs as the user).
void addonMenus()
{
    const QByteArray manifest = R"({"id":"uitest","name":"UI Test","settings":[{"id":"dest","type":"folder","label":"Folder","default":"{home}/Backups"}],"actions":[
        {"label":"Look Around","applies_to":"any","look_only":true,"system_disks":true,"command":["true"]},
        {"label":"Only USB","applies_to":"any","when":["removable"],"command":["true"]},
        {"label":"Format Everything","applies_to":"any","command":["true"]}]})";
    QString error;
    report(Addons::install(manifest, &error), QStringLiteral("a test add-on installs"), error);

    UDisks udisks;
    udisks.setInteractive(false);
    MainWindow window(&udisks);
    window.show();
    QString systemPart;
    for (const Disk &d : udisks.disks()) {
        if (d.isSystem && !d.volumes.isEmpty())
            systemPart = d.volumes.last().device;
    }
    if (systemPart.isEmpty() || !window.selectDevice(systemPart)) {
        out << "SKIP  no system disk to try the add-on menus on" << Qt::endl;
        return;
    }
    auto text = [](const QAction *a) { return a->text().remove(QLatin1Char('&')).section(QLatin1Char('\t'), 0, 0); };

    QMenu *tools = nullptr;
    for (QAction *a : window.menuBar()->actions()) {
        if (a->text().remove(QLatin1Char('&')) == QLatin1String("Tools"))
            tools = a->menu();
    }
    Q_EMIT tools->aboutToShow();
    QMap<QString, QAction *> listed;
    for (QAction *a : tools->actions())
        listed.insert(text(a), a);
    report(listed.value(QStringLiteral("Look Around")) && listed.value(QStringLiteral("Look Around"))->isEnabled()
               && listed.value(QStringLiteral("Format Everything")) && !listed.value(QStringLiteral("Format Everything"))->isEnabled()
               && listed.value(QStringLiteral("Format Everything"))->toolTip().contains(QLatin1String("system disk")),
           QStringLiteral("Tools lists every add-on action; one that doesn't fit is greyed out and says why"),
           listed.value(QStringLiteral("Format Everything")) ? listed.value(QStringLiteral("Format Everything"))->toolTip() : QStringLiteral("missing"));

    QMenu context;
    window.buildContextMenu(&context);
    QStringList offered;
    for (QAction *a : context.actions())
        offered << text(a);
    report(offered.contains(QLatin1String("Look Around")) && !offered.contains(QLatin1String("Format Everything"))
               && !offered.contains(QLatin1String("Only USB")),
           QStringLiteral("the system disk's right-click menu only offers the look-only add-on"), offered.join(QStringLiteral(", ")));

    Addons addons;
    addons.load();
    auto keyOf = [&addons](const QString &label) {
        for (const Addon &a : addons.all()) {
            for (const AddonAction &act : a.actions) {
                if (act.label == label)
                    return Addons::actionKey(a, act);
            }
        }
        return QString();
    };
    Addons::setPinned(keyOf(QStringLiteral("Look Around")), true);
    Addons::setPinned(keyOf(QStringLiteral("Format Everything")), true);
    window.refreshAddons();
    auto *toolbar = window.findChild<QToolBar *>(QStringLiteral("mainToolbar"));
    QAction *lookPin = nullptr, *formatPin = nullptr;
    for (QAction *a : toolbar->actions()) {
        if (text(a) == QLatin1String("Look Around"))
            lookPin = a;
        if (text(a) == QLatin1String("Format Everything"))
            formatPin = a;
    }
    report(lookPin && lookPin->isEnabled() && formatPin && !formatPin->isEnabled() && formatPin->toolTip().contains(QLatin1String("system disk")),
           QStringLiteral("pinned add-on actions go on the toolbar and follow the selection"));

    Addons::setShortcut(keyOf(QStringLiteral("Format Everything")), QStringLiteral("Ctrl+Alt+J"));
    Addons::setShortcut(keyOf(QStringLiteral("Look Around")), QStringLiteral("Ctrl+Alt+K"));
    window.refreshAddons();
    auto shortcutFor = [&window](const QString &keys) -> QShortcut * {
        for (QShortcut *s : window.findChildren<QShortcut *>()) {
            if (s->key() == QKeySequence(keys))
                return s;
        }
        return nullptr;
    };
    QShortcut *formatKey = shortcutFor(QStringLiteral("Ctrl+Alt+J"));
    if (formatKey)
        Q_EMIT formatKey->activated();
    const QString message = window.statusBar()->currentMessage();
    report(formatKey && message.contains(QLatin1String("Format Everything")) && message.contains(QLatin1String("system disk")),
           QStringLiteral("a shortcut for an action that doesn't fit says why in the status bar"), message);
    QString asked;
    Answerer answerer;
    answerer.answer = [&asked](QWidget *modal) {
        asked = modal->windowTitle();
        modal->close();
        return true;
    };
    if (QShortcut *lookKey = shortcutFor(QStringLiteral("Ctrl+Alt+K")))
        Q_EMIT lookKey->activated();
    waitUntil([&] { return !asked.isEmpty(); }, 3000);
    report(asked == QLatin1String("Run Add-on?"), QStringLiteral("a shortcut runs the add-on action (it asks first)"), asked);

    // The Add-ons window: shortcuts that clash or have no modifier are refused, settings save.
    addons.load();
    // Spelled out: the offscreen platform the tests use has no key for QKeySequence::Quit.
    AddonsDialog dialog(&addons, {QKeySequence(QStringLiteral("Ctrl+Q"))});
    dialog.show();
    QStringList warnings;
    answerer.answer = [&warnings](QWidget *modal) {
        if (auto *form = qobject_cast<AddonFormDialog *>(modal)) {
            form->findChild<QLineEdit *>()->setText(QStringLiteral("/srv/test-backups"));
            form->accept();
            return true;
        }
        if (auto *box = qobject_cast<QMessageBox *>(modal)) {
            warnings << box->text();
            box->close();
            return true;
        }
        return false;
    };
    const auto edits = dialog.findChildren<QKeySequenceEdit *>();
    if (!edits.isEmpty()) {
        edits.first()->setKeySequence(QKeySequence(QStringLiteral("Ctrl+Q")));
        Q_EMIT edits.first()->editingFinished();
        edits.first()->setKeySequence(QKeySequence(Qt::Key_J));
        Q_EMIT edits.first()->editingFinished();
    }
    report(warnings.size() == 2 && warnings.value(0).contains(QLatin1String("DiskForge's own")) && warnings.value(1).contains(QLatin1String("Ctrl"))
               && Addons::shortcut(keyOf(QStringLiteral("Look Around"))) == QLatin1String("Ctrl+Alt+K"),
           QStringLiteral("the Add-ons window refuses a shortcut DiskForge uses, or one without Ctrl/Alt"), warnings.join(QStringLiteral(" / ")));
    for (QPushButton *button : dialog.findChildren<QPushButton *>()) {
        if (button->text() == QStringLiteral("Settings…"))
            button->click();
    }
    QApplication::processEvents();
    report(Addons::setting(addons.all().value(0), QStringLiteral("dest")) == QLatin1String("/srv/test-backups"),
           QStringLiteral("an add-on's settings save from the Add-ons window"), Addons::setting(addons.all().value(0), QStringLiteral("dest")));
}

// Themes: built-ins switch and switch back, broken colors get fixed, and a theme that
// turned up from outside isn't used.
void themes()
{
    Addons addons;
    addons.load();
    Theme &theme = Theme::instance();
    const QString desktopStyle = QApplication::style()->name();
    const QColor desktopWindow = QApplication::palette().color(QPalette::Window);
    theme.select(QStringLiteral("deadshadow"), addons);
    report(theme.currentId() == QLatin1String("deadshadow") && QApplication::palette().color(QPalette::Window) == QColor(0x0a, 0x0a, 0x0f)
               && QApplication::style()->name().compare(QLatin1String("fusion"), Qt::CaseInsensitive) == 0
               && theme.partitionColor(QStringLiteral("ext4")) == QColor(0x00, 0xe6, 0x76),
           QStringLiteral("a built-in theme sets the window colors and its own map colors"), QApplication::style()->name());
    theme.select(QStringLiteral("system"), addons);
    report(QApplication::palette().color(QPalette::Window) == desktopWindow && QApplication::style()->name() == desktopStyle,
           QStringLiteral("going back to System restores the desktop's look"), QApplication::style()->name());

    AddonTheme sneaky;
    sneaky.colors.insert(QStringLiteral("danger"), 0x00ff00);  // green "danger"
    sneaky.colors.insert(QStringLiteral("warning"), 0xfafa00); // yellow on a light window: unreadable
    sneaky.palette.insert(QStringLiteral("window"), 0x000000);
    sneaky.palette.insert(QStringLiteral("text"), 0x111111);   // black on black
    QStringList replaced;
    const AddonTheme fixed = Theme::checked(sneaky, &replaced);
    report(replaced.contains(QLatin1String("danger")) && replaced.contains(QLatin1String("palette")) && fixed.palette.isEmpty()
               && QColor(QRgb(fixed.colors.value(QStringLiteral("danger")))).hsvHue() <= 20,
           QStringLiteral("a theme can't make danger green or its text unreadable"), replaced.join(QLatin1Char(' ')));

    const QByteArray mine = R"({"id":"my-theme","name":"Mine","theme":{"colors":{"partition":"#123456"}}})";
    QString error;
    Addons::install(mine, &error);
    const QString planted = Addons::userDir() + QStringLiteral("/planted-theme/addon.json");
    QDir().mkpath(QFileInfo(planted).path());
    QFile f(planted);
    if (f.open(QIODevice::WriteOnly))
        f.write(R"({"id":"planted-theme","name":"Planted","theme":{"colors":{"partition":"#654321"}}})");
    f.close();
    addons.load();
    bool plantedUsable = true, mineListed = false;
    for (const Theme::Choice &c : Theme::choices(addons)) {
        if (c.id == QLatin1String("planted-theme"))
            plantedUsable = c.usable;
        mineListed = mineListed || (c.id == QLatin1String("my-theme") && c.usable);
    }
    theme.use(QStringLiteral("planted-theme"), addons);
    report(mineListed && !plantedUsable && theme.currentId() == QLatin1String("system"),
           QStringLiteral("a theme that turned up from outside is listed but not used"), theme.currentId());
    theme.select(QStringLiteral("my-theme"), addons);
    report(theme.color(Theme::Role::Partition) == QColor(0x12, 0x34, 0x56), QStringLiteral("an installed theme add-on is used"));
    theme.select(QStringLiteral("system"), addons);
}

// The Add-on Maker: what it saves loads and isn't flagged, replacing asks first, editing an
// add-on that turned up from outside keeps the flag, and Test hands over the action.
void maker()
{
    Addons addons;
    addons.load();
    auto clickButton = [](QWidget *window, const QString &text) {
        for (QPushButton *b : window->findChildren<QPushButton *>()) {
            if (b->text() == text) {
                b->click();
                return true;
            }
        }
        return false;
    };
    auto find = [&addons](const QString &id) {
        for (const Addon &a : addons.all()) {
            if (a.id == id)
                return a;
        }
        return Addon();
    };
    auto fillIn = [](AddonMaker &m, const QString &name, const QString &label, const QString &program) {
        m.findChild<QLineEdit *>(QStringLiteral("name"))->setText(name);
        m.findChild<QLineEdit *>(QStringLiteral("label"))->setText(label);
        auto *command = m.findChild<QTableWidget *>(QStringLiteral("command"));
        command->item(0, 0)->setText(program);
    };
    {
        AddonMaker m(&addons, nullptr, {});
        fillIn(m, QStringLiteral("Maker Test"), QStringLiteral("Say Hi"), QStringLiteral("echo"));
        clickButton(&m, QStringLiteral("Save"));
    }
    addons.load();
    const Addon made = find(QStringLiteral("maker-test"));
    report(made.error.isEmpty() && !made.outside && made.actions.value(0).label == QLatin1String("Say Hi")
               && made.actions.value(0).command == QStringList{QStringLiteral("echo")},
           QStringLiteral("the Add-on Maker saves an add-on that loads and isn't flagged"), made.error);

    QStringList asked;
    Answerer answerer;
    answerer.answer = [&asked](QWidget *modal) {
        if (auto *box = qobject_cast<QMessageBox *>(modal)) {
            asked << box->text();
            if (QAbstractButton *no = box->button(QMessageBox::No))
                no->click();
            else
                box->close();
            return true;
        }
        return false;
    };
    {
        AddonMaker m(&addons, nullptr, {});
        fillIn(m, QStringLiteral("Maker Test"), QStringLiteral("Something Else"), QStringLiteral("true"));
        clickButton(&m, QStringLiteral("Save"));
    }
    addons.load();
    report(asked.value(0).contains(QLatin1String("already")) && find(QStringLiteral("maker-test")).actions.value(0).label == QLatin1String("Say Hi"),
           QStringLiteral("saving over an existing add-on asks first (and No keeps it)"), asked.join(QStringLiteral(" / ")));

    const QString planted = Addons::userDir() + QStringLiteral("/planted-maker/addon.json");
    QDir().mkpath(QFileInfo(planted).path());
    QFile f(planted);
    if (f.open(QIODevice::WriteOnly))
        f.write(R"({"id":"planted-maker","name":"Planted","actions":[{"label":"Hi","command":["echo","hi"]}]})");
    f.close();
    addons.load();
    {
        const Addon outside = find(QStringLiteral("planted-maker"));
        asked.clear();
        AddonMaker m(&addons, &outside, {});
        m.findChild<QLineEdit *>(QStringLiteral("label"))->setText(QStringLiteral("Hello"));
        clickButton(&m, QStringLiteral("Save"));
    }
    addons.load();
    report(find(QStringLiteral("planted-maker")).outside && find(QStringLiteral("planted-maker")).actions.value(0).label == QLatin1String("Hello"),
           QStringLiteral("editing an add-on that turned up from outside keeps the flag"), asked.join(QStringLiteral(" / ")));

    QString tested;
    {
        AddonMaker m(&addons, nullptr, [&tested](const Addon &, const AddonAction &action) { tested = action.label; });
        fillIn(m, QStringLiteral("Try Me"), QStringLiteral("Try This"), QStringLiteral("true"));
        clickButton(&m, QStringLiteral("Test"));
    }
    report(tested == QLatin1String("Try This"), QStringLiteral("Test hands the unsaved action over to be run"), tested);
}

// The warning banner, with health faked through the test hook: a bar for a failing drive,
// Details opens the Health window, Dismiss hides it, and it comes back when it gets worse.
void banner()
{
    UDisks udisks;
    udisks.setInteractive(false);
    if (udisks.disks().isEmpty()) {
        report(true, QStringLiteral("the warning banner (skipped: no drives)"));
        return;
    }
    for (const Disk &d : udisks.disks()) {
        Health healthy;
        healthy.state = Health::State::Healthy;
        healthy.summary = QStringLiteral("Healthy");
        healthy.key = QStringLiteral("test-healthy");
        udisks.setHealthForTest(d.blockPath, healthy);
    }
    udisks.refresh();
    MainWindow window(&udisks);
    window.show();
    auto *notices = window.findChild<QWidget *>(QStringLiteral("notices"));
    auto bars = [notices] {
        QList<NoticeBar *> out;
        for (NoticeBar *b : notices->findChildren<NoticeBar *>()) {
            if (b->isVisibleTo(notices))
                out << b;
        }
        return out;
    };
    report(notices && !notices->isVisible(), QStringLiteral("no banner while every drive is healthy"));
    if (!notices)
        return;

    const Disk target = udisks.disks().constFirst();
    Health failing;
    failing.state = Health::State::Failing;
    failing.summary = QStringLiteral("Failing, back up now");
    failing.key = QStringLiteral("test-failing");
    failing.reasons = {{HealthReason::Level::Failing, QStringLiteral("drive-failing"), -1, QStringLiteral("The drive itself says it's failing.")}};
    udisks.setHealthForTest(target.blockPath, failing);
    udisks.refresh();
    QList<NoticeBar *> shown = bars();
    QStringList buttons;
    for (QPushButton *b : shown.isEmpty() ? QList<QPushButton *>() : shown[0]->findChildren<QPushButton *>())
        buttons << b->text();
    const QStringList expected = target.isSystem ? QStringList{QStringLiteral("Details…"), QStringLiteral("Dismiss")}
                                                 : QStringList{QStringLiteral("Rescue Copy…"), QStringLiteral("Details…"), QStringLiteral("Dismiss")};
    report(notices->isVisible() && shown.size() == 1 && shown[0]->text().contains(QLatin1String("is failing")) && buttons == expected,
           QStringLiteral("a failing drive gets a bar on top, with what to do"),
           (shown.isEmpty() ? QString() : shown[0]->text()) + QStringLiteral(" [") + buttons.join(QStringLiteral(", ")) + QLatin1Char(']'));

    QString opened;
    Answerer answerer;
    answerer.answer = [&opened](QWidget *modal) {
        if (modal->inherits("HealthDialog")) {
            opened = modal->windowTitle();
            modal->close();
            return true;
        }
        return false;
    };
    auto click = [&shown](const QString &text) {
        for (QPushButton *b : shown.value(0) ? shown[0]->findChildren<QPushButton *>() : QList<QPushButton *>()) {
            if (b->text() == text)
                b->click();
        }
    };
    click(QStringLiteral("Details…"));
    waitUntil([&opened] { return !opened.isEmpty(); }, 5000);
    report(!opened.isEmpty(), QStringLiteral("Details… opens the Health window"), opened);

    click(QStringLiteral("Dismiss"));
    waitUntil([notices] { return !notices->isVisible(); }, 3000);
    udisks.refresh();
    report(!notices->isVisible(), QStringLiteral("Dismiss hides it, and it stays hidden"));

    failing.reasons << HealthReason{HealthReason::Level::Warning, QStringLiteral("pending"), 3, QStringLiteral("3 unreadable sectors.")};
    udisks.setHealthForTest(target.blockPath, failing);
    udisks.refresh();
    report(notices->isVisible() && bars().size() == 1, QStringLiteral("it comes back when it gets worse"));
}

// Stop from the bar on top: a real wipe on a slow test disk, stopped with the bar's own
// button. The question is answered "Stop Wiping"; then the bar and the toolbar's Stop go,
// and the status bar says where it stopped (no error box).
void stopWipe()
{
    QTemporaryDir dir;
    SlowDisk slow;
    if (!slow.create(dir.path(), 128, 40)) {
        report(false, QStringLiteral("make a slow test disk"), slow.error);
        slow.remove();
        return;
    }
    UDisks udisks;
    udisks.setInteractive(false);
    MainWindow window(&udisks);
    window.show();
    QString blockPath;
    waitUntil([&] {
        udisks.refresh();
        for (const Disk &d : udisks.disks()) {
            if (d.device == slow.loop)
                blockPath = d.blockPath;
        }
        return !blockPath.isEmpty();
    }, 15000);
    if (blockPath.isEmpty()) {
        report(false, QStringLiteral("the slow test disk shows up"), slow.loop);
        slow.remove();
        return;
    }
    udisks.wipe(*udisks.diskByPath(blockPath));
    QAction *stop = findAction(window, QStringLiteral("Stop"));
    NoticeBar *bar = nullptr;
    QPushButton *stopWiping = nullptr;
    waitUntil([&] {
        for (NoticeBar *b : window.findChildren<NoticeBar *>(QStringLiteral("job"))) {
            for (QPushButton *button : b->findChildren<QPushButton *>()) {
                if (!b->isHidden() && button->text() == QLatin1String("Stop Wiping")) {
                    bar = b;
                    stopWiping = button;
                }
            }
        }
        return bar != nullptr;
    }, 15000);
    report(bar && stop && stop->isEnabled(), QStringLiteral("a running wipe gets a bar on top with Stop, and the toolbar's Stop is on"),
           bar ? bar->text() : QString());
    if (!stopWiping) {
        slow.remove();
        return;
    }
    QStringList asked;
    Answerer answerer;
    answerer.answer = [&asked](QWidget *modal) {
        if (auto *box = qobject_cast<QMessageBox *>(modal)) {
            for (QAbstractButton *b : box->buttons()) {
                if (b->text() == QLatin1String("Stop Wiping")) {
                    asked << box->text();
                    b->click();
                    return true;
                }
            }
        }
        return false;
    };
    stopWiping->click();
    auto noBars = [&window] {
        for (NoticeBar *b : window.findChildren<NoticeBar *>(QStringLiteral("job"))) {
            if (!b->isHidden())
                return false;
        }
        return true;
    };
    waitUntil([&] { return noBars() && !stop->isEnabled(); }, 20000);
    const QString status = window.statusBar()->currentMessage();
    report(asked.size() == 1 && asked[0].startsWith(QLatin1String("Stop wiping")) && noBars() && !stop->isEnabled()
               && status.startsWith(QLatin1String("Stopped wiping")) && answerer.boxes.isEmpty(),
           QStringLiteral("Stop asks first, then the wipe stops: no error box, and the bar and Stop go away"),
           status + QStringLiteral(" | ") + answerer.boxes.join(QStringLiteral(" / ")));
    slow.remove();
}

// Type and Flags through the window: pick Linux /home, tick Don't mount automatically,
// press Change, and the partition has both.
void typeAndFlags()
{
    QTemporaryDir dir;
    const QString image = dir.filePath(QStringLiteral("types.img"));
    sh(QStringLiteral("truncate"), {QStringLiteral("-s"), QStringLiteral("64M"), image});
    QProcess sfdisk;
    sfdisk.start(QStringLiteral("sfdisk"), {QStringLiteral("-q"), image});
    sfdisk.waitForStarted();
    sfdisk.write("label: gpt\n,,L\n");
    sfdisk.closeWriteChannel();
    sfdisk.waitForFinished();
    QString loop;
    sh(QStringLiteral("losetup"), {QStringLiteral("-fP"), QStringLiteral("--show"), image}, &loop);
    loop = loop.trimmed();
    const QString part = loop + QStringLiteral("p1");

    UDisks udisks;
    udisks.setInteractive(false);
    MainWindow window(&udisks);
    window.show();
    waitUntil([&] {
        udisks.refresh();
        return window.selectDevice(part);
    }, 15000);
    QAction *action = findAction(window, QStringLiteral("Partition Type and Flags"));
    report(action && action->isEnabled(), QStringLiteral("Type and Flags is offered for a partition"));
    bool answered = false;
    Answerer answerer;
    answerer.answer = [&answered](QWidget *modal) {
        auto *dialog = qobject_cast<PartitionTypeDialog *>(modal);
        if (!dialog)
            return false;
        auto *types = dialog->findChild<QComboBox *>();
        types->setCurrentIndex(types->findData(QStringLiteral("933ac7e1-2eb4-4f13-b844-0e14e2aef915")));
        for (QCheckBox *check : dialog->findChildren<QCheckBox *>()) {
            if (check->text() == QLatin1String("Don't mount automatically"))
                check->setChecked(true);
        }
        if (QPushButton *change = findButton(dialog, QStringLiteral("Change"))) {
            answered = change->isEnabled();
            change->click();
        }
        return true;
    };
    if (action)
        action->trigger();
    const Volume *v = nullptr;
    waitUntil([&] {
        udisks.refresh();
        for (const Disk &d : udisks.disks()) {
            for (const Volume &x : d.volumes) {
                if (x.device == part)
                    v = &x;
            }
        }
        return v && v->partType == QLatin1String("933ac7e1-2eb4-4f13-b844-0e14e2aef915") && v->partFlags == (quint64(1) << 63);
    }, 20000);
    report(answered && v && v->partType == QLatin1String("933ac7e1-2eb4-4f13-b844-0e14e2aef915") && v->partFlags == (quint64(1) << 63),
           QStringLiteral("the dialog's type and flag end up on the partition"), v ? v->partType + QStringLiteral(" flags ") + QString::number(v->partFlags) : QString());
    sh(QStringLiteral("losetup"), {QStringLiteral("-d"), loop});
}

// The lock on an encrypted partition: clicking it asks for the passphrase and unlocks,
// clicking it again locks. A LUKS partition on a loop device.
void lockInMap()
{
    QTemporaryDir dir;
    const QString image = dir.filePath(QStringLiteral("luks.img"));
    sh(QStringLiteral("truncate"), {QStringLiteral("-s"), QStringLiteral("64M"), image});
    QProcess sfdisk;
    sfdisk.start(QStringLiteral("sfdisk"), {QStringLiteral("-q"), image});
    sfdisk.waitForStarted();
    sfdisk.write("label: gpt\n,,L\n");
    sfdisk.closeWriteChannel();
    sfdisk.waitForFinished();
    QString loop;
    sh(QStringLiteral("losetup"), {QStringLiteral("-fP"), QStringLiteral("--show"), image}, &loop);
    loop = loop.trimmed();
    const QString part = loop + QStringLiteral("p1");
    QThread::msleep(500);
    QProcess luks;
    luks.start(QStringLiteral("cryptsetup"), {QStringLiteral("luksFormat"), QStringLiteral("--batch-mode"), QStringLiteral("--type"), QStringLiteral("luks2"),
                                              QStringLiteral("--pbkdf"), QStringLiteral("pbkdf2"), QStringLiteral("--pbkdf-force-iterations"), QStringLiteral("1000"),
                                              QStringLiteral("--key-file=-"), part});
    luks.waitForStarted();
    luks.write("diskforge-test");
    luks.closeWriteChannel();
    luks.waitForFinished(60000);
    report(luks.exitCode() == 0, QStringLiteral("make a LUKS test partition"), QString::fromLocal8Bit(luks.readAllStandardError()).trimmed());

    UDisks udisks;
    udisks.setInteractive(false);
    MainWindow window(&udisks);
    window.resize(1200, 800);
    window.show();
    auto volume = [&]() -> const Volume * {
        for (const Disk &d : udisks.disks()) {
            for (const Volume &v : d.volumes) {
                if (v.device == part)
                    return &v;
            }
        }
        return nullptr;
    };
    waitUntil([&] {
        udisks.refresh();
        return volume() && volume()->encrypted;
    }, 15000);
    auto *map = window.findChild<DiskMap *>();
    const QString path = volume() ? volume()->objectPath : QString();
    auto clickLock = [map, &path] {
        const QPoint at = map->lockRect(path).center();
        QMouseEvent press(QEvent::MouseButtonPress, at, map->mapToGlobal(at), Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
        QCoreApplication::sendEvent(map, &press);
        QMouseEvent release(QEvent::MouseButtonRelease, at, map->mapToGlobal(at), Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
        QCoreApplication::sendEvent(map, &release);
    };
    report(map && !path.isEmpty() && !map->lockRect(path).isNull(), QStringLiteral("an encrypted partition has a lock on it in the map"));
    if (!map || path.isEmpty() || map->lockRect(path).isNull()) {
        sh(QStringLiteral("losetup"), {QStringLiteral("-d"), loop});
        return;
    }
    QString asked;
    Answerer answerer;
    answerer.answer = [&asked](QWidget *modal) {
        if (auto *input = qobject_cast<QInputDialog *>(modal)) {
            asked = input->windowTitle();
            input->setTextValue(QStringLiteral("diskforge-test"));
            input->accept();
            return true;
        }
        return false;
    };
    clickLock();
    // UDisks shows the unlocked side a moment before it answers; wait for both.
    waitUntil([&] {
        udisks.refresh();
        return volume() && !volume()->cleartextPath.isEmpty() && !udisks.isBusy();
    }, 20000);
    report(asked.startsWith(QLatin1String("Unlock")) && volume() && !volume()->cleartextPath.isEmpty(),
           QStringLiteral("clicking the lock asks for the passphrase and unlocks it"), asked);
    clickLock();
    waitUntil([&] {
        udisks.refresh();
        return volume() && volume()->cleartextPath.isEmpty();
    }, 20000);
    report(volume() && volume()->cleartextPath.isEmpty() && answerer.boxes.isEmpty(), QStringLiteral("clicking it again locks it"),
           answerer.boxes.join(QStringLiteral(" / ")));
    sh(QStringLiteral("losetup"), {QStringLiteral("-d"), loop});
}

// Job bars, with jobs faked through the test hook: a firmware erase says it can't be
// stopped and has no Stop; a wipe has Stop and turns on the toolbar's Stop; when the jobs
// end, the bars go.
void jobBars()
{
    UDisks udisks;
    udisks.setInteractive(false);
    if (udisks.disks().isEmpty()) {
        report(true, QStringLiteral("job bars (skipped: no drives)"));
        return;
    }
    const Disk d = udisks.disks().constFirst();
    Job erase;
    erase.path = QStringLiteral("/test/jobs/erase");
    erase.operation = QStringLiteral("ata-secure-erase");
    erase.objects = {d.drivePath};
    erase.progress = 0.1;
    erase.progressValid = true;
    udisks.setJobsForTest({erase});
    udisks.refresh();
    MainWindow window(&udisks);
    window.show();
    QAction *stop = findAction(window, QStringLiteral("Stop"));
    auto jobBars = [&window] {
        QList<NoticeBar *> out;
        for (NoticeBar *b : window.findChildren<NoticeBar *>(QStringLiteral("job"))) {
            if (!b->isHidden())
                out << b;
        }
        return out;
    };
    QList<NoticeBar *> bars = jobBars();
    report(bars.size() == 1 && bars[0]->text().contains(QLatin1String("can't be interrupted")) && bars[0]->findChildren<QPushButton *>().isEmpty()
               && stop && !stop->isEnabled(),
           QStringLiteral("a firmware erase gets a bar that says it can't be stopped, with no Stop"), bars.value(0) ? bars[0]->text() : QString());

    Job wipe;
    wipe.path = QStringLiteral("/test/jobs/wipe");
    wipe.operation = QStringLiteral("format-erase");
    wipe.cancelable = true;
    wipe.objects = {d.blockPath};
    wipe.progress = 0.34;
    wipe.progressValid = true;
    wipe.rate = 49000000;
    udisks.setJobsForTest({wipe});
    udisks.refresh();
    bars = jobBars();
    const QList<QPushButton *> buttons = bars.value(0) ? bars[0]->findChildren<QPushButton *>() : QList<QPushButton *>();
    report(bars.size() == 1 && bars[0]->text().startsWith(QLatin1String("Wiping")) && bars[0]->text().contains(QLatin1String("34%"))
               && buttons.size() == 1 && buttons[0]->text() == QLatin1String("Stop Wiping") && stop && stop->isEnabled(),
           QStringLiteral("a wipe gets a bar with Stop Wiping, and the toolbar's Stop is on"), bars.value(0) ? bars[0]->text() : QString());

    udisks.setJobsForTest({});
    udisks.refresh();
    QCoreApplication::processEvents();
    report(jobBars().isEmpty() && stop && !stop->isEnabled(), QStringLiteral("when the jobs end, the bars and Stop go"));
}

int userScenarios()
{
    QTemporaryDir home;
    qputenv("XDG_DATA_HOME", QFile::encodeName(home.filePath(QStringLiteral("data"))));
    qputenv("XDG_CONFIG_HOME", QFile::encodeName(home.filePath(QStringLiteral("config"))));
    outputWindow();
    addonMenus();
    themes();
    maker();
    banner();
    jobBars();
    out << (failures ? QStringLiteral("%1 step(s) failed").arg(failures) : QStringLiteral("All steps passed")) << Qt::endl;
    return failures ? 1 : 0;
}

} // namespace

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    // As the user: the add-on, menu and theme parts, which need no test disks.
    if (app.arguments().contains(QStringLiteral("--user")))
        return userScenarios();
    if (geteuid() != 0) {
        out << "Run as root: it creates a loop device (or use --user for the parts that don't)." << Qt::endl;
        return 2;
    }

    // A disk image with one ext4 partition whose root folder has a wrong link count.
    QTemporaryDir dir;
    const QString image = dir.filePath(QStringLiteral("damaged.img"));
    sh(QStringLiteral("truncate"), {QStringLiteral("-s"), QStringLiteral("128M"), image});
    QProcess sfdisk;
    sfdisk.start(QStringLiteral("sfdisk"), {QStringLiteral("-q"), image});
    sfdisk.waitForStarted();
    sfdisk.write("label: gpt\n,,L\n");
    sfdisk.closeWriteChannel();
    sfdisk.waitForFinished();
    QString loop;
    sh(QStringLiteral("losetup"), {QStringLiteral("-fP"), QStringLiteral("--show"), image}, &loop);
    const QString part = loop + QStringLiteral("p1");
    QThread::msleep(500);
    sh(QStringLiteral("mkfs.ext4"), {QStringLiteral("-q"), QStringLiteral("-L"), QStringLiteral("DAMAGED"), part});
    sh(QStringLiteral("debugfs"), {QStringLiteral("-w"), QStringLiteral("-R"), QStringLiteral("set_inode_field <2> links_count 7"), part});
    report(sh(QStringLiteral("e2fsck"), {QStringLiteral("-fn"), part}) == 4, QStringLiteral("test partition has real file system errors"), part);

    UDisks udisks;
    udisks.setInteractive(false);
    MainWindow window(&udisks);
    window.show();
    waitUntil([&] {
        udisks.refresh();
        return window.selectDevice(part);
    }, 15000);
    report(window.selectDevice(part), QStringLiteral("select it in the window"));

    QStringList messages;
    bool askedToRepair = false;
    QObject::connect(&udisks, &UDisks::operationFinished, [&](bool, const QString &m) { messages << m; });

    // Answer the dialogs the way a user would, refreshing the disk list first.
    QTimer clicker;
    QObject::connect(&clicker, &QTimer::timeout, [&] {
        auto *box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget());
        if (!box)
            return;
        if (box->text().contains(QLatin1String("Repair them now"))) {
            askedToRepair = true;
            for (int i = 0; i < 3; ++i)
                udisks.refresh(); // replaces the disk list while the question is open
            box->button(QMessageBox::Yes)->click();
        } else {
            messages << QStringLiteral("dialog: ") + box->text();
            box->close();
        }
    });
    clicker.start(100);

    QAction *check = nullptr;
    for (QAction *a : window.findChildren<QAction *>()) {
        if (a->text().remove(QLatin1Char('&')).startsWith(QLatin1String("Check for Errors")))
            check = a;
    }
    report(check && check->isEnabled(), QStringLiteral("Check for Errors is available"));
    if (check)
        check->trigger();

    waitUntil([&] {
        return std::any_of(messages.cbegin(), messages.cend(), [](const QString &m) {
            return m.contains(QLatin1String("epair")) || m.contains(QLatin1String("not found")) || m.contains(QLatin1String("isn't there"));
        });
    }, 60000);
    clicker.stop();

    report(askedToRepair, QStringLiteral("Check found the errors and asked to repair"));
    const bool repaired = std::any_of(messages.cbegin(), messages.cend(), [](const QString &m) { return m.startsWith(QLatin1String("Repaired")); });
    report(repaired, QStringLiteral("repair reached the partition after the refresh"), messages.join(QStringLiteral(" | ")));
    report(sh(QStringLiteral("e2fsck"), {QStringLiteral("-fn"), part}) == 0, QStringLiteral("the file system is clean now"));

    sh(QStringLiteral("losetup"), {QStringLiteral("-d"), loop});

    systemMenus(window, udisks);
    cloneThroughWindow(window, udisks, dir);
    backupAndRestore(window, udisks, dir);
    stopWipe();
    typeAndFlags();
    lockInMap();

    out << (failures ? QStringLiteral("%1 step(s) failed").arg(failures) : QStringLiteral("All steps passed")) << Qt::endl;
    return failures ? 1 : 0;
}
