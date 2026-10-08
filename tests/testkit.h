// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// Shared by the self-test suites.

#include <QStringList>
#include <QTextStream>

extern QTextStream out;
extern int failures;

void report(bool ok, const QString &step, const QString &detail = {});
// Runs a program and returns its trimmed standard output.
QString sh(const QString &program, const QStringList &args, int *exitCode = nullptr);
QString sha256File(const QString &path, qint64 offset = 0, qint64 length = -1);

// Suites in their own files.
void gptTests();
void copyTests();
void blockMapTests();
