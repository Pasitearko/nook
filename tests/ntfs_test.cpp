#include "ntfs.h"
#include "ntfs_image.h"
#include "searchindex.h"
#include <QRandomGenerator>
#include <QTemporaryDir>
#include <QtTest>
#include <fcntl.h>
#include <set>
#include <sqlite3.h>

using namespace nook::ntfs;

class NtfsTest : public QObject {
    Q_OBJECT
    QTemporaryDir dir{QDir::currentPath() + "/ntfs-XXXXXX"}; // not under /tmp, which the index skips
    QString imagePath;

    bool write(const QByteArray &image) {
        // Rewritten in place: a descriptor opened earlier sees the new content, like a partition.
        QFile file(imagePath);
        return file.open(QIODevice::WriteOnly | QIODevice::Truncate) && file.write(image) == image.size();
    }
    Volume *open(const QByteArray &image) {
        if (!write(image))
            return nullptr;
        const int fd = ::open(QFile::encodeName(imagePath).constData(), O_RDONLY | O_CLOEXEC);
        return fd < 0 ? nullptr : new Volume(fd);
    }

  private Q_SLOTS:
    void initTestCase() {
        QVERIFY(dir.isValid());
        imagePath = dir.path() + "/volume.img";
    }

    void runLists() {
        // 8 clusters at 4, then 8 at 40 (+36), then a sparse run of 2.
        const auto encoded = testimage::runs({{8, 4}, {8, 40}, {2, -1}});
        std::vector<Run> runs;
        QVERIFY(decodeRuns(encoded.constData(), encoded.size(), 0, runs));
        QCOMPARE(runs.size(), size_t(3));
        QCOMPARE(runs[1].lcn, std::int64_t(40));
        QCOMPARE(runs[1].vcn, std::int64_t(8));
        QCOMPARE(runs[2].lcn, std::int64_t(-1));
        // Negative deltas move backwards.
        const auto back = testimage::runs({{1, 100}, {1, 3}});
        runs.clear();
        QVERIFY(decodeRuns(back.constData(), back.size(), 0, runs));
        QCOMPARE(runs[1].lcn, std::int64_t(3));
        // Truncated input is refused rather than read past.
        runs.clear();
        QVERIFY(!decodeRuns(encoded.constData(), 2, 0, runs));
    }

    void enumerateVolume() {
        std::unique_ptr<Volume> volume(open(testimage::build()));
        QVERIFY(volume);
        QString error;
        QVERIFY2(volume->open(&error), qPrintable(error));
        QCOMPARE(volume->recordCount(), std::uint64_t(64)); // both $MFT runs were followed
        std::vector<Entry> entries;
        Journal journal;
        QVERIFY2(volume->enumerate([&](const Entry &e) { entries.push_back(e); }, journal, nullptr, &error),
                 qPrintable(error));
        std::map<QString, Entry> byPath;
        for (const auto &entry : entries)
            byPath[entry.path] = entry;
        QStringList paths = QStringList();
        for (const auto &[path, entry] : byPath)
            paths << path;
        const QStringList expected{"Users",
                                   "Users/alice",
                                   "Users/alice/a long name.txt",
                                   "Users/alice/a.txt",
                                   "Users/alice/big.bin",
                                   "Users/alice/extended.txt",
                                   "Users/alice/notes.txt",
                                   "Users/b.txt"};
        QCOMPARE(paths, expected); // no metafiles, Windows, $Recycle.Bin, deleted, orphaned or DOS names
        QCOMPARE(byPath["Users/alice/notes.txt"].size, qint64(5));
        QCOMPARE(byPath["Users/alice/notes.txt"].mtime, testimage::kUnixTime);
        QCOMPARE(byPath["Users/alice/big.bin"].size, qint64(1000000));
        QVERIFY(byPath["Users/alice"].folder);
        QCOMPARE(byPath["Users/alice/a.txt"].record, byPath["Users/b.txt"].record); // hard link
        QCOMPARE(byPath["Users/alice/extended.txt"].record, std::uint64_t(37));
        QVERIFY(journal.present);
        QCOMPARE(journal.record, std::uint64_t(40));
        QCOMPARE(journal.id, std::uint64_t(0x1234));
        QCOMPARE(journal.lowest, qint64(4096));
        QCOMPARE(journal.next, testimage::kJournalEnd);
    }

    void journalChanges() {
        std::unique_ptr<Volume> volume(open(testimage::build()));
        QVERIFY(volume && volume->open(nullptr));
        Journal journal;
        QString error;
        QVERIFY2(volume->journal(40, journal, &error), qPrintable(error));
        std::vector<Change> changes;
        // From 0: the sparse head below LowestValidUsn is skipped, not parsed.
        QVERIFY2(volume->changes(journal, 0, journal.next, changes, &error), qPrintable(error));
        QCOMPARE(changes.size(), size_t(3));
        QCOMPARE(changes[0].record, std::uint64_t(26));
        QCOMPARE(changes[0].name, QStringLiteral("notes.txt"));
        QCOMPARE(changes[0].reason, std::uint32_t(0x100));
        QCOMPARE(changes[1].record, std::uint64_t(34));
        QCOMPARE(changes[1].parent, std::uint64_t(25));
        QCOMPARE(changes[2].record, std::uint64_t(33)); // a version 3 record
        QCOMPARE(changes[2].name, QStringLiteral("a long name.txt"));
        // Resuming from the end yields nothing.
        changes.clear();
        QVERIFY(volume->changes(journal, journal.next, journal.next, changes, &error));
        QVERIFY(changes.empty());
        // $Max may still say 0 after Windows freed the head of $J: the first allocated cluster wins.
        std::unique_ptr<Volume> trimmed(open(testimage::build(0, 0)));
        QVERIFY(trimmed && trimmed->open(nullptr));
        Journal head;
        QVERIFY(trimmed->journal(40, head, &error));
        QCOMPARE(head.lowest, qint64(4096));
        // The wrong record is not mistaken for the journal.
        QVERIFY(!volume->journal(26, journal, &error));
    }

    void currentState() {
        std::unique_ptr<Volume> volume(open(testimage::build()));
        QVERIFY(volume && volume->open(nullptr));
        auto links = volume->current(32);
        std::set<QString> names;
        for (const auto &entry : links)
            names.insert(entry.path);
        QCOMPARE(names, (std::set<QString>{"Users/alice/a.txt", "Users/b.txt"}));
        QVERIFY(volume->current(34).empty()); // deleted
        QVERIFY(volume->current(30).empty()); // inside Windows/
        QVERIFY(volume->current(35).empty()); // parent reference is stale
        const auto extended = volume->current(37); // name lives in an extension record
        QCOMPARE(extended.size(), size_t(1));
        QCOMPARE(extended[0].path, QStringLiteral("Users/alice/extended.txt"));
        QVERIFY(volume->current(5).empty() && volume->current(0).empty());
    }

    void indexFollowsJournal() {
        // The index is built from the file table once, then kept current by replaying the journal.
        QVERIFY(write(testimage::build(0)));
        QTemporaryDir root(dir.path() + "/mnt-XXXXXX");
        const QString database = dir.path() + "/db/index.sqlite"; // the index skips its own folder
        SearchIndex index(database);
        index.setRoots({root.path()});
        index.setNtfsJournal(true);
        int opened = 0;
        index.setVolumeOpener([&](const QString &key, QString *) {
            ++opened;
            return key == root.path() ? ::open(QFile::encodeName(imagePath).constData(), O_RDONLY | O_CLOEXEC) : -1;
        });
        auto scan = [&] {
            index.start();
            QTRY_VERIFY_WITH_TIMEOUT(!index.scanning(), 10000);
        };
        auto found = [&](const QString &text) {
            index.query(text);
            [&] { QTRY_VERIFY(!index.model()->loading()); }();
            QStringList paths;
            for (int i = 0; i < index.model()->count(); ++i)
                paths << QUrl(index.model()->entry(i)["fileUri"].toString()).toLocalFile().mid(root.path().size() + 1);
            paths.sort();
            return paths;
        };
        auto built = [&] {
            sqlite3 *db = nullptr;
            sqlite3_open_v2(QFile::encodeName(database).constData(), &db, SQLITE_OPEN_READONLY, nullptr);
            sqlite3_stmt *stmt = nullptr;
            sqlite3_prepare_v2(db, "SELECT value FROM meta WHERE key=?", -1, &stmt, nullptr);
            const QByteArray key = "ntfs:" + QFile::encodeName(root.path()) + ":built";
            sqlite3_bind_text(stmt, 1, key.constData(), key.size(), SQLITE_TRANSIENT);
            const qint64 value = sqlite3_step(stmt) == SQLITE_ROW ? sqlite3_column_int64(stmt, 0) : 0;
            sqlite3_finalize(stmt);
            sqlite3_close(db);
            return value;
        };
        scan();
        // (Only watching fails: the image's folders do not exist under the mount point.)
        QVERIFY2(!index.warnings().join(' ').contains(QStringLiteral("逐目录")), qPrintable(index.warnings().join(' ')));
        QCOMPARE(found("notes"), QStringList{"Users/alice/notes.txt"});
        QCOMPARE(found(".txt"), (QStringList{"Users/alice/a long name.txt", "Users/alice/a.txt",
                                             "Users/alice/extended.txt", "Users/alice/notes.txt", "Users/b.txt"}));
        QCOMPARE(found("kernel"), QStringList{}); // Windows/ is left out
        QCOMPARE(found("big.bin"), QStringList{"Users/alice/big.bin"});
        const qint64 firstBuild = built();
        QVERIFY(firstBuild > 0);
        scan(); // nothing happened: the journal says so, nothing is read again
        QCOMPARE(built(), firstBuild);

        // A Windows session later: alice → bob, notes → todo, big.bin deleted, new.docx added.
        QVERIFY(write(testimage::build(1)));
        scan();
        QCOMPARE(built(), firstBuild); // replayed, not rebuilt
        QCOMPARE(found("notes"), QStringList{});
        QCOMPARE(found("big.bin"), QStringList{});
        QCOMPARE(found("new.docx"), QStringList{"Users/new.docx"});
        QCOMPARE(found("alice"), QStringList{});
        QCOMPARE(found("bob"), QStringList{"Users/bob"});
        // Unchanged files moved along with their renamed folder.
        QCOMPARE(found(".txt"), (QStringList{"Users/b.txt", "Users/bob/a long name.txt", "Users/bob/a.txt",
                                             "Users/bob/extended.txt", "Users/bob/todo.txt"}));
        QCOMPARE(opened, 1); // the partition stays open: the helper (and its prompt) runs once

        // A journal that was recreated cannot vouch for the index: the file table is read again.
        QByteArray recreated = testimage::build(1);
        const auto at = recreated.indexOf(QByteArray::fromHex("3412000000000000"));
        QVERIFY(at > 0);
        recreated[at] = 0x35;
        QVERIFY(write(recreated));
        QTest::qWait(5);
        scan();
        QVERIFY(built() > firstBuild);
        QCOMPARE(found("bob"), QStringList{"Users/bob"});
        index.stop();
    }
    void unreadablePartitionIsWalked() {
        QTemporaryDir root(dir.path() + "/mnt-XXXXXX");
        QFile plain(root.path() + "/plain.txt");
        QVERIFY(plain.open(QIODevice::WriteOnly));
        plain.close();
        SearchIndex index(dir.path() + "/db/walked.sqlite");
        index.setRoots({root.path()});
        index.setNtfsJournal(true);
        index.setVolumeOpener([](const QString &, QString *error) {
            *error = QStringLiteral("未获得授权");
            return -1;
        });
        index.start();
        QTRY_VERIFY_WITH_TIMEOUT(!index.scanning(), 10000);
        QVERIFY2(index.warnings().join(" ").contains(QStringLiteral("未获得授权")), qPrintable(index.warnings().join(" ") + index.status()));
        index.query("plain");
        QTRY_VERIFY(!index.model()->loading());
        QCOMPARE(index.model()->count(), 1);
        index.stop();
    }

    void rejectsOtherFileSystems() {
        QByteArray image = testimage::build();
        image.replace(3, 8, "EXFAT   ");
        std::unique_ptr<Volume> volume(open(image));
        QVERIFY(volume);
        QVERIFY(!volume->open(nullptr));
    }

    void survivesCorruption() {
        // On-disk metadata is untrusted input: damaged records are skipped, never followed.
        const QByteArray clean = testimage::build();
        auto *random = QRandomGenerator::global();
        for (int round = 0; round < 300; ++round) {
            QByteArray image = clean;
            const int hits = 1 + int(random->bounded(40));
            for (int i = 0; i < hits; ++i) {
                // Mostly inside the $MFT runs and the journal, sometimes anywhere.
                const qint64 regions[] = {4 * 4096, 40 * 4096, 60 * 4096, 0};
                const qint64 base = regions[random->bounded(4)];
                const qint64 at = base + random->bounded(base ? 8 * 4096 : int(image.size()));
                if (at < image.size())
                    image[at] = char(random->bounded(256));
            }
            std::unique_ptr<Volume> volume(open(image));
            QVERIFY(volume);
            if (!volume->open(nullptr))
                continue;
            Journal journal;
            volume->enumerate([](const Entry &) {}, journal, nullptr, nullptr);
            for (std::uint64_t record = 0; record < 64; ++record)
                volume->current(record);
            if (journal.present) {
                std::vector<Change> changes;
                volume->changes(journal, 0, journal.next, changes, nullptr);
            }
        }
    }
};

QTEST_GUILESS_MAIN(NtfsTest)
#include "ntfs_test.moc"
