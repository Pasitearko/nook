#include "archivejob.h"
#include "archivenames.h"
#include <QDir>
#include <QDirIterator>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QMimeDatabase>
#include <QRandomGenerator>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QtTest>
#include <archive.h>
#include <clocale>
#include <archive_entry.h>
#include <functional>
#include <sys/stat.h>
#include <unistd.h>

using namespace nook::archive;

namespace {

struct Entry {
    QByteArray path;
    mode_t type = AE_IFREG;
    QByteArray data;
    mode_t perm = 0644;
    QByteArray link;
    QByteArray hardlink;
};

void writeArchive(const QString &file, const QList<Entry> &entries, bool zip, const QByteArray &password = {}) {
    // In the C locale names are stored as the given bytes, without the ZIP UTF-8 flag, the way
    // legacy Windows tools wrote them.
    const QByteArray locale = setlocale(LC_CTYPE, nullptr);
    setlocale(LC_CTYPE, "C");
    struct Restore {
        QByteArray locale;
        ~Restore() { setlocale(LC_CTYPE, locale.constData()); }
    } restore{locale};
    struct archive *a = archive_write_new();
    if (zip)
        archive_write_set_format_zip(a);
    else
        archive_write_set_format_pax_restricted(a);
    if (!password.isEmpty()) {
        QCOMPARE(archive_write_set_options(a, "zip:encryption=aes256"), ARCHIVE_OK);
        archive_write_set_passphrase(a, password.constData());
    }
    QCOMPARE(archive_write_open_filename(a, file.toLocal8Bit().constData()), ARCHIVE_OK);
    for (const auto &item : entries) {
        archive_entry *entry = archive_entry_new();
        archive_entry_copy_pathname(entry, item.path.constData());
        archive_entry_set_filetype(entry, item.type);
        archive_entry_set_perm(entry, item.perm);
        archive_entry_set_mtime(entry, 1700000000, 0);
        if (!item.link.isEmpty())
            archive_entry_copy_symlink(entry, item.link.constData());
        if (!item.hardlink.isEmpty())
            archive_entry_copy_hardlink(entry, item.hardlink.constData());
        archive_entry_set_size(entry, item.type == AE_IFREG && item.hardlink.isEmpty() ? item.data.size() : 0);
        QVERIFY(archive_write_header(a, entry) >= ARCHIVE_WARN);
        if (!item.data.isEmpty() && item.hardlink.isEmpty())
            archive_write_data(a, item.data.constData(), size_t(item.data.size()));
        archive_entry_free(entry);
    }
    archive_write_close(a);
    archive_write_free(a);
}

QByteArray read(const QString &path) {
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray("<missing>");
}

void put(const QString &path, const QByteArray &data) {
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write(data);
}

QByteArray noise(qsizetype size) {
    QByteArray data(size, Qt::Uninitialized);
    QRandomGenerator::global()->fillRange(reinterpret_cast<quint32 *>(data.data()), size / 4);
    return data;
}

QStringList tree(const QString &root) {
    QStringList paths;
    QDirIterator it(root, QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot,
                    QDirIterator::Subdirectories);
    while (it.hasNext())
        paths << QDir(root).relativeFilePath(it.next());
    paths.sort();
    return paths;
}

struct Outcome {
    QVariantMap done;
    QList<QVariantMap> events;
    QList<QVariantMap> of(const QString &type) const {
        QList<QVariantMap> found;
        for (const auto &event : events) {
            if (event.value("event") == type)
                found << event;
        }
        return found;
    }
    QStringList paths() const {
        QStringList all;
        for (const auto &batch : of("entries")) {
            for (const auto &item : batch.value("items").toList())
                all << item.toMap().value("path").toString();
        }
        all.sort();
        return all;
    }
};

using Handler = std::function<void(ArchiveJob &, const QVariantMap &)>;

Outcome runJob(const QVariantMap &request, const Handler &handler = {}) {
    ArchiveJob job;
    Outcome outcome;
    QEventLoop loop;
    QObject::connect(&job, &ArchiveJob::workerEvent, &job, [&](const QVariantMap &event) {
        outcome.events << event;
        const QString type = event.value("event").toString();
        if (handler)
            handler(job, event);
        else if (type == "need-password")
            job.cancel();
        else if (type == "conflict")
            job.resolveConflict(event.value("id").toInt(), "skip", false);
    });
    QObject::connect(&job, &ArchiveJob::finished, &loop, [&](const QVariantMap &result) {
        outcome.done = result;
        loop.quit();
    });
    QTimer::singleShot(60000, &loop, &QEventLoop::quit);
    job.start(request);
    loop.exec();
    return outcome;
}

bool sevenZipAvailable() { return !QStandardPaths::findExecutable("7z").isEmpty(); }

// A package in any format libarchive writes: format and filter by name ("raw" holds one stream).
bool writeWith(const QString &file, const char *format, const char *filter, const QList<QPair<QByteArray, QByteArray>> &files) {
    struct archive *a = archive_write_new();
    bool ok = archive_write_set_format_by_name(a, format) == ARCHIVE_OK
              && (!filter || archive_write_add_filter_by_name(a, filter) == ARCHIVE_OK)
              && archive_write_open_filename(a, QFile::encodeName(file).constData()) == ARCHIVE_OK;
    for (const auto &[name, data] : files) {
        if (!ok)
            break;
        archive_entry *entry = archive_entry_new();
        archive_entry_copy_pathname(entry, name.constData());
        archive_entry_set_filetype(entry, AE_IFREG);
        archive_entry_set_perm(entry, 0644);
        archive_entry_set_mtime(entry, 1700000000, 0);
        archive_entry_set_size(entry, data.size());
        ok = archive_write_header(a, entry) == ARCHIVE_OK
             && archive_write_data(a, data.constData(), size_t(data.size())) == data.size();
        archive_entry_free(entry);
    }
    ok = archive_write_close(a) == ARCHIVE_OK && ok;
    archive_write_free(a);
    return ok;
}

// Formats nothing here writes, put together byte by byte from their specifications (small and
// uncompressed): enough to show that each is routed to a backend that reads it.
QByteArray le16(quint16 v) {
    QByteArray bytes(2, '\0'); // (not {a, b}: that is QByteArray(size, fill))
    bytes[0] = char(v & 0xff);
    bytes[1] = char(v >> 8);
    return bytes;
}
QByteArray le32(quint32 v) { return le16(quint16(v & 0xffff)) + le16(quint16(v >> 16)); }
using Files = QList<QPair<QByteArray, QByteArray>>;

// SVR4 cpio with checksums ("070702": newc plus the byte sum of each file).
QByteArray sv4crc(const Files &files) {
    QByteArray out;
    quint32 inode = 1;
    const auto entry = [&](const QByteArray &name, const QByteArray &data, quint32 mode) {
        quint32 sum = 0;
        for (const char c : data)
            sum += quint8(c);
        out += "070702";
        for (const quint32 value : {inode++, mode, 0u, 0u, 1u, 1700000000u, quint32(data.size()), 0u, 0u, 0u, 0u,
                                    quint32(name.size() + 1), sum})
            out += QByteArray::number(value, 16).rightJustified(8, '0');
        out += name + '\0';
        while (out.size() % 4)
            out += '\0';
        out += data;
        while (out.size() % 4)
            out += '\0';
    };
    for (const auto &[name, data] : files)
        entry(name, data, 0100644);
    entry("TRAILER!!!", {}, 0);
    return out;
}

// An RPM: the lead, an empty signature and an empty header, then the cpio payload.
QByteArray rpm(const QByteArray &payload) {
    QByteArray lead(96, '\0');
    lead.replace(0, 6, QByteArray("\xed\xab\xee\xdb\x03\x00", 6));
    lead[9] = 1; // architecture
    lead.replace(10, 10, "nook-1.0-1");
    lead[77] = 1; // operating system
    lead[79] = 5; // signature type: a header
    const QByteArray emptyHeader("\x8e\xad\xe8\x01\0\0\0\0\0\0\0\0\0\0\0\0", 16);
    return lead + emptyHeader + emptyHeader + payload;
}

// A Microsoft cabinet with one uncompressed folder (names flagged UTF-8, "\" between parts).
QByteArray cab(const Files &files) {
    QByteArray entries, data;
    for (const auto &[name, content] : files) {
        QByteArray dosName = name;
        dosName.replace('/', '\\');
        entries += le32(quint32(content.size())) + le32(quint32(data.size())) + le16(0)
                   + le16(((2023 - 1980) << 9) | (11 << 5) | 14) + le16(0) + le16(0x20 | 0x80) + dosName + '\0';
        data += content;
    }
    QByteArray blocks;
    int count = 0;
    for (qsizetype at = 0; at < data.size(); at += 32768, ++count) {
        const QByteArray block = data.mid(at, 32768);
        blocks += le32(0) + le16(quint16(block.size())) + le16(quint16(block.size())) + block; // no checksum
    }
    const quint32 filesAt = 36 + 8, dataAt = filesAt + quint32(entries.size());
    const QByteArray folder = le32(dataAt) + le16(quint16(count)) + le16(0);
    const QByteArray header = QByteArray("MSCF") + le32(0) + le32(dataAt + quint32(blocks.size())) + le32(0)
                              + le32(filesAt) + le32(0) + QByteArray("\x03\x01", 2) + le16(1)
                              + le16(quint16(files.size())) + le16(0) + le16(0) + le16(0);
    return header + folder + entries + blocks;
}

// LHA, level-0 headers, stored ("-lh0-").
QByteArray lha(const Files &files) {
    const auto crc16 = [](const QByteArray &bytes) {
        quint16 crc = 0;
        for (const char c : bytes) {
            crc ^= quint8(c);
            for (int bit = 0; bit < 8; ++bit)
                crc = crc & 1 ? (crc >> 1) ^ 0xa001 : crc >> 1;
        }
        return crc;
    };
    QByteArray out;
    for (const auto &[name, content] : files) {
        const QByteArray body = QByteArray("-lh0-") + le32(quint32(content.size())) + le32(quint32(content.size()))
                                + le32((quint32(((2023 - 1980) << 9) | (11 << 5) | 14) << 16)) + char(0x20) + char(0)
                                + char(name.size()) + name + le16(crc16(content));
        quint8 sum = 0;
        for (const char c : body)
            sum += quint8(c);
        out += QByteArray(1, char(body.size())) + QByteArray(1, char(sum)) + body + content;
    }
    return out + '\0';
}

// The MIME types nook解压缩 says it opens (and so takes as the default compression software).
QStringList claimedTypes() {
    QFile desktop(QString(NOOK_SOURCE_DIR) + "/packaging/local.nook.archive.desktop");
    if (!desktop.open(QIODevice::ReadOnly))
        return {};
    QString line;
    for (const auto &each : QString::fromUtf8(desktop.readAll()).split(u'\n'))
        if (each.startsWith("MimeType="))
            line = each.mid(9);
    QStringList types;
    for (const auto &type : line.split(u';', Qt::SkipEmptyParts))
        types << QMimeDatabase().mimeTypeForName(type).name(); // aliases resolved
    return types;
}

}

class ArchiveTest : public QObject {
    Q_OBJECT
    QTemporaryDir m_dir{qEnvironmentVariable("NOOK_TEST_KEEP", QDir::tempPath()) + "/archive-test-XXXXXX"};
    bool keep = [this] { m_dir.setAutoRemove(!qEnvironmentVariableIsSet("NOOK_TEST_KEEP")); return true; }();
    QString path(const QString &name) const { return m_dir.filePath(name); }
    QString fresh(const QString &name) const {
        QDir(path(name)).removeRecursively();
        QDir().mkpath(path(name));
        return path(name);
    }
    QVariantMap extract(const QString &archive, const QString &destination, QVariantMap extra = {}) const {
        extra.insert("op", "extract");
        extra.insert("archive", archive);
        extra.insert("destination", destination);
        return extra;
    }

  private Q_SLOTS:
    void initTestCase() {
        // The legacy-name fallback follows the desktop language; these cases assume Simplified Chinese.
        qputenv("LC_ALL", "zh_CN.UTF-8");
        qputenv("LANG", "zh_CN.UTF-8");
        QVERIFY(m_dir.isValid());
        QVERIFY2(QFileInfo(ArchiveJob::workerPath()).isExecutable(), qPrintable(ArchiveJob::workerPath()));
    }

    void names() {
        QCOMPARE(stemOf("photos.tar.gz"), QString("photos"));
        QCOMPARE(stemOf("pack.part01.rar"), QString("pack"));
        QCOMPARE(stemOf("data.7z.001"), QString("data"));
        QCOMPARE(stemOf("notes.txt.gz"), QString("notes.txt"));
        QCOMPARE(stemOf("README"), QString("README"));
        QCOMPARE(numberedName("report.txt", 2, false), QString("report (2).txt"));
        QCOMPARE(numberedName("v1.2", 3, true), QString("v1.2 (3)"));
        QCOMPARE(numberedName(".bashrc", 2, false), QString(".bashrc (2)"));
        for (const auto *part : {"set.7z.001", "set.7z.002", "set.7z.003", "old.rar", "old.r00", "old.r01",
                                 "new.part1.rar", "new.part2.rar"})
            put(path(part), "x");
        const Volumes volumes = volumesOf(path("set.7z.002"));
        QVERIFY(volumes.split);
        QCOMPARE(volumes.first, path("set.7z.001"));
        QCOMPARE(volumes.parts.size(), 3);
        QCOMPARE(volumesOf(path("old.r01")).first, path("old.rar"));
        QCOMPARE(volumesOf(path("old.rar")).parts.size(), 3);
        QCOMPARE(volumesOf(path("new.part2.rar")).first, path("new.part1.rar"));
        put(path("plain.rar"), "x");
        QVERIFY(!volumesOf(path("plain.rar")).split);
        put(path("backup.2024"), "x");
        QVERIFY(!volumesOf(path("backup.2024")).split);
    }

    void encodings() {
        const QByteArray gbk("\xd6\xd0\xce\xc4\xce\xc4\xbc\xfe.txt"); // 中文文件.txt
        QVERIFY(!isUtf8(gbk));
        QVERIFY(isUtf8("plain ascii"));
        QVERIFY(isUtf8(QString("中文").toUtf8()));
        QCOMPARE(detectEncoding({"readme.txt", QString("说明.txt").toUtf8()}), QString("UTF-8"));
        QCOMPARE(detectEncoding({gbk, "readme.txt"}), QString("GB18030"));
        QCOMPARE(decodeName(gbk, "GB18030"), QString("中文文件.txt"));
        bool ok = true;
        decodeName(gbk, "UTF-8", &ok);
        QVERIFY(!ok);
    }

    void listsLegacyZipNames() {
        const QString zip = path("gbk.zip");
        writeArchive(zip, {{"\xd6\xd0\xce\xc4/", AE_IFDIR}, {"\xd6\xd0\xce\xc4/\xce\xc4\xbc\xfe.txt", AE_IFREG, "1"}}, true);
        Outcome listed = runJob({{"op", "list"}, {"archive", zip}});
        QVERIFY2(listed.done.value("ok").toBool(), qPrintable(listed.done.value("error").toMap().value("message").toString()));
        const auto info = listed.of("info").value(0);
        QCOMPARE(info.value("encoding").toString(), QString("GB18030"));
        QVERIFY(info.value("legacyNames").toBool());
        QCOMPARE(listed.paths(), QStringList({"中文", "中文/文件.txt"}));
        // An explicit choice applies to every name, even when it is the wrong one.
        Outcome big5 = runJob({{"op", "list"}, {"archive", zip}, {"encoding", "Big5"}});
        QCOMPARE(big5.of("info").value(0).value("encoding").toString(), QString("Big5"));
        QVERIFY(!big5.paths().contains("中文/文件.txt"));
        const QString out = fresh("gbk-out");
        Outcome extracted = runJob(extract(zip, out));
        QVERIFY(extracted.done.value("ok").toBool());
        QCOMPARE(read(out + "/中文/文件.txt"), QByteArray("1"));
    }

    void smartFolder() {
        const QString single = path("single.zip");
        writeArchive(single, {{"top/", AE_IFDIR}, {"top/a.txt", AE_IFREG, "a"}}, true);
        const QString many = path("many.zip");
        writeArchive(many, {{"a.txt", AE_IFREG, "a"}, {"b.txt", AE_IFREG, "b"}}, true);
        QString out = fresh("smart");
        QVERIFY(runJob(extract(single, out)).done.value("ok").toBool());
        QVERIFY(runJob(extract(many, out)).done.value("ok").toBool());
        QCOMPARE(tree(out), QStringList({"many", "many/a.txt", "many/b.txt", "top", "top/a.txt"}));
        QCOMPARE(QFileInfo(out + "/many").permissions() & QFileDevice::ReadOwner, QFileDevice::ReadOwner);
        out = fresh("never");
        QVERIFY(runJob(extract(many, out, {{"subfolder", "never"}})).done.value("ok").toBool());
        QCOMPARE(tree(out), QStringList({"a.txt", "b.txt"}));
        out = fresh("always");
        const Outcome always = runJob(extract(single, out, {{"subfolder", "always"}, {"folderName", "命名"}}));
        QCOMPARE(always.done.value("created").toStringList(), QStringList{"命名"});
        QCOMPARE(tree(out), QStringList({"命名", "命名/top", "命名/top/a.txt"}));
    }

    void conflicts() {
        const QString zip = path("conflict.zip");
        writeArchive(zip, {{"dir/", AE_IFDIR}, {"dir/new.txt", AE_IFREG, "new"}, {"dir/same.txt", AE_IFREG, "archive"}}, true);
        const QString out = fresh("conflicts");
        put(out + "/dir/same.txt", "mine");
        put(out + "/dir/old.txt", "old");
        // Ask: the folder merges, the clashing file is renamed on request.
        Outcome renamed = runJob(extract(zip, out), [](ArchiveJob &job, const QVariantMap &event) {
            if (event.value("event") == "conflict")
                job.resolveConflict(event.value("id").toInt(), "rename", false);
        });
        QVERIFY(renamed.done.value("ok").toBool());
        QCOMPARE(renamed.of("conflict").size(), 1);
        QCOMPARE(renamed.of("conflict").value(0).value("path").toString(), QString("dir/same.txt"));
        QCOMPARE(read(out + "/dir/same.txt"), QByteArray("mine"));
        QCOMPARE(read(out + "/dir/same (2).txt"), QByteArray("archive"));
        QCOMPARE(read(out + "/dir/old.txt"), QByteArray("old"));
        QCOMPARE(renamed.done.value("created").toStringList(), QStringList({"dir/new.txt", "dir/same (2).txt"}));
        // Skip leaves everything as it was.
        Outcome skipped = runJob(extract(zip, out));
        QCOMPARE(skipped.done.value("skipped").toInt(), 2);
        QCOMPARE(read(out + "/dir/same.txt"), QByteArray("mine"));
        // Overwrite with "apply to all" asks once.
        Outcome overwritten = runJob(extract(zip, out), [](ArchiveJob &job, const QVariantMap &event) {
            if (event.value("event") == "conflict")
                job.resolveConflict(event.value("id").toInt(), "overwrite", true);
        });
        QCOMPARE(overwritten.of("conflict").size(), 1);
        QCOMPARE(read(out + "/dir/same.txt"), QByteArray("archive"));
        QVERIFY(tree(out).filter(".nook-").isEmpty());
    }

    void refusesEscapes() {
        const QString outside = fresh("outside");
        const QString tar = path("hostile.tar");
        writeArchive(tar, {
                              {"../escape.txt", AE_IFREG, "x"},
                              {"/absolute.txt", AE_IFREG, "abs"},
                              {"jump", AE_IFLNK, {}, 0777, outside.toUtf8()},
                              {"jump/planted.txt", AE_IFREG, "x"},
                              {"up", AE_IFLNK, {}, 0777, ".."},
                              {"dir/", AE_IFDIR, {}, 0755},
                              {"dir/inner", AE_IFLNK, {}, 0777, "../file.txt"},
                              {"dir/hop", AE_IFLNK, {}, 0777, "../up/x"}, // escapes only through "up"
                              {"file.txt", AE_IFREG, "content", 04755},
                              {"fifo", AE_IFIFO},
                              {"hard", AE_IFREG, {}, 0644, {}, "file.txt"},
                          },
                     false);
        const QString out = fresh("hostile");
        Outcome result = runJob(extract(tar, out, {{"subfolder", "never"}}));
        QVERIFY(result.done.value("ok").toBool());
        QCOMPARE(tree(outside), QStringList());
        QVERIFY(!QFileInfo::exists(path("escape.txt")));
        QCOMPARE(tree(out), QStringList({"absolute.txt", "dir", "dir/inner", "file.txt", "hard"}));
        QCOMPARE(QFileInfo(out + "/dir/inner").symLinkTarget(), QFileInfo(out + "/file.txt").absoluteFilePath());
        struct stat st{};
        QCOMPARE(::stat(QFile::encodeName(out + "/file.txt").constData(), &st), 0);
        const mode_t mask = ::umask(022);
        ::umask(mask);
        QCOMPARE(st.st_mode & 07777, mode_t(0755 & ~mask));
        QCOMPARE(st.st_nlink, nlink_t(2));
        QCOMPARE(read(out + "/hard"), QByteArray("content"));
        QStringList warnings;
        for (const auto &warning : result.of("warning"))
            warnings << warning.value("code").toString() + ":" + warning.value("path").toString();
        warnings.sort();
        QCOMPARE(warnings, QStringList({"special:fifo", "unsafe-link:dir/hop", "unsafe-link:jump", "unsafe-link:up",
                                        "unsafe-path:../escape.txt", "unsafe-path:jump/planted.txt"}));
    }

    void zipPasswords() {
        const QString zip = path("secret.zip");
        writeArchive(zip, {{"secret.txt", AE_IFREG, "classified"}}, true, "right");
        const QString out = fresh("secret");
        QStringList asked;
        Outcome result = runJob(extract(zip, out), [&](ArchiveJob &job, const QVariantMap &event) {
            if (event.value("event") != "need-password")
                return;
            asked << (event.value("wrong").toBool() ? "wrong" : "first");
            job.sendPassword(asked.size() == 1 ? "guess" : "right");
        });
        QVERIFY(result.done.value("ok").toBool());
        QCOMPARE(asked, QStringList({"first", "wrong"}));
        QCOMPARE(read(out + "/secret.txt"), QByteArray("classified"));
        // Giving up on the password cancels, and nothing is left behind.
        const QString empty = fresh("secret-cancel");
        Outcome cancelled = runJob(extract(zip, empty));
        QVERIFY(cancelled.done.value("cancelled").toBool());
        QCOMPARE(tree(empty), QStringList());
    }

    void createsAndReadsEveryFormat() {
        if (!sevenZipAvailable())
            QSKIP("7z is not installed");
        const QString source = fresh("source");
        put(source + "/project/readme.md", "# 说明");
        put(source + "/project/src/main.c", "int main(){}");
        QVERIFY(QFile::link("readme.md", source + "/project/link.md"));
        put(source + "/single.txt", noise(4096).toBase64());
        const QString outputs = fresh("created");
        for (const QString format : {"zip", "7z", "tar", "tar.gz", "tar.bz2", "tar.xz", "tar.zst"}) {
            const QString output = outputs + "/project." + format;
            Outcome made = runJob({{"op", "create"}, {"format", format}, {"output", output},
                                   {"items", QStringList{source + "/project"}}, {"testAfter", true},
                                   {"options", QVariantMap{{"level", 5}}}});
            QVERIFY2(made.done.value("ok").toBool(), qPrintable(format + ": " + made.done.value("error").toMap().value("message").toString()));
            QCOMPARE(made.done.value("output").toString(), output);
            Outcome listed = runJob({{"op", "list"}, {"archive", output}});
            QVERIFY2(listed.paths().contains("project/src/main.c"), qPrintable(format + ": " + listed.paths().join(", ")));
            QVERIFY(listed.paths().contains("project/link.md"));
            const QString out = fresh("roundtrip-" + format);
            QVERIFY(runJob(extract(output, out)).done.value("ok").toBool());
            QCOMPARE(read(out + "/project/readme.md"), QByteArray("# 说明"));
            const QFileInfo link(out + "/project/link.md");
            // Links stay links in 7z and tar; ZIP stores what they point to.
            QCOMPARE(link.isSymLink(), format != "zip");
            QCOMPARE(read(link.filePath()), QByteArray("# 说明"));
        }
        for (const QString format : {"gz", "xz", "zst", "bz2"}) {
            const QString output = outputs + "/single.txt." + format;
            Outcome made = runJob({{"op", "create"}, {"format", format}, {"output", output},
                                   {"items", QStringList{source + "/single.txt"}}});
            QVERIFY2(made.done.value("ok").toBool(), qPrintable(format));
            Outcome listed = runJob({{"op", "list"}, {"archive", output}});
            QCOMPARE(listed.paths(), QStringList{"single.txt"});
            QVERIFY(listed.of("info").value(0).value("stream").toBool());
            const QString out = fresh("stream-" + format);
            QVERIFY(runJob(extract(output, out)).done.value("ok").toBool());
            QCOMPARE(read(out + "/single.txt"), read(source + "/single.txt"));
        }
        Outcome refused = runJob({{"op", "create"}, {"format", "gz"}, {"output", outputs + "/two.gz"},
                                  {"items", QStringList{source + "/single.txt", source + "/project"}}});
        QCOMPARE(refused.done.value("error").toMap().value("code").toString(), QString("options"));
    }

    void encryptedSevenZip() {
        if (!sevenZipAvailable())
            QSKIP("7z is not installed");
        const QString source = fresh("enc-source");
        put(source + "/a.txt", "alpha");
        put(source + "/b/c.txt", "gamma");
        const QString output = path("hidden.7z");
        QFile::remove(output);
        Outcome made = runJob({{"op", "create"}, {"format", "7z"}, {"output", output}, {"password", "pw 密码"},
                               {"items", QStringList{source + "/a.txt", source + "/b"}},
                               {"options", QVariantMap{{"encryptNames", true}}}});
        QVERIFY(made.done.value("ok").toBool());
        // Names are hidden too: listing needs the password.
        Outcome refused = runJob({{"op", "list"}, {"archive", output}});
        QVERIFY(refused.done.value("cancelled").toBool());
        QCOMPARE(refused.paths(), QStringList());
        int asked = 0;
        Outcome listed = runJob({{"op", "list"}, {"archive", output}}, [&](ArchiveJob &job, const QVariantMap &event) {
            if (event.value("event") == "need-password")
                job.sendPassword(++asked == 1 ? "nope" : "pw 密码");
        });
        QCOMPARE(asked, 2);
        QCOMPARE(listed.paths(), QStringList({"a.txt", "b", "b/c.txt"}));
        QVERIFY(listed.of("info").value(0).value("headerEncrypted").toBool());
        const QString out = fresh("enc-out");
        Outcome extracted = runJob(extract(output, out, {{"password", "pw 密码"}}));
        QVERIFY(extracted.done.value("ok").toBool());
        QCOMPARE(read(out + "/hidden/b/c.txt"), QByteArray("gamma"));
        Outcome tested = runJob({{"op", "test"}, {"archive", output}, {"password", "pw 密码"}});
        QVERIFY(tested.done.value("ok").toBool());
        // ZIP: AES-256 by default.
        const QString zip = path("hidden.zip");
        QFile::remove(zip);
        QVERIFY(runJob({{"op", "create"}, {"format", "zip"}, {"output", zip}, {"password", "zz"},
                        {"items", QStringList{source + "/a.txt"}}})
                    .done.value("ok").toBool());
        QProcess sevenZip;
        sevenZip.start("7z", {"l", "-slt", zip});
        QVERIFY(sevenZip.waitForFinished());
        QVERIFY(sevenZip.readAllStandardOutput().contains("AES-256"));
    }

    void volumes() {
        if (!sevenZipAvailable())
            QSKIP("7z is not installed");
        const QString source = fresh("vol-source");
        const QByteArray data = noise(300 * 1024);
        put(source + "/big.bin", data);
        const QString outputs = fresh("vol-out");
        Outcome made = runJob({{"op", "create"}, {"format", "7z"}, {"output", outputs + "/set.7z"},
                               {"items", QStringList{source + "/big.bin"}},
                               {"options", QVariantMap{{"volumeSize", 100 * 1024}, {"level", 0}}}});
        QVERIFY(made.done.value("ok").toBool());
        const QStringList parts = made.done.value("created").toStringList();
        QVERIFY(parts.size() >= 3);
        QCOMPARE(parts.first(), QString("set.7z.001"));
        QVERIFY(parts.filter(QRegularExpression("^set\\.7z\\.\\d{3}$")).size() == parts.size());
        // Opening any part opens the set.
        const QString out = fresh("vol-extract");
        Outcome extracted = runJob(extract(outputs + "/set.7z.003", out));
        QVERIFY(extracted.done.value("ok").toBool());
        QCOMPARE(read(out + "/big.bin"), data);
        // A second set with the same name is renamed as a whole.
        Outcome again = runJob({{"op", "create"}, {"format", "7z"}, {"output", outputs + "/set.7z"},
                                {"items", QStringList{source + "/big.bin"}}, {"conflict", "rename"},
                                {"options", QVariantMap{{"volumeSize", 100 * 1024}, {"level", 0}}}});
        QCOMPARE(again.done.value("created").toStringList().value(0), QString("set (2).7z.001"));
        QFile::remove(outputs + "/set.7z.002");
        Outcome missing = runJob(extract(outputs + "/set.7z.001", fresh("vol-missing")));
        QVERIFY(!missing.done.value("ok").toBool());
    }

    void selectionPrefixAndFlatten() {
        const QString zip = path("select.zip");
        writeArchive(zip, {{"d/e/f.txt", AE_IFREG, "f"}, {"d/g.txt", AE_IFREG, "g"}, {"h.txt", AE_IFREG, "h"}}, true);
        QString out = fresh("select");
        QVERIFY(runJob(extract(zip, out, {{"entries", QStringList{"d/e"}}, {"prefix", "d"}})).done.value("ok").toBool());
        QCOMPARE(tree(out), QStringList({"e", "e/f.txt"}));
        out = fresh("flatten");
        QVERIFY(runJob(extract(zip, out, {{"flatten", true}, {"subfolder", "never"}})).done.value("ok").toBool());
        QCOMPARE(tree(out), QStringList({"f.txt", "g.txt", "h.txt"}));
        if (!sevenZipAvailable())
            return;
        const QString source = fresh("select-source");
        put(source + "/d/e/f.txt", "f");
        put(source + "/d/g.txt", "g");
        const QString seven = path("select.7z");
        QFile::remove(seven);
        QVERIFY(runJob({{"op", "create"}, {"format", "7z"}, {"output", seven}, {"items", QStringList{source + "/d"}}})
                    .done.value("ok").toBool());
        out = fresh("select-7z");
        QVERIFY(runJob(extract(seven, out, {{"entries", QStringList{"d/e"}}, {"prefix", "d"}})).done.value("ok").toBool());
        QCOMPARE(tree(out), QStringList({"e", "e/f.txt"}));
    }

    void cancelKeepsOnlyWholeFiles() {
        const QString tar = path("large.tar");
        QList<Entry> entries;
        for (int i = 0; i < 6; ++i)
            entries << Entry{"part" + QByteArray::number(i) + ".bin", AE_IFREG, noise(24 << 20)};
        writeArchive(tar, entries, false);
        const QString out = fresh("cancel");
        Outcome result = runJob(extract(tar, out, {{"subfolder", "never"}}), [](ArchiveJob &job, const QVariantMap &event) {
            if (event.value("event") == "progress")
                job.cancel();
        });
        QVERIFY(result.done.value("cancelled").toBool());
        for (const auto &name : tree(out))
            QCOMPARE(QFileInfo(out + u'/' + name).size(), qint64(24 << 20));
        QCOMPARE(result.done.value("created").toStringList(), tree(out));
        // Stopping mid-way, at a password prompt: what was complete stays, nothing partial does.
        if (!sevenZipAvailable())
            QSKIP("7z is not installed");
        const QString zip = path("late-password.zip");
        const QString source = fresh("late-source");
        for (const auto *name : {"1.bin", "2.bin", "3.bin", "4.bin"})
            put(source + u'/' + name, noise(1 << 20));
        QCOMPARE(QProcess::execute("7z", {"a", "-bso0", zip, source + "/1.bin", source + "/2.bin", source + "/3.bin"}), 0);
        QCOMPARE(QProcess::execute("7z", {"a", "-bso0", "-plate", "-mem=AES256", zip, source + "/4.bin"}), 0);
        const QString partial = fresh("cancel-partial");
        Outcome stopped = runJob(extract(zip, partial, {{"subfolder", "never"}}));
        QVERIFY(stopped.done.value("cancelled").toBool());
        QCOMPARE(tree(partial), QStringList({"1.bin", "2.bin", "3.bin"}));
        QCOMPARE(stopped.done.value("created").toStringList(), tree(partial));
        for (const auto &name : tree(partial))
            QCOMPARE(QFileInfo(partial + u'/' + name).size(), qint64(1 << 20));
    }

    void reportsDamage() {
        const QString zip = path("damaged.zip");
        writeArchive(zip, {{"data.bin", AE_IFREG, noise(64 * 1024)}}, true);
        QFile file(zip);
        QVERIFY(file.open(QIODevice::ReadWrite));
        file.seek(2000);
        file.write(QByteArray(64, '\0'));
        file.close();
        Outcome tested = runJob({{"op", "test"}, {"archive", zip}});
        QVERIFY(!tested.done.value("ok").toBool());
        QVERIFY(!tested.done.value("errors").toList().isEmpty());
        Outcome missing = runJob({{"op", "list"}, {"archive", path("nothing.zip")}});
        QCOMPARE(missing.done.value("error").toMap().value("code").toString(), QString("not-found"));
        put(path("text.zip"), "not an archive at all");
        Outcome unknown = runJob({{"op", "list"}, {"archive", path("text.zip")}});
        QVERIFY(!unknown.done.value("ok").toBool());
    }

    void sandboxAndCleanup() {
        const QString zip = path("single.zip");
        Outcome result = runJob(extract(zip, fresh("sandbox")));
        QVERIFY(result.of("started").value(0).value("sandbox").toBool());
        // A worker killed outright leaves no staging folder behind.
        const QString tar = path("large.tar");
        const QString out = fresh("killed");
        {
            ArchiveJob job;
            QEventLoop loop;
            QObject::connect(&job, &ArchiveJob::workerEvent, &loop, [&](const QVariantMap &event) {
                if (event.value("event") == "progress")
                    loop.quit();
            });
            QTimer::singleShot(20000, &loop, &QEventLoop::quit);
            job.start(extract(tar, out));
            loop.exec();
        }
        QCOMPARE(tree(out), QStringList());
    }

    // Changing packages (phase 5).
    QStringList listed(const QString &archive, const QString &password = {}) {
        QVariantMap request{{"op", "list"}, {"archive", archive}};
        if (!password.isEmpty())
            request.insert("password", password);
        return runJob(request).paths();
    }
    QStringList leftovers(const QString &folder) {
        return QDir(folder).entryList({".nook-*"}, QDir::AllEntries | QDir::Hidden);
    }

    void modifyEachFormat_data() {
        QTest::addColumn<QString>("format");
        QTest::newRow("zip") << "zip";
        QTest::newRow("7z") << "7z";
        QTest::newRow("tar.gz") << "tar.gz";
        QTest::newRow("tar") << "tar";
    }
    void modifyEachFormat() {
        QFETCH(QString, format);
        if (!sevenZipAvailable() && (format == "zip" || format == "7z"))
            QSKIP("7z is not installed");
        const QString source = fresh("modify-source-" + format);
        put(source + "/a.txt", "alpha");
        put(source + "/dir/b.txt", "bravo");
        const QString folder = fresh("modify-" + format);
        const QString archive = folder + "/pack." + format;
        Outcome made = runJob({{"op", "create"}, {"format", format}, {"output", archive},
                               {"items", QStringList{source + "/a.txt", source + "/dir"}}});
        QVERIFY2(made.done.value("ok").toBool(), qPrintable(made.done.value("error").toMap().value("message").toString()));

        const QString extra = fresh("modify-extra-" + format);
        put(extra + "/new.txt", "new");
        put(extra + "/newdir/c.txt", "charlie");
        put(extra + "/a.txt", "replacement");
        // Into a folder of the package.
        Outcome added = runJob({{"op", "add"}, {"archive", archive}, {"prefix", "dir"},
                                {"items", QStringList{extra + "/new.txt", extra + "/newdir"}}});
        QVERIFY2(added.done.value("ok").toBool(), qPrintable(added.done.value("error").toMap().value("message").toString()));
        QStringList paths = listed(archive);
        QVERIFY2(paths.contains("dir/new.txt") && paths.contains("dir/newdir/c.txt"), qPrintable(paths.join(", ")));

        // A name that is taken: asked about; "keep both" numbers the new one.
        Outcome renamed = runJob({{"op", "add"}, {"archive", archive}, {"items", QStringList{extra + "/a.txt"}}},
                                 [](ArchiveJob &job, const QVariantMap &event) {
                                     if (event.value("event") == "conflict")
                                         job.resolveConflict(event.value("id").toInt(), "rename", false);
                                 });
        QVERIFY(renamed.done.value("ok").toBool());
        QCOMPARE(renamed.of("conflict").size(), 1);
        QCOMPARE(renamed.of("conflict").first().value("path").toString(), QStringLiteral("a.txt"));
        QVERIFY(listed(archive).contains("a (2).txt"));
        Outcome replaced = runJob({{"op", "add"}, {"archive", archive}, {"conflict", "overwrite"}, {"items", QStringList{extra + "/a.txt"}}});
        QVERIFY(replaced.done.value("ok").toBool());

        Outcome moved = runJob({{"op", "rename"}, {"archive", archive}, {"from", "dir/newdir"}, {"to", "dir/moved"}});
        QVERIFY2(moved.done.value("ok").toBool(), qPrintable(moved.done.value("error").toMap().value("message").toString()));
        paths = listed(archive);
        QVERIFY(paths.contains("dir/moved/c.txt"));
        QVERIFY(!paths.contains("dir/newdir/c.txt"));
        // Renaming onto an existing name fails and leaves the package alone.
        const QByteArray before = read(archive);
        Outcome clash = runJob({{"op", "rename"}, {"archive", archive}, {"from", "dir/moved"}, {"to", "dir/new.txt"}});
        QCOMPARE(clash.done.value("error").toMap().value("code").toString(), QStringLiteral("exists"));
        QCOMPARE(read(archive), before);

        Outcome removed = runJob({{"op", "delete"}, {"archive", archive}, {"entries", QStringList{"dir/moved", "a (2).txt"}}});
        QVERIFY(removed.done.value("ok").toBool());
        const QString out = fresh("modify-out-" + format);
        QVERIFY(runJob(extract(archive, out, {{"subfolder", "never"}})).done.value("ok").toBool());
        QCOMPARE(tree(out), QStringList({"a.txt", "dir", "dir/b.txt", "dir/new.txt"}));
        QCOMPARE(read(out + "/a.txt"), QByteArray("replacement"));
        QCOMPARE(read(out + "/dir/new.txt"), QByteArray("new"));
        QCOMPARE(leftovers(folder), QStringList());
    }

    void modifyEncryptedSevenZip() {
        if (!sevenZipAvailable())
            QSKIP("7z is not installed");
        const QString source = fresh("enc-modify-source");
        put(source + "/secret.txt", "s");
        put(source + "/more.txt", "m");
        const QString folder = fresh("enc-modify");
        const QString archive = folder + "/locked.7z";
        QVERIFY(runJob({{"op", "create"}, {"format", "7z"}, {"output", archive}, {"password", "pw"},
                        {"items", QStringList{source + "/secret.txt"}}}).done.value("ok").toBool());
        // Names are hidden: changing it needs the password, asked for (wrong first).
        int asked = 0;
        Outcome added = runJob({{"op", "add"}, {"archive", archive}, {"items", QStringList{source + "/more.txt"}}},
                               [&](ArchiveJob &job, const QVariantMap &event) {
                                   if (event.value("event") == "need-password")
                                       job.sendPassword(++asked == 1 ? "nope" : "pw");
                               });
        QVERIFY2(added.done.value("ok").toBool(), qPrintable(added.done.value("error").toMap().value("message").toString()));
        QVERIFY(asked >= 2);
        QVERIFY(listed(archive).isEmpty()); // still hidden without the password
        QCOMPARE(listed(archive, "pw"), QStringList({"more.txt", "secret.txt"}));
        // The added file is encrypted too.
        const QString out = fresh("enc-modify-out");
        QVERIFY(!runJob(extract(archive, out, {{"entries", QStringList{"more.txt"}}, {"password", "wrong"}})).done.value("ok").toBool());
        QVERIFY(runJob(extract(archive, out, {{"entries", QStringList{"more.txt"}}, {"password", "pw"}})).done.value("ok").toBool());
        QCOMPARE(read(out + "/more.txt"), QByteArray("m"));
        QVERIFY(runJob({{"op", "delete"}, {"archive", archive}, {"password", "pw"}, {"entries", QStringList{"secret.txt"}}}).done.value("ok").toBool());
        QCOMPARE(listed(archive, "pw"), QStringList({"more.txt"}));
    }

    void modifyRefusesAndKeepsOriginal() {
        const QString folder = fresh("refuse");
        put(folder + "/fake.rar", QByteArray("Rar!\x1a\x07\x01\x00", 8) + noise(100));
        Outcome rar = runJob({{"op", "delete"}, {"archive", folder + "/fake.rar"}, {"entries", QStringList{"x"}}});
        QCOMPARE(rar.done.value("error").toMap().value("code").toString(), QStringLiteral("unsupported"));
        // Cancelled while asking: nothing changes, nothing is left behind.
        const QString zip = folder + "/keep.zip";
        writeArchive(zip, {{"a.txt", AE_IFREG, "a"}}, true);
        put(folder + "/a.txt", "other");
        const QByteArray before = read(zip);
        Outcome stopped = runJob({{"op", "add"}, {"archive", zip}, {"items", QStringList{folder + "/a.txt"}}},
                                 [](ArchiveJob &job, const QVariantMap &event) {
                                     if (event.value("event") == "conflict")
                                         job.cancel();
                                 });
        QVERIFY(stopped.done.value("cancelled").toBool());
        QCOMPARE(read(zip), before);
        QTRY_COMPARE(leftovers(folder), QStringList());
        Outcome missing = runJob({{"op", "delete"}, {"archive", zip}, {"entries", QStringList{"nothing"}}});
        QCOMPARE(missing.done.value("error").toMap().value("code").toString(), QStringLiteral("not-found"));
        QCOMPARE(read(zip), before);
    }

    void convertKeepsNamesAndEncrypts() {
        if (!sevenZipAvailable())
            QSKIP("7z is not installed");
        // A legacy ZIP (GBK names) to a 7z with hidden names; the original stays.
        const QString folder = fresh("convert");
        const QString zip = folder + "/旧.zip";
        writeArchive(zip, {{QByteArray("\xd6\xd0\xce\xc4/\xb1\xa8\xb8\xe6.txt"), AE_IFREG, "report"}}, true);
        const QByteArray before = read(zip);
        Outcome converted = runJob({{"op", "convert"}, {"archive", zip}, {"format", "7z"}, {"output", folder + "/新.7z"},
                                    {"newPassword", "pw"}, {"options", QVariantMap{{"encryptNames", true}}}});
        QVERIFY2(converted.done.value("ok").toBool(), qPrintable(converted.done.value("error").toMap().value("message").toString()));
        QCOMPARE(listed(folder + "/新.7z", "pw"), QStringList({"中文", "中文/报告.txt"}));
        QCOMPARE(read(zip), before);
        QCOMPARE(leftovers(folder), QStringList());
        // And a tar.gz to ZIP.
        const QString source = fresh("convert-source");
        put(source + "/x.txt", "x");
        QVERIFY(runJob({{"op", "create"}, {"format", "tar.gz"}, {"output", folder + "/t.tar.gz"}, {"items", QStringList{source + "/x.txt"}}}).done.value("ok").toBool());
        QVERIFY(runJob({{"op", "convert"}, {"archive", folder + "/t.tar.gz"}, {"format", "zip"}, {"output", folder + "/t.zip"}}).done.value("ok").toBool());
        QCOMPARE(listed(folder + "/t.zip"), QStringList({"x.txt"}));
    }

    // Every format the desktop file claims that can be made here opens, and the type the desktop
    // gives such a file is among the claimed ones (so "设为默认" covers it).
    void readsEveryClaimedFormat() {
        const QStringList claimed = claimedTypes();
        QVERIFY(claimed.size() > 40);
        const QByteArray text = "内容 " + noise(3000).toBase64();
        const QList<QPair<QByteArray, QByteArray>> files{{"docs/说明.txt", text}, {"docs/b.txt", "b"}};
        struct Sample {
            QString name;
            const char *format;
            const char *filter;
        };
        const QString base = fresh("claimed");
        const QList<Sample> samples{
            {"a.cpio", "newc", nullptr},   {"odc.cpio", "odc", nullptr}, {"a.iso", "iso9660", nullptr},
            {"a.xar", "xar", nullptr},     {"a.tar.Z", "pax", "compress"},  {"a.taz", "ustar", "compress"},
            {"a.tar.lz", "pax", "lzip"},   {"a.tar.lz4", "pax", "lz4"},     {"a.tar.lzma", "pax", "lzma"},
            {"a.apk", "zip", nullptr},     {"a.jar", "zip", nullptr},       {"a.gtar", "gnutar", nullptr},
            {"a.ar", "arsvr4", nullptr},
        };
        const auto check = [&](const QString &file, const QStringList &expected, const QString &member, const QByteArray &data) {
            const QString type = QMimeDatabase().mimeTypeForFile(file).name();
            QVERIFY2(claimed.contains(type), qPrintable(QFileInfo(file).fileName() + " is " + type + ", not claimed"));
            Outcome listed = runJob({{"op", "list"}, {"archive", file}});
            QVERIFY2(listed.done.value("ok").toBool(), qPrintable(file + ": " + listed.done.value("error").toMap().value("message").toString()));
            for (const auto &path : expected)
                QVERIFY2(listed.paths().contains(path), qPrintable(QFileInfo(file).fileName() + ": " + listed.paths().join(", ")));
            const QString out = fresh("claimed-out/" + QFileInfo(file).fileName());
            QVERIFY(runJob(extract(file, out, {{"subfolder", "never"}})).done.value("ok").toBool());
            QCOMPARE(read(out + "/" + member), data);
        };
        for (const auto &sample : samples) {
            const QString file = base + "/" + sample.name;
            // ar holds no directories.
            const bool flat = QByteArray(sample.format) == "arsvr4";
            const QList<QPair<QByteArray, QByteArray>> flatFiles{{"b.txt", "b"}, {"c.txt", text}};
            QVERIFY2(writeWith(file, sample.format, sample.filter, flat ? flatFiles : files), qPrintable(sample.name));
            if (flat)
                check(file, {"b.txt", "c.txt"}, "c.txt", text);
            else
                check(file, {"docs/说明.txt", "docs/b.txt"}, "docs/说明.txt", text);
        }
        // A Debian package is an ar archive of tarballs.
        const QString control = base + "/control.tar.gz", data = base + "/data.tar.xz";
        QVERIFY(writeWith(control, "ustar", "gzip", {{"./control", "Package: nook\n"}}));
        QVERIFY(writeWith(data, "ustar", "xz", {{"./usr/bin/nook", "binary"}}));
        QFile controlFile(control), dataFile(data);
        QVERIFY(controlFile.open(QIODevice::ReadOnly) && dataFile.open(QIODevice::ReadOnly));
        QVERIFY(writeWith(base + "/nook.deb", "arsvr4", nullptr,
                          {{"debian-binary", "2.0\n"}, {"control.tar.gz", controlFile.readAll()}, {"data.tar.xz", dataFile.readAll()}}));
        check(base + "/nook.deb", {"debian-binary", "control.tar.gz", "data.tar.xz"}, "debian-binary", "2.0\n");
        // Single compressed streams.
        const QList<QPair<QString, const char *>> streams{
            {"notes.txt.Z", "compress"}, {"notes.txt.lz", "lzip"}, {"notes.txt.lz4", "lz4"}, {"notes.txt.lzma", "lzma"}, {"page.ps.gz", "gzip"}};
        for (const auto &[name, filter] : streams) {
            const QString file = base + "/" + name;
            const QString inner = QFileInfo(name).completeBaseName();
            QVERIFY2(writeWith(file, "raw", filter, {{inner.toUtf8(), text}}), qPrintable(name));
            check(file, {inner}, inner, text);
        }
        // Hand-made: SVR4 cpio with checksums, RPM (a cpio inside), cabinet, LHA.
        put(base + "/a.sv4crc", sv4crc(files));
        check(base + "/a.sv4crc", {"docs/说明.txt", "docs/b.txt"}, "docs/说明.txt", text);
        const QString payload = base + "/payload.cpio.gz";
        QVERIFY(writeWith(payload, "newc", "gzip", files));
        QFile payloadFile(payload);
        QVERIFY(payloadFile.open(QIODevice::ReadOnly));
        put(base + "/nook-1.0-1.x86_64.rpm", rpm(payloadFile.readAll()));
        check(base + "/nook-1.0-1.x86_64.rpm", {"docs/说明.txt", "docs/b.txt"}, "docs/说明.txt", text);
        put(base + "/a.cab", cab(files));
        check(base + "/a.cab", {"docs/说明.txt", "docs/b.txt"}, "docs/说明.txt", text);
        put(base + "/a.lzh", lha({{"b.txt", "b"}, {"c.txt", text}}));
        check(base + "/a.lzh", {"b.txt", "c.txt"}, "c.txt", text);
        if (sevenZipAvailable()) {
            const QString wim = base + "/a.wim";
            const QString source = fresh("claimed-wim");
            put(source + "/docs/说明.txt", text);
            QProcess sevenZip;
            sevenZip.setWorkingDirectory(source);
            sevenZip.start("7z", {"a", "-twim", "-bso0", "-bsp0", wim, "docs"});
            QVERIFY(sevenZip.waitForFinished(30000) && sevenZip.exitCode() == 0);
            check(wim, {"docs/说明.txt"}, "docs/说明.txt", text);
        }
    }
};

QTEST_GUILESS_MAIN(ArchiveTest)
#include "archive_test.moc"
