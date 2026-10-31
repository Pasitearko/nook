#include "application.h"
#include <QGuiApplication>
#include <QJsonArray>
#include <QTemporaryDir>
#include <QtTest>

class ViewStateTest : public QObject {
    Q_OBJECT
  private Q_SLOTS:
    void layoutsAndFolderViews() {
        QTemporaryDir folders;
        QVERIFY(folders.isValid());
        QVERIFY(QDir().mkpath(folders.path() + "/a"));
        QVERIFY(QDir().mkpath(folders.path() + "/b"));
        AppSettings settings;
        settings.set("startFolder", folders.path() + "/a");
        SearchIndex search;
        Operations operations;
        Application app(&search, &operations, &settings, false);
        app.setRemembersSession(false);
        const auto a = nook::normalizeUri(folders.path() + "/a");
        const auto b = nook::normalizeUri(folders.path() + "/b");
        QTRY_VERIFY(!app.left()->loading());
        app.setGrid(true);
        QVERIFY(app.grid());
        app.navigate(b);
        QTRY_VERIFY(!app.left()->loading());
        QVERIFY(!app.grid());
        app.left()->back();
        QTRY_VERIFY(!app.left()->loading());
        QCOMPARE(app.left()->uri(), a);
        QVERIFY(app.grid());
        app.setSplitLayout("quad");
        QVERIFY(app.dual());
        QCOMPARE(app.third()->uri(), a);
        QCOMPARE(app.fourth()->uri(), a);
        app.setActiveSide(3);
        QCOMPARE(app.activePane(), app.fourth());
        app.navigate(b);
        QVERIFY(!app.grid());
        QVERIFY(app.gridFor(app.left()));
        app.setSplitLayout("triple");
        QCOMPARE(app.activeSide(), 0);
        app.setActiveSide(2);
        QCOMPARE(app.activePane(), app.third());
        app.setSplitLayout("vertical");
        app.setActiveSide(1);
        QCOMPARE(app.activePane(), app.right());
        app.setSplitLayout("invalid");
        QCOMPARE(app.splitLayout(), QString("vertical"));
        const auto state = app.state();
        Application restored(&search, &operations, &settings, false, nullptr, state);
        restored.setRemembersSession(false);
        QCOMPARE(restored.splitLayout(), QString("vertical"));
        QCOMPARE(restored.fourth()->uri(), b);
        app.setDual(false);
        QCOMPARE(app.activePane(), app.left());
        QCOMPARE(app.splitLayout(), QString("single"));
        app.setDual(true);
        QCOMPARE(app.splitLayout(), QString("horizontal"));
        app.closeTab(0);
        app.addTab(b);
        app.setSplitLayout("quad");
        app.closeTab(1);
        QCOMPARE(app.tabs().size(), 1);
    }
    void legacyGridSessionMigration() {
        QTemporaryDir folder;
        QVERIFY(folder.isValid());
        AppSettings settings;
        SearchIndex search;
        Operations operations;
        const QJsonObject legacy{{"tabs", QJsonArray{QJsonObject{{"left", folder.path()}, {"right", folder.path()}, {"grid", true}, {"dual", true}}}}};
        Application restored(&search, &operations, &settings, false, nullptr, legacy);
        restored.setRemembersSession(false);
        QCOMPARE(restored.splitLayout(), QString("horizontal"));
        QVERIFY(restored.gridFor(restored.left()));
        QVERIFY(restored.gridFor(restored.right()));
        restored.setGrid(false);
        Application restoredAgain(&search, &operations, &settings, false, nullptr, legacy);
        restoredAgain.setRemembersSession(false);
        QVERIFY(!restoredAgain.grid()); // explicit new preference wins
    }
    void columnOrderPersistsAndRejectsInvalidOrders() {
        const QVariantList standard{0, 1, 2, 3, 4}, reordered{4, 2, 0, 1, 3};
        AppSettings settings;
        settings.store().remove("columnOrder");
        QCOMPARE(settings.values()["columnOrder"].toList(), standard);
        settings.set("columnOrder", reordered);
        AppSettings reopened;
        QCOMPARE(reopened.values()["columnOrder"].toList(), reordered);
        for (const QVariantList &invalid : {QVariantList{0, 1, 2, 3}, QVariantList{0, 1, 2, 3, 3},
                QVariantList{-1, 1, 2, 3, 4}, QVariantList{0, 1, 2, 3, 5}, QVariantList{0, 1.1, 2, 3, 4}}) {
            settings.set("columnOrder", invalid);
            QCOMPARE(settings.values()["columnOrder"].toList(), reordered);
        }
        settings.store().setValue("columnOrder", QVariantList{0, 0, 0, 0, 0});
        QCOMPARE(settings.values()["columnOrder"].toList(), standard);
        settings.store().remove("columnOrder");
    }
    void dateSettingPersists() {
        { AppSettings settings; settings.set("smartDates", false); }
        AppSettings settings;
        QCOMPARE(settings.values().value("smartDates").metaType(), QMetaType::fromType<bool>());
        QVERIFY(!settings.values().value("smartDates").toBool());
        settings.set("smartDates", true);
        QVERIFY(settings.values().value("smartDates").toBool());
    }
};

int main(int argc, char **argv) {
    QTemporaryDir env;
    qputenv("XDG_CONFIG_HOME", QFile::encodeName(env.path() + "/config"));
    qputenv("XDG_DATA_HOME", QFile::encodeName(env.path() + "/data"));
    qputenv("XDG_CACHE_HOME", QFile::encodeName(env.path() + "/cache"));
    qputenv("GIO_USE_VFS", "local");
    QGuiApplication app(argc, argv);
    ViewStateTest test;
    return QTest::qExec(&test, argc, argv);
}
#include "viewstate_test.moc"
