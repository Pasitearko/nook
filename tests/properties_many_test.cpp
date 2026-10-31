#include "fileproperties.h"
#include "files.h"
#include <QDir>
#include <QFile>
#include <QPointer>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QUrl>
#include <QtTest>
#include <algorithm>
#include <fcntl.h>
#include <gio/gio.h>
#include <sys/stat.h>
#include <unistd.h>

class PropertiesManyTest : public QObject {
    Q_OBJECT
  private:
    static QString uri(const QString &path) {
        return QUrl::fromLocalFile(path).toString(QUrl::FullyEncoded);
    }
    static void put(const QString &path, int bytes) {
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        QCOMPARE(file.write(QByteArray(bytes, 'x')), qint64(bytes));
    }
    static void setMode(const QString &path, int mode) {
        QCOMPARE(::chmod(QFile::encodeName(path).constData(), mode), 0);
    }
    static int mode(const QString &path) {
        struct stat st{};
        if (::lstat(QFile::encodeName(path).constData(), &st) != 0)
            return -1;
        return st.st_mode & 0777;
    }
    static void wait(FileProperties &properties) {
        QTRY_VERIFY_WITH_TIMEOUT(!properties.busy(), 10000);
    }
  private Q_SLOTS:
    void filesCommonValuesAndLocation() {
        QTemporaryDir fixture;
        QVERIFY(fixture.isValid());
        const auto a = fixture.filePath("中文 #?.txt"), b = fixture.filePath("empty");
        put(a, 1701);
        put(b, 0);
        setMode(a, 0640);
        setMode(b, 0640);
        FileProperties properties;
        QSignalSpy errors(&properties, &FileProperties::error);
        properties.inspectMany({uri(a), uri(b)});
        QVERIFY(properties.busy());
        wait(properties);
        const auto d = properties.details();
        QVERIFY(d.value("multiple").toBool());
        QCOMPARE(d.value("selectionCount").toInt(), 2);
        QCOMPARE(d.value("fileCount").toLongLong(), 2);
        QCOMPARE(d.value("directoryCount").toLongLong(), 0);
        QCOMPARE(d.value("totalSize").toLongLong(), 1701);
        QCOMPARE(d.value("size").toLongLong(), 1701);
        QVERIFY(!d.value("sizeText").toString().isEmpty());
        QCOMPARE(d.value("commonLocation").toString(), fixture.path());
        QCOMPARE(d.value("commonLocationUri").toString(), uri(fixture.path()));
        QCOMPARE(d.value("type").toString(), QString("file"));
        QCOMPARE(d.value("permissions").toInt(), 0640);
        QVERIFY(!d.value("permissionsMixed").toBool());
        QVERIFY(!d.value("ownerMixed").toBool());
        QVERIFY(!d.value("partial").toBool());
        QVERIFY(!d.value("editable").toBool());
        QVERIFY(!d.value("canHide").toBool());
        QVERIFY(!d.contains("uri")); // No accidental single mutation target.
        QCOMPARE(errors.count(), 0);
    }
    void mixedMetadataAndReadOnly() {
        QTemporaryDir fixture;
        QVERIFY(fixture.isValid());
        const auto a = fixture.filePath("visible"), b = fixture.filePath(".hidden-dir");
        put(a, 7);
        QVERIFY(QDir().mkdir(b));
        setMode(a, 0640);
        setMode(b, 0700);
        const timespec early[2] = {{1600000000, 0}, {1600000000, 0}};
        const timespec late[2] = {{1700000000, 0}, {1700000000, 0}};
        QCOMPARE(::utimensat(AT_FDCWD, QFile::encodeName(a).constData(), early, 0), 0);
        QCOMPARE(::utimensat(AT_FDCWD, QFile::encodeName(b).constData(), late, 0), 0);
        FileProperties properties;
        properties.inspect(uri(a)); // Ensure the old pinned single-file target is cleared.
        wait(properties);
        properties.inspectMany({uri(a), uri(b)});
        properties.setHidden(true); // Also harmless while the new result is pending.
        wait(properties);
        const auto d = properties.details();
        for (const auto &key : {"permissions", "hidden", "type", "modified"}) {
            QVERIFY2(d.value(QString(key) + "Mixed").toBool(), key);
            QVERIFY2(!d.contains(key), key);
        }
        QVERIFY(!d.contains("modifiedText"));
        QSignalSpy applied(&properties, &FileProperties::permissionsApplied);
        QSignalSpy hidden(&properties, &FileProperties::hiddenApplied);
        properties.applyPermissions(0777);
        properties.setHidden(true);
        QVERIFY(!properties.busy());
        QCOMPARE(applied.count(), 0);
        QCOMPARE(hidden.count(), 0);
        QCOMPARE(mode(a), 0640);
        QCOMPARE(mode(b), 0700);
        QVERIFY(QFile::exists(b));
        QVERIFY(!QFile::exists(fixture.filePath(".hidden")));
    }
    void recursiveUnion_data() {
        QTest::addColumn<bool>("parentFirst");
        QTest::newRow("parent-first") << true;
        QTest::newRow("child-first") << false;
    }
    void recursiveUnion() {
        QFETCH(bool, parentFirst);
        QTemporaryDir fixture;
        QVERIFY(fixture.isValid());
        const auto tree = fixture.filePath("tree"), sub = tree + "/sub";
        const auto outside = fixture.filePath("outside"), sibling = fixture.filePath("sibling");
        QVERIFY(QDir().mkpath(sub));
        QVERIFY(QDir().mkdir(outside));
        put(tree + "/file", 10);
        put(sub + "/child", 7);
        put(tree + "/.secret", 3);
        put(sibling, 5);
        put(outside + "/not-counted", 1000000);
        QCOMPARE(::link(QFile::encodeName(tree + "/file").constData(),
                        QFile::encodeName(sub + "/hardlink").constData()), 0);
        QCOMPARE(::symlink(".", QFile::encodeName(tree + "/loop").constData()), 0);
        QCOMPARE(::symlink("missing", QFile::encodeName(tree + "/broken").constData()), 0);
        const auto outsideBytes = QFile::encodeName(outside);
        QCOMPARE(::symlink(outsideBytes.constData(), QFile::encodeName(tree + "/outside-link").constData()), 0);
        QCOMPARE(::mkfifo(QFile::encodeName(tree + "/fifo").constData(), 0600), 0);
        QStringList selection = {uri(tree), uri(sub), uri(sub + "/child"), uri(tree + "/file"),
                                 uri(sibling), uri(tree)};
        if (!parentFirst)
            std::reverse(selection.begin(), selection.end());
        FileProperties properties;
        properties.inspectMany(selection);
        wait(properties);
        const auto d = properties.details();
        QCOMPARE(d.value("selectionCount").toInt(), 5);
        QCOMPARE(d.value("fileCount").toLongLong(), 4); // Hardlink inode counted once.
        QCOMPARE(d.value("directoryCount").toLongLong(), 2);
        QCOMPARE(d.value("symlinkCount").toLongLong(), 3);
        QCOMPARE(d.value("otherCount").toLongLong(), 1);
        QCOMPARE(d.value("itemCount").toLongLong(), 10);
        QCOMPARE(d.value("totalSize").toLongLong(), 25 + 1 + 7 + outsideBytes.size());
        QVERIFY(!d.value("partial").toBool());
        QCOMPARE(d.value("commonLocation").toString(), fixture.path());
    }
    void duplicateAliasesRetainSingleBehavior() {
        QTemporaryDir fixture;
        QVERIFY(fixture.isValid());
        const auto path = fixture.filePath("file");
        put(path, 4);
        setMode(path, 0640);
        FileProperties properties;
        properties.inspectMany({path, uri(path), fixture.path() + "/./file", uri(path)});
        wait(properties);
        QVERIFY(!properties.details().value("multiple").toBool());
        QCOMPARE(properties.details().value("uri").toString(), uri(path));
        QVERIFY(properties.details().value("editable").toBool());
        properties.applyPermissions(0600);
        wait(properties);
        QCOMPARE(mode(path), 0600);
        properties.inspect(uri(path));
        wait(properties);
        QCOMPARE(properties.details().value("size").toLongLong(), 4);
    }
    void emptyAndInvalidNeverSelectImplicitTarget() {
        QTemporaryDir fixture;
        QVERIFY(fixture.isValid());
        const auto path = fixture.filePath("file");
        put(path, 8);
        FileProperties properties;
        properties.inspectMany({});
        wait(properties);
        QCOMPARE(properties.details().value("errorCode").toString(), QString("invalidUri"));
        properties.inspectMany({uri(path), QString()});
        wait(properties);
        QVERIFY(properties.details().value("multiple").toBool());
        QVERIFY(properties.details().value("partial").toBool());
        QVERIFY(!properties.details().value("editable").toBool());
        QCOMPARE(properties.details().value("size").toLongLong(), 8);
        properties.inspectMany({uri(path), uri(fixture.filePath("missing"))});
        wait(properties);
        QCOMPARE(properties.details().value("errorCode").toString(), QString("notFound"));
        QVERIFY(properties.details().value("partial").toBool());
        QCOMPARE(properties.details().value("fileCount").toLongLong(), 1);
        QCOMPARE(properties.details().value("size").toLongLong(), 8);
    }
    void nonUtf8NamesAndDotHiddenList() {
        QTemporaryDir fixture;
        QVERIFY(fixture.isValid());
        const auto root = QFile::encodeName(fixture.path());
        const QByteArray raw = root + "/raw-\xff%#";
        const int fd = ::open(raw.constData(), O_CREAT | O_EXCL | O_WRONLY | O_CLOEXEC, 0640);
        QVERIFY(fd >= 0);
        QCOMPARE(::write(fd, "abc", 3), ssize_t(3));
        QCOMPARE(::close(fd), 0);
        GFile *file = g_file_new_for_path(raw.constData());
        char *encoded = g_file_get_uri(file);
        const auto originalUri = QString::fromUtf8(encoded);
        g_free(encoded);
        g_object_unref(file);
        const auto visible = fixture.filePath("visible");
        put(visible, 2);
        QFile hidden(fixture.filePath(".hidden"));
        QVERIFY(hidden.open(QIODevice::WriteOnly));
        QCOMPARE(hidden.write("visible\n"), qint64(8));
        hidden.close();
        FileProperties properties;
        properties.inspectMany({originalUri, uri(visible)});
        wait(properties);
        QCOMPARE(properties.details().value("size").toLongLong(), 5);
        QVERIFY(properties.details().value("hiddenMixed").toBool());
        properties.setHidden(false);
        QVERIFY(QFile::exists(visible));
        QVERIFY(hidden.open(QIODevice::ReadOnly));
        QCOMPARE(hidden.readAll(), QByteArray("visible\n"));
        QCOMPARE(::unlink(raw.constData()), 0);
    }
    void latestGenerationAndDestruction() {
        QTemporaryDir fixture;
        QVERIFY(fixture.isValid());
        const auto tree = fixture.filePath("tree"), latest = fixture.filePath("latest");
        QVERIFY(QDir().mkdir(tree));
        for (int i = 0; i < 250; ++i)
            put(tree + '/' + QString::number(i), 13);
        put(latest, 9);
        FileProperties properties;
        QSignalSpy errors(&properties, &FileProperties::error);
        for (int i = 0; i < 20; ++i)
            properties.inspectMany({uri(tree), uri(fixture.filePath("missing"))});
        properties.inspect(uri(latest));
        wait(properties);
        QTest::qWait(100);
        QCOMPARE(properties.details().value("uri").toString(), uri(latest));
        QCOMPARE(properties.details().value("size").toLongLong(), 9);
        QVERIFY(!properties.details().value("multiple").toBool());
        QCOMPARE(errors.count(), 0);
        properties.inspectMany({uri(latest), uri(tree)});
        wait(properties);
        QVERIFY(properties.details().value("multiple").toBool());
        QCOMPARE(properties.details().value("size").toLongLong(), 3259);
        {
            FileProperties outstanding;
            for (int i = 0; i < 10; ++i)
                outstanding.inspectMany({uri(tree), uri(latest)});
        }
    }
    void unsafeHiddenFilesAreIgnored() {
        QTemporaryDir fixture;
        QVERIFY(fixture.isValid());
        const auto hidden = QFile::encodeName(fixture.filePath(".hidden"));
        QVERIFY(::symlink("/dev/zero", hidden.constData()) == 0);
        QVERIFY(nook::hiddenNames(QFile::encodeName(fixture.path())).isEmpty());
        QVERIFY(QFile::remove(fixture.filePath(".hidden")));
        QCOMPARE(::mkfifo(hidden.constData(), 0600), 0);
        QVERIFY(nook::hiddenNames(QFile::encodeName(fixture.path())).isEmpty());
        put(fixture.filePath("a"), 1);
        put(fixture.filePath("b"), 2);
        FileProperties properties;
        properties.inspectMany({uri(fixture.filePath("a")), uri(fixture.filePath("b"))});
        wait(properties); // a FIFO in .hidden must not hold the properties job open
        QCOMPARE(properties.details()["size"].toLongLong(), 3);
        QString error;
        const auto entries = nook::enumerate(uri(fixture.path()), true,
            std::make_shared<std::atomic_bool>(false), &error);
        QCOMPARE(entries.size(), 3);
        QVERIFY(error.isEmpty());
        QVERIFY(QFile::remove(fixture.filePath(".hidden")));
        put(fixture.filePath(".hidden"), 1024 * 1024 + 1);
        QVERIFY(nook::hiddenNames(QFile::encodeName(fixture.path())).isEmpty());
        QFile file(fixture.filePath(".hidden"));
        QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
        file.write("a\r\nb\n");
        file.close();
        QCOMPARE(nook::hiddenNames(QFile::encodeName(fixture.path())), (QSet<QByteArray>{"a", "b"}));
    }
    void wideTreesStopAtBudgetAndCanCancel() {
        QTemporaryDir fixture;
        QVERIFY(fixture.isValid());
        QVERIFY(QDir().mkdir(fixture.filePath("wide")));
        for (int i = 0; i < 128; ++i) put(fixture.filePath("wide/") + QString::number(i), 1);
        put(fixture.filePath("other"), 1);
        FileProperties properties;
        properties.m_entryLimit = 32;
        properties.inspectMany({uri(fixture.filePath("wide")), uri(fixture.filePath("other"))});
        wait(properties);
        QVERIFY(properties.details()["partial"].toBool());
        QCOMPARE(properties.details()["errorCode"].toString(), QString("limitExceeded"));
        QVERIFY(properties.details()["itemCount"].toLongLong() <= 32);
        properties.inspectMany({uri(fixture.filePath("wide")), uri(fixture.filePath("other"))});
        QVERIFY(properties.busy());
        properties.cancelInspection();
        QVERIFY(!properties.busy());
        QVERIFY(properties.details().isEmpty());
        QTest::qWait(50);
        QVERIFY(properties.details().isEmpty()); // stale completion cannot repopulate a closed dialog
    }
    void excessiveDepthIsExplicitlyPartial() {
        QTemporaryDir fixture;
        QVERIFY(fixture.isValid());
        QString path = fixture.filePath("deep");
        QVERIFY(QDir().mkdir(path));
        const auto root = path;
        for (int i = 0; i < 140; ++i) {
            path += "/d";
            QVERIFY(QDir().mkdir(path));
        }
        put(path + "/leaf", 42);
        const auto sibling = fixture.filePath("sibling");
        put(sibling, 5);
        FileProperties properties;
        properties.inspectMany({uri(root), uri(sibling)});
        wait(properties);
        QVERIFY(properties.details().value("partial").toBool());
        QCOMPARE(properties.details().value("errorCode").toString(), QString("notSupported"));
        QCOMPARE(properties.details().value("size").toLongLong(), 5);
        QCOMPARE(properties.details().value("directoryCount").toLongLong(), 129);
    }
};

QTEST_GUILESS_MAIN(PropertiesManyTest)
#include "properties_many_test.moc"
