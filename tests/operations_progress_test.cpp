#include "operations.h"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMetaProperty>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QUrl>
#include <QtTest>

class OperationsProgressTest : public QObject {
    Q_OBJECT
  private:
    static QString uri(const QString &path) {
        return QUrl::fromLocalFile(path).toString(QUrl::FullyEncoded);
    }
    static void put(const QString &path, const QByteArray &contents = "sample") {
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        QCOMPARE(file.write(contents), qint64(contents.size()));
    }
    static void watch(Operations &operations) {
        connect(&operations, &Operations::changed, &operations, [&operations] {
            QVERIFY(operations.completedItems() >= 0);
            QVERIFY(operations.completedItems() <= operations.totalItems());
            const double progress = operations.progress();
            QVERIFY(progress == -1 || (progress >= 0 && progress <= 1));
            if (operations.busy())
                QVERIFY(progress == -1 || progress <= 0.99);
        });
    }
  private Q_SLOTS:
    void propertiesAndReset() {
        Operations operations;
        QCOMPARE(operations.kind(), QString());
        QCOMPARE(operations.completedItems(), 0);
        QCOMPARE(operations.totalItems(), 0);
        QCOMPARE(operations.progress(), -1.0);
        for (const auto *name : {"kind", "completedItems", "totalItems", "progress"}) {
            const auto property =
                operations.metaObject()->property(operations.metaObject()->indexOfProperty(name));
            QVERIFY(property.isValid());
            QCOMPARE(property.notifySignal().name(), QByteArray("changed"));
        }
        watch(operations);
        QTemporaryDir fixture;
        QVERIFY(fixture.isValid());
        QVERIFY(QDir().mkdir(fixture.filePath("destination")));
        put(fixture.filePath("one"));
        put(fixture.filePath("two"));
        QSignalSpy finished(&operations, &Operations::finished);
        operations.run("copy", {uri(fixture.filePath("one")), uri(fixture.filePath("two"))},
                       uri(fixture.filePath("destination")));
        QCOMPARE(operations.kind(), QString("copy"));
        QCOMPARE(operations.completedItems(), 0);
        QCOMPARE(operations.totalItems(), 2);
        QCOMPARE(operations.progress(), -1.0);
        QTRY_VERIFY_WITH_TIMEOUT(!operations.busy(), 5000);
        QCOMPARE(operations.completedItems(), 2);
        QCOMPARE(operations.progress(), 1.0);
        QVERIFY(operations.errors().isEmpty());

        operations.run("mkdir", {}, uri(fixture.path()), "created");
        QCOMPARE(operations.kind(), QString("mkdir"));
        QCOMPARE(operations.completedItems(), 0);
        QCOMPARE(operations.totalItems(), 1);
        QCOMPARE(operations.progress(), -1.0);
        QTRY_VERIFY_WITH_TIMEOUT(!operations.busy(), 5000);
        QCOMPARE(operations.completedItems(), 1);
        QCOMPARE(operations.progress(), 1.0);

        // A failed creation must not retain the preceding successful count.
        operations.run("touch", {}, uri(fixture.path()), "one");
        QCOMPARE(operations.kind(), QString("touch"));
        QCOMPARE(operations.completedItems(), 0);
        QCOMPARE(operations.totalItems(), 1);
        QCOMPARE(operations.progress(), -1.0);
        QTRY_VERIFY_WITH_TIMEOUT(!operations.busy(), 5000);
        QCOMPARE(operations.completedItems(), 0);
        QCOMPARE(operations.progress(), 0.0);
        QCOMPARE(operations.errors().size(), 1);
        QCOMPARE(finished.count(), 3);
    }
    void taskKind_data() {
        QTest::addColumn<QString>("kind");
        for (const auto *kind : {"copy", "move", "rename", "delete", "mkdir", "touch"})
            QTest::newRow(kind) << QString::fromLatin1(kind);
    }
    void taskKind() {
        QFETCH(QString, kind);
        QTemporaryDir fixture;
        QVERIFY(fixture.isValid());
        QVERIFY(QDir().mkdir(fixture.filePath("destination")));
        put(fixture.filePath("source"));
        Operations operations;
        watch(operations);
        const bool creation = kind == "mkdir" || kind == "touch";
        operations.run(kind, creation ? QStringList{} : QStringList{uri(fixture.filePath("source"))},
                       uri(fixture.filePath("destination")), "new-name");
        QCOMPARE(operations.kind(), kind);
        QCOMPARE(operations.totalItems(), 1);
        QTRY_VERIFY_WITH_TIMEOUT(!operations.busy(), 5000);
        QCOMPARE(operations.kind(), kind);
        QCOMPARE(operations.completedItems(), 1);
        QCOMPARE(operations.progress(), 1.0);
        QVERIFY(operations.errors().isEmpty());
    }
    void byteProgressBeforeItemCompletion() {
        QTemporaryDir fixture;
        QVERIFY(fixture.isValid());
        QVERIFY(QDir().mkdir(fixture.filePath("destination")));
        put(fixture.filePath("source"), QByteArray(4 * 1024 * 1024, 'x'));
        Operations operations;
        watch(operations);
        bool sawByteProgress = false;
        connect(&operations, &Operations::changed, &operations, [&] {
            if (operations.busy() && operations.completedItems() == 0 && operations.progress() >= 0)
                sawByteProgress = true;
        });
        operations.run("copy", {uri(fixture.filePath("source"))}, uri(fixture.filePath("destination")));
        QTRY_VERIFY_WITH_TIMEOUT(!operations.busy(), 5000);
        QVERIFY(sawByteProgress);
        QCOMPARE(operations.completedItems(), 1);
        QCOMPARE(operations.progress(), 1.0);
        QCOMPARE(QFileInfo(fixture.filePath("destination/source")).size(), qint64(4 * 1024 * 1024));
    }
    void selectedItemsNotRecursiveEntries() {
        QTemporaryDir fixture;
        QVERIFY(fixture.isValid());
        QVERIFY(QDir().mkpath(fixture.filePath("tree/nested")));
        QVERIFY(QDir().mkdir(fixture.filePath("destination")));
        put(fixture.filePath("tree/one"));
        put(fixture.filePath("tree/nested/two"));
        put(fixture.filePath("tree/nested/three"));
        Operations operations;
        watch(operations);
        connect(&operations, &Operations::changed, &operations, [&] {
            QCOMPARE(operations.totalItems(), 1);
            if (operations.busy() && operations.completedItems() == 0)
                QCOMPARE(operations.progress(), -1.0);
        });
        operations.run("copy", {uri(fixture.filePath("tree"))}, uri(fixture.filePath("destination")));
        QTRY_VERIFY_WITH_TIMEOUT(!operations.busy(), 5000);
        QCOMPARE(operations.completedItems(), 1);
        QCOMPARE(operations.totalItems(), 1);
        QCOMPARE(operations.progress(), 1.0);
        QVERIFY(operations.errors().isEmpty());
        QVERIFY(QFile::exists(fixture.filePath("destination/tree/nested/three")));
    }
    void skippedItemsAreNotCompleted() {
        QTemporaryDir fixture;
        QVERIFY(fixture.isValid());
        QVERIFY(QDir().mkdir(fixture.filePath("destination")));
        put(fixture.filePath("conflict"), "new");
        put(fixture.filePath("good"));
        put(fixture.filePath("destination/conflict"), "old");
        Operations operations;
        watch(operations);
        operations.run("copy", {uri(fixture.filePath("conflict")), uri(fixture.filePath("good"))},
                       uri(fixture.filePath("destination")));
        QTRY_VERIFY_WITH_TIMEOUT(operations.waitingConflict(), 5000);
        QCOMPARE(operations.completedItems(), 0);
        QCOMPARE(operations.totalItems(), 2);
        QCOMPARE(operations.progress(), -1.0);
        operations.resolveConflict(0);
        QTRY_VERIFY_WITH_TIMEOUT(!operations.busy(), 5000);
        QCOMPARE(operations.completedItems(), 1);
        QCOMPARE(operations.totalItems(), 2);
        QCOMPARE(operations.progress(), 0.5);
        QVERIFY(operations.errors().isEmpty());
        QFile existing(fixture.filePath("destination/conflict"));
        QVERIFY(existing.open(QIODevice::ReadOnly));
        QCOMPARE(existing.readAll(), QByteArray("old"));
    }
    void failedItemsAreNotCompleted() {
        QTemporaryDir fixture;
        QVERIFY(fixture.isValid());
        QVERIFY(QDir().mkdir(fixture.filePath("destination")));
        put(fixture.filePath("good"));
        Operations operations;
        watch(operations);
        operations.run("copy", {uri(fixture.filePath("missing")), uri(fixture.filePath("good"))},
                       uri(fixture.filePath("destination")));
        QTRY_VERIFY_WITH_TIMEOUT(!operations.busy(), 5000);
        QCOMPARE(operations.completedItems(), 1);
        QCOMPARE(operations.totalItems(), 2);
        QCOMPARE(operations.progress(), 0.5);
        QCOMPARE(operations.errors().size(), 1);
    }
    void largeFailingBatchCoalescesErrorNotifications() {
        QTemporaryDir fixture;
        QVERIFY(fixture.isValid());
        QVERIFY(QDir().mkdir(fixture.filePath("destination")));
        QStringList sources;
        for (int i = 0; i < 3000; ++i)
            sources.append(uri(fixture.filePath("missing-" + QString::number(i))));
        Operations operations;
        QSignalSpy notifications(&operations, &Operations::errorsChanged);
        QTimer heartbeat;
        heartbeat.setInterval(1);
        QSignalSpy ticks(&heartbeat, &QTimer::timeout);
        heartbeat.start();
        operations.run("copy", sources, uri(fixture.filePath("destination")));
        QTRY_VERIFY_WITH_TIMEOUT(!operations.busy(), 10000);
        QCOMPARE(operations.errors().size(), sources.size());
        QVERIFY(operations.errors().first().contains("missing-0"));
        QVERIFY(operations.errors().last().contains("missing-2999"));
        QVERIFY(notifications.count() < 100);
        QVERIFY(ticks.count() > 0);
    }
    void errorsPublishedWhileLaterItemsAreBlocked() {
        QTemporaryDir fixture;
        QVERIFY(fixture.isValid());
        QVERIFY(QDir().mkdir(fixture.filePath("destination")));
        put(fixture.filePath("conflict"));
        put(fixture.filePath("destination/conflict"), "old");
        Operations operations;
        operations.run("copy", {uri(fixture.filePath("missing")), uri(fixture.filePath("conflict"))},
                       uri(fixture.filePath("destination")));
        QTRY_VERIFY_WITH_TIMEOUT(operations.waitingConflict(), 5000);
        QVERIFY(operations.busy());
        QCOMPARE(operations.errors().size(), 1);
        QVERIFY(operations.errors().first().contains("missing"));
        operations.cancel();
        QTRY_VERIFY_WITH_TIMEOUT(!operations.busy(), 5000);
    }
    void cancellationRetainsCompletedCountAndResets() {
        QTemporaryDir fixture;
        QVERIFY(fixture.isValid());
        QVERIFY(QDir().mkdir(fixture.filePath("destination")));
        put(fixture.filePath("first"));
        put(fixture.filePath("conflict"));
        put(fixture.filePath("last"));
        put(fixture.filePath("destination/conflict"), "old");
        Operations operations;
        watch(operations);
        operations.run("copy",
                       {uri(fixture.filePath("first")), uri(fixture.filePath("conflict")),
                        uri(fixture.filePath("last"))},
                       uri(fixture.filePath("destination")));
        QTRY_VERIFY_WITH_TIMEOUT(operations.waitingConflict(), 5000);
        QCOMPARE(operations.completedItems(), 1);
        QCOMPARE(operations.totalItems(), 3);
        operations.cancel();
        QTRY_VERIFY_WITH_TIMEOUT(!operations.busy(), 5000);
        QVERIFY(!operations.waitingConflict());
        QCOMPARE(operations.kind(), QString("copy"));
        QCOMPARE(operations.completedItems(), 1);
        QCOMPARE(operations.totalItems(), 3);
        QCOMPARE(operations.progress(), 1.0 / 3);
        QVERIFY(operations.status().startsWith(QStringLiteral("已取消")));
        QVERIFY(QFile::exists(fixture.filePath("destination/first")));
        QVERIFY(!QFile::exists(fixture.filePath("destination/last")));

        operations.run("touch", {}, uri(fixture.path()), "after-cancel");
        QCOMPARE(operations.kind(), QString("touch"));
        QCOMPARE(operations.completedItems(), 0);
        QCOMPARE(operations.totalItems(), 1);
        QCOMPARE(operations.progress(), -1.0);
        QTRY_VERIFY_WITH_TIMEOUT(!operations.busy(), 5000);
        QCOMPARE(operations.completedItems(), 1);
        QCOMPARE(operations.progress(), 1.0);
        QVERIFY(operations.errors().isEmpty());
    }
    void emptySelection() {
        Operations operations;
        watch(operations);
        operations.run("copy", {});
        QCOMPARE(operations.totalItems(), 0);
        QCOMPARE(operations.progress(), -1.0);
        QTRY_VERIFY_WITH_TIMEOUT(!operations.busy(), 5000);
        QCOMPARE(operations.completedItems(), 0);
        QCOMPARE(operations.progress(), 0.0);
    }
};

QTEST_GUILESS_MAIN(OperationsProgressTest)
#include "operations_progress_test.moc"
