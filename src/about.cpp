#include "about.h"

#include <QApplication>
#include <QDialogButtonBox>
#include <QFile>
#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QSysInfo>
#include <QTabWidget>
#include <QTextBrowser>
#include <QVBoxLayout>

namespace {

QLabel *richLabel(const QString &html)
{
    auto *label = new QLabel(html);
    label->setWordWrap(true);
    label->setTextFormat(Qt::RichText);
    label->setOpenExternalLinks(true);
    label->setAlignment(Qt::AlignTop | Qt::AlignLeft);
    label->setContentsMargins(12, 12, 12, 12);
    return label;
}

QString link(const QString &url, const QString &text = {})
{
    return QStringLiteral("<a href=\"%1\">%2</a>").arg(url, text.isEmpty() ? url : text);
}

QString resourceText(const QString &path)
{
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? QString::fromUtf8(file.readAll()) : QString();
}

} // namespace

AboutDialog::AboutDialog(const QString &udisksVersion, QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle(tr("About DiskForge"));
    const QString homepage = QStringLiteral(APP_HOMEPAGE);

    auto *icon = new QLabel;
    icon->setPixmap(QApplication::windowIcon().pixmap(64, 64));
    auto *title = new QLabel(tr("<span style=\"font-size:18pt; font-weight:bold\">DiskForge</span><br>"
                                "Version %1<br>Disk manager and partition editor")
                                 .arg(QApplication::applicationVersion()));
    auto *header = new QHBoxLayout;
    header->addWidget(icon);
    header->addSpacing(8);
    header->addWidget(title, 1);

    auto *tabs = new QTabWidget;
    tabs->addTab(richLabel(tr("<p>View disks and partitions the way Windows Disk Management shows them, "
                              "and mount, format, create, delete and resize partitions.</p>"
                              "<p>Every change goes through UDisks2 and polkit, and the disk your system "
                              "runs from is locked so it can't be changed by accident.</p>"
                              "<p>© 2026 %1<br>Released under the MIT license.</p>"
                              "<p>%2<br>%3</p>")
                               .arg(QStringLiteral(APP_AUTHOR), link(homepage, tr("Project website")),
                                    link(homepage + QStringLiteral("/issues"), tr("Report a bug")))),
                 tr("&About"));
    tabs->addTab(richLabel(tr("<p><b>%1</b><br>Creator and maintainer<br>%2</p>")
                               .arg(QStringLiteral(APP_AUTHOR),
                                    link(QStringLiteral(APP_AUTHOR_URL)))),
                 tr("A&uthor"));

    auto *license = new QTextBrowser;
    license->setPlainText(resourceText(QStringLiteral(":/LICENSE")));
    tabs->addTab(license, tr("&License"));

    tabs->addTab(richLabel(tr("<p>Qt %1<br>UDisks2 %2<br>%3</p>")
                               .arg(QString::fromLatin1(qVersion()),
                                    udisksVersion.isEmpty() ? tr("(not running)") : udisksVersion,
                                    QSysInfo::prettyProductName().toHtmlEscaped())),
                 tr("&Components"));

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    auto *layout = new QVBoxLayout(this);
    layout->addLayout(header);
    layout->addWidget(tabs, 1);
    layout->addWidget(buttons);
    resize(520, 420);
}

HelpWindow::HelpWindow(QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle(tr("DiskForge Handbook"));
    auto *browser = new QTextBrowser;
    browser->setOpenExternalLinks(true);
    browser->setHtml(resourceText(QStringLiteral(":/data/help.html")));

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::close);

    auto *layout = new QVBoxLayout(this);
    layout->addWidget(browser, 1);
    layout->addWidget(buttons);
    resize(760, 680);
}
