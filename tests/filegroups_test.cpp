#include "directorymodel.h"
#include <QAbstractItemModelTester>
#include <QDateTime>
#include <QGuiApplication>
#include <QTemporaryDir>
#include <QtTest>

class FileGroupsTest : public QObject {
    Q_OBJECT
    nook::Entry file(const QString &path, const QString &name, qint64 size = 1, int age = 0) {
        nook::Entry e;
        e.name = name; e.uri = nook::childUri(nook::normalizeUri(path), name);
        e.size = size; e.modified = QDateTime(QDate::currentDate().addDays(-age), QTime(12, 0)).toSecsSinceEpoch();
        e.icon = nook::iconFor(name, false);
        return e;
    }
    QStringList keys(FileGroups *groups) {
        QStringList result;
        for (int i = 0; i < groups->rowCount(); ++i)
            if (groups->data(groups->index(i), FileGroups::HeaderRole).toBool())
                result.append(groups->data(groups->index(i), FileGroups::KeyRole).toString());
        return result;
    }
  private Q_SLOTS:
    void bucketsAndBoundaries() {
        QTemporaryDir dir;
        DirectoryModel model;
        model.navigate(dir.path()); QTRY_VERIFY(!model.loading());
        auto *groups = model.groups();
        QAbstractItemModelTester tester(groups, QAbstractItemModelTester::FailureReportingMode::QtTest);
        QVERIFY(groups->available()); QCOMPARE(groups->mode(), QString("none"));
        QList<nook::Entry> files;
        for (int age : {-1, 0, 1, 2, 6, 7, 29, 30}) files.append(file(dir.path(), QString::number(age) + ".txt", 1, age));
        auto folder = file(dir.path(), "folder"); folder.directory = true; files.append(folder);
        auto unknown = file(dir.path(), "unknown"); unknown.modified = 0; files.append(unknown);
        model.replaceEntries(files); groups->setMode("date");
        QCOMPARE(keys(groups), QStringList({"00-folders", "01-future", "02-today", "03-yesterday", "04-week", "05-month", "06-earlier", "99-unknown"}));
        QCOMPARE(model.count(), 10); // headers are never operation targets
        files.clear();
        for (qint64 size : {0LL, 1024LL*1024-1, 1024LL*1024, 100LL*1024*1024-1, 100LL*1024*1024, 1024LL*1024*1024-1, 1024LL*1024*1024})
            files.append(file(dir.path(), QString::number(size), size));
        model.replaceEntries(files); groups->setMode("size");
        QCOMPARE(keys(groups), QStringList({"01-small", "02-medium", "03-large", "04-huge"}));
        QCOMPARE(groups->groupInfo(0)["count"].toInt(), 2);
        groups->setMode("invalid"); QCOMPARE(groups->mode(), QString("size"));
    }
    void typeCategoriesUseMimeNotOnlyIcons() {
        QTemporaryDir dir;
        DirectoryModel model; model.navigate(dir.path()); QTRY_VERIFY(!model.loading());
        QList<nook::Entry> files;
        for (const auto &name : {"photo.png", "clip.mp4", "music.wav", "notes.txt", "book.epub",
                                 "sheet.xlsx", "slides.pptx", "archive.zip", "archive.tar.gz", "data.7z", "raw.unknown-ext"})
            files.append(file(dir.path(), name));
        model.replaceEntries(files);
        auto *groups = model.groups(); groups->setMode("type");
        QCOMPARE(keys(groups), QStringList({"01-images", "02-video", "03-audio", "04-documents", "05-archives", "06-other"}));
        QCOMPARE(groups->groupInfo(groups->layoutForGroup("04-documents"))["count"].toInt(), 4);
        QCOMPARE(groups->groupInfo(groups->layoutForGroup("05-archives"))["count"].toInt(), 3);
    }
    void selectionFollowsVisualOrderAndCollapse() {
        QTemporaryDir dir;
        DirectoryModel model; model.navigate(dir.path()); QTRY_VERIFY(!model.loading());
        model.replaceEntries({file(dir.path(), "a.txt"), file(dir.path(), "b.png"), file(dir.path(), "c.txt"), file(dir.path(), "d.mp3")});
        auto *groups = model.groups(); groups->setMode("type");
        QCOMPARE(groups->moveSource(-1, 1), 1); // images before audio before documents, unlike source order
        QCOMPARE(groups->moveSource(1, 1), 3);
        groups->selectSource(1, 0); groups->selectSource(0, 2);
        QCOMPARE(model.selectedCount(), 3);
        QVERIFY(model.data(model.index(1), DirectoryModel::SelectedRole).toBool());
        QVERIFY(model.data(model.index(3), DirectoryModel::SelectedRole).toBool());
        QVERIFY(!model.data(model.index(2), DirectoryModel::SelectedRole).toBool());
        groups->toggle("01-images");
        QCOMPARE(groups->layoutRow(1), -1); QCOMPARE(model.selectedCount(), 3);
        QCOMPARE(groups->groupInfo(0)["picked"].toInt(), 1);
        QCOMPARE(groups->moveSource(-1, 1), 3);
        groups->selectGroup("04-documents"); QCOMPARE(model.selectedCount(), 2);
        groups->collapseAll(); QCOMPARE(groups->moveSource(-1, 1), -1);
        QCOMPARE(model.selectedCount(), 2);
        groups->expandAll(); QCOMPARE(groups->moveSource(-1, -2147483647), 1);
        QCOMPARE(groups->moveSource(-1, 2147483647), 2);
        groups->setMode("none"); QCOMPARE(groups->rowCount(), 0); QCOMPARE(model.selectedCount(), 2);
    }
    void gridGeometryAndNavigation() {
        QTemporaryDir dir;
        DirectoryModel model; model.navigate(dir.path()); QTRY_VERIFY(!model.loading());
        model.replaceEntries({file(dir.path(), "a.png"), file(dir.path(), "b.txt"), file(dir.path(), "c.txt"), file(dir.path(), "d.txt"), file(dir.path(), "e.txt")});
        auto *groups = model.groups(); groups->setColumns(3); groups->setMode("type");
        QCOMPARE(groups->rowCount(), 5); // image heading, one image, document heading, 3 documents, 1 document
        QCOMPARE(groups->layoutRow(0), 1); QCOMPARE(groups->layoutRow(4), 4);
        QCOMPARE(groups->moveVertical(0, 1), 1); // retain column across a partially filled group
        QCOMPARE(groups->moveVertical(3, -1), 0);
        QCOMPARE(groups->moveVertical(3, 1), 4);
        QCOMPARE(groups->sourcesBetweenRows(0, 0, 0, 600, 200, true), QList<int>());
        QCOMPARE(groups->sourcesBetweenRows(1, 1, 0, 195, 200, true), QList<int>({0}));
        QCOMPARE(groups->sourcesBetweenRows(3, 3, 205, 395, 200, true), QList<int>({2}));
        QCOMPARE(groups->sourcesBetweenRows(0, 99, 0, 600, 200, true), QList<int>({0,1,2,3,4}));
        groups->toggle("04-documents");
        QCOMPARE(groups->sourcesBetweenRows(0, 99, 0, 600, 200, true), QList<int>({0}));
        groups->setColumns(1); QCOMPARE(groups->rowCount(), 3);
    }
    void folderPersistenceAndPaneIndependence() {
        QTemporaryDir a, b;
        DirectoryModel first; first.navigate(a.path()); QTRY_VERIFY(!first.loading());
        first.replaceEntries({file(a.path(), "a.txt")}); first.groups()->setMode("type");
        first.groups()->toggle("04-documents");
        DirectoryModel second; second.navigate(a.path()); QTRY_VERIFY(!second.loading());
        QCOMPARE(second.groups()->mode(), QString("type"));
        second.replaceEntries({file(a.path(), "a.txt")});
        QTRY_VERIFY(second.groups()->groupInfo(0)["collapsed"].toBool());
        second.groups()->setMode("size");
        QCOMPARE(first.groups()->mode(), QString("type"));
        first.navigate(b.path()); QTRY_VERIFY(!first.loading());
        QCOMPARE(first.groups()->mode(), QString("none"));
        first.back(); QTRY_VERIFY(!first.loading()); QCOMPARE(first.groups()->mode(), QString("size"));
        first.navigate("tag:red"); QTRY_VERIFY(!first.loading());
        QVERIFY(!first.groups()->available()); QVERIFY(!first.groups()->active());
        first.groups()->setMode("date"); QCOMPARE(first.groups()->mode(), QString("none"));
    }
    void refreshRemapsUrisAndRetainsSelection() {
        QTemporaryDir dir;
        DirectoryModel model; model.navigate(dir.path()); QTRY_VERIFY(!model.loading());
        auto a = file(dir.path(), "a.png"), b = file(dir.path(), "b.txt");
        model.replaceEntries({a,b}); auto *groups = model.groups(); groups->setMode("type");
        groups->selectSource(0, 0);
        model.replaceEntries({b,a});
        QVERIFY(!groups->ready());
        QCOMPARE(groups->sourceForUri(a.uri), -1);
        groups->selectGroup("04-documents");
        groups->selectSource(0, 0); // reject stale numeric indices before the queued rebuild
        QCOMPARE(model.selectedUris(), QStringList{a.uri});
        QTRY_COMPARE(groups->sourceForUri(a.uri), 1);
        QCOMPARE(model.selectedUris(), QStringList{a.uri});
        QCOMPARE(groups->moveSource(-1, 1), 1);
        model.replaceEntries({b}); QTRY_COMPARE(groups->sourceForUri(a.uri), -1);
        QCOMPARE(model.selectedCount(), 0);
    }
};
int main(int argc, char **argv) {
    QTemporaryDir env;
    qputenv("XDG_CONFIG_HOME", QFile::encodeName(env.path() + "/config"));
    qputenv("XDG_DATA_HOME", QFile::encodeName(env.path() + "/data"));
    qputenv("XDG_CACHE_HOME", QFile::encodeName(env.path() + "/cache"));
    qputenv("GIO_USE_VFS", "local");
    QGuiApplication app(argc, argv);
    FileGroupsTest test; return QTest::qExec(&test, argc, argv);
}
#include "filegroups_test.moc"
