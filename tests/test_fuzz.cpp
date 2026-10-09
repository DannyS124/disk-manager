// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

// Crash tests. Everything DiskForge reads that someone else could have made (add-ons, the
// online list and its signature, backup descriptions, rescue maps, partition tables,
// GitHub's update answer, mount tables, drive names) gets thousands of damaged versions of
// a good file. Nothing may crash or hang, and nothing broken may come out looking valid.
// release.sh also runs this in a build with AddressSanitizer, which catches memory errors
// that don't crash.
//
//   DISKFORGE_FUZZ_SEED=n    repeat a run (the seed is printed)
//   DISKFORGE_FUZZ_ROUNDS=n  rounds per kind of file (default 3000)

#include "testkit.h"

#include "../src/addons.h"
#include "../src/clone.h"
#include "../src/format.h"
#include "../src/gpt.h"
#include "../src/imagebackup.h"
#include "../src/outputfilter.h"
#include "../src/rescuecopy.h"
#include "../src/signature.h"
#include "../src/snapper.h"
#include "../src/updates.h"

#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonDocument>
#include <QProcess>
#include <QRandomGenerator>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QTemporaryDir>

#include <algorithm>
#include <cctype>

#include <fcntl.h>
#include <unistd.h>

namespace {

QRandomGenerator rng;

const QList<QByteArray> kTokens = {
    "{", "}", "[", "]", "\"", ",", ":", "null", "true", "-1", "0", "1e999", "18446744073709551615", "4294967296",
    "\\u0000", "\\ud800", "\\u202e", "\n", "\r", " ", "-", "--", "..", "/", "{label}", "{mountpoint}", "{home}", "<b>",
    "\xff\xfe", "\xc3\x28", "#", "+", "?", "0x", "-----END SSH SIGNATURE-----", "=", "{ask:a}", "{setting:d}", "{ask:}",
    "\"ask\":[{\"id\":\"a\"}],", "\"output\":\"window\",", "\"type\":\"choice\",", "\"min\":-1,", "#ff0000", "#12345",
};
const QList<QByteArray> kNumbers = {
    "0", "1", "-1", "4294967295", "4294967296", "18446744073709551615", "18446744073709551616",
    "0xffffffffffffffff", "99999999999999999999999", "1e308",
};

QByteArray mutate(QByteArray data)
{
    const int edits = 1 + int(rng.bounded(8));
    for (int i = 0; i < edits; ++i) {
        const qsizetype pos = data.isEmpty() ? 0 : qsizetype(rng.bounded(quint32(data.size())));
        switch (rng.bounded(7)) {
        case 0:
            if (!data.isEmpty())
                data[pos] = char(data[pos] ^ char(1 << rng.bounded(8)));
            break;
        case 1:
            if (!data.isEmpty())
                data[pos] = char(rng.bounded(256));
            break;
        case 2:
            data.insert(pos, kTokens[qsizetype(rng.bounded(quint32(kTokens.size())))]);
            break;
        case 3:
            data.remove(pos, qsizetype(rng.bounded(16)) + 1);
            break;
        case 4:
            data.insert(pos, data.mid(pos, qsizetype(rng.bounded(64))));
            break;
        case 5:
            data.truncate(pos + qsizetype(rng.bounded(4)));
            break;
        case 6: { // swap a number for an extreme one
            qsizetype start = pos;
            while (start < data.size() && !std::isdigit(uchar(data[start])))
                ++start;
            qsizetype end = start;
            while (end < data.size() && std::isxdigit(uchar(data[end])))
                ++end;
            if (start < data.size())
                data.replace(start, end - start, kNumbers[qsizetype(rng.bounded(quint32(kNumbers.size())))]);
            break;
        }
        }
    }
    return data;
}

int rounds()
{
    bool ok = false;
    const int n = qEnvironmentVariableIntValue("DISKFORGE_FUZZ_ROUNDS", &ok);
    return ok && n > 0 ? n : 3000;
}

QByteArray readFile(const QString &path)
{
    QFile f(path);
    return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
}

bool writeFile(const QString &path, const QByteArray &data)
{
    QFile f(path);
    return f.open(QIODevice::WriteOnly | QIODevice::Truncate) && f.write(data) == data.size();
}

const QRegularExpression kId(QStringLiteral("^[a-z0-9][a-z0-9-]*$"));

void addonFiles(const QString &tmp)
{
    QList<QByteArray> seeds;
    const QString examples = QStringLiteral(SOURCE_DIR "/examples/addons");
    for (const QString &name : QDir(examples).entryList(QDir::Dirs | QDir::NoDotAndDotDot))
        seeds << readFile(examples + QLatin1Char('/') + name + QStringLiteral("/addon.json"));
    seeds << R"({"id":"x","actions":[{"label":"L","look_only":true,"system_disks":true,"applies_to":"any",
                "when":["mounted","filesystem:ext4|vfat"],"confirm":"Sure about {label}?","command":["{home}/bin/x","--n={label}","{uuid}"]}]})";
    seeds << R"({"id":"full","settings":[{"id":"dest","type":"folder","default":"{home}/Backups"}],
                "theme":{"palette":{"window":"#0a0a0f"},"colors":{"danger":"#ff2d55"},"filesystems":{"ext4":"#00b4ff"},"usage":["#112233"]},
                "actions":[{"label":"Copy","output":"window","command":["rsync","{ask:c}","{ask:m}","--n={ask:t}","{setting:dest}/{label}"],
                "ask":[{"id":"c","type":"check","on":"--checksum","off":""},{"id":"m","type":"choice","choices":["-q","-v"]},
                       {"id":"t","type":"text","default":"{label}"},{"id":"n","type":"number","min":1,"max":9}]}]})";
    int accepted = 0;
    QStringList bad;
    Disk disk;
    disk.device = QStringLiteral("/dev/sdz");
    Volume vol;
    vol.device = QStringLiteral("/dev/sdz1");
    vol.label = QStringLiteral("stick");
    vol.mountPoints = {QStringLiteral("/run/media/me/stick")};
    disk.volumes = {vol};
    for (int i = 0; i < rounds(); ++i) {
        const Addon a = Addons::parseData(mutate(seeds[qsizetype(rng.bounded(quint32(seeds.size())))]), QString());
        if (!a.error.isEmpty())
            continue;
        ++accepted;
        bool ok = kId.match(a.id).hasMatch() && (!a.actions.isEmpty() || !a.theme.isEmpty());
        for (const AddonAction &act : a.actions) {
            ok = ok && !act.label.isEmpty() && !act.command.isEmpty() && !act.command.first().isEmpty();
            // The program can't come from the drive or a form: no "{" but {home}.
            ok = ok && !QString(act.command.value(0)).remove(QStringLiteral("{home}")).contains(QLatin1Char('{'));
            AddonAction unboxed = act;
            unboxed.lookOnly = false;
            ok = ok && !(act.lookOnly && !Addons::risks(unboxed, a.settings).admin.isEmpty());
            for (const AddonField &f : act.ask)
                ok = ok && !f.id.isEmpty() && f.min >= 0 && f.max >= f.min && (f.type != AddonField::Type::Choice || !f.choices.isEmpty());
            // Whatever it fills in for a drive and random answers: no hidden characters, no
            // empty arguments, and no typed answer at the start of an argument beginning with "-".
            QMap<QString, QString> answers;
            for (const AddonField &f : act.ask)
                answers.insert(f.id, rng.bounded(3) == 0 ? QStringLiteral("-x") : rng.bounded(2) ? QString() : QStringLiteral("value"));
            QString error;
            const QStringList argv = Addons::fillCommand(a, act, disk, &disk.volumes[0], answers, &error);
            for (const QString &part : argv)
                ok = ok && !hasHiddenCharacters(part);
        }
        if (!ok)
            bad << a.id;
    }
    report(bad.isEmpty(), QStringLiteral("%1 damaged add-ons: no crash, and the %2 that still load follow the rules").arg(rounds()).arg(accepted),
           bad.join(QLatin1Char(' ')));

    // Through the file too, the way Install from File and the add-on folder read them.
    const QString file = tmp + QStringLiteral("/addon.json");
    for (int i = 0; i < 200; ++i) {
        writeFile(file, mutate(seeds.value(0)));
        Addons::parse(file);
    }
}

QByteArray catalogSeed()
{
    const QString url = QStringLiteral("https://raw.githubusercontent.com/DannyS124/diskforge-addons/0123456789abcdef0123456789abcdef01234567/addons/a/addon.json");
    return QStringLiteral(R"({"format":"diskforge-addon-catalog","version":1,"addons":[
        {"id":"a","name":"A","version":"1.0","author":"x","description":"d","url":"%1","sha256":"%2"},
        {"id":"b-2","name":"B","version":"2","url":"%1","sha256":"%2"}]})")
        .arg(url, QString(64, QLatin1Char('a')))
        .toUtf8();
}

void catalogs()
{
    const QByteArray seed = catalogSeed();
    static const QRegularExpression sha(QStringLiteral("^[0-9a-f]{64}$"));
    int listed = 0;
    QStringList bad;
    for (int i = 0; i < rounds(); ++i) {
        QString error;
        for (const CatalogEntry &e : Addons::parseCatalog(mutate(seed), &error)) {
            ++listed;
            if (!kId.match(e.id).hasMatch() || !sha.match(e.sha256).hasMatch() || !Addons::isPinnedUrl(e.url))
                bad << e.id;
        }
    }
    report(bad.isEmpty(), QStringLiteral("%1 damaged add-on lists: no crash, and the %2 entries left all check out").arg(rounds()).arg(listed),
           bad.join(QLatin1Char(' ')));
}

// The bytes inside the armor, for comparing two signature files.
QByteArray signaturePayload(QByteArray armored)
{
    armored = armored.trimmed();
    armored.replace("-----BEGIN SSH SIGNATURE-----", "").replace("-----END SSH SIGNATURE-----", "");
    armored.replace('\n', QByteArray()).replace('\r', QByteArray()).replace(' ', QByteArray());
    const auto decoded = QByteArray::fromBase64Encoding(armored, QByteArray::AbortOnBase64DecodingErrors);
    return decoded ? *decoded : QByteArray("not base64");
}

void signatures(const QString &tmp)
{
    if (QStandardPaths::findExecutable(QStringLiteral("ssh-keygen")).isEmpty()) {
        out << "SKIP  ssh-keygen isn't installed, so signatures aren't crash-tested" << Qt::endl;
        return;
    }
    const QString key = tmp + QStringLiteral("/key");
    sh(QStringLiteral("ssh-keygen"), {QStringLiteral("-q"), QStringLiteral("-t"), QStringLiteral("ed25519"), QStringLiteral("-N"), QString(),
                                      QStringLiteral("-C"), QStringLiteral("fuzz"), QStringLiteral("-f"), key});
    const QStringList keys = {QString::fromLatin1(readFile(key + QStringLiteral(".pub"))).trimmed()};
    const QByteArray data = catalogSeed();
    writeFile(tmp + QStringLiteral("/list"), data);
    sh(QStringLiteral("ssh-keygen"), {QStringLiteral("-Y"), QStringLiteral("sign"), QStringLiteral("-f"), key, QStringLiteral("-n"),
                                      QStringLiteral("diskforge-addons"), tmp + QStringLiteral("/list")});
    const QByteArray sig = readFile(tmp + QStringLiteral("/list.sig"));
    QString error;
    report(signature::verify(data, sig, QStringLiteral("diskforge-addons"), keys, &error), QStringLiteral("the untouched signature checks out"), error);

    const QByteArray payload = signaturePayload(sig);
    int forged = 0;
    for (int i = 0; i < rounds(); ++i) {
        const QByteArray damaged = mutate(sig);
        // Only a change that leaves the signed bytes the same (spacing, say) may still pass.
        if (signature::verify(data, damaged, QStringLiteral("diskforge-addons"), keys, nullptr) && signaturePayload(damaged) != payload)
            ++forged;
        const QByteArray changed = mutate(data);
        if (changed != data && signature::verify(changed, sig, QStringLiteral("diskforge-addons"), keys, nullptr))
            ++forged;
    }
    report(forged == 0, QStringLiteral("%1 damaged signatures and lists: none passes").arg(rounds()), QString::number(forged));
}

void updateAnswers()
{
    const QByteArray seed = R"({"url":"https://api.github.com/repos/DannyS124/diskforge/releases/1","html_url":"https://github.com/DannyS124/diskforge/releases/tag/v0.5.0",
        "tag_name":"v0.5.0","name":"DiskForge 0.5.0","draft":false,"prerelease":false,"assets":[{"name":"diskforge-0.5.0.tar.gz","size":123}],
        "body":"notes"})";
    static const QRegularExpression version(QStringLiteral("^\\d{1,4}(\\.\\d{1,4}){1,3}$"));
    QStringList bad;
    for (int i = 0; i < rounds(); ++i) {
        QString error;
        const QString v = parseLatestRelease(mutate(seed), &error);
        if (!v.isEmpty() && (!version.match(v).hasMatch() || !releasePageUrl(v).startsWith(QLatin1String(APP_HOMEPAGE "/releases/tag/v"))))
            bad << v;
    }
    QString error;
    report(bad.isEmpty() && parseLatestRelease(seed, &error) == QLatin1String("0.5.0"),
           QStringLiteral("%1 damaged update answers: only plain version numbers come out").arg(rounds()), bad.join(QLatin1Char(' ')));
    report(parseLatestRelease(R"({"tag_name":"v1.0\"><a href=x>","html_url":"file:///etc/passwd"})", &error).isEmpty(),
           QStringLiteral("a version that isn't a version is refused"));
}

void backupDescriptions(const QString &tmp)
{
    const QByteArray seed = R"({"format":"diskforge-backup","version":1,"kind":"disk","device":"/dev/sdb","model":"Stick","serial":"123",
        "label":"","filesystem":"","table":"gpt","size":"1048576","sectorSize":512,"sha256":"00","fileSha256":"11",
        "app":"DiskForge 0.5.0","created":"2026-10-08T12:00:00Z"})";
    const QString image = tmp + QStringLiteral("/backup.img.zst");
    // A zstd frame header that records its size, then junk; describe() only reads the header.
    QByteArray frame = QByteArray::fromHex("28b52ffd2400010000");
    writeFile(image, frame);
    for (int i = 0; i < rounds(); ++i) {
        const QByteArray damaged = mutate(seed);
        BackupInfo::fromJson(QJsonDocument::fromJson(damaged).object());
        if (i % 10 == 0) {
            writeFile(imagebackup::infoPath(image), damaged);
            imagebackup::describe(image);
        }
    }
    QFile::remove(imagebackup::infoPath(image));
    for (int i = 0; i < 500; ++i) {
        writeFile(image, mutate(frame));
        imagebackup::describe(image);
    }
    const BackupInfo hidden = BackupInfo::fromJson(QJsonDocument::fromJson(R"({"format":"diskforge-backup","version":1,"size":"5",
        "model":"Stick\u202e\u200bX"})").object());
    report(hidden.model == QLatin1String("StickX"), QStringLiteral("damaged backup descriptions: no crash, and hidden characters in names are dropped"),
           hidden.model);
}

void rescueMaps(const QString &tmp)
{
    const quint64 size = 0x1000000;
    const QByteArray seed = "# Mapfile. Created by DiskForge\n# current_pos  current_status  current_pass\n0x00100000     ?               1\n"
                            "#      pos        size  status\n0x00000000  0x00100000  +\n0x00100000  0x00010000  -\n"
                            "0x00110000  0x00010000  *\n0x00120000  0x00EE0000  ?\n";
    const QString file = tmp + QStringLiteral("/rescue.map");
    QStringList bad;
    int loaded = 0;
    for (int i = 0; i < rounds(); ++i) {
        writeFile(file, mutate(seed));
        RescueMap map;
        QString error;
        if (!map.load(file, size, &error))
            continue;
        ++loaded;
        quint64 pos = 0;
        bool ok = map.currentPos <= size;
        for (int b = 0; b < map.blocks().size(); ++b) {
            const RescueMap::Block &block = map.blocks()[b];
            ok = ok && block.pos == pos && block.size <= size - pos && QByteArray("?*/-+").contains(block.status)
                && (b == 0 || map.blocks()[b - 1].status != block.status);
            pos += block.size;
        }
        if (!ok || pos != size)
            bad << QString::number(i);
    }
    report(bad.isEmpty(), QStringLiteral("%1 damaged rescue maps: no crash, and the %2 that load cover the drive exactly").arg(rounds()).arg(loaded),
           bad.join(QLatin1Char(' ')));
    // Two huge blocks that wrap around past 2^64 and land on the right total.
    writeFile(file, "0 ? 1\n0x0 0xffffffffffffffff +\n0xffffffffffffffff 0x1000001 ?\n");
    RescueMap map;
    QString error;
    report(!map.load(file, size, &error), QStringLiteral("a map whose blocks wrap around is refused"), error);
}

void partitionTables(const QString &tmp)
{
    if (QStandardPaths::findExecutable(QStringLiteral("sfdisk")).isEmpty()) {
        out << "SKIP  sfdisk isn't installed, so partition tables aren't crash-tested" << Qt::endl;
        return;
    }
    const QString image = tmp + QStringLiteral("/gpt.img");
    const qint64 imageSize = 4 * 1024 * 1024, edge = 1024 * 1024;
    {
        QFile f(image);
        if (f.open(QIODevice::WriteOnly))
            f.resize(imageSize);
    }
    QProcess sfdisk;
    sfdisk.start(QStringLiteral("sfdisk"), {QStringLiteral("-q"), QStringLiteral("--no-reread"), QStringLiteral("--no-tell-kernel"), image});
    sfdisk.write("label: gpt\nstart=2048, size=1024\nstart=4096, size=2048\n");
    sfdisk.closeWriteChannel();
    sfdisk.waitForFinished();
    const QByteArray pristine = readFile(image);
    report(pristine.size() == imageSize && pristine.mid(512, 8) == "EFI PART", QStringLiteral("made a GPT image to damage"));
    if (pristine.mid(512, 8) != "EFI PART")
        return;

    // Damage the header fields and the partition list, then fix the checksums so the
    // code past the checksum checks gets the damage too.
    auto crc32 = [](const char *data, qsizetype len) {
        quint32 crc = 0xFFFFFFFFu;
        for (qsizetype i = 0; i < len; ++i) {
            crc ^= uchar(data[i]);
            for (int k = 0; k < 8; ++k)
                crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
        }
        return ~crc;
    };
    auto put32 = [](char *p, quint32 v) {
        for (int i = 0; i < 4; ++i)
            p[i] = char(v >> (8 * i));
    };
    auto put64 = [](char *p, quint64 v) {
        for (int i = 0; i < 8; ++i)
            p[i] = char(v >> (8 * i));
    };
    auto get32 = [](const char *p) {
        quint32 v = 0;
        for (int i = 3; i >= 0; --i)
            v = v << 8 | uchar(p[i]);
        return v;
    };
    const QList<quint64> extremes = {0, 1, 2, 33, 34, 127, 128, 4095, 4096, 0xFFFFFFFFu, 0x100000000ull, 0xFFFFFFFFFFFFFFFFull, 1ull << 48};
    const int fd = ::open(QFile::encodeName(image).constData(), O_RDWR);
    int relocated = 0, grew = 0;
    for (int i = 0; i < rounds() / 3; ++i) {
        QByteArray disk = pristine;
        char *h = disk.data() + 512;
        const int fields[] = {12, 24, 32, 40, 48, 72, 80, 84};
        const int changes = 1 + int(rng.bounded(3));
        for (int c = 0; c < changes; ++c) {
            const int field = fields[rng.bounded(8)];
            const quint64 value = rng.bounded(2) ? extremes[qsizetype(rng.bounded(quint32(extremes.size())))] : rng.generate64() % 8192;
            if (field == 12 || field == 80 || field == 84)
                put32(h + field, quint32(value));
            else
                put64(h + field, value);
        }
        for (int c = int(rng.bounded(4)); c > 0; --c) // and some bytes in the partition list
            disk[1024 + qsizetype(rng.bounded(128 * 128))] = char(rng.bounded(256));
        const quint64 entriesBytes = std::min<quint64>(quint64(get32(h + 80)) * get32(h + 84), quint64(edge - 1024));
        put32(h + 88, crc32(disk.constData() + 1024, qsizetype(entriesBytes)));
        put32(h + 16, 0);
        put32(h + 16, crc32(h, qsizetype(std::min<quint32>(get32(h + 12), 512))));
        // Only the first and last MiB matter (tables, backup); the middle stays as it was.
        if (::pwrite(fd, disk.constData(), size_t(edge), 0) != edge
            || ::pwrite(fd, disk.constData() + imageSize - edge, size_t(edge), imageSize - edge) != edge || ::ftruncate(fd, imageSize) != 0)
            break;
        gpt::isGpt(fd, 512);
        if (gpt::relocateBackup(fd, 512, rng.bounded(2)).ok)
            ++relocated;
        grew += QFileInfo(image).size() != imageSize;
    }
    ::close(fd);
    report(grew == 0, QStringLiteral("%1 damaged partition tables: no crash, %2 still relocated, none wrote past the disk's end")
                          .arg(rounds() / 3).arg(relocated), QString::number(grew));
}

void mountTables()
{
    QByteArray seed = readFile(QStringLiteral("/proc/self/mountinfo"));
    seed += "36 35 0:31 /@home /home rw,relatime shared:2 - btrfs /dev/nvme0n1p2 rw,ssd,subvolid=257,subvol=/@home\n"
            "37 35 8:17 / /run/media/me/My\\040Stick rw,nosuid,nodev - vfat /dev/sdb1 rw,uid=1000\n";
    for (int i = 0; i < rounds(); ++i)
        snapper::mountedSubvolumes(mutate(seed));
    report(true, QStringLiteral("%1 damaged mount tables: no crash").arg(rounds()));
}

void clonePlans()
{
    auto upTo = [](quint64 n) { return n == ~quint64(0) ? rng.generate64() : rng.generate64() % (n + 1); };
    auto randomDisk = [&upTo](const QString &path) {
        Disk d;
        d.blockPath = path;
        d.device = QStringLiteral("/dev/") + path.section(QLatin1Char('/'), -1);
        const quint64 sizes[] = {0, 512, 1ull << 20, 1ull << 30, 1ull << 40, 0xFFFFFFFFFFFFFFFFull};
        d.size = rng.bounded(3) ? rng.generate64() % (1ull << 40) : sizes[rng.bounded(6)];
        d.sectorSize = rng.bounded(4) ? 512 : 4096;
        d.tableType = QStringList{QStringLiteral("gpt"), QStringLiteral("dos"), QString()}[rng.bounded(3)];
        for (int n = int(rng.bounded(9)); n > 0; --n) {
            Volume v;
            v.number = n;
            v.offset = rng.bounded(4) ? upTo(d.size) : rng.generate64();
            v.size = rng.bounded(4) ? upTo(d.size) : rng.generate64();
            v.isContainer = rng.bounded(6) == 0;
            v.isContained = !v.isContainer && rng.bounded(4) == 0;
            d.volumes << v;
        }
        return d;
    };
    QStringList bad;
    int planned = 0;
    for (int i = 0; i < rounds(); ++i) {
        const Disk source = randomDisk(QStringLiteral("/org/freedesktop/UDisks2/block_devices/sdy"));
        Disk target = randomDisk(QStringLiteral("/org/freedesktop/UDisks2/block_devices/sdz"));
        if (rng.bounded(2))
            target.size = source.size + rng.generate64() % (1ull << 30);
        target.sectorSize = source.sectorSize;
        const diskclone::Plan plan = diskclone::plan(source, target);
        if (!plan.error.isEmpty())
            continue;
        ++planned;
        for (const blockcopy::Extent &e : plan.extents) {
            if (e.source > source.size || e.length > source.size - e.source || e.target > target.size || e.length > target.size - e.target)
                bad << QString::number(i);
        }
    }
    report(bad.isEmpty(), QStringLiteral("%1 random partition layouts: the %2 clone plans made stay inside both drives").arg(rounds()).arg(planned),
           bad.mid(0, 5).join(QLatin1Char(' ')));
}

void commandOutput()
{
    // Random bytes, escape codes cut anywhere, broken UTF-8: never a crash, never a hidden
    // character or an overlong line in what's shown.
    const QList<QByteArray> pieces = {"\x1b", "[", "31m", "]", "0;t", "\x07", "\\", "\r", "\n", "\b", "\t", "\xc3", "\xa9",
                                      "\xe2\x80\xae", "\xff", "abc", "%", "\x00"};
    QStringList bad;
    for (int i = 0; i < rounds(); ++i) {
        OutputFilter filter;
        QStringList lines;
        for (int n = int(rng.bounded(20)); n > 0; --n) {
            QByteArray chunk;
            for (int k = int(rng.bounded(8)); k > 0; --k)
                chunk += rng.bounded(4) ? pieces[qsizetype(rng.bounded(quint32(pieces.size())))] : QByteArray(1, char(rng.bounded(256)));
            lines += filter.feed(chunk);
        }
        lines << filter.current();
        for (const QString &line : std::as_const(lines)) {
            if (hasHiddenCharacters(line) || line.size() > OutputFilter::kMaxLine + 1)
                bad << QString::number(i);
        }
    }
    report(bad.isEmpty(), QStringLiteral("%1 streams of junk output: shown as plain, short lines").arg(rounds()), bad.mid(0, 5).join(QLatin1Char(' ')));
}

void driveNames()
{
    // Names made of the characters that cause trouble, through the backup add-on's command.
    const QList<char32_t> pieces = {U'a', U'Z', U'0', U' ', U'-', U'.', U'/', U'\\', U'"', U'\'', U'$', U'\n', U'\t', U'\0',
                                    0x202E, 0x200B, 0x2066, 0xFEFF, 0x00AD, 0x2028, 0x1F4BE, 0x00E4, U'{', U'}', U'<', U'>'};
    const QStringList command = {QStringLiteral("rsync"), QStringLiteral("{label}"), QStringLiteral("{home}/Backups/{label}/"),
                                 QStringLiteral("--name={label}"), QStringLiteral("{model}")};
    QStringList bad;
    int ran = 0;
    for (int i = 0; i < rounds(); ++i) {
        QString name;
        for (int n = 1 + int(rng.bounded(8)); n > 0; --n) {
            const char32_t c = pieces[qsizetype(rng.bounded(quint32(pieces.size())))];
            name += QString::fromUcs4(&c, 1);
        }
        // What UDisks gives DiskForge is cleaned first; check the add-on rules on raw names too.
        const QString label = rng.bounded(2) ? cleanName(name) : name;
        if (hasHiddenCharacters(cleanName(name)) || cleanName(cleanName(name)) != cleanName(name))
            bad << QStringLiteral("cleanName");
        Disk d;
        d.device = QStringLiteral("/dev/sdz");
        d.model = label;
        Volume v;
        v.device = QStringLiteral("/dev/sdz1");
        v.label = label;
        v.mountPoints = {QStringLiteral("/run/media/me/x")};
        d.volumes = {v};
        QString error;
        const QStringList argv = Addons::expand(command, d, &d.volumes[0], &error);
        if (argv.isEmpty())
            continue;
        ++ran;
        const QString prefix = QDir::homePath() + QStringLiteral("/Backups/");
        QString folder = argv[2].mid(prefix.size());
        if (folder.endsWith(QLatin1Char('/')))
            folder.chop(1);
        if (argv[1].startsWith(QLatin1Char('-')) || argv[4].startsWith(QLatin1Char('-')) || hasHiddenCharacters(argv.join(QString()))
            || !argv[2].startsWith(prefix) || folder.isEmpty() || folder.contains(QLatin1Char('/')) || folder == QLatin1String("..")
            || folder == QLatin1String("."))
            bad << name.toHtmlEscaped();
    }
    report(bad.isEmpty(), QStringLiteral("%1 nasty drive names: none turns into an option, another folder or hidden text (%2 ran)").arg(rounds()).arg(ran),
           bad.mid(0, 5).join(QLatin1Char(' ')));
}

} // namespace

void fuzzTests()
{
    bool given = false;
    const quint32 seed = quint32(qEnvironmentVariableIntValue("DISKFORGE_FUZZ_SEED", &given));
    const quint32 used = given ? seed : QRandomGenerator::system()->generate();
    rng.seed(used);
    out << "seed " << used << " (DISKFORGE_FUZZ_SEED=" << used << " repeats this run)" << Qt::endl;

    QTemporaryDir tmp;
    QElapsedTimer timer;
    timer.start();
    addonFiles(tmp.path());
    catalogs();
    signatures(tmp.path());
    updateAnswers();
    backupDescriptions(tmp.path());
    rescueMaps(tmp.path());
    partitionTables(tmp.path());
    mountTables();
    clonePlans();
    driveNames();
    commandOutput();
    out << "took " << timer.elapsed() / 1000.0 << " s" << Qt::endl;
}
