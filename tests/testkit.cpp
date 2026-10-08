// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#include "testkit.h"

#include <QCryptographicHash>
#include <QFile>
#include <QProcess>

QTextStream out(stdout);
int failures = 0;

void report(bool ok, const QString &step, const QString &detail)
{
    out << (ok ? "PASS  " : "FAIL  ") << step;
    if (!detail.isEmpty())
        out << "  (" << detail << ")";
    out << Qt::endl;
    if (!ok)
        ++failures;
}

QString sh(const QString &program, const QStringList &args, int *exitCode)
{
    QProcess p;
    p.start(program, args);
    p.waitForFinished(120000);
    if (exitCode)
        *exitCode = p.exitStatus() == QProcess::NormalExit ? p.exitCode() : -1;
    return QString::fromLocal8Bit(p.readAllStandardOutput()).trimmed();
}

QString sha256File(const QString &path, qint64 offset, qint64 length)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly) || !f.seek(offset))
        return {};
    QCryptographicHash hash(QCryptographicHash::Sha256);
    qint64 left = length < 0 ? f.size() - offset : length;
    while (left > 0) {
        const QByteArray chunk = f.read(std::min<qint64>(left, 4 << 20));
        if (chunk.isEmpty())
            break;
        hash.addData(chunk);
        left -= chunk.size();
    }
    return QString::fromLatin1(hash.result().toHex());
}
