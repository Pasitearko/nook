#include "defaultapps.h"
#include <QDir>
#include <QFile>
#include <QMimeDatabase>
#include <QTemporaryDir>
#include <QtTest>
#include <gio/gdesktopappinfo.h>
#include <gio/gio.h>
#include <memory>

namespace {

std::unique_ptr<QTemporaryDir> fixture;

void put(const QString &path, const QByteArray &data) {
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    if (file.open(QIODevice::WriteOnly))
        file.write(data);
}

QByteArray read(const QString &path) {
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}

// The associations as the user has them before nook解压缩 is asked: ZIP and 7z go to another
// archive manager, text to an editor. Applications are the fixture's, plus the system's (whose
// MIME database the lookups need).
void setUpFixture() {
    fixture = std::make_unique<QTemporaryDir>(QString(NOOK_BINARY_DIR) + "/defaults-fixture-XXXXXX");
    const QString root = fixture->path();
    qputenv("XDG_CONFIG_HOME", QFile::encodeName(root + "/config"));
    qputenv("XDG_CONFIG_DIRS", QFile::encodeName(root + "/sysconfig"));
    qputenv("XDG_DATA_HOME", QFile::encodeName(root + "/data"));
    qputenv("XDG_DATA_DIRS", "/usr/share");
    qputenv("XDG_CURRENT_DESKTOP", "nook-test");
    QByteArray ours = read(QString(NOOK_SOURCE_DIR) + "/packaging/local.nook.archive.desktop");
    ours.replace("Exec=nook-archive %U", "Exec=true %U");
    put(root + "/data/applications/local.nook.archive.desktop", ours);
    put(root + "/data/applications/fake-roller.desktop",
        "[Desktop Entry]\nType=Application\nName=Fake Roller\nExec=true %U\nMimeType=application/zip;application/x-7z-compressed;\n");
    put(root + "/data/applications/editor.desktop",
        "[Desktop Entry]\nType=Application\nName=Editor\nExec=true %U\nMimeType=text/plain;\n");
    put(root + "/config/mimeapps.list",
        "[Default Applications]\napplication/zip=fake-roller.desktop\napplication/x-7z-compressed=fake-roller.desktop\ntext/plain=editor.desktop\n");
}

// The [Default Applications] group of mimeapps.list as type → desktop id.
QHash<QString, QString> defaultsGroup() {
    QHash<QString, QString> group;
    bool inside = false;
    for (const auto &line : QString::fromUtf8(read(fixture->path() + "/config/mimeapps.list")).split(u'\n')) {
        if (line.startsWith(u'['))
            inside = line == "[Default Applications]";
        else if (inside && line.contains(u'='))
            group.insert(line.section(u'=', 0, 0), line.section(u'=', 1).section(u';', 0, 0));
    }
    return group;
}

} // namespace

class ArchiveDefaultsTest : public QObject {
    Q_OBJECT
  private Q_SLOTS:
    void notInstalled() {
        DefaultApps missing("nook-archive-missing.desktop");
        QCOMPARE(missing.state(), QStringLiteral("unavailable"));
        QVERIFY(missing.setAsDefault().contains("安装"));
        QVERIFY(!QFileInfo::exists(fixture->path() + "/config/nook/archive-defaults.json"));
    }

    void takesOverAndRestores() {
        const QString list = fixture->path() + "/config/mimeapps.list";
        const QByteArray original = read(list);
        DefaultApps defaults;
        QVERIFY(defaults.typeCount() > 35);
        // Aliases are counted once: ISO's old names resolve to the current one.
        QVERIFY(defaults.types().contains("application/vnd.efi.iso"));
        QVERIFY(!defaults.types().contains("application/x-iso9660-image"));
        QVERIFY(defaults.state() != "all");
        QCOMPARE(defaults.defaultFor("application/zip"), QStringLiteral("fake-roller.desktop"));
        QVERIFY(!defaults.otherName().isEmpty());
        QVERIFY(!defaults.canRestore());

        QCOMPARE(defaults.setAsDefault(), QString());
        QCOMPARE(defaults.state(), QStringLiteral("all"));
        QCOMPARE(defaults.ownedCount(), defaults.typeCount());
        QCOMPARE(defaults.defaultFor("application/zip"), QStringLiteral("local.nook.archive.desktop"));
        QCOMPARE(defaults.defaultFor("application/vnd.debian.binary-package"), QStringLiteral("local.nook.archive.desktop"));
        QCOMPARE(defaults.defaultFor("text/plain"), QStringLiteral("editor.desktop"));
        // The list as it was is kept next to it.
        QVERIFY(defaults.lastBackup().startsWith(list + ".bak-nook-archive-"));
        QCOMPARE(read(defaults.lastBackup()), original);
        QVERIFY(defaults.canRestore());
        QCOMPARE(defaultsGroup().value("application/zip"), QStringLiteral("local.nook.archive.desktop"));

        // Asking again changes nothing and keeps what was there before the first time.
        QCOMPARE(defaults.setAsDefault(), QString());

        // The user hands 7z to another application meanwhile: restoring leaves that choice alone.
        auto *roller = G_APP_INFO(g_desktop_app_info_new("fake-roller.desktop"));
        QVERIFY(roller);
        QVERIFY(g_app_info_set_as_default_for_type(roller, "application/x-7z-compressed", nullptr));
        g_object_unref(roller);
        defaults.refresh();
        QCOMPARE(defaults.state(), QStringLiteral("some"));

        QCOMPARE(defaults.restore(), QString());
        QCOMPARE(defaults.defaultFor("application/zip"), QStringLiteral("fake-roller.desktop"));
        QCOMPARE(defaults.defaultFor("application/x-7z-compressed"), QStringLiteral("fake-roller.desktop"));
        QCOMPARE(defaults.defaultFor("text/plain"), QStringLiteral("editor.desktop"));
        QVERIFY(!defaults.canRestore());
        // Types no one had chosen are not left pinned to nook解压缩.
        const auto group = defaultsGroup();
        for (auto it = group.cbegin(); it != group.cend(); ++it)
            QVERIFY2(it.value() != "local.nook.archive.desktop", qPrintable(it.key()));
    }
};

int main(int argc, char **argv) {
    setUpFixture();
    QCoreApplication app(argc, argv);
    ArchiveDefaultsTest test;
    return QTest::qExec(&test, argc, argv);
}

#include "archive_defaults_test.moc"
