// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#include "addonsdialog.h"

#include "addons.h"
#include "catalogdialog.h"

#include <QDesktopServices>
#include <QDir>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QMessageBox>
#include <QPushButton>
#include <QTextBrowser>
#include <QTreeWidget>
#include <QUrl>
#include <QVBoxLayout>

AddonsDialog::AddonsDialog(Addons *addons, QWidget *parent)
    : QDialog(parent)
    , m_addons(addons)
    , m_list(new QTreeWidget)
    , m_details(new QTextBrowser)
{
    setWindowTitle(tr("Add-ons"));
    m_list->setHeaderLabels({tr("Add-on"), tr("Version"), tr("Author"), tr("Status")});
    m_list->setRootIsDecorated(false);
    m_list->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_details->setOpenExternalLinks(true);

    auto *catalog = new QPushButton(QIcon::fromTheme(QStringLiteral("get-hot-new-stuff"), QIcon::fromTheme(QStringLiteral("download"))), tr("Get Add-ons…"));
    auto *install = new QPushButton(QIcon::fromTheme(QStringLiteral("list-add")), tr("Install from File…"));
    auto *remove = new QPushButton(QIcon::fromTheme(QStringLiteral("list-remove")), tr("Remove"));
    auto *folder = new QPushButton(QIcon::fromTheme(QStringLiteral("folder-open")), tr("Open Folder"));
    auto *guide = new QPushButton(QIcon::fromTheme(QStringLiteral("help-contents")), tr("How to Make One"));
    auto *close = new QPushButton(tr("Close"));

    connect(install, &QPushButton::clicked, this, [this] {
        const QString file = QFileDialog::getOpenFileName(this, tr("Install Add-on"), QDir::homePath(), tr("Add-on manifest (addon.json *.json)"));
        if (file.isEmpty())
            return;
        const Addon a = Addons::parse(file);
        if (!a.error.isEmpty()) {
            QMessageBox::warning(this, windowTitle(), tr("This add-on is broken: %1").arg(a.error));
            return;
        }
        QStringList commands;
        for (const AddonAction &act : a.actions)
            commands << QStringLiteral("• %1: %2").arg(act.label, act.command.join(QLatin1Char(' ')));
        const auto answer = QMessageBox::question(this, windowTitle(),
            tr("Install \"%1\"%2?\n\nIt adds these actions, which run these commands:\n%3\n\nOnly install add-ons you trust.")
                .arg(a.name, a.author.isEmpty() ? QString() : tr(" by %1").arg(a.author), commands.join(QLatin1Char('\n'))));
        if (answer != QMessageBox::Yes)
            return;
        QString error;
        if (!Addons::install(file, &error))
            QMessageBox::warning(this, windowTitle(), error);
        m_addons->load();
        fill();
    });
    connect(remove, &QPushButton::clicked, this, [this] {
        const int row = m_list->indexOfTopLevelItem(m_list->currentItem());
        if (row < 0 || row >= m_addons->all().size())
            return;
        const Addon a = m_addons->all()[row];
        if (QMessageBox::question(this, windowTitle(), tr("Remove \"%1\"?").arg(a.name)) != QMessageBox::Yes)
            return;
        QString error;
        if (!Addons::remove(a, &error))
            QMessageBox::warning(this, windowTitle(), error);
        m_addons->load();
        fill();
    });
    connect(folder, &QPushButton::clicked, this, [] {
        QDir().mkpath(Addons::userDir());
        QDesktopServices::openUrl(QUrl::fromLocalFile(Addons::userDir()));
    });
    connect(guide, &QPushButton::clicked, this, [] {
        QDesktopServices::openUrl(QUrl(QStringLiteral(APP_HOMEPAGE "/blob/main/docs/ADDONS.md")));
    });
    connect(catalog, &QPushButton::clicked, this, [this] {
        CatalogDialog(m_addons, this).exec();
        m_addons->load();
        fill();
    });
    connect(close, &QPushButton::clicked, this, &QDialog::accept);
    connect(m_list, &QTreeWidget::currentItemChanged, this, &AddonsDialog::showDetails);
    connect(m_list, &QTreeWidget::itemChanged, this, [this](QTreeWidgetItem *item) {
        if (m_filling)
            return;
        const int row = m_list->indexOfTopLevelItem(item);
        if (row >= 0 && row < m_addons->all().size())
            m_addons->setEnabled(m_addons->all()[row].id, item->checkState(0) == Qt::Checked);
    });

    auto *buttons = new QHBoxLayout;
    buttons->addWidget(catalog);
    buttons->addWidget(install);
    buttons->addWidget(remove);
    buttons->addWidget(folder);
    buttons->addWidget(guide);
    buttons->addStretch();
    buttons->addWidget(close);

    auto *layout = new QVBoxLayout(this);
    layout->addWidget(m_list, 2);
    layout->addWidget(m_details, 3);
    layout->addLayout(buttons);
    fill();
    resize(720, 520);
}

void AddonsDialog::fill()
{
    m_filling = true;
    m_list->clear();
    for (const Addon &a : m_addons->all()) {
        auto *item = new QTreeWidgetItem(m_list, {a.name.isEmpty() ? a.file : a.name, a.version, a.author,
                                                  a.error.isEmpty() ? (a.enabled ? tr("On") : tr("Off")) : tr("Broken")});
        item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
        item->setCheckState(0, a.enabled ? Qt::Checked : Qt::Unchecked);
        if (!a.error.isEmpty())
            item->setForeground(3, QColor(QStringLiteral("#e74c3c")));
    }
    m_filling = false;
    if (m_list->topLevelItemCount() > 0)
        m_list->setCurrentItem(m_list->topLevelItem(0));
    else
        m_details->setHtml(tr("<p>No add-ons installed.</p><p>Add-ons add actions to DiskForge's menus, like backing up a "
                              "drive or opening it in another tool. Install one with <b>Install from File</b>, or see "
                              "<b>How to Make One</b>.</p><p>They live in <code>%1</code>.</p>").arg(Addons::userDir()));
}

void AddonsDialog::showDetails()
{
    const int row = m_list->indexOfTopLevelItem(m_list->currentItem());
    if (row < 0 || row >= m_addons->all().size())
        return;
    const Addon &a = m_addons->all()[row];
    QString html = QStringLiteral("<h3>%1</h3>").arg(a.name.toHtmlEscaped());
    if (!a.description.isEmpty())
        html += QStringLiteral("<p>%1</p>").arg(a.description.toHtmlEscaped());
    if (!a.error.isEmpty())
        html += QStringLiteral("<p style=\"color:#e74c3c\"><b>%1</b></p>").arg(tr("Broken: %1").arg(a.error).toHtmlEscaped());
    for (const AddonAction &act : a.actions) {
        html += QStringLiteral("<p><b>%1</b> <small>(%2%3)</small><br><code>%4</code></p>")
                    .arg(act.label.toHtmlEscaped(), act.appliesTo, act.terminal ? tr(", in a terminal") : QString(),
                         act.command.join(QLatin1Char(' ')).toHtmlEscaped());
    }
    html += QStringLiteral("<p><small>%1</small></p>").arg(a.file.toHtmlEscaped());
    m_details->setHtml(html);
}
