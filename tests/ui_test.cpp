#include <QRegularExpression>
#include "application.h"
#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQmlComponent>
#include <QQuickItem>
#include <QQuickWindow>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QWheelEvent>
#include <QtTest>

class UiTest : public QObject {
    Q_OBJECT
  private Q_SLOTS:
    void interaction() {
        QTemporaryDir fixture(QString(NOOK_BINARY_DIR) + "/ui-fixture-XXXXXX");
        QFile file(fixture.path() + "/sample.txt");
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write("Hello, file manager\n");
        file.close();
        QDir().mkdir(fixture.path() + "/folder");
        AppSettings settings;
        settings.set("startFolder", fixture.path());
        settings.set("theme", "dark");
        // The offscreen screen is smaller than the window, which would otherwise count as roomy.
        settings.set("detailsPane", false);
        SearchIndex search;
        Operations ops;
        Application controller(&search, &ops, &settings, false);
        controller.preview()->setWorkerExecutable(QCoreApplication::applicationDirPath() + "/nook");
        QQmlEngine engine;
        engine.addImageProvider("thumb", new ThumbnailProvider);
        QSignalSpy warningSpy(&engine, &QQmlEngine::warnings);
        QQmlComponent component(&engine, QUrl::fromLocalFile(QString(NOOK_SOURCE_DIR) + "/qml/Main.qml"));
        QVERIFY2(component.isReady(), qPrintable(component.errorString()));
        std::unique_ptr<QObject> root(
            component.createWithInitialProperties({{"controller", QVariant::fromValue(&controller)}}));
        QVERIFY2(root, qPrintable(component.errorString()));
        auto *window = qobject_cast<QQuickWindow *>(root.get());
        QVERIFY(window);
        QVERIFY(QTest::qWaitForWindowExposed(window));
        QTRY_VERIFY(!controller.left()->loading());
        QCOMPARE(controller.left()->count(), 2);
        auto *left = root->findChild<QQuickItem *>("leftPane");
        QVERIFY(left);
        QTRY_VERIFY(left->width() > window->width() - 300);
        QTRY_VERIFY(left->height() > 400);
        auto click = [&](const char *name) {
            auto *item = root->findChild<QQuickItem *>(name);
            QVERIFY(item);
            QPointF point = item->mapToScene({item->width() / 2, item->height() / 2});
            QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, point.toPoint());
        };
        click("newTabButton");
        QCOMPARE(controller.tabs().size(), 2);
        QTest::qWait(30);
        for (auto *card : root->findChildren<QQuickItem *>("tabCard")) {
            const auto top = card->y(), bottom = card->parentItem()->height() - card->y() - card->height();
            QVERIFY2(top >= 4 && qAbs(top - bottom) < 0.5,
                     "Tabs must have equal top and bottom margins inside the title bar");
        }
        click("dualPaneButton");
        QVERIFY(controller.dual());
        QTRY_VERIFY(!controller.right()->loading());
        auto *right = root->findChild<QQuickItem *>("rightPane");
        QVERIFY(right);
        QVERIFY(right->isVisible());
        QTRY_VERIFY(right->width() > 200);
        QVERIFY(left->width() > 200);
        controller.setDual(false);
        // Real pointer and keyboard events verify focus routing, selection,
        // select-all, and that text editing does not invoke file shortcuts.
        QPoint row = left->mapToScene({120, 85}).toPoint();
        QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, row);
        QCOMPARE(controller.left()->selectedCount(), 1);
        QTest::keyClick(window, Qt::Key_A, Qt::ControlModifier);
        QCOMPARE(controller.left()->selectedCount(), 2);
        QTest::keyClick(window, Qt::Key_L, Qt::ControlModifier);
        auto *address = root->findChild<QQuickItem *>("addressField");
        QVERIFY(address);
        QTRY_VERIFY(address->hasActiveFocus());
        QTest::keyClick(window, Qt::Key_A, Qt::ControlModifier);
        QCOMPARE(controller.left()->selectedCount(), 2);
        auto *input = root->findChild<QObject *>("searchField");
        QVERIFY(input);
        input->setProperty("text", "missing");
        QTest::qWait(350);
        QTRY_VERIFY(!search.model()->loading());
        QCOMPARE(search.model()->count(), 0);
        input->setProperty("text", "");
        QTest::qWait(250);
        QVERIFY(window->grabWindow().save(QString(NOOK_BINARY_DIR) + "/ui-main.png"));
        root->setProperty("filtersVisible", true);
        QTest::qWait(200);
        QVERIFY(root->findChild<QQuickItem *>("searchFilterBar")->isVisible());
        QVERIFY(window->grabWindow().save(QString(NOOK_BINARY_DIR) + "/ui-filters.png"));
        {
            // The date filters are picked from a calendar, not typed.
            auto click = [&](QQuickItem *item) {
                QVERIFY(item);
                QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier,
                                  item->mapToScene({item->width() / 2, item->height() / 2}).toPoint());
            };
            auto *after = root->findChild<QQuickItem *>("afterDateField");
            QVERIFY(after);
            click(root->findChild<QQuickItem *>("afterDateFieldCalendar"));
            auto *grid = root->findChild<QQuickItem *>("datePickerGrid");
            QVERIFY(grid);
            QTRY_VERIFY(grid->isVisible() && grid->height() > 100);
            QTest::qWait(200);
            QVERIFY(window->grabWindow().save(QString(NOOK_BINARY_DIR) + "/ui-date-picker.png"));
            // The third week row always belongs to the shown month.
            const auto cell = grid->mapToScene({grid->width() / 14, grid->height() * 2.5 / 6}).toPoint();
            QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, cell);
            QTRY_VERIFY(!grid->isVisible());
            const auto picked = after->property("text").toString();
            QVERIFY2(QRegularExpression("^\\d{4}-\\d{2}-\\d{2}$").match(picked).hasMatch(), qPrintable(picked));
            QVERIFY(after->property("valid").toBool());
            click(root->findChild<QQuickItem *>("afterDateFieldCalendar"));
            QTRY_VERIFY(grid->isVisible());
            QTest::qWait(200);
            click(root->findChild<QQuickItem *>("datePickerClear"));
            QTRY_COMPARE(after->property("text").toString(), QString());
            QTRY_VERIFY(!grid->isVisible());
        }
        root->setProperty("filtersVisible", false);
        auto checkButtonBounds = [&] {
            for (auto *button : root->findChildren<QQuickItem *>()) {
                if (!button->isVisible() ||
                    !QString(button->metaObject()->className()).startsWith("ActionButton"))
                    continue;
                auto *content = button->property("contentItem").value<QQuickItem *>();
                if (!content)
                    continue;
                for (auto *child : content->childItems()) {
                    if (!child->isVisible())
                        continue;
                    auto rect = child->mapRectToItem(button, child->boundingRect());
                    QVERIFY2(rect.left() >= -1 && rect.right() <= button->width() + 1 && rect.top() >= -1 &&
                                 rect.bottom() <= button->height() + 1,
                             qPrintable(QString("Button content overflow: %1 (%2 x %3), child %4,%5 %6x%7")
                                            .arg(button->property("hint").toString())
                                            .arg(button->width())
                                            .arg(button->height())
                                            .arg(rect.x())
                                            .arg(rect.y())
                                            .arg(rect.width())
                                            .arg(rect.height())));
                }
            }
        };
        checkButtonBounds();
        window->resize(920, 680);
        QTest::qWait(150);
        checkButtonBounds();
        QVERIFY(left->width() > 400);
        controller.setDual(true);
        QTest::qWait(150);
        QVERIFY(left->width() >= 240);
        QVERIFY(right->width() >= 240);
        QVERIFY(window->grabWindow().save(QString(NOOK_BINARY_DIR) + "/ui-narrow.png"));
        window->resize(1280, 820);
        controller.setDual(false);
        QTest::qWait(100);
        controller.preview()->open(nook::normalizeUri(fixture.path() + "/sample.txt"));
        QTRY_VERIFY_WITH_TIMEOUT(!controller.preview()->loading(), 15000);
        QVERIFY2(!controller.preview()->content().contains("error"),
                 qPrintable(controller.preview()->content()["error"].toString()));
        QVERIFY(controller.preview()->content()["text"].toString().contains("Hello"));
        controller.preview()->close();
        click("settingsButton");
        QTest::qWait(150);
        QVERIFY2(warningSpy.isEmpty(), "QML runtime emitted warnings; inspect test output");
        QVERIFY(window->grabWindow().save(QString(NOOK_BINARY_DIR) + "/ui-test.png"));
        window->close();
    }

    void enhancedBrowsing() {
        QTemporaryDir fixture(QString(NOOK_BINARY_DIR) + "/enhanced-fixture-XXXXXX");
        QFile file(fixture.path() + "/sample.txt");
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write("Space toggles this preview\n");
        file.close();
        QDir().mkdir(fixture.path() + "/folder");
        AppSettings settings;
        settings.set("startFolder", fixture.path());
        settings.set("theme", "dark");
        // The offscreen screen is smaller than the window, which would otherwise count as roomy.
        settings.set("detailsPane", false);
        settings.set("columnWidths", QVariantList{});
        settings.set("quickAccess", QStringList{});
        SearchIndex search;
        Operations ops;
        Application controller(&search, &ops, &settings, false);
        controller.preview()->setWorkerExecutable(QCoreApplication::applicationDirPath() + "/nook");
        QQmlEngine engine;
        engine.addImageProvider("thumb", new ThumbnailProvider);
        QSignalSpy warningSpy(&engine, &QQmlEngine::warnings);
        QQmlComponent component(&engine, QUrl::fromLocalFile(QString(NOOK_SOURCE_DIR) + "/qml/Main.qml"));
        QVERIFY2(component.isReady(), qPrintable(component.errorString()));
        std::unique_ptr<QObject> root(
            component.createWithInitialProperties({{"controller", QVariant::fromValue(&controller)}}));
        QVERIFY2(root, qPrintable(component.errorString()));
        auto *window = qobject_cast<QQuickWindow *>(root.get());
        QVERIFY(window);
        QVERIFY(QTest::qWaitForWindowExposed(window));
        QTRY_VERIFY(!controller.left()->loading());
        auto *left = root->findChild<QQuickItem *>("leftPane");
        QVERIFY(left);
        QTRY_VERIFY(left->width() > window->width() - 300);
        QTRY_VERIFY(left->height() > 400);
        auto clickItem = [&](QQuickItem *item) {
            QVERIFY(item);
            QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier,
                              item->mapToScene({item->width() / 2, item->height() / 2}).toPoint());
        };
        std::function<QQuickItem *(QQuickItem *, const QString &)> findVisual =
            [&](QQuickItem *parent, const QString &name) -> QQuickItem * {
            if (parent->objectName() == name)
                return parent;
            for (auto *child : parent->childItems())
                if (auto *match = findVisual(child, name))
                    return match;
            return nullptr;
        };
        auto drag = [&](QQuickItem *item, QPoint delta) {
            QVERIFY(item);
            const auto start = item->mapToScene({item->width() / 2, item->height() / 2}).toPoint();
            QTest::mousePress(window, Qt::LeftButton, Qt::NoModifier, start);
            for (int i = 1; i <= 4; ++i)
                QTest::mouseMove(window, start + delta * i / 4, 20);
            QTest::mouseRelease(window, Qt::LeftButton, Qt::NoModifier, start + delta);
            QTest::qWait(40);
        };
        const auto sampleUri = nook::normalizeUri(fixture.path() + "/sample.txt");
        const auto folderUri = nook::normalizeUri(fixture.path() + "/folder");
        auto row = left->mapToScene({120, 140}).toPoint();
        QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, row);
        QCOMPARE(controller.left()->selectedUris(), QStringList{sampleUri});
        QTest::keyClick(window, Qt::Key_Space);
        QVERIFY(controller.preview()->visible());
        QTRY_VERIFY_WITH_TIMEOUT(!controller.preview()->loading(), 15000);
        QVERIFY(controller.preview()->content()["text"].toString().contains("Space toggles"));
        auto *preview = root->findChild<QQuickItem *>("previewPanel");
        QVERIFY(preview);
        const auto originalSize = preview->size();
        // Shrink inward so the test also works in compositor-constrained tiled windows.
        drag(findVisual(window->contentItem(), "previewResizeHandle3"), {-100, -60});
        QVERIFY(preview->width() <= originalSize.width() - 90);
        QVERIFY(preview->height() <= originalSize.height() - 50);
        const auto originalPosition = preview->position();
        drag(root->findChild<QQuickItem *>("previewMoveHandle"), {20, 16});
        QVERIFY(preview->x() > originalPosition.x());
        QVERIFY(preview->y() > originalPosition.y());
        QVERIFY(window->grabWindow().save(QString(NOOK_BINARY_DIR) + "/ui-preview.png"));
        auto *previewText = root->findChild<QQuickItem *>("previewText");
        QVERIFY(previewText);
        {
            // Read-only preview text: a themed Chinese menu at the pointer, without edit actions.
            const auto at = previewText->mapToScene({60, 12}).toPoint();
            QTest::mouseClick(window, Qt::RightButton, Qt::NoModifier, at);
            auto *menu = previewText->findChild<QObject *>("textContextMenu");
            QVERIFY(menu);
            QTRY_VERIFY(menu->property("opened").toBool());
            QTest::qWait(120);
            auto *frame = menu->property("contentItem").value<QQuickItem *>()->parentItem();
            const auto rect = frame->mapRectToScene(frame->boundingRect());
            QVERIFY2(qAbs(rect.left() - at.x()) < 24 && qAbs(rect.top() - at.y()) < 24,
                     qPrintable(QString("menu at %1,%2; pointer %3,%4").arg(rect.left()).arg(rect.top()).arg(at.x()).arg(at.y())));
            QVERIFY(window->grabWindow().save(QString(NOOK_BINARY_DIR) + "/ui-text-menu.png"));
            QQuickItem *selectAll = nullptr;
            int editItems = 0;
            for (auto *entry : menu->findChildren<QQuickItem *>()) {
                if (entry->objectName() == "textMenuSelectAll") selectAll = entry;
                const auto label = entry->property("text").toString();
                if ((label == "撤销" || label == "剪切" || label == "粘贴") && entry->isVisible()) ++editItems;
            }
            QCOMPARE(editItems, 0);
            QVERIFY(selectAll);
            QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier,
                              selectAll->mapToScene({selectAll->width() / 2, selectAll->height() / 2}).toPoint());
            QTRY_VERIFY(!menu->property("visible").toBool());
            QCOMPARE(previewText->property("selectedText").toString().size(), previewText->property("length").toInt());
            QVERIFY(controller.preview()->visible());
        }
        previewText->forceActiveFocus();
        QTest::keyClick(window, Qt::Key_Space);
        QVERIFY(!controller.preview()->visible());
        QTest::keyClick(window, Qt::Key_Space);
        QVERIFY(controller.preview()->visible());
        QKeyEvent repeat(QEvent::KeyPress, Qt::Key_Space, Qt::NoModifier, " ", true);
        QCoreApplication::sendEvent(window, &repeat);
        QVERIFY(controller.preview()->visible());
        QTest::keyClick(window, Qt::Key_Space);
        QVERIFY(!controller.preview()->visible());
        QTest::qWait(200);
        QVERIFY(!controller.preview()->visible());

        // A drag is temporary; navigating away and back discards it.
        auto *handle = findVisual(left, "columnResizeHandle0");
        QVERIFY(handle);
        const auto defaultX = handle->mapToItem(left, {0, 0}).x();
        drag(handle, {-100, 0});
        QVERIFY(handle->mapToItem(left, {0, 0}).x() < defaultX - 90);
        QVERIFY(settings.values()["columnWidths"].toList().isEmpty());
        controller.navigate(folderUri);
        QTRY_VERIFY(!controller.left()->loading());
        controller.navigate(nook::normalizeUri(fixture.path()));
        QTRY_VERIFY(!controller.left()->loading());
        QTRY_VERIFY(qAbs(handle->mapToItem(left, {0, 0}).x() - defaultX) < 1);

        drag(handle, {-120, 0});
        clickItem(left->findChild<QQuickItem *>("saveColumnWidthsButton"));
        const auto saved = settings.values()["columnWidths"].toList();
        QCOMPARE(saved.size(), 4);
        AppSettings reloaded;
        QCOMPARE(reloaded.values()["columnWidths"].toList(), saved);
        const auto savedX = handle->mapToItem(left, {0, 0}).x();
        drag(handle, {-60, 0});
        QCOMPARE(settings.values()["columnWidths"].toList(), saved);
        controller.navigate(folderUri);
        QTRY_VERIFY(!controller.left()->loading());
        controller.navigate(nook::normalizeUri(fixture.path()));
        QTRY_VERIFY(!controller.left()->loading());
        QTRY_VERIFY(qAbs(handle->mapToItem(left, {0, 0}).x() - savedX) < 1);

        // Right-click folder -> quick access is persisted, de-duplicated and removable.
        QTest::mouseClick(window, Qt::RightButton, Qt::NoModifier, left->mapToScene({120, 96}).toPoint());
        auto *menu = root->findChild<QObject *>("fileContextMenu");
        QVERIFY(menu);
        QTRY_VERIFY(menu->property("visible").toBool());
        QTest::qWait(160); // Capture the settled menu, not its enter transition.
        QVERIFY(window->grabWindow().save(QString(NOOK_BINARY_DIR) + "/ui-context.png"));
        auto *quick = root->findChild<QObject *>("addQuickAccessMenuItem");
        QVERIFY(quick);
        QVERIFY(quick->property("enabled").toBool());
        clickItem(qobject_cast<QQuickItem *>(quick));
        QTRY_VERIFY(settings.values()["quickAccess"].toStringList().contains(folderUri));
        controller.addQuickAccess(folderUri);
        QCOMPARE(settings.values()["quickAccess"].toStringList().count(folderUri), 1);
        controller.removeQuickAccess(folderUri);
        QVERIFY(!settings.values()["quickAccess"].toStringList().contains(folderUri));
        QMetaObject::invokeMethod(menu, "close");

        // Open With submenu can be opened even when no optional app is installed.
        QTest::mouseClick(window, Qt::RightButton, Qt::NoModifier, left->mapToScene({120, 140}).toPoint());
        auto *openWith = root->findChild<QObject *>("openWithMenu");
        QVERIFY(openWith);
        QVERIFY(openWith->property("enabled").toBool());
        QVERIFY(QMetaObject::invokeMethod(openWith, "open"));
        QTest::qWait(150);
        QVERIFY(openWith->property("visible").toBool());
        QVERIFY(window->grabWindow().save(QString(NOOK_BINARY_DIR) + "/ui-open-with.png"));
        QMetaObject::invokeMethod(openWith, "close");
        QMetaObject::invokeMethod(menu, "close");
        QTest::keyClick(window, Qt::Key_Escape);
        QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, left->mapToScene({120, 140}).toPoint());
        QTest::keyClick(window, Qt::Key_Delete, Qt::ShiftModifier);
        auto *confirmation = root->findChild<QObject *>("deleteDialog");
        QVERIFY(confirmation);
        QTRY_VERIFY(confirmation->property("visible").toBool());
        QVERIFY(QFileInfo::exists(fixture.path() + "/sample.txt"));
        QVERIFY(confirmation->property("title").toString().contains("永久"));
        clickItem(root->findChild<QQuickItem *>("confirmDeleteButton"));
        QTRY_VERIFY_WITH_TIMEOUT(!ops.busy(), 10000);
        QVERIFY2(ops.errors().isEmpty(), qPrintable(ops.errors().join("\n")));
        QVERIFY(!QFileInfo::exists(fixture.path() + "/sample.txt"));

        // Large directories exercise actual wheel events, not only preference storage.
        for (int i = 0; i < 80; ++i) {
            QFile entry(fixture.path() + QString("/folder/file-%1.txt").arg(i));
            QVERIFY(entry.open(QIODevice::WriteOnly));
            entry.write("test");
        }
        controller.navigate(folderUri);
        QTRY_VERIFY(!controller.left()->loading());
        QCOMPARE(controller.left()->count(), 80);
        auto *list = left->findChild<QQuickItem *>("fileList");
        QVERIFY(list);
        QTRY_VERIFY(list->property("contentHeight").toReal() > list->height());
        QTest::qWait(250); // allow confirmation popup exit animation to release pointer grab
        QElapsedTimer wheelClock;
        wheelClock.start();
        auto scrollByWheel = [&](int speed) {
            settings.set("scrollSpeed", speed);
            QMetaObject::invokeMethod(list, "cancelFlick");
            list->setProperty("contentY", 0);
            QTest::qWait(50);
            auto point = list->mapToScene({100, 100});
            QWheelEvent wheel(point, window->mapToGlobal(point.toPoint()), {}, {0, -120}, Qt::NoButton,
                              Qt::NoModifier, Qt::NoScrollPhase, false);
            wheel.setTimestamp(100000 + wheelClock.elapsed());
            QCoreApplication::sendEvent(window, &wheel);
            QTest::qWait(400);
            return list->property("contentY").toReal();
        };
        const auto slow = scrollByWheel(1);
        const auto fast = scrollByWheel(3);
        QVERIFY2(slow > 0 && fast > slow * 1.5,
                 qPrintable(QString("Wheel distance: 1x=%1, 3x=%2").arg(slow).arg(fast)));
        QPointingDevice touchpad(
            "test-touchpad", 9876, QInputDevice::DeviceType::TouchPad, QPointingDevice::PointerType::Finger,
            QInputDevice::Capability::Position | QInputDevice::Capability::PixelScroll, 5, 1);
        auto scrollByPixel = [&](int speed) {
            settings.set("scrollSpeed", speed);
            QMetaObject::invokeMethod(list, "cancelFlick");
            list->setProperty("contentY", 0);
            QTest::qWait(50);
            const auto point = list->mapToScene({100, 100});
            for (auto phase : {Qt::ScrollBegin, Qt::ScrollUpdate, Qt::ScrollUpdate, Qt::ScrollUpdate,
                               Qt::ScrollUpdate, Qt::ScrollEnd}) {
                QWheelEvent event(point, window->mapToGlobal(point.toPoint()),
                                  phase == Qt::ScrollUpdate ? QPoint(0, -40) : QPoint(), {}, Qt::NoButton,
                                  Qt::NoModifier, phase, false, Qt::MouseEventSynthesizedBySystem, &touchpad);
                event.setTimestamp(100000 + wheelClock.elapsed());
                QCoreApplication::sendEvent(window, &event);
                QTest::qWait(20);
            }
            QTest::qWait(150);
            return list->property("contentY").toReal();
        };
        const auto pixelSlow = scrollByPixel(1);
        const auto pixelFast = scrollByPixel(3);
        QVERIFY2(pixelSlow > 0 && qAbs(pixelFast - pixelSlow) < 1,
                 qPrintable(QString("Touchpad distance: 1x=%1, 3x=%2").arg(pixelSlow).arg(pixelFast)));
        settings.set("scrollSpeed", 3);
        clickItem(root->findChild<QQuickItem *>("settingsButton"));
        QTest::qWait(120);
        auto *settingsPanel = root->findChild<QObject *>("settingsDialog");
        QVERIFY(settingsPanel);
        settingsPanel->setProperty("category", 1);
        QTest::qWait(120);
        clickItem(root->findChild<QQuickItem *>("resetColumnWidthsButton"));
        QVERIFY(settings.values()["columnWidths"].toList().isEmpty());
        QVERIFY(window->grabWindow().save(QString(NOOK_BINARY_DIR) + "/ui-settings-browsing.png"));
        QMetaObject::invokeMethod(settingsPanel, "close");
        QTest::qWait(150);
        drag(handle, {-50, 0});
        QVERIFY(qAbs(handle->mapToItem(left, {0, 0}).x() - defaultX) > 40);
        settings.set("columnWidths",
                     QVariantList{}); // reset also clears unsaved drags when no baseline exists
        QTRY_VERIFY(qAbs(handle->mapToItem(left, {0, 0}).x() - defaultX) < 1);
        clickItem(root->findChild<QQuickItem *>("settingsButton"));
        QTest::qWait(100);
        settingsPanel->setProperty("category", 2);
        QTest::qWait(100);
        QVERIFY(window->grabWindow().save(QString(NOOK_BINARY_DIR) + "/ui-settings-search.png"));
        auto *ntfsToggle = root->findChild<QQuickItem *>("ntfsJournalToggle");
        QVERIFY(ntfsToggle && ntfsToggle->isVisible());
        QVERIFY(!ntfsToggle->property("checked").toBool()); // off until the user authorizes it
        if (!controller.ntfsInstalled()) { // never start pkexec from a test
            QSignalSpy notices(&controller, &Application::notice);
            ntfsToggle->setProperty("checked", true);
            QMetaObject::invokeMethod(ntfsToggle, "toggled", Q_ARG(bool, true));
            QCOMPARE(notices.size(), 1); // explains that the package's helper is needed
            QVERIFY(!settings.values()["ntfsJournal"].toBool());
            QVERIFY(!ntfsToggle->property("checked").toBool()); // snapped back
        }
        for (auto *item = ntfsToggle->parentItem(); item; item = item->parentItem())
            if (item->inherits("QQuickFlickable")) {
                item->setProperty("contentY", item->property("contentHeight").toReal() - item->height());
                break;
            }
        QTest::qWait(100);
        QVERIFY(window->grabWindow().save(QString(NOOK_BINARY_DIR) + "/ui-settings-ntfs.png"));
        auto *rootsInput = root->findChild<QObject *>("rootsInput");
        auto *excludeInput = root->findChild<QObject *>("excludeInput");
        QVERIFY(rootsInput && excludeInput);
        const auto scopeRoot = fixture.path() + "/folder";
        const auto excluded = scopeRoot + "/cache";
        rootsInput->setProperty("text", scopeRoot);
        excludeInput->setProperty("text", excluded);
        settings.set("scrollSpeed", 4); // unrelated settings must not overwrite drafts
        QCOMPARE(excludeInput->property("text").toString(), excluded);
        clickItem(root->findChild<QQuickItem *>("saveIndexScopeButton"));
        QCOMPARE(settings.values()["roots"].toStringList(), QStringList{scopeRoot});
        QCOMPARE(settings.values()["exclusions"].toStringList(), QStringList{excluded});
        settings.set("theme", "light");
        // The offscreen screen is smaller than the window, which would otherwise count as roomy.
        settings.set("detailsPane", false);
        settingsPanel->setProperty("category", 0);
        QTest::qWait(150);
        QVERIFY(window->grabWindow().save(QString(NOOK_BINARY_DIR) + "/ui-settings-light.png"));
        QVERIFY2(warningSpy.isEmpty(), "QML runtime emitted warnings; inspect test output");
        window->close();
    }
};
int main(int argc, char **argv) {
    QTemporaryDir env(QString(NOOK_BINARY_DIR) + "/ui-env-XXXXXX");
    qputenv("XDG_CONFIG_HOME", QFile::encodeName(env.path() + "/config"));
    qputenv("XDG_DATA_HOME", QFile::encodeName(env.path() + "/data"));
    qputenv("XDG_CACHE_HOME", QFile::encodeName(env.path() + "/cache"));
    QGuiApplication app(argc, argv);
    app.setApplicationName("nook-UI-test");
    UiTest test;
    int rc = QTest::qExec(&test, argc, argv);
    QThreadPool::globalInstance()->waitForDone();
    return rc;
}
#include "ui_test.moc"
