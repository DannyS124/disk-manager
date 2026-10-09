// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QDialog>
#include <QKeySequence>
#include <QList>

class Addons;
class QPushButton;
class QTableWidget;
class QTextBrowser;
class QTreeWidget;

// Lists installed add-ons, shows exactly what each action runs, and installs or removes them.
class AddonsDialog : public QDialog
{
    Q_OBJECT
public:
    // takenShortcuts: the main window's own, which add-on actions can't use.
    AddonsDialog(Addons *addons, const QList<QKeySequence> &takenShortcuts = {}, QWidget *parent = nullptr);

private:
    void fill();
    void showDetails();
    void showActions(int row);
    int currentRow() const;
    QString shortcutProblem(const QKeySequence &keys, const QString &actionKey) const;

    Addons *m_addons;
    QTreeWidget *m_list;
    QTextBrowser *m_details;
    QPushButton *m_accept;
    QPushButton *m_settings;
    QTableWidget *m_actions;
    QList<QKeySequence> m_taken;
    bool m_filling = false;
};
