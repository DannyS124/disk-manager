#include "format.h"
#include "mainwindow.h"
#include "udisks.h"

#include <QApplication>
#include <QCommandLineParser>
#include <QIcon>
#include <QMessageBox>
#include <QTextStream>
#include <QTimer>

namespace {

// for comparing against lsblk
void dump(const QVector<Disk> &disks)
{
    QTextStream out(stdout);
    for (int i = 0; i < disks.size(); ++i) {
        const Disk &d = disks[i];
        out << "Disk " << i << "  " << d.device << "  " << d.model << "  " << formatSize(d.size)
            << "  " << tableName(d) << "  " << diskKind(d);
        if (d.isSystem)
            out << "  [system: " << d.systemReason << "]";
        out << "\n";
        for (const Span &s : diskSpans(d)) {
            if (s.isFree()) {
                out << "    " << QStringLiteral("unallocated").leftJustified(14) << formatSize(s.size).rightJustified(11)
                    << "  at " << s.offset << "\n";
                continue;
            }
            const Volume &v = d.volumes[s.volume];
            out << "    " << shortDevice(v.device).leftJustified(14) << formatSize(v.size).rightJustified(11)
                << "  at " << v.offset << "  " << (v.fsType.isEmpty() ? QStringLiteral("-") : v.fsType)
                << "  " << volumeStatus(v);
            if (v.fsTotal)
                out << "  (" << formatSize(v.fsFree) << " free)";
            out << "\n";
        }
    }
}

} // namespace

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    QApplication::setApplicationName(QStringLiteral("diskforge"));
    QApplication::setApplicationDisplayName(QStringLiteral("DiskForge"));
    QApplication::setApplicationVersion(QStringLiteral(APP_VERSION));
    QApplication::setDesktopFileName(QStringLiteral(APP_ID));
    QApplication::setWindowIcon(QIcon::fromTheme(QStringLiteral(APP_ID), QIcon(QStringLiteral(":/data/" APP_ID ".svg"))));

    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral("A Windows-style disk manager built on UDisks2."));
    parser.addHelpOption();
    parser.addVersionOption();
    const QCommandLineOption dumpOption(QStringLiteral("dump"), QStringLiteral("Print the disk layout and exit."));
    const QCommandLineOption screenshotOption(QStringLiteral("screenshot"),
                                              QStringLiteral("Render the window to <file> and exit."),
                                              QStringLiteral("file"));
    const QCommandLineOption selectOption(QStringLiteral("select"),
                                          QStringLiteral("Start with partition <device> selected."),
                                          QStringLiteral("device"));
    parser.addOptions({dumpOption, screenshotOption, selectOption});
    parser.process(app);

    UDisks udisks;
    if (!udisks.isAvailable()) {
        const QString message = QStringLiteral("Can't reach UDisks2: %1\nIs udisks2 installed and running?")
                                    .arg(udisks.lastError());
        if (parser.isSet(dumpOption)) {
            QTextStream(stderr) << message << "\n";
            return 1;
        }
        QMessageBox::critical(nullptr, QStringLiteral("DiskForge"), message);
        return 1;
    }

    if (parser.isSet(dumpOption)) {
        dump(udisks.disks());
        return 0;
    }

    MainWindow window(&udisks);
    window.resize(1280, 800);
    window.show();
    if (parser.isSet(selectOption) && !window.selectDevice(parser.value(selectOption)))
        QTextStream(stderr) << "No partition " << parser.value(selectOption) << "\n";

    if (parser.isSet(screenshotOption)) {
        const QString file = parser.value(screenshotOption);
        QTimer::singleShot(500, &app, [&window, file] {
            window.grab().save(file);
            QApplication::quit();
        });
    }
    return app.exec();
}
