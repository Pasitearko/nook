#include "fileproperties.h"
#include <QDir>
#include <QFile>
#include <QPointer>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QUrl>
#include <QtTest>
#include <fcntl.h>
#include <gio/gio.h>
#include <limits>
#include <sys/stat.h>
#include <unistd.h>

class PropertiesTest : public QObject {
    Q_OBJECT
  private:
    static void put(const QString &path) {
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        QCOMPARE(file.write("sample"), qint64(6));
    }
    static QString uri(const QString &path) {
        return QUrl::fromLocalFile(path).toString(QUrl::FullyEncoded);
    }
    static int mode(const QString &path) {
        struct stat st{};
        if (::lstat(QFile::encodeName(path).constData(), &st) != 0)
            return -1;
        return int(st.st_mode & 07777);
    }
    static void chmod(const QString &path, int bits) {
        QCOMPARE(::chmod(QFile::encodeName(path).constData(), mode_t(bits)), 0);
    }
    static void wait(FileProperties &properties) {
        QTRY_VERIFY_WITH_TIMEOUT(!properties.busy(), 5000);
    }
  private Q_SLOTS:
    void inspectRegularAndDirectory() {
        QTemporaryDir fixture;
        QVERIFY(fixture.isValid());
        const auto path = fixture.filePath("中文 #?.txt");
        put(path);
        chmod(path, 0640);
        FileProperties properties;
        QSignalSpy errors(&properties, &FileProperties::error);
        properties.inspect(uri(path));
        QVERIFY(properties.busy());
        wait(properties);
        const auto details = properties.details();
        QCOMPARE(details.value("name").toString(), QString("中文 #?.txt"));
        QCOMPARE(details.value("uri").toString(), uri(path));
        QCOMPARE(details.value("path").toString(), path);
        QCOMPARE(details.value("type").toString(), QString("file"));
        QCOMPARE(details.value("size").toLongLong(), qint64(6));
        QVERIFY(!details.value("sizeText").toString().isEmpty());
        QVERIFY(details.value("modified").toLongLong() > 0);
        QVERIFY(!details.value("modifiedText").toString().isEmpty());
        if (details.contains("created"))
            QVERIFY(details.value("created").toLongLong() > 0);
        QVERIFY(!details.value("owner").toString().isEmpty());
        QVERIFY(!details.value("group").toString().isEmpty());
        QCOMPARE(details.value("permissions").toInt(), 0640);
        QVERIFY(details.value("permissionsSupported").toBool());
        QVERIFY(details.value("editable").toBool());
        properties.inspect(uri(fixture.path()));
        wait(properties);
        QCOMPARE(properties.details().value("type").toString(), QString("directory"));
        QVERIFY(properties.details().value("editable").toBool());
        QCOMPARE(errors.count(), 0);
    }
    void applyActualModeAndPreserveCurrentSpecialBits() {
        QTemporaryDir fixture;
        QVERIFY(fixture.isValid());
        const auto path = fixture.filePath("file");
        put(path);
        chmod(path, 0400); // Chmod authority is ownership, not write-access permission.
        FileProperties properties;
        QSignalSpy applied(&properties, &FileProperties::permissionsApplied);
        QSignalSpy errors(&properties, &FileProperties::error);
        properties.inspect(uri(path));
        wait(properties);
        QVERIFY(properties.details().value("editable").toBool());
        chmod(path, 01400); // Special bits added since inspection must also be preserved.
        properties.applyPermissions(0640);
        QVERIFY(properties.busy());
        wait(properties);
        QCOMPARE(mode(path), 01640);
        QCOMPARE(properties.details().value("permissions").toInt(), 0640);
        QCOMPARE(applied.count(), 1);
        QCOMPARE(errors.count(), 0);
        properties.applyPermissions(0000);
        wait(properties);
        QCOMPARE(mode(path), 01000);
        properties.applyPermissions(0640);
        wait(properties);
        const int restored = mode(path);
        chmod(path, 01640);
        QCOMPARE(restored, 01640);
        QCOMPARE(applied.count(), 3);
        QFile contents(path);
        QVERIFY(contents.open(QIODevice::ReadOnly));
        QCOMPARE(contents.readAll(), QByteArray("sample"));
    }
    void unreadableTargets_data() {
        QTest::addColumn<bool>("directory");
        QTest::addColumn<int>("initialMode");
        QTest::newRow("file-0000") << false << 0000;
        QTest::newRow("file-write-only") << false << 0200;
        QTest::newRow("directory-0000") << true << 0000;
    }
    void unreadableTargets() {
        QFETCH(bool, directory);
        QFETCH(int, initialMode);
        QTemporaryDir fixture;
        QVERIFY(fixture.isValid());
        const auto path = fixture.filePath("target");
        if (directory)
            QVERIFY(QDir().mkdir(path));
        else
            put(path);
        chmod(path, initialMode);
        FileProperties properties;
        properties.inspect(uri(path));
        wait(properties);
        if (!properties.details().value("editable").toBool()) {
            chmod(path, directory ? 0700 : 0600);
            QCOMPARE(properties.details().value("permissionsErrorCode").toString(),
                     QString("permissionDenied"));
            QSKIP("This kernel cannot safely chmod an initially unreadable inode (needs fchmodat2)");
        }
        QSignalSpy applied(&properties, &FileProperties::permissionsApplied);
        properties.applyPermissions(directory ? 0700 : 0600);
        wait(properties);
        const int actual = mode(path);
        chmod(path, directory ? 0700 : 0600); // Ensure fixture cleanup even when an assertion fails.
        QCOMPARE(actual, directory ? 0700 : 0600);
        QCOMPARE(applied.count(), 1);
        properties.applyPermissions(0000);
        wait(properties);
        QCOMPARE(mode(path), 0000);
        properties.applyPermissions(directory ? 0700 : 0600);
        wait(properties);
        const int restored = mode(path);
        chmod(path, directory ? 0700 : 0600);
        QCOMPARE(restored, directory ? 0700 : 0600);
        QCOMPARE(applied.count(), 3);
    }
    void preserveSetgid() {
        QTemporaryDir fixture;
        QVERIFY(fixture.isValid());
        const auto path = fixture.filePath("file");
        put(path);
        chmod(path, 02640);
        QCOMPARE(mode(path), 02640);
        FileProperties properties;
        properties.inspect(uri(path));
        wait(properties);
        properties.applyPermissions(0600);
        wait(properties);
        QCOMPARE(mode(path), 02600);
    }
    void foreignGroupSetgidRefused() {
        if (::geteuid() != 0)
            QSKIP("Creating a foreign-group SGID fixture requires root; no privilege escalation is used");
        QTemporaryDir fixture;
        QVERIFY(fixture.isValid());
        const auto path = fixture.filePath("file");
        put(path);
        gid_t foreign = 60000;
        const int count = ::getgroups(0, nullptr);
        QVERIFY(count >= 0);
        QList<gid_t> groups(count);
        QCOMPARE(::getgroups(count, groups.data()), count);
        while (foreign == ::getegid() || groups.contains(foreign))
            ++foreign;
        QCOMPARE(::chown(QFile::encodeName(path).constData(), uid_t(-1), foreign), 0);
        chmod(path, 02640);
        FileProperties properties;
        properties.inspect(uri(path));
        wait(properties);
        QVERIFY(!properties.details().value("editable").toBool());
        properties.applyPermissions(0600);
        wait(properties);
        QCOMPARE(mode(path), 02640);
        QCOMPARE(properties.details().value("errorCode").toString(), QString("notSupported"));
    }
    void directoryIsNotRecursive() {
        QTemporaryDir fixture;
        QVERIFY(fixture.isValid());
        const auto directory = fixture.filePath("folder");
        QVERIFY(QDir().mkdir(directory));
        const auto child = directory + "/child";
        put(child);
        chmod(child, 0640);
        FileProperties properties;
        properties.inspect(uri(directory));
        wait(properties);
        properties.applyPermissions(0750);
        wait(properties);
        QCOMPARE(mode(directory), 0750);
        QCOMPARE(mode(child), 0640);
    }
    void invalidModes_data() {
        QTest::addColumn<int>("bits");
        QTest::newRow("negative") << -1;
        QTest::newRow("sticky") << 01000;
        QTest::newRow("setuid") << 04755;
        QTest::newRow("type-bits") << 0100644;
        QTest::newRow("large") << std::numeric_limits<int>::max();
    }
    void invalidModes() {
        QFETCH(int, bits);
        QTemporaryDir fixture;
        QVERIFY(fixture.isValid());
        const auto path = fixture.filePath("file");
        put(path);
        chmod(path, 0640);
        FileProperties properties;
        properties.inspect(uri(path));
        wait(properties);
        QSignalSpy applied(&properties, &FileProperties::permissionsApplied);
        QSignalSpy errors(&properties, &FileProperties::error);
        properties.applyPermissions(bits);
        wait(properties);
        QCOMPARE(mode(path), 0640);
        QCOMPARE(properties.details().value("errorCode").toString(), QString("invalidMode"));
        QCOMPARE(errors.count(), 1);
        QCOMPARE(applied.count(), 0);
    }
    void symlinkRejected() {
        QTemporaryDir fixture;
        QVERIFY(fixture.isValid());
        const auto target = fixture.filePath("target");
        const auto link = fixture.filePath("link");
        put(target);
        chmod(target, 0640);
        QCOMPARE(::symlink("target", QFile::encodeName(link).constData()), 0);
        FileProperties properties;
        properties.inspect(uri(link));
        wait(properties);
        QCOMPARE(properties.details().value("type").toString(), QString("symlink"));
        QVERIFY(!properties.details().value("permissionsSupported").toBool());
        QVERIFY(!properties.details().value("editable").toBool());
        QSignalSpy errors(&properties, &FileProperties::error);
        QSignalSpy applied(&properties, &FileProperties::permissionsApplied);
        properties.applyPermissions(0600);
        wait(properties);
        QCOMPARE(mode(target), 0640);
        QCOMPARE(errors.count(), 1);
        QCOMPARE(applied.count(), 0);
        QCOMPARE(properties.details().value("errorCode").toString(), QString("notSupported"));
    }
    void replacedFileRejected() {
        QTemporaryDir fixture;
        QVERIFY(fixture.isValid());
        const auto path = fixture.filePath("file"), saved = fixture.filePath("original");
        put(path);
        chmod(path, 0640);
        FileProperties properties;
        properties.inspect(uri(path));
        wait(properties);
        QVERIFY(QFile::rename(path, saved));
        put(path);
        chmod(path, 0644);
        QSignalSpy errors(&properties, &FileProperties::error);
        QSignalSpy applied(&properties, &FileProperties::permissionsApplied);
        properties.applyPermissions(0600);
        wait(properties);
        QCOMPARE(mode(path), 0644);
        QCOMPARE(mode(saved), 0640);
        QCOMPARE(errors.count(), 1);
        QCOMPARE(applied.count(), 0);
        QCOMPARE(properties.details().value("errorCode").toString(), QString("replaced"));
        QVERIFY(!properties.details().value("editable").toBool());
    }
    void replacedBySymlinkOrFifo_data() {
        QTest::addColumn<bool>("symlink");
        QTest::newRow("symlink") << true;
        QTest::newRow("fifo") << false;
    }
    void replacedBySymlinkOrFifo() {
        QFETCH(bool, symlink);
        QTemporaryDir fixture;
        QVERIFY(fixture.isValid());
        const auto path = fixture.filePath("file"), target = fixture.filePath("target");
        put(path);
        put(target);
        chmod(target, 0640);
        FileProperties properties;
        properties.inspect(uri(path));
        wait(properties);
        QVERIFY(QFile::remove(path));
        const auto raw = QFile::encodeName(path);
        if (symlink)
            QCOMPARE(::symlink("target", raw.constData()), 0);
        else
            QCOMPARE(::mkfifo(raw.constData(), 0600), 0);
        QSignalSpy errors(&properties, &FileProperties::error);
        QSignalSpy applied(&properties, &FileProperties::permissionsApplied);
        properties.applyPermissions(0777);
        wait(properties);
        QCOMPARE(errors.count(), 1);
        QCOMPARE(applied.count(), 0);
        QCOMPARE(mode(target), 0640);
        if (!symlink)
            QCOMPARE(mode(path), 0600);
    }
    void nonUtf8Uri() {
        QTemporaryDir fixture;
        QVERIFY(fixture.isValid());
        const QByteArray raw = QFile::encodeName(fixture.path()) + "/raw-\xff%#";
        const int fd = ::open(raw.constData(), O_CREAT | O_EXCL | O_WRONLY | O_CLOEXEC, 0640);
        QVERIFY(fd >= 0);
        QCOMPARE(::close(fd), 0);
        GFile *file = g_file_new_for_path(raw.constData());
        char *encoded = g_file_get_uri(file);
        const auto originalUri = QString::fromUtf8(encoded);
        g_free(encoded);
        g_object_unref(file);
        QVERIFY(originalUri.contains("%FF", Qt::CaseInsensitive));
        FileProperties properties;
        properties.inspect(originalUri);
        wait(properties);
        QCOMPARE(properties.details().value("uri").toString(), originalUri);
        QVERIFY(properties.details().value("editable").toBool());
        properties.applyPermissions(0600);
        wait(properties);
        struct stat st{};
        QCOMPARE(::lstat(raw.constData(), &st), 0);
        QCOMPARE(int(st.st_mode & 0777), 0600);
        QCOMPARE(::unlink(raw.constData()), 0);
    }
    void latestGenerationAndDestruction() {
        QTemporaryDir fixture;
        QVERIFY(fixture.isValid());
        const auto path = fixture.filePath("file");
        put(path);
        FileProperties properties;
        QSignalSpy errors(&properties, &FileProperties::error);
        for (int i = 0; i < 16; ++i)
            properties.inspect(uri(fixture.filePath("missing")));
        properties.inspect(uri(path));
        wait(properties);
        QCOMPARE(properties.details().value("uri").toString(), uri(path));
        QVERIFY(properties.details().value("editable").toBool());
        QTest::qWait(50);
        QCOMPARE(errors.count(), 0);
        {
            FileProperties outstanding;
            for (int i = 0; i < 16; ++i)
                outstanding.inspect(uri(path));
        } // Destruction cancels and waits even without delivering finished signals.
    }
    void signalReentrancy() {
        QTemporaryDir fixture;
        QVERIFY(fixture.isValid());
        const auto path = fixture.filePath("file");
        put(path);
        FileProperties properties;
        QSignalSpy errors(&properties, &FileProperties::error);
        connect(&properties, &FileProperties::changed, &properties, [&] {
            if (!properties.busy() && properties.details().value("errorCode") == "notFound")
                properties.inspect(uri(path));
        });
        properties.inspect(uri(fixture.filePath("missing")));
        wait(properties);
        QCOMPARE(properties.details().value("uri").toString(), uri(path));
        QCOMPARE(errors.count(), 0); // The old failure is stale after changed() starts a new inspection.
    }
    void deletionFromChanged() {
        QTemporaryDir fixture;
        QVERIFY(fixture.isValid());
        QPointer<FileProperties> missing = new FileProperties;
        connect(missing, &FileProperties::changed, this, [&] {
            if (!missing->busy())
                delete missing.data();
        });
        missing->inspect(uri(fixture.filePath("missing")));
        QTRY_VERIFY_WITH_TIMEOUT(!missing, 5000);

        QPointer<FileProperties> invalid = new FileProperties;
        connect(invalid, &FileProperties::changed, this, [&] { delete invalid.data(); });
        invalid->applyPermissions(-1);
        QVERIFY(!invalid);

        const auto path = fixture.filePath("file");
        put(path);
        QPointer<FileProperties> applied = new FileProperties;
        applied->inspect(uri(path));
        wait(*applied);
        connect(applied, &FileProperties::changed, this, [&] {
            if (!applied->busy())
                delete applied.data();
        });
        applied->applyPermissions(0600);
        QTRY_VERIFY_WITH_TIMEOUT(!applied, 5000);
        QCOMPARE(mode(path), 0600);
    }
    void blankAndMissing() {
        QTemporaryDir fixture;
        QVERIFY(fixture.isValid());
        FileProperties properties;
        QSignalSpy errors(&properties, &FileProperties::error);
        properties.inspect({}); // The caller, not this backend, chooses the current directory.
        wait(properties);
        QCOMPARE(properties.details().value("errorCode").toString(), QString("invalidUri"));
        properties.inspect(uri(fixture.filePath("missing")));
        wait(properties);
        QCOMPARE(properties.details().value("errorCode").toString(), QString("notFound"));
        QCOMPARE(errors.count(), 2);
    }
};

QTEST_GUILESS_MAIN(PropertiesTest)
#include "properties_test.moc"
