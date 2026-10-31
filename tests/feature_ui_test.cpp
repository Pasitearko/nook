#include "application.h"
#include <QGuiApplication>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQmlExpression>
#include <QQuickItem>
#include <QQuickWindow>
#include <QTemporaryDir>
#include <QtTest>
#include <memory>

namespace {
QQuickItem *findItem(QQuickItem *item, const QString &name) {
    if (item->objectName() == name) return item;
    for (auto *child : item->childItems())
        if (auto *found = findItem(child, name)) return found;
    return nullptr;
}
}
class FeatureUiTest : public QObject {
    Q_OBJECT
  private Q_SLOTS:
    void paneLayoutsAndSelection() {
        QTemporaryDir fixture;
        QVERIFY(fixture.isValid());
        QFile file(fixture.path() + "/sample.txt");
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write("preview text");
        file.close();
        AppSettings settings;
        settings.store().clear();
        settings.set("startFolder", fixture.path());
        settings.set("animations", false);
        settings.set("detailsPane", false);
        SearchIndex search;
        Operations operations;
        Application app(&search, &operations, &settings, false);
        app.setRemembersSession(false);
        QQmlEngine engine;
        engine.addImageProvider("thumb", new ThumbnailProvider);
        QQmlComponent component(&engine, QUrl::fromLocalFile(QString(NOOK_SOURCE_DIR) + "/qml/Main.qml"));
        QVERIFY2(component.isReady(), qPrintable(component.errorString()));
        std::unique_ptr<QObject> root(component.createWithInitialProperties({{"controller", QVariant::fromValue(&app)}}));
        QVERIFY2(root, qPrintable(component.errorString()));
        auto *window = qobject_cast<QQuickWindow *>(root.get());
        QVERIFY(window);
        QVERIFY(QTest::qWaitForWindowExposed(window));
        window->requestActivate();
        QTRY_VERIFY(window->isActive());
        auto *left = findItem(window->contentItem(), "leftPane");
        auto *right = findItem(window->contentItem(), "rightPane");
        auto *third = findItem(window->contentItem(), "thirdPane");
        auto *fourth = findItem(window->contentItem(), "fourthPane");
        QVERIFY(left && right && third && fourth);
        QTRY_VERIFY(left->height() > 300);
        QVERIFY(!right->isVisible());
        app.setSplitLayout("vertical");
        QTRY_VERIFY(right->isVisible());
        QTRY_VERIFY(right->height() > 120);
        QVERIFY(right->mapToScene({0,0}).y() > left->mapToScene({0,0}).y());
        auto *handle = findItem(window->contentItem(), "paneSplitHandle");
        QVERIFY(handle && handle->height() >= 8);
        const auto before = right->mapToScene({0,0}).y();
        const auto grip = handle->mapToScene(QPointF(handle->width() / 2, handle->height() / 2)).toPoint();
        QTest::mouseMove(window, grip);
        QTest::mousePress(window, Qt::LeftButton, Qt::NoModifier, grip);
        QTest::mouseMove(window, grip + QPoint(0, 35), 30);
        QTest::mouseRelease(window, Qt::LeftButton, Qt::NoModifier, grip + QPoint(0, 35));
        QTRY_VERIFY(right->mapToScene({0,0}).y() > before + 20);
        QTemporaryDir other;
        QVERIFY(other.isValid());
        app.setActiveSide(1);
        app.navigate(other.path());
        app.setGrid(true);
        app.setActiveSide(0);
        QTRY_VERIFY(right->property("gridMode").toBool() && !left->property("gridMode").toBool());
        const auto listZoom = settings.values()["listZoom"].toDouble();
        const auto gridZoom = settings.values()["gridZoom"].toDouble();
        QVERIFY(QMetaObject::invokeMethod(right, "zoomStep", Q_ARG(double, 0.1)));
        QCOMPARE(settings.values()["listZoom"].toDouble(), listZoom);
        QCOMPARE(settings.values()["gridZoom"].toDouble(), gridZoom + 0.1);
        app.right()->navigate(fixture.path());
        app.setSplitLayout("quad");
        QTRY_VERIFY(third->isVisible() && fourth->isVisible());
        QTRY_VERIFY(third->height() > 120 && fourth->width() > 150);
        QVERIFY(third->mapToScene({0,0}).y() > left->mapToScene({0,0}).y());
        QVERIFY(fourth->mapToScene({0,0}).x() > third->mapToScene({0,0}).x());
        app.setActiveSide(3);
        QTRY_VERIFY(!app.fourth()->loading());
        QMetaObject::invokeMethod(root.get(), "focusActivePane");
        QTest::keyClick(window, Qt::Key_A, Qt::ControlModifier);
        QTRY_COMPARE(app.fourth()->selectedCount(), 1);
        QCOMPARE(app.left()->selectedCount(), 0);
        app.setGrid(true);
        QTRY_VERIFY(fourth->property("gridMode").toBool());
        QTRY_VERIFY(left->property("gridMode").toBool()); // same folder, same remembered view
        settings.set("smartDates", false);
        QTRY_VERIFY(!app.left()->smartDates() && !app.fourth()->smartDates());
        settings.set("visibleColumns", QVariantList{false, false, true, false});
        QCOMPARE(settings.values()["visibleColumns"].toList(), (QVariantList{true, false, true, false, false}));
        settings.set("visibleColumns", QVariantList{true, true, true, true, true});
        app.setGrid(false);
        auto *created = left->findChild<QObject *>("createdColumnMenuItem");
        QVERIFY(created);
        auto *indicator = created->property("indicator").value<QQuickItem *>();
        QVERIFY(indicator && !indicator->isVisible()); // no native tick over the Chinese label
        auto *columnMenu = left->findChild<QObject *>("columnMenu");
        QVERIFY(columnMenu);
        QVERIFY(QMetaObject::invokeMethod(columnMenu, "open"));
        QTRY_VERIFY(columnMenu->property("visible").toBool());
        QVERIFY(window->grabWindow().save(QCoreApplication::applicationDirPath() + "/feature-column-menu.png"));
        QVERIFY(QMetaObject::invokeMethod(columnMenu, "close"));
        QTRY_VERIFY(findItem(left, "createdTimeCell") && findItem(left, "createdTimeCell")->isVisible());
        QVERIFY(!findItem(left, "createdTimeCell")->property("text").toString().isEmpty());
        QVERIFY(QMetaObject::invokeMethod(left, "resizeColumn", Q_ARG(QVariant, 4), Q_ARG(QVariant, 211)));
        QVERIFY(QMetaObject::invokeMethod(left, "saveColumns"));
        QCOMPARE(settings.values()["columnWidths"].toList().size(), 5);
        QCOMPARE(settings.values()["columnWidths"].toList()[4].toInt(), 211);
        app.setSplitLayout("triple");
        QTRY_VERIFY(third->isVisible() && !fourth->isVisible());
        app.setSplitLayout("single");
        QTRY_VERIFY(!right->isVisible() && !third->isVisible());
        QTRY_VERIFY(left->height() > 300);
        auto *split = findItem(window->contentItem(), "dualPaneButton");
        QVERIFY(split);
        QVERIFY(!findItem(window->contentItem(), "splitLayoutBox"));
        const QStringList layouts{"horizontal", "vertical", "triple", "quad", "single"};
        const QStringList icons{"split", "split-vertical", "split-triple", "split-quad", "split-single"};
        QCOMPARE(split->property("iconName").toString(), QString("split-single"));
        for (int i = 0; i < layouts.size(); ++i) {
            QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier,
                split->mapToScene({split->width()/2, split->height()/2}).toPoint());
            QTRY_COMPARE(app.splitLayout(), layouts[i]);
            QCOMPARE(split->property("iconName").toString(), icons[i]);
            QVERIFY(!split->property("hint").toString().isEmpty());
        }
        settings.set("columnWidths", QVariantList{220, 140, 90, 80, 140});
        const auto center = [](QQuickItem *item) {
            return item->mapToScene({item->width()/2, item->height()/2}).toPoint();
        };
        const auto header = [&](int id) { return findItem(left, "columnHeader" + QString::number(id)); };
        const auto dragColumn = [&](int id, QPoint target) {
            auto *area = findItem(left, "columnDragArea" + QString::number(id));
            QVERIFY(area && area->isVisible());
            const auto start = center(area);
            QTest::mouseMove(window, start);
            QTest::mousePress(window, Qt::LeftButton, Qt::NoModifier, start);
            QTest::mouseMove(window, target, 30);
            QTest::mouseRelease(window, Qt::LeftButton, Qt::NoModifier, target);
        };
        const auto sortKey = app.left()->sortKey();
        const auto descending = app.left()->sortDescending();
        dragColumn(0, center(header(2)) + QPoint(15, 0));
        QTRY_COMPARE(settings.values()["columnOrder"].toList(), (QVariantList{1, 2, 0, 3, 4}));
        QCOMPARE(app.left()->sortKey(), sortKey); // a drag is not a click-to-sort
        QCOMPARE(app.left()->sortDescending(), descending);
        QTRY_VERIFY(header(0)->x() > header(2)->x());
        auto *nameCell = findItem(left, "nameColumnCell");
        QVERIFY(nameCell);
        QTRY_COMPARE(nameCell->mapToScene({0,0}).x(), header(0)->mapToScene({0,0}).x());
        QQmlExpression iconX(qmlContext(left), left, "iconRect(Qt.point(0,0), false, 40).x");
        QCOMPARE(iconX.evaluate().toDouble(), header(0)->x() + 16.0);
        dragColumn(4, header(1)->mapToScene({10, header(1)->height()/2}).toPoint());
        QTRY_COMPARE(settings.values()["columnOrder"].toList(), (QVariantList{4, 1, 2, 0, 3}));
        // Hidden columns retain logical IDs and reappear at their remembered position.
        settings.set("visibleColumns", QVariantList{true, true, false, true, true});
        dragColumn(3, header(4)->mapToScene({10, header(4)->height()/2}).toPoint());
        QTRY_COMPARE(settings.values()["columnOrder"].toList(), (QVariantList{3, 4, 1, 2, 0}));
        settings.set("visibleColumns", QVariantList{true, true, true, true, true});
        QTRY_VERIFY(header(2)->isVisible() && header(2)->x() < header(0)->x());
        QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, center(header(2)));
        QTRY_COMPARE(app.left()->sortKey(), QString("type"));
        auto *resize = findItem(left, "columnResizeHandle4");
        QVERIFY(resize);
        const auto oldWidth = header(4)->width();
        const auto start = center(resize);
        QTest::mousePress(window, Qt::LeftButton, Qt::NoModifier, start);
        QTest::mouseMove(window, start + QPoint(30,0), 30);
        QTest::mouseRelease(window, Qt::LeftButton, Qt::NoModifier, start + QPoint(30,0));
        QTRY_VERIFY(header(4)->width() > oldWidth + 20);
        QCOMPARE(settings.values()["columnOrder"].toList(), (QVariantList{3, 4, 1, 2, 0}));
        QVERIFY(window->grabWindow().save(QCoreApplication::applicationDirPath() + "/feature-column-reorder.png"));
        AppSettings reopened;
        QCOMPARE(reopened.values()["columnOrder"].toList(), (QVariantList{3, 4, 1, 2, 0}));
        // Changing panes or opening a chooser uses the same persisted order.
        app.setSplitLayout("horizontal");
        QTRY_VERIFY(header(0)->x() > header(3)->x());
        auto *rightName = findItem(right, "columnHeader0");
        auto *rightSize = findItem(right, "columnHeader3");
        QVERIFY(rightName && rightSize);
        QTRY_VERIFY(rightName->x() > rightSize->x());
        // A grabbed header stays alive during a reorder; hovering the edge scrolls wide lists.
        app.setSplitLayout("single");
        settings.set("columnWidths", QVariantList{1000, 250, 150, 120, 200});
        auto *sizeArea = findItem(left, "columnDragArea3");
        QVERIFY(sizeArea);
        const auto sizeStart = center(sizeArea);
        const QPoint edge(int(left->mapToScene({left->width()-14, 0}).x()), sizeStart.y());
        QTest::mousePress(window, Qt::LeftButton, Qt::NoModifier, sizeStart);
        QTest::mouseMove(window, edge, 30);
        QQmlExpression lastColumn(qmlContext(left), left, "columnOrder[4]");
        QTRY_COMPARE(lastColumn.evaluate().toInt(), 3);
        QTest::mouseRelease(window, Qt::LeftButton, Qt::NoModifier, edge);
        QTRY_COMPARE(settings.values()["columnOrder"].toList(), (QVariantList{4, 1, 2, 0, 3}));
        // Cancel restores the saved order instead of committing a half-finished operation.
        const auto committed = settings.values()["columnOrder"].toList();
        QVERIFY(QMetaObject::invokeMethod(left, "moveColumnDrag", Q_ARG(QVariant, 3), Q_ARG(QVariant, 0), Q_ARG(QVariant, QVariant())));
        QVERIFY(QMetaObject::invokeMethod(left, "finishColumnDrag", Q_ARG(QVariant, false)));
        QCOMPARE(settings.values()["columnOrder"].toList(), committed);
        QQmlExpression orderRestored(qmlContext(left), left, "JSON.stringify(columnOrder) === JSON.stringify(savedColumnOrder)");
        QVERIFY(orderRestored.evaluate().toBool());

        // With motion enabled, the grabbed header follows the cursor while neighbours ease.
        settings.set("columnWidths", QVariantList{220, 140, 90, 80, 140});
        settings.set("columnOrder", QVariantList{0, 1, 2, 3, 4});
        QTest::qWait(250); // let the preceding cancellation settle before starting a fresh gesture
        settings.set("animations", true);
        QTRY_COMPARE(header(1)->x(), 220.0);
        auto *nameArea = findItem(left, "columnDragArea0");
        const auto nameStart = nameArea->mapToScene({30, nameArea->height()/2}).toPoint();
        const auto nameTarget = header(2)->mapToScene({70, header(2)->height()/2}).toPoint();
        QTest::mousePress(window, Qt::LeftButton, Qt::NoModifier, nameStart);
        QTest::mouseMove(window, nameTarget, 1);
        QTest::qWait(35);
        QCOMPARE(left->property("draggedColumn").toInt(), 0);
        QVERIFY(header(1)->x() > 0 && header(1)->x() < 220); // not a hard jump to its new slot
        QVERIFY2(qAbs(header(0)->mapToScene({30, 0}).x() - nameTarget.x()) < 1.0,
            qPrintable(QString("header=%1 target=%2 pointer=%3 offset=%4 content=%5")
                .arg(header(0)->mapToScene({30, 0}).x()).arg(nameTarget.x())
                .arg(left->property("columnDragPointer").toDouble()).arg(left->property("columnDragOffset").toDouble())
                .arg(left->property("columnDragContentX").toDouble())));
        QCOMPARE(nameCell->mapToScene({0,0}).x(), header(0)->mapToScene({0,0}).x());
        QVERIFY(window->grabWindow().save(QCoreApplication::applicationDirPath() + "/feature-column-drag-motion.png"));
        QTest::mouseRelease(window, Qt::LeftButton, Qt::NoModifier, nameTarget);
        QTRY_COMPARE(settings.values()["columnOrder"].toList(), (QVariantList{1, 2, 0, 3, 4}));
        QVERIFY(header(0)->x() > 230.0); // release also animates rather than snapping
        QCOMPARE(header(0)->z(), 2.0);
        QVERIFY(findItem(nameCell, "columnDragBackdrop0")->isVisible()); // protected throughout settling
        QTRY_COMPARE(header(0)->x(), 230.0); // released column eases into its reserved slot
        QTRY_COMPARE(header(1)->x(), 0.0);
        QTRY_VERIFY(!left->property("columnMotionActive").toBool());
        // Interrupt another column's in-flight easing with a new grab: it must not lag behind.
        dragColumn(0, header(1)->mapToScene({10, header(1)->height()/2}).toPoint());
        QTest::qWait(85); // expose the middle, away from the lifted Name column and resize handle
        auto *dateArea = findItem(left, "columnDragArea1");
        const auto dateStart = dateArea->mapToScene({70, dateArea->height()/2}).toPoint();
        const auto dateTarget = header(3)->mapToScene({20, header(3)->height()/2}).toPoint();
        QTest::mousePress(window, Qt::LeftButton, Qt::NoModifier, dateStart);
        const auto heldX = header(1)->x();
        QTest::qWait(70); // a held column must not continue sliding before the drag threshold
        QVERIFY2(qAbs(header(1)->x() - heldX) < 0.001,
            qPrintable(QString("x=%1 held=%2 pressed=%3 holdId=%4 holdX=%5")
                .arg(header(1)->x()).arg(heldX).arg(left->property("pressedColumn").toInt())
                .arg(left->property("columnHeldId").toInt()).arg(left->property("columnHeldX").toDouble())));
        QTest::mouseMove(window, dateTarget, 1);
        QTest::qWait(20);
        const auto offset = left->property("columnDragOffset").toDouble();
        QVERIFY(qAbs(header(1)->mapToScene({offset, 0}).x() - dateTarget.x()) < 1.0);
        QTest::mouseRelease(window, Qt::LeftButton, Qt::NoModifier, dateTarget);
        QTRY_VERIFY(!left->property("columnMotionActive").toBool());
        settings.set("animations", false);
        settings.set("columnOrder", QVariantList{1, 2, 0, 3, 4});
        dragColumn(0, header(1)->mapToScene({10, header(1)->height()/2}).toPoint());
        QCOMPARE(header(0)->x(), 0.0); // disabling lightweight motion is still respected
    }
};
int main(int argc, char **argv) {
    QTemporaryDir env;
    qputenv("XDG_CONFIG_HOME", QFile::encodeName(env.path() + "/config"));
    qputenv("XDG_DATA_HOME", QFile::encodeName(env.path() + "/data"));
    qputenv("XDG_CACHE_HOME", QFile::encodeName(env.path() + "/cache"));
    qputenv("GIO_USE_VFS", "local");
    qputenv("QT_QPA_PLATFORMTHEME", "generic");
    QGuiApplication app(argc, argv);
    FeatureUiTest test;
    return QTest::qExec(&test, argc, argv);
}
#include "feature_ui_test.moc"
