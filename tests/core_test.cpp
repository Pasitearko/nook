#include "application.h"
#include "storage.h"
#include <QScopeGuard>
#include <QStandardPaths>
#include <QFile>
#include <QGuiApplication>
#include <QImage>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>
#include <sqlite3.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

class CoreTest : public QObject {
    Q_OBJECT
  private:
    static void put(const QString &path, const QByteArray &data = "sample") {
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        QCOMPARE(file.write(data), data.size());
    }
    static QString uri(const QString &path) {
        return nook::normalizeUri(path);
    }
    static void wait(Operations &ops) {
        QTRY_VERIFY_WITH_TIMEOUT(!ops.busy(), 10000);
    }
  private Q_SLOTS:
    void pathsAndNames() {
        QTemporaryDir dir(QDir::currentPath() + "/fixture-XXXXXX");
        const auto a = uri(dir.path() + "/A:CON 中文 #?.txt");
        QVERIFY(a.contains("%23"));
        QVERIFY(a.contains("%3F"));
        QCOMPARE(nook::localPath(a), dir.path() + "/A:CON 中文 #?.txt");
        QVERIFY(nook::validName("CON"));
        QVERIFY(nook::validName("a:b."));
        QVERIFY(!nook::validName("a/b"));
        QVERIFY(!nook::validName(".."));
        QVERIFY(!nook::validName(QString(100, QChar(0x4e2d))));
        QVERIFY(uri(dir.path() + "/Name") != uri(dir.path() + "/name"));
        QVERIFY(nook::excludedPath("/proc/self", {}));
        QVERIFY(!nook::excludedPath("/process", {}));
        QVERIFY(nook::excludedPath("/home/a/.snapshots/1", {}));
        QVERIFY(!nook::excludedPath("/home/a/.config", {}));
    }
    void profileKeepsDesktopInputMethodPaths() {
        const bool hadProfile = qEnvironmentVariableIsSet("NOOK_PROFILE_DIR");
        const auto previous = qgetenv("NOOK_PROFILE_DIR");
        const auto restore = qScopeGuard([&] {
            nook::initializeProfile({});
            if (hadProfile)
                qputenv("NOOK_PROFILE_DIR", previous);
            else
                qunsetenv("NOOK_PROFILE_DIR");
        });
        qunsetenv("NOOK_PROFILE_DIR");
        QVERIFY(!nook::customProfile());
        AppSettings regular;
        QSettings previousFormat(QSettings::IniFormat, QSettings::UserScope, "nook", "preferences");
        QCOMPARE(regular.store().fileName(), previousFormat.fileName());
        QSettings organization(QSettings::IniFormat, QSettings::UserScope, "nook");
        const auto fallbackKey = "__profileFallbackTest_" + QString::number(QCoreApplication::applicationPid());
        const auto removeFallback = qScopeGuard([&] { organization.remove(fallbackKey); organization.sync(); });
        organization.setValue(fallbackKey, "inherited");
        organization.sync();
        QCOMPARE(regular.store().value(fallbackKey).toString(), "inherited");
        QCOMPARE(nook::storageDirectory(nook::Storage::Data),
                 QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation));
        QCOMPARE(nook::storageDirectory(nook::Storage::Cache),
                 QStandardPaths::writableLocation(QStandardPaths::CacheLocation));
        const auto configHome = qgetenv("XDG_CONFIG_HOME"), dataHome = qgetenv("XDG_DATA_HOME"),
                   cacheHome = qgetenv("XDG_CACHE_HOME"), inputMethod = qgetenv("QT_IM_MODULE");
        const auto genericConfig = QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation);
        const auto genericData = QStandardPaths::standardLocations(QStandardPaths::GenericDataLocation);
        QTemporaryDir fixture;
        QVERIFY(fixture.isValid());
        const auto profile = fixture.path() + "/独立配置";
        qputenv("NOOK_PROFILE_DIR", QFile::encodeName(profile));
        QVERIFY(nook::customProfile());
        AppSettings isolated;
        QVERIFY(!isolated.store().value(fallbackKey).isValid());
        isolated.set("theme", "dark");
        QCOMPARE(isolated.store().fileName(), profile + "/config/preferences.ini");
        QVERIFY(QFile::exists(isolated.store().fileName()));
        SearchIndex index;
        QCOMPARE(index.databasePath(), profile + "/data/index.sqlite");
        QVERIFY(QFile::exists(index.databasePath()));
        put(fixture.path() + "/preview.txt");
        makePreview(uri(fixture.path() + "/preview.txt"), 1);
        QVERIFY(QDir(profile + "/cache/previews").exists());
        const auto autostart = genericConfig + "/autostart/local.nook-background.desktop";
        const bool existed = QFile::exists(autostart);
        nook::syncAutostart(!existed);
        QCOMPARE(QFile::exists(autostart), existed);
        QCOMPARE(qgetenv("XDG_CONFIG_HOME"), configHome);
        QCOMPARE(qgetenv("XDG_DATA_HOME"), dataHome);
        QCOMPARE(qgetenv("XDG_CACHE_HOME"), cacheHome);
        QCOMPARE(qgetenv("QT_IM_MODULE"), inputMethod);
        QCOMPARE(QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation), genericConfig);
        QCOMPARE(QStandardPaths::standardLocations(QStandardPaths::GenericDataLocation), genericData);
        nook::initializeProfile(profile);
        QVERIFY(nook::customProfile());
        QVERIFY(!qEnvironmentVariableIsSet("NOOK_PROFILE_DIR"));
        QVERIFY(!QProcessEnvironment::systemEnvironment().contains("NOOK_PROFILE_DIR"));
        QCOMPARE(nook::workerEnvironment().value("NOOK_PROFILE_DIR"), profile);
        QCOMPARE(nook::storageDirectory(nook::Storage::Data), profile + "/data");
    }
    void sidebarPartitionFiltering() {
        const QString basic = "ebd0a0a2-b9e5-4433-87c0-68b6b72699c7";
        QVERIFY(nook::sidebarDataPartition("ntfs", basic, "Data"));
        QVERIFY(nook::sidebarDataPartition("ntfs3", basic.toUpper(), ""));
        QVERIFY(nook::sidebarDataPartition("exfat", basic, "Files"));
        QVERIFY(nook::sidebarDataPartition("vfat", basic, "User files"));
        QVERIFY(nook::sidebarDataPartition("ntfs", "0x07", ""));
        QVERIFY(!nook::sidebarDataPartition("vfat", "c12a7328-f81f-11d2-ba4b-00a0c93ec93b", ""));
        QVERIFY(!nook::sidebarDataPartition("ntfs", "de94bba4-06d1-4d40-a16a-bfd50179d6ac", ""));
        QVERIFY(!nook::sidebarDataPartition("vfat", "142f5f5d-b9e5-4433-87c0-68b6b72699c7", ""));
        QVERIFY(!nook::sidebarDataPartition("btrfs", "0fc63daf-8483-4772-8e79-3d69d8477de4", ""));
        QVERIFY(!nook::sidebarDataPartition("ntfs", "", "Recovery"));
        QVERIFY(!nook::sidebarDataPartition("ntfs", "", "系统保留"));
        QVERIFY(nook::sidebarDataPartition("ntfs", basic, "Recovery"));
        QVERIFY(nook::sidebarDataPartition("ntfs", basic, "Definitions"));
        QVERIFY(nook::sidebarDataPartition("ntfs", basic, "Benefits"));
        QVERIFY(!nook::sidebarDataPartition("vfat", "", ""));
    }
    void sizesUseWindowsUnits() {
        // KB/MB/GB in steps of 1024, as on Windows; never KiB/MiB.
        QVERIFY2(nook::formatSize(1536).endsWith(QStringLiteral("KB")), qPrintable(nook::formatSize(1536)));
        QVERIFY(nook::formatSize(1536).startsWith(QStringLiteral("1.5")));
        QVERIFY(nook::formatSize(3 * 1048576).endsWith(QStringLiteral("MB")));
        QVERIFY(nook::formatSize(5ll << 30).endsWith(QStringLiteral("GB")));
        QCOMPARE(nook::formatSize(5ll << 30), QStringLiteral("5 GB"));
        QCOMPARE(nook::formatSize(25), QStringLiteral("25 字节"));
    }
    void preferencesKeepTheirTypes() {
        // Values read back from the INI file are text; "false" must not become truthy in QML.
        AppSettings settings;
        settings.store().setValue("hoverPreview", "false");
        settings.store().setValue("animations", "true");
        settings.store().setValue("scrollSpeed", "5");
        settings.store().setValue("uiScale", "1.25");
        settings.store().setValue("columnWidths", QStringList{"500", "160", "110", "90"});
        const auto values = settings.values();
        QCOMPARE(values["hoverPreview"].typeId(), QMetaType::Bool);
        QVERIFY(!values["hoverPreview"].toBool());
        QCOMPARE(values["animations"].typeId(), QMetaType::Bool);
        QVERIFY(values["animations"].toBool());
        QCOMPARE(values["scrollSpeed"].typeId(), QMetaType::Int);
        QCOMPARE(values["uiScale"].typeId(), QMetaType::Double);
        QCOMPARE(values["columnWidths"].toList(), (QVariantList{500, 160, 110, 90}));
        QCOMPARE(values["columnWidths"].toList().first().typeId(), QMetaType::Int);
        settings.store().remove("hoverPreview");
        QVERIFY(!settings.values()["hoverPreview"].toBool()); // off unless the user turns it on
        settings.store().remove("animations");
        settings.store().remove("scrollSpeed");
        settings.store().remove("uiScale");
        settings.store().remove("columnWidths");
    }
    void savedPreferences() {
        AppSettings settings;
        settings.set("columnWidths", QVariantList{});
        QCOMPARE(settings.values()["columnWidths"].toList().size(), 0);
        settings.set("columnWidths", QVariantList{500, 160, 110, 90});
        AppSettings reloaded;
        QCOMPARE(reloaded.values()["columnWidths"].toList(), (QVariantList{500, 160, 110, 90}));
        settings.set("columnWidths", QVariantList{1, 2});
        QCOMPARE(settings.values()["columnWidths"].toList(), (QVariantList{500, 160, 110, 90}));
        settings.set("columnWidths", QVariantList{-1, 5000, 1, -4});
        QCOMPARE(settings.values()["columnWidths"].toList(), (QVariantList{140, 1600, 70, 65}));
        QSignalSpy resets(&settings, &AppSettings::columnWidthsReset);
        settings.set("columnWidths", QVariantList{});
        settings.set("columnWidths", QVariantList{});
        QCOMPARE(resets.count(), 2);
        settings.set("scrollSpeed", 100);
        QCOMPARE(settings.values()["scrollSpeed"].toInt(), 8);
        settings.set("scrollSpeed", 0);
        QCOMPARE(settings.values()["scrollSpeed"].toInt(), 1);
        settings.set("scrollSpeed", 3);
    }
    void recentLocationsFollowSuccessfulNavigation() {
        QTemporaryDir fixture;
        QVERIFY(fixture.isValid());
        for (int i = 0; i < 7; ++i)
            QVERIFY(QDir().mkdir(fixture.path() + "/folder" + QString::number(i)));
        AppSettings settings;
        settings.set("startFolder", fixture.path());
        settings.set("rememberRecentLocations", true);
        settings.store().remove("recentLocations");
        SearchIndex search(fixture.path() + "/index.sqlite");
        Operations operations;
        Application app(&search, &operations, &settings, false);
        QTRY_VERIFY(!app.left()->loading());
        QVERIFY(app.recentLocations().isEmpty()); // initial/session locations are not new visits
        for (int i = 0; i < 7; ++i) {
            app.navigate(uri(fixture.path() + "/folder" + QString::number(i)));
            QTRY_VERIFY(!app.activePane()->loading());
        }
        QCOMPARE(app.recentLocations().size(), 5);
        QCOMPARE(app.recentLocations().first().toMap()["uri"].toString(), uri(fixture.path() + "/folder6"));
        app.left()->back();
        QTRY_VERIFY(!app.left()->loading());
        QCOMPARE(app.recentLocations().first().toMap()["uri"].toString(), uri(fixture.path() + "/folder5"));
        app.left()->forward();
        QTRY_VERIFY(!app.left()->loading());
        QCOMPARE(app.recentLocations().first().toMap()["uri"].toString(), uri(fixture.path() + "/folder6"));
        app.setDual(true);
        QTRY_VERIFY(!app.right()->loading());
        app.setActiveSide(1);
        const auto leftUri = app.left()->uri();
        app.navigate(uri(fixture.path() + "/folder0"));
        QTRY_VERIFY(!app.right()->loading());
        QCOMPARE(app.left()->uri(), leftUri);
        QCOMPARE(app.recentLocations().first().toMap()["uri"].toString(), app.right()->uri());
        const auto beforeFailure = app.recentLocations();
        app.navigate(uri(fixture.path() + "/missing"));
        QTRY_VERIFY(!app.right()->loading());
        QVERIFY(!app.right()->error().isEmpty());
        QCOMPARE(app.recentLocations(), beforeFailure);
        app.navigate(uri(fixture.path() + "/folder1"));
        QTRY_VERIFY(!app.right()->loading());
        const auto saved = app.recentLocations();
        app.right()->refresh();
        QTRY_VERIFY(!app.right()->loading());
        QCOMPARE(app.recentLocations(), saved);
        {
            AppSettings reloaded;
            Application restored(&search, &operations, &reloaded, true);
            QTRY_VERIFY(!restored.left()->loading());
            QCOMPARE(restored.recentLocations(), saved);
        }
        app.clearRecentLocations();
        app.right()->refresh();
        QTRY_VERIFY(!app.right()->loading());
        QVERIFY(app.recentLocations().isEmpty());
        QVERIFY(!settings.store().contains("recentLocations"));
        app.navigate(uri(fixture.path() + "/folder2"));
        QTRY_VERIFY(!app.right()->loading());
        QCOMPARE(app.recentLocations().size(), 1);
        settings.set("rememberRecentLocations", false);
        QVERIFY(app.recentLocations().isEmpty());
        app.navigate(uri(fixture.path() + "/folder3"));
        QTRY_VERIFY(!app.right()->loading());
        QVERIFY(app.recentLocations().isEmpty());
        QVERIFY(!settings.store().contains("recentLocations"));
        // Turning recording off also discards a navigation still in flight.
        settings.set("rememberRecentLocations", true);
        app.navigate(uri(fixture.path() + "/folder4"));
        settings.set("rememberRecentLocations", false);
        settings.set("rememberRecentLocations", true);
        QTRY_VERIFY(!app.right()->loading());
        QVERIFY(app.recentLocations().isEmpty());
    }
    void recentLocationsSynchronizeAcrossWindows() {
        QTemporaryDir fixture;
        QVERIFY(fixture.isValid());
        for (const auto &name : {"one", "two", "three"})
            QVERIFY(QDir().mkdir(fixture.path() + "/" + name));
        AppSettings settings;
        settings.set("startFolder", fixture.path());
        settings.set("rememberRecentLocations", true);
        settings.store().remove("recentLocations");
        SearchIndex search(fixture.path() + "/index.sqlite");
        Operations operations;
        Application first(&search, &operations, &settings, false);
        Application second(&search, &operations, &settings, false);
        QTRY_VERIFY(!first.left()->loading() && !second.left()->loading());
        first.navigate(uri(fixture.path() + "/one"));
        QTRY_VERIFY(!first.left()->loading());
        QCOMPARE(first.recentLocations(), second.recentLocations());
        QCOMPARE(second.recentLocations().size(), 1);
        second.navigate(uri(fixture.path() + "/two"));
        first.clearRecentLocations();
        QTRY_VERIFY(!second.left()->loading());
        QVERIFY(first.recentLocations().isEmpty());
        QVERIFY(second.recentLocations().isEmpty());
        second.navigate(uri(fixture.path() + "/three"));
        QTRY_VERIFY(!second.left()->loading());
        QCOMPARE(first.recentLocations(), second.recentLocations());
        QCOMPARE(first.recentLocations().size(), 1);
        QCOMPARE(settings.store().value("recentLocations").toStringList(),
                 QStringList{uri(fixture.path() + "/three")});
        settings.set("rememberRecentLocations", false);
        QVERIFY(first.recentLocations().isEmpty());
        QVERIFY(second.recentLocations().isEmpty());
        QVERIFY(!settings.store().contains("recentLocations"));
    }
    void openWithUriApplications() {
        // Desktop metadata and MIME associations live exclusively in the test's isolated XDG homes.
        QTemporaryDir fixture(QDir::currentPath() + "/openwith-XXXXXX");
        const auto appsDir = qEnvironmentVariable("XDG_DATA_HOME") + "/applications";
        const auto configDir = qEnvironmentVariable("XDG_CONFIG_HOME");
        QVERIFY(QDir().mkpath(appsDir));
        QVERIFY(QDir().mkpath(configDir));
        const auto script = fixture.path() + "/receive-uri.sh";
        const auto marker = fixture.path() + "/received-uri";
        put(script, ("#!/bin/sh\nprintf '%s' \"$1\" > \"" + marker + "\"\n").toUtf8());
        put(appsDir + "/nook-test-uri.desktop",
            ("[Desktop Entry]\nType=Application\nName=Nook URI Test\nExec=/bin/sh \"" + script +
             "\" %U\nTerminal=false\nMimeType=text/plain;\n")
                .toUtf8());
        put(configDir + "/mimeapps.list", "[Added Associations]\ntext/plain=nook-test-uri.desktop;\n");
        put(fixture.path() + "/sample.txt");
        AppSettings settings;
        SearchIndex search(fixture.path() + "/index.sqlite");
        Operations ops;
        Application controller(&search, &ops, &settings, false);
        const auto target = uri(fixture.path() + "/sample.txt");
        bool found = false;
        const auto applications = controller.openWithApplications(target);
        for (const auto &application : applications)
            found |= application.toMap()["id"] == "nook-test-uri.desktop";
        QVERIFY2(found, "URI-only (%U) MIME application must be available in Open With");
        controller.openWith(target, "nook-test-uri.desktop");
        QTRY_VERIFY_WITH_TIMEOUT(QFileInfo(marker).size() > 0, 5000);
        QFile result(marker);
        QVERIFY(result.open(QIODevice::ReadOnly));
        // GIO may substitute a local filename for a file:// URI even with %U.
        QCOMPARE(nook::normalizeUri(QString::fromUtf8(result.readAll())), target);
        QVERIFY(QFile::remove(appsDir + "/nook-test-uri.desktop"));
        QVERIFY(QFile::remove(configDir + "/mimeapps.list"));
    }
    void unicodeAndPinyin() {
        auto keys = nook::pinyinKeys("农业经济学.txt");
        QVERIFY2(keys.first.startsWith("nongyejingjixue"), qPrintable(keys.first));
        QVERIFY(keys.second.startsWith("nyjjx"));
        QCOMPARE(nook::folded("École"), QString::fromUtf8("école"));
    }
    void directoryCancellationAndSelection() {
        QTemporaryDir dir(QDir::currentPath() + "/fixture-XXXXXX");
        QDir().mkdir(dir.path() + "/folder");
        put(dir.path() + "/中文.txt");
        put(dir.path() + "/A");
        put(dir.path() + "/a");
        put(dir.path() + "/.hidden");
        QByteArray raw = QFile::encodeName(dir.path()) + "/raw-\xff";
        FILE *file = fopen(raw.constData(), "w");
        QVERIFY(file);
        fclose(file);
        DirectoryModel model;
        model.navigate(dir.path());
        QTRY_VERIFY_WITH_TIMEOUT(!model.loading(), 5000);
        QCOMPARE(model.count(), 5);
        QCOMPARE(model.entry(0)["fileName"].toString(), QString("folder"));
        model.select(1);
        QCOMPARE(model.selectedCount(), 1);
        model.select(2, 1);
        QCOMPARE(model.selectedCount(), 2);
        model.refresh();
        QTRY_VERIFY_WITH_TIMEOUT(!model.loading(), 5000);
        QCOMPARE(model.selectedCount(), 2);
        model.setShowHidden(true);
        QTRY_VERIFY_WITH_TIMEOUT(!model.loading(), 5000);
        QCOMPARE(model.count(), 6);
        QString rawUri;
        for (int i = 0; i < model.count(); ++i)
            if (model.entry(i)["fileUri"].toString().contains("%FF", Qt::CaseInsensitive))
                rawUri = model.entry(i)["fileUri"].toString();
        QVERIFY(!rawUri.isEmpty());
        model.navigate(dir.path() + "/missing");
        model.navigate(dir.path() + "/folder");
        QTRY_VERIFY_WITH_TIMEOUT(!model.loading(), 5000);
        QCOMPARE(model.count(), 0);
        QVERIFY(model.error().isEmpty());
        QVERIFY(::unlink(raw.constData()) == 0);
    }
    void selectionAfterShrinkAndSort() {
        QTemporaryDir dir(QDir::currentPath() + "/fixture-XXXXXX");
        for (const auto &name : {"a", "b", "c"})
            put(dir.path() + "/" + name);
        DirectoryModel model;
        model.navigate(dir.path());
        QTRY_VERIFY_WITH_TIMEOUT(!model.loading(), 5000);
        QCOMPARE(model.count(), 3);
        model.select(2);
        QVERIFY(QFile::remove(dir.path() + "/c"));
        model.refresh();
        QTRY_VERIFY_WITH_TIMEOUT(!model.loading(), 5000);
        QCOMPARE(model.count(), 2);
        model.select(0, 2); // stale last-row anchor must never index beyond the new list
        QCOMPARE(model.selectedCount(), 1);
        model.select(1);
        model.sortBy("name", true);
        QTRY_VERIFY_WITH_TIMEOUT(!model.loading(), 5000);
        model.select(1, 2); // reset anchor rather than extending from a different file
        QCOMPARE(model.selectedCount(), 1);
    }
    void safeFileOperations() {
        QTemporaryDir dir(QDir::currentPath() + "/fixture-XXXXXX");
        QDir().mkdir(dir.path() + "/out");
        QDir().mkdir(dir.path() + "/tree");
        put(dir.path() + "/tree/inside", "inside");
        put(dir.path() + "/file", "original");
        QVERIFY(::symlink("../file", QFile::encodeName(dir.path() + "/tree/link").constData()) == 0);
        Operations ops;
        ops.run("copy", {uri(dir.path() + "/tree")}, uri(dir.path() + "/out"));
        wait(ops);
        QVERIFY2(ops.errors().isEmpty(), qPrintable(ops.errors().join("\n")));
        QVERIFY(QFileInfo::exists(dir.path() + "/out/tree/inside"));
        QVERIFY(QFileInfo(dir.path() + "/out/tree/link").isSymLink());
        ops.run("copy", {uri(dir.path() + "/tree")}, uri(dir.path() + "/tree"));
        wait(ops);
        QVERIFY(!ops.errors().isEmpty());
        ops.run("rename", {uri(dir.path() + "/file")}, {}, "CON:a.");
        wait(ops);
        QVERIFY(ops.errors().isEmpty());
        QVERIFY(QFileInfo::exists(dir.path() + "/CON:a."));
        ops.run("move", {uri(dir.path() + "/CON:a.")}, uri(dir.path() + "/out"));
        wait(ops);
        QVERIFY(ops.errors().isEmpty());
        QVERIFY(!QFileInfo::exists(dir.path() + "/CON:a."));
        ops.run("mkdir", {}, uri(dir.path()), "new folder");
        wait(ops);
        QVERIFY(QFileInfo(dir.path() + "/new folder").isDir());
        ops.run("delete", {uri(dir.path() + "/out/tree")});
        wait(ops);
        QVERIFY(ops.errors().isEmpty());
        QVERIFY(QFileInfo::exists(dir.path() + "/out/CON:a."));
    }
    void collisionAndCancellation() {
        QTemporaryDir dir(QDir::currentPath() + "/fixture-XXXXXX");
        QDir().mkdir(dir.path() + "/out");
        put(dir.path() + "/file", "new");
        put(dir.path() + "/out/file", "old");
        Operations ops;
        ops.run("copy", {uri(dir.path() + "/file")}, uri(dir.path() + "/out"));
        QTRY_VERIFY_WITH_TIMEOUT(ops.waitingConflict(), 5000);
        ops.resolveConflict(0);
        wait(ops);
        QFile original(dir.path() + "/out/file");
        QVERIFY(original.open(QIODevice::ReadOnly));
        QCOMPARE(original.readAll(), QByteArray("old"));
        original.close();
        ops.run("copy", {uri(dir.path() + "/file")}, uri(dir.path() + "/out"));
        QTRY_VERIFY_WITH_TIMEOUT(ops.waitingConflict(), 5000);
        ops.resolveConflict(2);
        wait(ops);
        QVERIFY(QFileInfo::exists(dir.path() + "/out/file (2)"));
        ops.run("copy", {uri(dir.path() + "/file")}, uri(dir.path() + "/out"));
        QTRY_VERIFY_WITH_TIMEOUT(ops.waitingConflict(), 5000);
        ops.cancel();
        wait(ops);
        QVERIFY(original.open(QIODevice::ReadOnly));
        QCOMPARE(original.readAll(), QByteArray("old"));
    }
    void crossFilesystemMove() {
        QTemporaryDir dir(QDir::currentPath() + "/fixture-XXXXXX");
        QTemporaryDir dest("/tmp/nook-cross-XXXXXX");
        QDir().mkdir(dir.path() + "/nested");
        put(dir.path() + "/nested/file", "must survive move");
        Operations ops;
        ops.run("move", {uri(dir.path() + "/nested")}, uri(dest.path()));
        wait(ops);
        QVERIFY2(ops.errors().isEmpty(), qPrintable(ops.errors().join("\n")));
        QVERIFY(!QFileInfo::exists(dir.path() + "/nested"));
        QFile file(dest.path() + "/nested/file");
        QVERIFY(file.open(QIODevice::ReadOnly));
        QCOMPARE(file.readAll(), QByteArray("must survive move"));
    }
    void trashAndRestore() {
        QTemporaryDir dir(QDir::currentPath() + "/fixture-XXXXXX");
        const QString path = dir.path() + "/trash-roundtrip.txt";
        put(path, "restore me");
        Operations ops;
        ops.run("trash", {uri(path)});
        wait(ops);
        QVERIFY2(ops.errors().isEmpty(), qPrintable(ops.errors().join("\n")));
        QVERIFY(!QFileInfo::exists(path));
        auto cancel = std::make_shared<std::atomic_bool>(false);
        QString failure, trashed;
        auto entries = nook::enumerate("trash:///", true, cancel, &failure);
        QVERIFY2(failure.isEmpty(), qPrintable(failure));
        for (const auto &entry : entries)
            if (entry.originalUri == uri(path))
                trashed = entry.uri;
        QVERIFY2(!trashed.isEmpty(), "Isolated GVfs trash must expose the moved test file");
        ops.run("restore", {trashed});
        wait(ops);
        QVERIFY2(ops.errors().isEmpty(), qPrintable(ops.errors().join("\n")));
        QFile file(path);
        QVERIFY(file.open(QIODevice::ReadOnly));
        QCOMPARE(file.readAll(), QByteArray("restore me"));
    }
    void indexLifecycleAndQueries() {
        QTemporaryDir dir(QDir::currentPath() + "/fixture-XXXXXX");
        QDir().mkdir(dir.path() + "/data");
        QDir().mkdir(dir.path() + "/.cache");
        put(dir.path() + "/data/农业经济学.txt");
        put(dir.path() + "/data/城市照片.png");
        put(dir.path() + "/.cache/secret");
        put(dir.path() + "/data/A.txt");
        put(dir.path() + "/data/a.txt");
        put(dir.path() + "/data/100%.txt");
        QVERIFY(::symlink("../data", QFile::encodeName(dir.path() + "/data/loop").constData()) == 0);
        SearchIndex index(dir.path() + "/db/index.sqlite");
        index.setRoots({dir.path() + "/data", dir.path() + "/.cache"});
        index.start();
        QTRY_VERIFY_WITH_TIMEOUT(!index.scanning(), 10000);
        QVERIFY2(index.warnings().isEmpty(), qPrintable(index.warnings().join("\n")));
        index.query("nyjjx");
        QTRY_VERIFY(!index.model()->loading());
        QCOMPARE(index.model()->count(), 1);
        index.query("nongye");
        QTRY_VERIFY(!index.model()->loading());
        QCOMPARE(index.model()->count(), 1);
        index.query("农");
        QTRY_VERIFY(!index.model()->loading());
        QCOMPARE(index.model()->count(), 1);
        index.query("照片", {}, "image");
        QTRY_VERIFY(!index.model()->loading());
        QCOMPARE(index.model()->count(), 1);
        index.query("100%");
        QTRY_VERIFY(!index.model()->loading());
        QCOMPARE(index.model()->count(), 1);
        index.query(".txt");
        QTRY_VERIFY(!index.model()->loading());
        QCOMPARE(index.model()->count(), 4);
        index.query("secret");
        QTRY_VERIFY(!index.model()->loading());
        QCOMPARE(index.model()->count(), 0);
        QFile::remove(dir.path() + "/data/农业经济学.txt");
        index.start();
        QTRY_VERIFY_WITH_TIMEOUT(!index.scanning(), 10000);
        index.query("nyjjx");
        QTRY_VERIFY(!index.model()->loading());
        QCOMPARE(index.model()->count(), 0);
        index.stop();
    }
    void incrementalRefresh() {
        // Replaying the diff must always reproduce the target list exactly.
        auto make = [](const QStringList &names) {
            QList<nook::Entry> rows;
            for (const auto &name : names) {
                nook::Entry e;
                e.uri = "file:///x/" + name;
                e.name = name;
                rows.append(e);
            }
            return rows;
        };
        QRandomGenerator random(7);
        for (int round = 0; round < 300; ++round) {
            QStringList pool;
            for (int i = 0; i < 40; ++i)
                pool.append(QString::number(i));
            QStringList before, after;
            for (const auto &name : pool) {
                if (random.bounded(3))
                    before.append(name);
                if (random.bounded(3))
                    after.append(name);
            }
            for (int i = 0; i < random.bounded(4); ++i)
                if (after.size() > 1)
                    after.move(random.bounded(int(after.size())), random.bounded(int(after.size())));
            auto current = make(before);
            const auto next = make(after);
            if (!after.isEmpty() && random.bounded(2))
                const_cast<nook::Entry &>(next[0]).size = 99;
            QList<DirectoryModel::Change> changes;
            if (!DirectoryModel::diff(current, next, &changes))
                continue;
            for (const auto &c : changes) {
                if (c.kind == DirectoryModel::Change::Remove)
                    current.remove(c.at, c.count);
                else if (c.kind == DirectoryModel::Change::Insert)
                    for (int k = 0; k < c.entries.size(); ++k)
                        current.insert(c.at + k, c.entries[k]);
                else if (c.kind == DirectoryModel::Change::Move) {
                    QVERIFY(c.at < c.from);
                    current.move(c.from, c.at);
                } else
                    current[c.at] = c.entries.first();
            }
            QCOMPARE(current.size(), next.size());
            for (int i = 0; i < next.size(); ++i) {
                QCOMPARE(current[i].uri, next[i].uri);
                QCOMPARE(current[i].size, next[i].size);
            }
        }
        // A real refresh inserts/removes rows in place and keeps the selection: no model reset.
        QTemporaryDir dir(QDir::currentPath() + "/fixture-XXXXXX");
        put(dir.path() + "/b.txt");
        put(dir.path() + "/d.txt");
        DirectoryModel model;
        model.navigate(dir.path());
        QTRY_VERIFY_WITH_TIMEOUT(!model.loading(), 5000);
        model.select(1);
        QSignalSpy resets(&model, &QAbstractItemModel::modelReset);
        QSignalSpy inserted(&model, &QAbstractItemModel::rowsInserted);
        QSignalSpy removed(&model, &QAbstractItemModel::rowsRemoved);
        put(dir.path() + "/c.txt");
        QVERIFY(QFile::remove(dir.path() + "/b.txt"));
        model.refresh();
        QTRY_VERIFY_WITH_TIMEOUT(!model.loading(), 5000);
        QCOMPARE(resets.count(), 0);
        QCOMPARE(inserted.count(), 1);
        QCOMPARE(removed.count(), 1);
        QCOMPARE(model.count(), 2);
        QCOMPARE(model.entry(0)["fileName"].toString(), QStringLiteral("c.txt"));
        QCOMPARE(model.selectedUris(), QStringList{uri(dir.path() + "/d.txt")});
        QCOMPARE(model.firstSelected()["fileName"].toString(), QStringLiteral("d.txt"));
    }
    void rescanKeepsUnchangedRows() {
        QTemporaryDir dir(QDir::currentPath() + "/fixture-XXXXXX");
        QDir().mkdir(dir.path() + "/data");
        put(dir.path() + "/data/stable.txt");
        put(dir.path() + "/data/grows.txt", "1");
        const QString database = dir.path() + "/db/index.sqlite";
        SearchIndex index(database);
        index.setRoots({dir.path() + "/data"});
        index.start();
        QTRY_VERIFY_WITH_TIMEOUT(!index.scanning(), 10000);
        auto seenOf = [&](const QString &name) {
            sqlite3 *db = nullptr;
            sqlite3_open_v2(QFile::encodeName(database).constData(), &db, SQLITE_OPEN_READONLY, nullptr);
            sqlite3_stmt *stmt = nullptr;
            sqlite3_prepare_v2(db, "SELECT seen FROM entries WHERE name=?", -1, &stmt, nullptr);
            const auto bytes = name.toUtf8();
            sqlite3_bind_text(stmt, 1, bytes.constData(), bytes.size(), SQLITE_TRANSIENT);
            const qint64 seen = sqlite3_step(stmt) == SQLITE_ROW ? sqlite3_column_int64(stmt, 0) : -1;
            sqlite3_finalize(stmt);
            sqlite3_close(db);
            return seen;
        };
        const auto before = seenOf("stable.txt");
        QVERIFY(before > 0);
        put(dir.path() + "/data/grows.txt", QByteArray(4096, 'x'));
        put(dir.path() + "/data/new.txt");
        QTest::qWait(5);
        index.start();
        QTRY_VERIFY_WITH_TIMEOUT(!index.scanning(), 10000);
        // An unchanged entry is only read during verification, never rewritten.
        QCOMPARE(seenOf("stable.txt"), before);
        QVERIFY(seenOf("new.txt") > 0);
        index.query("grows", {}, "all", 4000);
        QTRY_VERIFY(!index.model()->loading());
        QCOMPARE(index.model()->count(), 1);
        index.stop();
    }
    // Generated and cached folders stay out of the index unless asked for; names that need no
    // pinyin store none, and Chinese names are still found by it.
    void noiseFoldersAndCompactRows() {
        QTemporaryDir dir(QDir::currentPath() + "/fixture-XXXXXX");
        const auto data = dir.path() + "/data";
        for (const auto &folder : {"/node_modules/pkg", "/proj/target/debug", "/proj/src", "/env", "/.git/objects",
                                   "/app/Code Cache", "/cmake-out"})
            QVERIFY(QDir().mkpath(data + folder));
        put(data + "/node_modules/pkg/index.js");
        put(data + "/proj/target/CACHEDIR.TAG", "Signature: 8a477f597d28d172789f06886806bc55");
        put(data + "/proj/target/debug/needle-build.bin");
        put(data + "/proj/src/needle.rs");
        put(data + "/env/pyvenv.cfg");
        put(data + "/env/needle-venv.py");
        put(data + "/.git/objects/needle-object");
        put(data + "/app/Code Cache/needle-cache");
        put(data + "/cmake-out/CMakeCache.txt");
        put(data + "/cmake-out/needle-cmake.o");
        put(data + "/农业经济学.txt");
        const QString database = dir.path() + "/db/index.sqlite";
        SearchIndex index(database);
        index.setRoots({data});
        index.setIncluded(true, false);
        auto names = [&](const QString &text) {
            index.query(text);
            QTest::qWaitFor([&] { return !index.model()->loading(); }, 5000);
            QStringList found;
            for (int row = 0; row < index.model()->count(); ++row)
                found << index.model()->entry(row)["fileName"].toString();
            found.sort();
            return found;
        };
        index.start();
        QTRY_VERIFY_WITH_TIMEOUT(!index.scanning(), 10000);
        QCOMPARE(names("needle"), QStringList{"needle.rs"});
        QCOMPARE(names("node_modules"), QStringList{});
        QCOMPARE(names("target"), QStringList{});
        // Pinyin, initials and short queries still reach Chinese names.
        QCOMPARE(names("nongye"), QStringList{"农业经济学.txt"});
        QCOMPARE(names("nyjj"), QStringList{"农业经济学.txt"});
        QCOMPARE(names("经济"), QStringList{"农业经济学.txt"});
        QCOMPARE(names("ny"), QStringList{"农业经济学.txt"});
        {
            sqlite3 *db = nullptr;
            sqlite3_open_v2(QFile::encodeName(database).constData(), &db, SQLITE_OPEN_READONLY, nullptr);
            sqlite3_stmt *stmt = nullptr;
            sqlite3_prepare_v2(db, "SELECT name,path,pinyin,initials FROM entries WHERE name IN ('needle.rs','农业经济学.txt') ORDER BY name",
                               -1, &stmt, nullptr);
            QCOMPARE(sqlite3_step(stmt), SQLITE_ROW);
            QCOMPARE(QString::fromUtf8(reinterpret_cast<const char *>(sqlite3_column_text(stmt, 0))), QString("needle.rs"));
            QCOMPARE(QByteArray(reinterpret_cast<const char *>(sqlite3_column_text(stmt, 1))),
                     QFile::encodeName(data + "/proj/src/needle.rs"));
            QCOMPARE(sqlite3_column_bytes(stmt, 2), 0);
            QCOMPARE(sqlite3_column_bytes(stmt, 3), 0);
            QCOMPARE(sqlite3_step(stmt), SQLITE_ROW);
            QCOMPARE(QString::fromUtf8(reinterpret_cast<const char *>(sqlite3_column_text(stmt, 3))), QString("nyjjx.txt"));
            sqlite3_finalize(stmt);
            sqlite3_close(db);
        }
        // Asked for, they come back; turned off again, their rows go.
        index.setIncluded(true, true);
        index.start();
        QTRY_VERIFY_WITH_TIMEOUT(!index.scanning(), 10000);
        QCOMPARE(names("needle"), (QStringList{"needle-build.bin", "needle-cache", "needle-cmake.o", "needle-object",
                                              "needle-venv.py", "needle.rs"}));
        index.setIncluded(true, false);
        index.start();
        QTRY_VERIFY_WITH_TIMEOUT(!index.scanning(), 10000);
        QCOMPARE(names("needle"), QStringList{"needle.rs"});
        index.stop();
        QVERIFY(nook::noisePath("a/node_modules/b.js"));
        QVERIFY(nook::noisePath("Users/x/AppData/Code Cache"));
        QVERIFY(!nook::noisePath("Users/x/Documents/build/report.docx"));
    }
    // An index from before v3 (URIs, pinyin everywhere) is emptied and rebuilt by the next scan.
    void oldIndexIsRebuilt() {
        QTemporaryDir dir(QDir::currentPath() + "/fixture-XXXXXX");
        QDir().mkdir(dir.path() + "/data");
        QDir().mkdir(dir.path() + "/db");
        put(dir.path() + "/data/fresh.txt");
        const QString database = dir.path() + "/db/index.sqlite";
        {
            sqlite3 *db = nullptr;
            QCOMPARE(sqlite3_open(QFile::encodeName(database).constData(), &db), SQLITE_OK);
            QCOMPARE(sqlite3_exec(db,
                                  "CREATE TABLE entries(id INTEGER PRIMARY KEY,uri TEXT UNIQUE NOT NULL,root TEXT NOT NULL,"
                                  "name TEXT NOT NULL,name_fold TEXT NOT NULL,pinyin TEXT NOT NULL,initials TEXT NOT NULL,"
                                  "folder INTEGER,size INTEGER,mtime INTEGER,icon TEXT,seen INTEGER,ref INTEGER);"
                                  "INSERT INTO entries(uri,root,name,name_fold,pinyin,initials) "
                                  "VALUES('file:///gone/old.txt','/gone','old.txt','old.txt','old.txt','old.txt');"
                                  "PRAGMA user_version=2;",
                                  nullptr, nullptr, nullptr),
                     SQLITE_OK);
            sqlite3_close(db);
        }
        SearchIndex index(database);
        index.setRoots({dir.path() + "/data"});
        index.start();
        QTRY_VERIFY_WITH_TIMEOUT(!index.scanning(), 10000);
        index.query("old");
        QTRY_VERIFY(!index.model()->loading());
        QCOMPARE(index.model()->count(), 0);
        index.query("fresh");
        QTRY_VERIFY(!index.model()->loading());
        QCOMPARE(index.model()->count(), 1);
        index.stop();
    }
    void incrementalFolderTimes() {
        // Between daily verifications a sweep trusts unchanged folder times: names stay exact
        // (any add/remove/rename changes the folder's mtime), only in-place edits wait a day.
        QTemporaryDir dir(QDir::currentPath() + "/fixture-XXXXXX");
        const auto data = dir.path() + "/data";
        QVERIFY(QDir().mkpath(data + "/old/deep"));
        put(data + "/old/a.txt", "1");
        put(data + "/old/deep/b.txt");
        const time_t twoDaysAgo = time(nullptr) - 172800;
        auto age = [twoDaysAgo](const QString &path) { // as if last changed two days ago
            const timespec when[2] = {{twoDaysAgo, 0}, {twoDaysAgo, 0}};
            QCOMPARE(::utimensat(AT_FDCWD, QFile::encodeName(path).constData(), when, 0), 0);
        };
        age(data + "/old");
        age(data + "/old/deep");
        const QString database = dir.path() + "/db/index.sqlite";
        SearchIndex index(database);
        index.setRoots({data});
        auto scan = [&] {
            index.start();
            QTRY_VERIFY_WITH_TIMEOUT(!index.scanning(), 10000);
        };
        auto found = [&](const QString &text, qint64 minBytes = 0) {
            index.query(text, {}, "all", minBytes);
            [&] { QTRY_VERIFY(!index.model()->loading()); }();
            return index.model()->count();
        };
        scan(); // first pass verifies everything
        QCOMPARE(found("b.txt"), 1);
        QTest::qWait(2100); // the folders above are now older than the previous pass
        put(data + "/old/a.txt", QByteArray(4096, 'x')); // in-place edit: folder time unchanged
        age(data + "/old");
        put(data + "/old/deep/c.txt");                    // new name: folder time changes
        QVERIFY(QFile::remove(data + "/old/deep/b.txt"));
        scan();
        QCOMPARE(found("c.txt"), 1);
        QCOMPARE(found("b.txt"), 0);
        QCOMPARE(found("a.txt", 4000), 0); // listing-only pass: the size waits for verification
        {
            sqlite3 *db = nullptr;
            sqlite3_open_v2(QFile::encodeName(database).constData(), &db, SQLITE_OPEN_READWRITE, nullptr);
            QCOMPARE(sqlite3_exec(db, "UPDATE meta SET value=0 WHERE key='verified'", nullptr, nullptr, nullptr),
                     SQLITE_OK);
            sqlite3_close(db);
        }
        scan(); // the daily verification catches the edit
        QCOMPARE(found("a.txt", 4000), 1);
        index.stop();
    }
    void hiddenAttribute() {
        QTemporaryDir dir(QDir::currentPath() + "/fixture-XXXXXX");
        put(dir.path() + "/plain.txt");
        put(dir.path() + "/.dotted");
        put(dir.path() + "/dotted");
        QString error, next;
        QVERIFY(nook::setHidden(uri(dir.path() + "/plain.txt"), true, &error, &next));
        QVERIFY(nook::hiddenFile(QFile::encodeName(dir.path() + "/plain.txt")));
        QVERIFY(QFileInfo::exists(dir.path() + "/plain.txt"));
        DirectoryModel model;
        model.navigate(dir.path());
        QTRY_VERIFY_WITH_TIMEOUT(!model.loading(), 5000);
        QCOMPARE(model.count(), 1); // only "dotted"
        model.setShowHidden(true);
        QTRY_VERIFY_WITH_TIMEOUT(!model.loading(), 5000);
        int hidden = 0;
        for (int i = 0; i < model.count(); ++i)
            hidden += model.entry(i)["hidden"].toBool();
        QCOMPARE(hidden, 3); // .dotted, .hidden, plain.txt
        QVERIFY(nook::setHidden(uri(dir.path() + "/plain.txt"), false, &error, &next));
        QVERIFY(!nook::hiddenFile(QFile::encodeName(dir.path() + "/plain.txt")));
        QVERIFY(!QFileInfo::exists(dir.path() + "/.hidden")); // emptied list is removed
        // A dot name un-hides by renaming, but never over an existing file.
        QVERIFY(!nook::setHidden(uri(dir.path() + "/.dotted"), false, &error, &next));
        QVERIFY(error.contains("dotted"));
        QVERIFY(QFile::remove(dir.path() + "/dotted"));
        QVERIFY(nook::setHidden(uri(dir.path() + "/.dotted"), false, &error, &next));
        QCOMPARE(next, uri(dir.path() + "/dotted"));
        QVERIFY(QFileInfo::exists(dir.path() + "/dotted"));
    }
    void dropAndQuickAccess() {
        QTemporaryDir fixture(QDir::currentPath() + "/fixture-XXXXXX");
        QDir().mkdir(fixture.path() + "/folder");
        put(fixture.path() + "/a.txt");
        AppSettings settings;
        SearchIndex search(fixture.path() + "/index.sqlite");
        Operations ops;
        Application controller(&search, &ops, &settings, false);
        const auto here = uri(fixture.path()), file = uri(fixture.path() + "/a.txt"),
                   folder = uri(fixture.path() + "/folder");
        // Dropping back into the same folder, or a folder onto itself, is a silent no-op.
        QVERIFY(!controller.canDrop({file}, here));
        QVERIFY(!controller.canDrop({folder}, folder));
        QVERIFY(controller.canDrop({file}, folder));
        QCOMPARE(controller.dropAction({file, folder}, folder), QStringLiteral("move"));
        QCOMPARE(controller.dropAction({file}, "trash:///"), QStringLiteral("trash"));
        QSignalSpy started(&ops, &Operations::changed);
        controller.dragDrop({file}, here);
        QVERIFY(!ops.busy());
        QCOMPARE(started.count(), 0);
        controller.dragDrop({file}, folder);
        wait(ops);
        QVERIFY2(ops.errors().isEmpty(), qPrintable(ops.errors().join("\n")));
        QVERIFY(QFileInfo::exists(fixture.path() + "/folder/a.txt"));
        QVERIFY(!QFileInfo::exists(fixture.path() + "/a.txt"));
        // Built-in places are removable too, and come back with a reset.
        controller.removeQuickAccess("trash:///");
        auto hasTrash = [&] {
            for (const auto &place : controller.places())
                if (place.toMap()["uri"] == "trash:///")
                    return true;
            return false;
        };
        QTRY_VERIFY(!hasTrash());
        controller.resetQuickAccess();
        QTRY_VERIFY(hasTrash());
        QVERIFY(!controller.openLocation(fixture.path() + "/missing"));
        QVERIFY(controller.openLocation(fixture.path() + "/folder/a.txt"));
        QTRY_COMPARE(controller.activePane()->uri(), folder);
    }
    void paginationAndStaleQueries() {
        QTemporaryDir dir(QDir::currentPath() + "/fixture-XXXXXX");
        QDir().mkdir(dir.path() + "/data");
        for (int i = 0; i < 230; ++i)
            put(dir.path() + "/data/item-" + QString::number(i));
        SearchIndex index(dir.path() + "/db/index.sqlite");
        index.setRoots({dir.path() + "/data"});
        index.start();
        QTRY_VERIFY_WITH_TIMEOUT(!index.scanning(), 10000);
        index.query("item");
        QTRY_VERIFY(!index.model()->loading());
        QCOMPARE(index.model()->count(), 200);
        QVERIFY(index.hasMore());
        index.nextPage();
        QTRY_VERIFY(!index.model()->loading());
        QCOMPARE(index.model()->count(), 230);
        QVERIFY(!index.hasMore());
        index.query("item");
        index.query("absent");
        QTRY_VERIFY(!index.model()->loading());
        QCOMPARE(index.model()->count(), 0);
    }
    void boundedPreviews() {
        QTemporaryDir dir(QDir::currentPath() + "/fixture-XXXXXX");
        put(dir.path() + "/sample.json", "{\"hello\":\"world\"}");
        auto json = makePreview(uri(dir.path() + "/sample.json"), 1);
        QCOMPARE(json["mode"].toString(), QString("text"));
        QVERIFY(json["text"].toString().contains("hello"));
        put(dir.path() + "/sample.csv", "name,quote\n\"a,b\",\"<script>\"\n");
        auto csv = makePreview(uri(dir.path() + "/sample.csv"), 1);
        QVERIFY(csv["text"].toString().contains("a,b"));
        QVERIFY(!csv["text"].toString().contains("<script>"));
        put(dir.path() + "/sample.md", "# Hello\n![remote](https://invalid.example/image.png)\n<img "
                                       "src=\"https://invalid.example/x\">\n");
        auto md = makePreview(uri(dir.path() + "/sample.md"), 1);
        QVERIFY(!md["text"].toString().contains("<img"));
        put(dir.path() + "/large.txt", QByteArray(400000, 'x'));
        auto large = makePreview(uri(dir.path() + "/large.txt"), 1);
        QVERIFY(!large["notice"].toString().isEmpty());
        QVERIFY(large["text"].toString().size() <= 256 * 1024);
        QImage image(900, 600, QImage::Format_RGB32);
        image.fill(Qt::darkCyan);
        QVERIFY(image.save(dir.path() + "/image.png"));
        auto png = makePreview(uri(dir.path() + "/image.png"), 1, true);
        QVERIFY2(!png.contains("error"), qPrintable(png["error"].toString()));
        QVERIFY(QFileInfo::exists(QUrl(png["image"].toString()).toLocalFile()));
        put(dir.path() + "/broken.png", "invalid image");
        auto bad = makePreview(uri(dir.path() + "/broken.png"), 1);
        QVERIFY(bad.contains("error"));
    }
};
int main(int argc, char **argv) {
    QTemporaryDir env(QDir::currentPath() + "/test-env-XXXXXX");
    if (!qEnvironmentVariableIsSet("NOOK_TEST_ENV_PRESET")) {
        qputenv("XDG_CONFIG_HOME", QFile::encodeName(env.path() + "/config"));
        qputenv("XDG_DATA_HOME", QFile::encodeName(env.path() + "/data"));
        qputenv("XDG_CACHE_HOME", QFile::encodeName(env.path() + "/cache"));
    }
    QGuiApplication app(argc, argv);
    app.setApplicationName("nook-test");
    CoreTest test;
    int rc = QTest::qExec(&test, argc, argv);
    QThreadPool::globalInstance()->waitForDone();
    return rc;
}
#include "core_test.moc"
