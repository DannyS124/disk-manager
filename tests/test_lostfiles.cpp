// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

// Find Lost Files, finding files by their contents: real files of every kind it knows (made
// with Qt, libarchive, or by hand where nothing here writes them) dropped at known places in
// a stretch of random bytes, then found again, measured to the byte, saved and compared. Also
// damaged and cut-off files, bad spots, Stop, and random damage that must never crash it.

#include "testkit.h"

#include "../src/filetypes.h"
#include "../src/lostscan.h"
#include "../src/lostsource.h"

#include <QBuffer>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QImage>
#include <QPainter>
#include <QRandomGenerator>
#include <QTemporaryDir>

#include <archive.h>
#include <archive_entry.h>

namespace {

constexpr qsizetype kMiB = 1024 * 1024;

struct Sample {
    QString type;    // what it should be found as
    QByteArray data;
    quint64 offset = 0;
};

QByteArray picture(const char *format)
{
    QImage image(320, 200, QImage::Format_RGB32);
    QPainter p(&image);
    QLinearGradient g(0, 0, 320, 200);
    g.setColorAt(0, QColor(0x06, 0x0a, 0x16));
    g.setColorAt(1, QColor(0x00, 0xe5, 0xff));
    p.fillRect(image.rect(), g);
    p.setBrush(QColor(0xff, 0x33, 0x55));
    p.drawEllipse(QRect(40, 40, 120, 120));
    p.setBrush(QColor(0x2b, 0xee, 0x8a));
    p.drawRect(QRect(180, 60, 100, 80));
    p.end();
    QByteArray out;
    QBuffer buffer(&out);
    buffer.open(QIODevice::WriteOnly);
    image.save(&buffer, format, 85);
    return out;
}

// A small but proper PDF: one page with a line of text, the object table and the trailer.
QByteArray pdf()
{
    const QByteArray stream = "BT /F1 24 Tf 72 720 Td (A PDF that was lost and found again.) Tj ET";
    const QList<QByteArray> objects = {
        "<< /Type /Catalog /Pages 2 0 R >>",
        "<< /Type /Pages /Kids [3 0 R] /Count 1 >>",
        "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] /Contents 4 0 R /Resources << /Font << /F1 5 0 R >> >> >>",
        "<< /Length " + QByteArray::number(stream.size()) + " >>\nstream\n" + stream + "\nendstream",
        "<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>",
    };
    QByteArray out = "%PDF-1.4\n%\xe2\xe3\xcf\xd3\n";
    QList<qsizetype> offsets;
    for (int i = 0; i < objects.size(); ++i) {
        offsets << out.size();
        out += QByteArray::number(i + 1) + " 0 obj\n" + objects[i] + "\nendobj\n";
    }
    const qsizetype xref = out.size();
    out += "xref\n0 " + QByteArray::number(objects.size() + 1) + "\n0000000000 65535 f \n";
    for (qsizetype o : offsets)
        out += QByteArray::number(o).rightJustified(10, '0') + " 00000 n \n";
    out += "trailer\n<< /Size " + QByteArray::number(objects.size() + 1) + " /Root 1 0 R >>\nstartxref\n" + QByteArray::number(xref) + "\n%%EOF\n";
    return out;
}

// A ZIP with these files in it, stored as they are (the mimetype of an OpenDocument has to be).
QByteArray zip(const QList<QPair<QByteArray, QByteArray>> &files)
{
    QByteArray out(4 * kMiB, '\0');
    size_t used = 0;
    archive *a = archive_write_new();
    archive_write_set_format_zip(a);
    archive_write_set_options(a, "zip:compression=store");
    archive_write_open_memory(a, out.data(), size_t(out.size()), &used);
    for (const auto &f : files) {
        archive_entry *e = archive_entry_new();
        archive_entry_set_pathname(e, f.first.constData());
        archive_entry_set_size(e, f.second.size());
        archive_entry_set_filetype(e, AE_IFREG);
        archive_entry_set_perm(e, 0644);
        archive_write_header(a, e);
        archive_write_data(a, f.second.constData(), size_t(f.second.size()));
        archive_entry_free(e);
    }
    archive_write_close(a);
    archive_write_free(a);
    out.resize(qsizetype(used));
    return out;
}

void be32(QByteArray &d, quint32 v)
{
    d.append(char(v >> 24)).append(char(v >> 16)).append(char(v >> 8)).append(char(v));
}

void le32(QByteArray &d, quint32 v)
{
    d.append(char(v)).append(char(v >> 8)).append(char(v >> 16)).append(char(v >> 24));
}

QByteArray box(const char *type, const QByteArray &body)
{
    QByteArray b;
    be32(b, quint32(8 + body.size()));
    b.append(type, 4).append(body);
    return b;
}

QByteArray mp4()
{
    QByteArray ftyp("isom");
    be32(ftyp, 512);
    ftyp.append("isomiso2avc1mp41");
    QByteArray media(200000, Qt::Uninitialized);
    for (qsizetype i = 0; i < media.size(); ++i)
        media[i] = char(i * 7 + 3);
    return box("ftyp", ftyp) + box("moov", box("mvhd", QByteArray(100, '\1'))) + box("mdat", media);
}

QByteArray wav()
{
    QByteArray fmt, data(44100, '\x40'), out("RIFF");
    le32(fmt, 0x00010001); // PCM, mono
    le32(fmt, 44100);
    le32(fmt, 44100);
    le32(fmt, 0x00080001);
    le32(out, quint32(4 + 8 + fmt.size() + 8 + data.size()));
    out.append("WAVEfmt ");
    le32(out, quint32(fmt.size()));
    out.append(fmt).append("data");
    le32(out, quint32(data.size()));
    return out.append(data);
}

QByteArray mp3()
{
    QByteArray out("ID3\x03\x00\x00\x00\x00\x00\x0a", 10);
    out.append(QByteArray(10, '\0')); // the tag's padding
    // MPEG-1 layer III, 128 kbit/s, 44.1 kHz: 417 bytes a frame.
    for (int i = 0; i < 12; ++i) {
        QByteArray frame(417, char(i));
        frame[0] = char(0xFF);
        frame[1] = char(0xFB);
        frame[2] = char(0x90);
        frame[3] = char(0x00);
        out.append(frame);
    }
    return out;
}

quint32 oggCrc(const QByteArray &data)
{
    quint32 crc = 0;
    for (const char c : data) {
        crc ^= quint32(uchar(c)) << 24;
        for (int i = 0; i < 8; ++i)
            crc = (crc & 0x80000000) ? (crc << 1) ^ 0x04C11DB7 : crc << 1;
    }
    return crc;
}

QByteArray oggPage(quint8 flags, quint32 sequence, const QByteArray &payload)
{
    QByteArray page("OggS\0", 5);
    page.append(char(flags));
    page.append(QByteArray(8, '\0'));         // granule position
    le32(page, 0x1234);                       // stream serial
    le32(page, sequence);
    le32(page, 0);                            // CRC, filled in below
    page.append(char(1)).append(char(payload.size()));
    page.append(payload);
    const quint32 crc = oggCrc(page);
    page[22] = char(crc);
    page[23] = char(crc >> 8);
    page[24] = char(crc >> 16);
    page[25] = char(crc >> 24);
    return page;
}

QByteArray ogg()
{
    return oggPage(0x02, 0, QByteArray("\x01vorbis") + QByteArray(23, '\x05')) + oggPage(0x00, 1, QByteArray(200, 'a'))
           + oggPage(0x04, 2, QByteArray(100, 'b'));
}

QByteArray sqlite()
{
    QByteArray out("SQLite format 3\0", 16);
    out.append(char(0x10)).append(char(0x00));  // 4096-byte pages
    out.append(QByteArray(6, '\x01'));
    be32(out, 7);                               // change counter
    be32(out, 3);                               // 3 pages
    out.append(QByteArray(92 - out.size(), '\0'));
    be32(out, 7);                               // valid for change 7
    out.append(QByteArray(3 * 4096 - out.size(), '\x2a'));
    return out;
}

QByteArray sevenZip()
{
    const QByteArray next = QByteArray("\x01\x04\x06\x00\x01\x09\x00\x00", 8);
    QByteArray start;
    auto le64 = [&start](quint64 v) {
        for (int i = 0; i < 8; ++i)
            start.append(char(v >> (8 * i)));
    };
    le64(16);
    le64(quint64(next.size()));
    le32(start, filetypes::crc32(reinterpret_cast<const uchar *>(next.constData()), next.size()));
    QByteArray out("7z\xbc\xaf\x27\x1c\x00\x04", 8);
    le32(out, filetypes::crc32(reinterpret_cast<const uchar *>(start.constData()), start.size()));
    return out + start + QByteArray(16, '\x33') + next;
}

QByteArray gif()
{
    return QByteArray::fromBase64("R0lGODlhAQABAIAAAP///wAAACH5BAEAAAAALAAAAAABAAEAAAICRAEAOw==");
}

QByteArray noise(qsizetype size, quint32 seed)
{
    QByteArray data(size, Qt::Uninitialized);
    QRandomGenerator rng(seed);
    for (qsizetype i = 0; i + 4 <= size; i += 4)
        *reinterpret_cast<quint32 *>(data.data() + i) = rng.generate();
    return data;
}

QVector<Sample> samples()
{
    QVector<Sample> s = {
        {QStringLiteral("jpg"), picture("JPG")},
        {QStringLiteral("png"), picture("PNG")},
        {QStringLiteral("bmp"), picture("BMP")},
        {QStringLiteral("gif"), gif()},
        {QStringLiteral("pdf"), pdf()},
        {QStringLiteral("docx"), zip({{"[Content_Types].xml", "<Types/>"}, {"word/document.xml", "<w:document>hello</w:document>"}})},
        {QStringLiteral("odt"), zip({{"mimetype", "application/vnd.oasis.opendocument.text"}, {"content.xml", "<office:document-content/>"}})},
        {QStringLiteral("zip"), zip({{"notes.txt", QByteArray(5000, 'n')}, {"inner.zip", zip({{"a.txt", "inside"}})}})},
        {QStringLiteral("mp4"), mp4()},
        {QStringLiteral("wav"), wav()},
        {QStringLiteral("mp3"), mp3()},
        {QStringLiteral("ogg"), ogg()},
        {QStringLiteral("sqlite"), sqlite()},
        {QStringLiteral("7z"), sevenZip()},
    };
    return s;
}

// The samples at sector starts in random bytes, with random gaps between them.
QByteArray image(QVector<Sample> &files, qsizetype size)
{
    QByteArray data = noise(size, 777);
    QRandomGenerator rng(31);
    quint64 at = 4096;
    for (Sample &f : files) {
        at = (at + 511) / 512 * 512;
        f.offset = at;
        data.replace(qsizetype(at), f.data.size(), f.data);
        at += quint64(f.data.size()) + 512 * rng.bounded(1, 400);
    }
    return data;
}

QVector<lost::Found> scan(std::shared_ptr<lost::Source> source, bool *stopped = nullptr, bool cancelFirst = false)
{
    QVector<lost::Found> all;
    lost::DeepScan deep(source);
    QObject::connect(&deep, &lost::DeepScan::found, [&all](const QVector<lost::Found> &batch) { all += batch; });
    QObject::connect(&deep, &lost::DeepScan::finished, [stopped](bool s) {
        if (stopped)
            *stopped = s;
    });
    if (cancelFirst)
        deep.cancel();
    deep.run();
    return all;
}

void finding(const QString &dir)
{
    QVector<Sample> files = samples();
    for (const Sample &f : std::as_const(files))
        report(!f.data.isEmpty(), QStringLiteral("test file made: %1 (%2 bytes)").arg(f.type).arg(f.data.size()));
    const QByteArray data = image(files, 32 * kMiB);
    auto source = lost::Source::fromMemory(data);
    const QVector<lost::Found> found = scan(source);

    for (const Sample &f : std::as_const(files)) {
        const lost::Found *hit = nullptr;
        for (const lost::Found &g : found) {
            if (!g.extents.isEmpty() && g.extents[0].start == f.offset)
                hit = &g;
        }
        const QString type = hit && hit->type >= 0 ? filetypes::types()[hit->type].id : QString();
        report(hit && type == f.type && hit->size == quint64(f.data.size()) && hit->condition == lost::Condition::Good,
               QStringLiteral("%1 found where it was, to the byte, whole").arg(f.type),
               hit ? QStringLiteral("%1, %2 bytes (should be %3), condition %4").arg(type).arg(hit->size).arg(f.data.size()).arg(int(hit->condition))
                   : QStringLiteral("not found"));
    }
    report(found.size() == files.size(), QStringLiteral("nothing else turns up in the random bytes (or inside a whole file)"),
           QStringLiteral("%1 found, %2 placed").arg(found.size()).arg(files.size()));
    bool named = true;
    for (const lost::Found &g : found)
        named = named && g.name.endsWith(QLatin1Char('.') + filetypes::types()[g.type].id) && g.origin == lost::Origin::Contents;
    report(named, QStringLiteral("each gets a made-up name with the right extension"), found.value(0).name);

    // Saving: every byte as it was, a name that's taken isn't overwritten, a report goes along.
    lost::Saver saver(source, found, dir);
    QString savedTo;
    int saved = 0, failedCount = 0;
    QObject::connect(&saver, &lost::Saver::finished, [&](int s, int f, const QString &folder, const QString &) {
        saved = s;
        failedCount = f;
        savedTo = folder;
    });
    saver.run();
    int same = 0;
    for (const lost::Found &g : found) {
        const QString category = filetypes::categoryName(filetypes::types()[g.type].category);
        QFile out(savedTo + QLatin1Char('/') + category + QLatin1Char('/') + g.name);
        const QByteArray want = data.mid(qsizetype(g.extents[0].start), qsizetype(g.size));
        if (out.open(QIODevice::ReadOnly) && out.readAll() == want)
            ++same;
    }
    report(saved == found.size() && failedCount == 0 && same == found.size(), QStringLiteral("everything saved, byte for byte, sorted into folders"),
           QStringLiteral("%1 saved, %2 the same, in %3").arg(saved).arg(same).arg(savedTo));
    report(QFileInfo::exists(savedTo + QStringLiteral("/What was saved.txt")), QStringLiteral("with \"What was saved.txt\""));
    lost::Saver again(source, {found[0]}, dir);
    QString againTo;
    QObject::connect(&again, &lost::Saver::finished, [&](int, int, const QString &folder, const QString &) { againTo = folder; });
    again.run();
    const QString category = filetypes::categoryName(filetypes::types()[found[0].type].category);
    const QString stem = found[0].name.section(QLatin1Char('.'), 0, -2), ext = found[0].name.section(QLatin1Char('.'), -1);
    report(againTo != savedTo || QFileInfo::exists(againTo + QLatin1Char('/') + category + QStringLiteral("/%1 (2).%2").arg(stem, ext)),
           QStringLiteral("saving again never overwrites"));
}

void damage()
{
    // A PNG without its end and a JPEG cut short: found, but not as whole.
    QByteArray png = picture("PNG"), jpg = picture("JPG");
    png.chop(12);
    jpg.chop(jpg.size() / 3);
    QVector<Sample> files = {{QStringLiteral("png"), png}, {QStringLiteral("jpg"), jpg}, {QStringLiteral("png"), picture("PNG")}};
    QByteArray data = image(files, 4 * kMiB);
    // A PNG with one changed byte in the middle: its CRCs give it away.
    data[qsizetype(files[2].offset) + files[2].data.size() / 2] ^= 0x40;
    const QVector<lost::Found> found = scan(lost::Source::fromMemory(data));
    auto at = [&found](quint64 offset) -> const lost::Found * {
        for (const lost::Found &g : found) {
            if (g.extents.value(0).start == offset)
                return &g;
        }
        return nullptr;
    };
    const lost::Found *cutPng = at(files[0].offset), *cutJpg = at(files[1].offset), *changed = at(files[2].offset);
    report(cutPng && cutPng->condition == lost::Condition::MaybeDamaged, QStringLiteral("a PNG without its end is found, as maybe damaged"));
    report(cutJpg && cutJpg->condition == lost::Condition::MaybeDamaged, QStringLiteral("a JPEG cut short is found, as maybe damaged"));
    report(changed && changed->condition == lost::Condition::MaybeDamaged && changed->size == quint64(files[2].data.size()),
           QStringLiteral("a PNG with a changed byte is found whole in size, but as maybe damaged (bad CRC)"));

    // Bad spots: the part of a file on them reads as zeros, and the file says so.
    QVector<Sample> one = {{QStringLiteral("bmp"), picture("BMP")}};
    const QByteArray withBmp = image(one, 2 * kMiB);
    auto source = lost::Source::fromMemory(withBmp);
    const quint64 badStart = (one[0].offset + 64 * 1024 + 65535) / 65536 * 65536;
    source->failReads({{badStart, 4096}});
    const QVector<lost::Found> bmp = scan(source);
    const lost::Found *b = bmp.isEmpty() ? nullptr : &bmp[0];
    QByteArray readBack;
    if (b) {
        lost::FileReader reader(source, *b);
        readBack = reader.head(qint64(b->size));
    }
    const qsizetype inFile = qsizetype(badStart - one[0].offset);
    report(b && b->condition == lost::Condition::Unreadable && readBack.size() == one[0].data.size() && readBack.mid(inFile, 4096) == QByteArray(4096, '\0')
               && readBack.left(inFile) == one[0].data.left(inFile),
           QStringLiteral("a file on a bad spot: found, marked unreadable, the bad part comes back as zeros"));
    report(source->unreadable(badStart, 1) && !source->unreadable(0, 4096), QStringLiteral("the bad spot is remembered, the rest isn't"));

    bool stopped = false;
    scan(lost::Source::fromMemory(withBmp), &stopped, true);
    report(stopped, QStringLiteral("Stop stops it"));
}

void names()
{
    report(lost::cleanName(QStringLiteral("a/b\\c:d*e?f\"g<h>i|j.txt")) == QLatin1String("a_b_c_d_e_f_g_h_i_j.txt")
               && lost::cleanName(QStringLiteral("..")) == QLatin1String("file") && lost::cleanName(QStringLiteral(" ok. ")) == QLatin1String("ok")
               && lost::cleanName(QString(QChar(7)) + QStringLiteral("x")) == QLatin1String("_x"),
           QStringLiteral("names any drive takes: no slashes, Windows' forbidden characters, control characters or dots at the ends"));
    const QString longName = QString(300, QLatin1Char('a')) + QStringLiteral(".jpeg");
    const QString cut = lost::cleanName(longName);
    report(cut.toUtf8().size() <= 200 && cut.endsWith(QLatin1String(".jpeg")), QStringLiteral("a name that's too long is shortened, keeping the extension"), cut.right(12));
}

// Random damage: measuring must never crash, never claim more than its limit and never read
// outside what it's given.
void fuzz()
{
    const QVector<Sample> files = samples();
    QRandomGenerator rng(4321);
    int rounds = 0;
    bool sane = true;
    for (; rounds < 1500; ++rounds) {
        QByteArray d = files[rounds % files.size()].data;
        const int changes = 1 + int(rng.bounded(12));
        for (int i = 0; i < changes && !d.isEmpty(); ++i) {
            const qsizetype at = qsizetype(rng.bounded(quint32(d.size())));
            d[at] = char(rng.bounded(256));
        }
        if (rng.bounded(4) == 0)
            d.truncate(qsizetype(rng.bounded(quint32(d.size()))));
        if (d.size() < 32)
            continue;
        const QVector<int> types = filetypes::candidates(reinterpret_cast<const uchar *>(d.constData()), int(std::min<qsizetype>(d.size(), 512)));
        for (int type : types) {
            const filetypes::Reader read = [&d](quint64 at, char *buf, qint64 len) -> qint64 {
                if (at >= quint64(d.size()))
                    return 0;
                const qint64 n = std::min<qint64>(len, d.size() - qint64(at));
                memcpy(buf, d.constData() + at, size_t(n));
                return n;
            };
            const filetypes::Measured m = filetypes::measure(type, read);
            sane = sane && m.size <= filetypes::sizeLimit(type) && (m.size == 0 || m.type >= 0) && (!m.complete || m.size <= quint64(d.size()));
        }
    }
    report(sane, QStringLiteral("%1 damaged files measured: no crash, nothing whole that runs past the data").arg(rounds));
}

} // namespace

void lostFilesTests()
{
    QTemporaryDir dir;
    finding(dir.path());
    damage();
    names();
    fuzz();
}
