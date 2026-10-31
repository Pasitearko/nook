#include "directorymodel.h"
#include "storage.h"
#include "tags.h"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJSValue>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickWindow>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QThreadPool>
#include <QtTest>
#include <memory>

class DirectoryModelFeaturesTest : public QObject {
    Q_OBJECT
    QTemporaryDir profile;

    static nook::Entry folder(const QString &path) {
        nook::Entry entry;
        entry.uri = nook::normalizeUri(path);
        entry.name = QFileInfo(path).fileName();
        entry.directory = true;
        return entry;
    }
    static QQuickItem *folderItem(QQuickItem *parent) {
        if (parent->objectName() == "fileEntry" && parent->property("folder").toBool())
            return parent;
        for (auto *child : parent->childItems())
            if (auto *found = folderItem(child))
                return found;
        return nullptr;
    }
    static void write(const QString &path, int bytes) {
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        QCOMPARE(file.write(QByteArray(bytes, 'x')), bytes);
    }

  private Q_SLOTS:
    void initTestCase() {
        QVERIFY(profile.isValid());
        nook::initializeProfile(profile.path());
    }
    void cleanup() {
        QThreadPool::globalInstance()->waitForDone();
        QCoreApplication::processEvents();
    }
    void intelligentDates() {
        DirectoryModel model;
        nook::Entry entry;
        entry.name = "dated";
        entry.uri = "file:///dated";
        entry.modified = QDateTime(QDate::currentDate(), QTime(12, 30)).toSecsSinceEpoch();
        model.replaceEntries({entry});
        QCOMPARE(model.data(model.index(0), DirectoryModel::ModifiedRole).toString(),
                 QStringLiteral("今天 12:30"));
        QSignalSpy changed(&model, &QAbstractItemModel::dataChanged);
        model.setSmartDates(false);
        QVERIFY(!changed.isEmpty());
        QCOMPARE(model.data(model.index(0), DirectoryModel::ModifiedRole).toString(),
                 QDateTime::fromSecsSinceEpoch(entry.modified).toString("yyyy-MM-dd HH:mm"));
        QCOMPARE(model.data(model.index(0), DirectoryModel::ModifiedEpochRole).toLongLong(), entry.modified);
        entry.modified = QDateTime(QDate::currentDate().addDays(-1), QTime(12, 30)).toSecsSinceEpoch();
        model.replaceEntries({entry});
        model.setSmartDates(true);
        QCOMPARE(model.data(model.index(0), DirectoryModel::ModifiedRole).toString(),
                 QStringLiteral("昨天 12:30"));
    }
    void createdTimes() {
        QTemporaryDir tree;
        write(tree.filePath("new.txt"), 3);
        nook::Entry file;
        QVERIFY(nook::entryAt(nook::normalizeUri(tree.filePath("new.txt")), &file));
        const auto birth = QFileInfo(tree.filePath("new.txt")).birthTime();
        if (birth.isValid()) {
            QVERIFY(file.createdKnown);
            QCOMPARE(file.created, birth.toSecsSinceEpoch());
        }
        QString error;
        const auto listed = nook::enumerate(nook::normalizeUri(tree.path()), false,
            std::make_shared<std::atomic_bool>(false), &error);
        QCOMPARE(listed.size(), 1);
        QCOMPARE(listed[0].createdKnown, file.createdKnown);
        QCOMPARE(listed[0].created, file.created);
        DirectoryModel model;
        nook::Entry known = file, unknown = file;
        known.name = "known"; known.uri = "file:///known"; known.createdKnown = true;
        known.created = QDateTime(QDate::currentDate(), QTime(11, 20)).toSecsSinceEpoch();
        unknown.name = "unknown"; unknown.uri = "file:///unknown"; unknown.createdKnown = false;
        model.replaceEntries({known, unknown});
        QCOMPARE(model.columnText(0, 4), QString("今天 11:20"));
        QCOMPARE(model.columnText(1, 4), QString("—"));
        QCOMPARE(model.columnText(0, 5), QString("/known"));
        model.setSmartDates(false);
        QCOMPARE(model.columnText(0, 4), QDateTime::fromSecsSinceEpoch(known.created).toString("yyyy-MM-dd HH:mm"));
        QVERIFY(model.entry(0)["createdKnown"].toBool());
        // An epoch-zero birth time is valid, not the unavailable sentinel.
        nook::Entry epoch = known; epoch.created = 0;
        model.replaceEntries({epoch});
        QCOMPARE(model.columnText(0, 4), QDateTime::fromSecsSinceEpoch(0).toString("yyyy-MM-dd HH:mm"));
        QList<DirectoryModel::Change> changes;
        QVERIFY(DirectoryModel::diff({known}, {epoch}, &changes));
        QCOMPARE(changes.size(), 1);
        QCOMPARE(changes[0].kind, DirectoryModel::Change::Update);
        model.replaceEntries({unknown, known});
        model.sortBy("created", true);
        QTRY_COMPARE(model.entry(0)["fileName"].toString(), QString("known"));
        model.sortBy("created", false);
        QTRY_COMPARE(model.entry(0)["fileName"].toString(), QString("known")); // unknown last either way
    }
    void recursiveSizesAndSymlinks() {
        QTemporaryDir tree;
        QVERIFY(QDir().mkpath(tree.path() + "/full/nested"));
        QVERIFY(QDir().mkpath(tree.path() + "/empty"));
        write(tree.path() + "/full/a", 17);
        write(tree.path() + "/full/nested/.hidden", 23);
        // Directory and file symlinks are not followed or counted, even when they form a cycle.
        QVERIFY(QFile::link(tree.path() + "/full", tree.path() + "/full/nested/cycle"));
        QVERIFY(QFile::link(tree.path() + "/full/a", tree.path() + "/full/link"));
        auto full = folder(tree.path() + "/full"), empty = folder(tree.path() + "/empty");
        auto missing = folder(tree.path() + "/missing");
        auto symlink = folder(tree.path() + "/full/nested/cycle");
        symlink.symlink = true;
        DirectoryModel model;
        model.replaceEntries({full, empty, missing, symlink});
        QCOMPARE(model.entry(0).value("bytes").toLongLong(), -1);
        QVERIFY(!model.entry(0).value("sizeKnown").toBool());
        for (const auto &entry : {full, empty, missing, symlink})
            model.requestFolderSize(entry.uri);
        QTRY_VERIFY(model.entry(0).value("sizeKnown").toBool());
        QTRY_VERIFY(model.entry(1).value("sizeKnown").toBool());
        QCOMPARE(model.entry(0).value("bytes").toLongLong(), 40);
        QCOMPARE(model.entry(1).value("bytes").toLongLong(), 0);
        QCOMPARE(model.entry(1).value("sizeText").toString(), nook::formatSize(0));
        QTRY_COMPARE(model.entry(2).value("sizeText").toString(), QStringLiteral("—"));
        QCOMPARE(model.entry(2).value("bytes").toLongLong(), -1);
        QVERIFY(!model.entry(2).value("sizeKnown").toBool());
        QVERIFY(!model.entry(3).value("sizeKnown").toBool());
    }
    void cancellationAndConsumers() {
        QTemporaryDir tree;
        QVERIFY(QDir().mkpath(tree.path() + "/folder"));
        write(tree.path() + "/folder/a", 31);
        auto entry = folder(tree.path() + "/folder");
        DirectoryModel model;
        model.replaceEntries({entry});
        model.requestFolderSize(entry.uri);
        model.requestFolderSize(entry.uri);
        model.releaseFolderSize(entry.uri); // The second consumer still needs it.
        QTRY_COMPARE(model.entry(0).value("bytes").toLongLong(), 31);
        model.replaceEntries({entry});
        model.requestFolderSize(entry.uri);
        model.releaseFolderSize(entry.uri); // Cancel before its queued result can be delivered.
        QThreadPool::globalInstance()->waitForDone();
        QCoreApplication::processEvents();
        QCOMPARE(model.entry(0).value("bytes").toLongLong(), -1);
        model.requestFolderSize(entry.uri);
        model.replaceEntries({}); // An obsolete result cannot populate another listing.
        QThreadPool::globalInstance()->waitForDone();
        QCoreApplication::processEvents();
        QCOMPARE(model.count(), 0);
        auto doomed = std::make_unique<DirectoryModel>();
        doomed->replaceEntries({entry});
        doomed->requestFolderSize(entry.uri);
        doomed.reset(); // Never waits for a traversal, and queued delivery uses a QPointer.
    }
    void sizeSortUsesTotals() {
        QTemporaryDir tree;
        QVERIFY(QDir().mkpath(tree.path() + "/a-large"));
        QVERIFY(QDir().mkpath(tree.path() + "/z-small"));
        write(tree.path() + "/a-large/data", 100);
        write(tree.path() + "/z-small/data", 5);
        auto large = folder(tree.path() + "/a-large"), small = folder(tree.path() + "/z-small");
        DirectoryModel model;
        model.replaceEntries({large, small});
        model.requestFolderSize(large.uri);
        model.requestFolderSize(small.uri);
        QTRY_VERIFY(model.entry(0).value("sizeKnown").toBool() && model.entry(1).value("sizeKnown").toBool());
        model.sortBy("size");
        QTRY_COMPARE(model.entry(0).value("fileName").toString(), QString("z-small"));
        QCOMPARE(model.entry(0).value("bytes").toLongLong(), 5);
        model.sortBy("size", true);
        QTRY_COMPARE(model.entry(0).value("fileName").toString(), QString("a-large"));
    }
    void largeSortNotifiesSelectionOrder() {
        DirectoryModel model;
        QList<nook::Entry> entries;
        for (int i = 0; i < 200; ++i) {
            nook::Entry entry;
            entry.name = QString::number(i).rightJustified(3, '0');
            entry.uri = "file:///" + entry.name;
            entries.append(entry);
        }
        model.replaceEntries(entries);
        model.selectAll();
        QCOMPARE(model.selectedUris().first(), entries.first().uri);
        QSignalSpy reset(&model, &QAbstractItemModel::modelReset);
        QSignalSpy selection(&model, &DirectoryModel::selectionChanged);
        model.sortBy("name", true);
        QTRY_COMPARE(model.selectedUris().first(), entries.last().uri);
        QVERIFY(!reset.isEmpty()); // Reverse order exceeds the incremental diff's move limit.
        QVERIFY(!selection.isEmpty());
    }
    void folderSortPersistence() {
        QTemporaryDir tree;
        QVERIFY(QDir().mkpath(tree.path() + "/one"));
        QVERIFY(QDir().mkpath(tree.path() + "/two"));
        DirectoryModel model;
        model.navigate(tree.path() + "/one");
        QTRY_VERIFY(!model.loading());
        model.sortBy("date", true);
        model.navigate(tree.path() + "/two");
        QTRY_VERIFY(!model.loading());
        QCOMPARE(model.sortKey(), QString("name"));
        QVERIFY(!model.sortDescending());
        model.sortBy("size");
        model.back();
        QCOMPARE(model.sortKey(), QString("date"));
        QVERIFY(model.sortDescending());
        model.forward();
        QCOMPARE(model.sortKey(), QString("size"));
        QVERIFY(!model.sortDescending());
        DirectoryModel restored;
        restored.navigate(tree.path() + "/one");
        QCOMPARE(restored.sortKey(), QString("date"));
        QVERIFY(restored.sortDescending());
        restored.sortBy("invalid");
        QCOMPARE(restored.sortKey(), QString("date"));
    }
    void paneHighlightColumnsAndFit() {
        DirectoryModel model;
        QTemporaryDir tree;
        QVERIFY(tree.isValid());
        auto entry = folder(tree.path());
        entry.name = QString(110, 'W') + "<img>.txt";
        entry.modified = QDateTime::currentSecsSinceEpoch();
        model.replaceEntries({entry});
        QQmlEngine engine;
        engine.rootContext()->setContextProperty("testModel", &model);
        engine.rootContext()->setContextProperty("testTags", TagStore::instance());
        QQmlComponent component(&engine);
        const auto source = QUrl::fromLocalFile(QStringLiteral(NOOK_SOURCE_DIR) + "/qml/FeatureTest.qml");
        component.setData(R"(
import QtQuick
import QtQuick.Controls
ApplicationWindow {
    id: window
    width: 1000; height: 650; visible: true
    property bool animations: false
    property bool dialogsOpen: false
    property bool selectionMode: false
    property int springDelay: 700
    property Theme theme: Theme {}
    property QtObject controller: QtObject {
        property var tags: testTags
        property var preview: ({visible: false})
        property QtObject settings: QtObject {
            property var values: ({animations: false, smartDates: true, columnWidths: [], visibleColumns: [true, true, true, true]})
            signal columnWidthsReset()
            function set(key, value) { const next = Object.assign({}, values); next[key] = value; values = next }
        }
    }
    FilePane { objectName: "testPane"; anchors.fill: parent; directoryModel: testModel; controller: window.controller }
})",
                          source);
        std::unique_ptr<QObject> root(component.create());
        QVERIFY2(root, qPrintable(component.errorString()));
        auto *pane = root->findChild<QQuickItem *>("testPane");
        QVERIFY(pane);
        QTRY_VERIFY(folderItem(pane));
        QTRY_VERIFY(folderItem(pane)->property("needsFolderSize").toBool());
        pane->setVisible(false);
        QTRY_VERIFY(!folderItem(pane)->property("needsFolderSize").toBool());
        pane->setVisible(true);
        QTRY_VERIFY(folderItem(pane)->property("needsFolderSize").toBool());
        pane->setProperty("searchMode", true);
        pane->setProperty("searchText", "<img>");
        QVariant highlighted;
        QVERIFY(QMetaObject::invokeMethod(pane, "highlightText", Q_RETURN_ARG(QVariant, highlighted),
                                          Q_ARG(QVariant, "<img>& [literal]")));
        QVERIFY(highlighted.toString().contains("&lt;img&gt;"));
        QVERIFY(highlighted.toString().contains("&amp;"));
        QVERIFY(!highlighted.toString().contains("<img>"));
        QVERIFY(highlighted.toString().contains("<b>"));
        pane->setProperty("searchText", "[literal]");
        QVERIFY(QMetaObject::invokeMethod(pane, "highlightText", Q_RETURN_ARG(QVariant, highlighted),
                                          Q_ARG(QVariant, "[literal]")));
        QVERIFY(highlighted.toString().contains("<b>"));
        QVERIFY(QMetaObject::invokeMethod(pane, "toggleColumn", Q_ARG(QVariant, 2)));
        QVariant width;
        QVERIFY(QMetaObject::invokeMethod(pane, "columnWidth", Q_RETURN_ARG(QVariant, width),
                                          Q_ARG(QVariant, 2)));
        QCOMPARE(width.toDouble(), 0.0);
        QVERIFY(QMetaObject::invokeMethod(pane, "fitColumns", Q_ARG(QVariant, 0)));
        QTRY_COMPARE(pane->property("localWidths").value<QJSValue>().toVariant().toList().size(), 5);
        const auto widths = pane->property("localWidths").value<QJSValue>().toVariant().toList();
        QVERIFY(widths[0].toDouble() > 1000);
        QVERIFY(widths[0].toDouble() <= 1600);
    }
};

QTEST_MAIN(DirectoryModelFeaturesTest)
#include "directorymodel_features_test.moc"
