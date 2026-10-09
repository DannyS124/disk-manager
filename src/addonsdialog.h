// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QDialog>

class Addons;
class QPushButton;
class QTextBrowser;
class QTreeWidget;

// Lists installed add-ons, shows exactly what each action runs, and installs or removes them.
class AddonsDialog : public QDialog
{
    Q_OBJECT
public:
    explicit AddonsDialog(Addons *addons, QWidget *parent = nullptr);

private:
    void fill();
    void showDetails();
    int currentRow() const;

    Addons *m_addons;
    QTreeWidget *m_list;
    QTextBrowser *m_details;
    QPushButton *m_accept;
    bool m_filling = false;
};
