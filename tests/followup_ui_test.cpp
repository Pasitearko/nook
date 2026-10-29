#include "application.h"
#include <QGuiApplication>
#include <QImage>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQmlExpression>
#include <QQmlProperty>
#include <QQuickItem>
#include <QQuickWindow>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QThreadPool>
#include <QtTest>
#include <QElapsedTimer>
#include <memory>
#include <sys/stat.h>

namespace {
// Popup and view delegates are not necessarily QObject children of Main.qml.
QQuickItem *findVisual(QQuickItem *parent, const QString &name) {
    if (!parent)
        return nullptr;
    if (parent->objectName() == name)
        return parent;
    for (auto *child : parent->childItems())
        if (auto *match = findVisual(child, name))
            return match;
    return nullptr;
}

QList<QQuickItem *> visualItems(QQuickItem *parent, const QString &name) {
    QList<QQuickItem *> result;
    if (!parent)
        return result;
    if (parent->objectName() == name)
        result.append(parent);
    for (auto *child : parent->childItems())
        result.append(visualItems(child, name));
    return result;
}

QStringList placeUris(const QVariantList &places, bool disk) {
    QStringList result;
    for (const auto &place : places) {
        const auto entry = place.toMap();
        if (entry.value("disk").toBool() == disk)
            result.append(entry.value("uri").toString());
    }
    return result;
}

int fileMode(const QString &path) {
    struct stat info{};
    return ::stat(QFile::encodeName(path).constData(), &info) == 0 ? int(info.st_mode & 0777) : -1;
}
} // namespace

class FollowupUiTest : public QObject {
    Q_OBJECT
  private:
    // Declared in lifetime order: QML is destroyed before its controller and fixtures.
    std::unique_ptr<QTemporaryDir> fixture;
    std::unique_ptr<AppSettings> settings;
    std::unique_ptr<SearchIndex> search;
    std::unique_ptr<Operations> operations;
    std::unique_ptr<Application> controller;
    std::unique_ptr<QQmlEngine> engine;
    std::unique_ptr<QObject> root;
    QQuickWindow *window = nullptr;
    QQuickItem *left = nullptr;

    QQuickItem *item(const QString &name) const {
        return findVisual(window->contentItem(), name);
    }
    QObject *object(const char *name) const {
        return root->findChild<QObject *>(name);
    }
    QString uri(const QString &name) const {
        return nook::normalizeUri(fixture->path() + "/" + name);
    }
    QQuickItem *entry(const QString &fileUri) const {
        for (auto *row : visualItems(left, "fileEntry"))
            if (row->property("fileUri").toString() == fileUri && row->isVisible())
                return row;
        return nullptr;
    }
    void click(QQuickItem *target, Qt::MouseButton button = Qt::LeftButton) {
        QVERIFY(target);
        QVERIFY(target->isVisible());
        QVERIFY(target->isEnabled());
        const auto point = target->mapToScene({target->width() / 2, target->height() / 2});
        QVERIFY2(window->contentItem()->boundingRect().contains(point), "Click must be inside the scene");
        QTest::mouseClick(window, button, Qt::NoModifier, point.toPoint());
    }
    void revealSetting(QQuickItem *target) {
        QVERIFY(target);
        auto *scroll = item("settingsScroll");
        QVERIFY(scroll);
        auto *page = scroll->property("contentItem").value<QQuickItem *>();
        QVERIFY(page);
        const auto offset = target->mapToItem(page, {0, 0}).y();
        page->setProperty("contentY", qBound(0.0, page->property("contentY").toReal() + offset - 30,
                                            qMax(0.0, page->property("contentHeight").toReal() - page->height())));
        QTest::qWait(100);
    }
    void blankContext() {
        auto *area = item("emptyAreaPointer");
        QVERIFY(area);
        // Below both fixture rows, away from scroll bars and the pane footer.
        QTest::mouseClick(window, Qt::RightButton, Qt::NoModifier,
                          area->mapToScene({area->width() - 40, area->height() - 40}).toPoint());
        auto *menu = object("blankContextMenu");
        QVERIFY(menu);
        QTRY_VERIFY(menu->property("opened").toBool());
        // opened is synchronous with disabled animations; popup layout and edge
        // clamping still need a render/polish pass before pointer hit testing.
        QTest::qWait(100);
    }
    void closeSettings() {
        auto *panel = object("settingsDialog");
        QVERIFY(panel);
        auto *header = panel->property("header").value<QQuickItem *>();
        QVERIFY(header);
        QQuickItem *close = nullptr;
        for (auto *child : header->childItems())
            if (child->property("hint").toString() == QStringLiteral("关闭设置"))
                close = child;
        QVERIFY(close);
        click(close);
        QTRY_VERIFY(!panel->property("visible").toBool());
    }
    void checkPopupBounds(QObject *popup) {
        QVERIFY(popup);
        auto *content = popup->property("contentItem").value<QQuickItem *>();
        QVERIFY(content);
        auto *frame = content->parentItem();
        QVERIFY(frame);
        const auto rect = frame->mapRectToScene(frame->boundingRect());
        QVERIFY2(rect.left() >= -1 && rect.top() >= -1 && rect.right() <= window->width() + 1 &&
                     rect.bottom() <= window->height() + 1,
                 qPrintable(QString("Popup outside %1x%2 scene: %3,%4 %5x%6")
                                .arg(window->width())
                                .arg(window->height())
                                .arg(rect.x())
                                .arg(rect.y())
                                .arg(rect.width())
                                .arg(rect.height())));
    }

    // Stand-in for a file drag. The platform drag cannot be driven offscreen, but Qt Quick's
    // internal drags reach the same DropAreas; the source names the uris (see dragUris).
    QQuickItem *dragProbe(const QStringList &uris) {
        QQmlComponent component(engine.get());
        component.setData("import QtQuick\nItem { property var fileUris: []; width: 1; height: 1;"
                          " Drag.dragType: Drag.Internal; function begin() { Drag.active = true }"
                          " function release() { return Drag.drop() } function abandon() { Drag.cancel() } }",
                          QUrl());
        auto *probe = qobject_cast<QQuickItem *>(component.create());
        if (!probe)
            return nullptr;
        probe->setParent(root.get());
        probe->setParentItem(window->contentItem());
        probe->setProperty("fileUris", uris);
        QMetaObject::invokeMethod(probe, "begin");
        return probe;
    }
    void hoverAt(QQuickItem *probe, QQuickItem *target, QPointF offset = {}) {
        QVERIFY(probe && target);
        probe->setPosition(target->mapToScene(QPointF(target->width() / 2, target->height() / 2) + offset));
    }
    QQuickItem *crumb(const QString &title) const {
        for (auto *button : visualItems(window->contentItem(), "crumbButton"))
            if (button->property("text").toString() == title && button->isVisible())
                return button;
        return nullptr;
    }

  private Q_SLOTS:
    void init() {
        fixture = std::make_unique<QTemporaryDir>(QString(NOOK_BINARY_DIR) + "/followup-fixture-XXXXXX");
        QVERIFY(fixture->isValid());
        QFile sample(fixture->path() + "/sample.txt");
        QVERIFY(sample.open(QIODevice::WriteOnly));
        QCOMPARE(sample.write("Followup preview fixture\n"), qint64(25));
        sample.close();
        QVERIFY(QDir().mkdir(fixture->path() + "/folder"));
        settings = std::make_unique<AppSettings>();
        settings->store().clear();
        settings->set("startFolder", fixture->path());
        settings->set("roots", QStringList{fixture->path()});
        settings->set("theme", "dark");
        settings->set("animations", false);
        settings->set("hoverPreview", false);
        settings->set("uiScale", 1.0);
        settings->set("quickAccess", QStringList{});
        settings->set("quickAccessOrder", QStringList{});
        search = std::make_unique<SearchIndex>();
        operations = std::make_unique<Operations>();
        controller = std::make_unique<Application>(search.get(), operations.get(), settings.get(), false);
        controller->preview()->setWorkerExecutable(QCoreApplication::applicationDirPath() + "/nook");
        engine = std::make_unique<QQmlEngine>();
        engine->addImageProvider("thumb", new ThumbnailProvider);
        QQmlComponent component(engine.get(),
                                QUrl::fromLocalFile(QString(NOOK_SOURCE_DIR) + "/qml/Main.qml"));
        QVERIFY2(component.isReady(), qPrintable(component.errorString()));
        root.reset(
            component.createWithInitialProperties({{"controller", QVariant::fromValue(controller.get())}}));
        QVERIFY2(root, qPrintable(component.errorString()));
        window = qobject_cast<QQuickWindow *>(root.get());
        QVERIFY(window);
        QVERIFY(QTest::qWaitForWindowExposed(window));
        window->requestActivate();
        QTRY_VERIFY(window->isActive());
        QTRY_VERIFY(!controller->left()->loading());
        QCOMPARE(controller->left()->count(), 2);
        left = item("leftPane");
        QVERIFY(left);
        QTRY_VERIFY(left->height() > 400);
        QTRY_VERIFY(entry(uri("sample.txt")));
    }

    void cleanup() {
        if (window)
            window->close();
        root.reset();
        window = nullptr;
        left = nullptr;
        engine.reset();
        controller.reset();
        operations.reset();
        search.reset();
        settings.reset();
        QThreadPool::globalInstance()->waitForDone();
        fixture.reset();
    }

    void undoHistoryAndTaskCard() {
        auto *card = item("recentActivity");
        QVERIFY(card && !card->isVisible());
        QVERIFY(!item("undoHistoryHint")->isVisible());
        controller->navigate(uri("folder"));
        QTRY_VERIFY(!controller->left()->loading());
        QVERIFY(operations->undoHistory().isEmpty());
        QVERIFY(!card->isVisible());
        controller->navigate(uri(""));
        QTRY_VERIFY(!controller->left()->loading());
        operations->run("copy", {uri("sample.txt")}, uri("folder"));
        QTRY_VERIFY(!operations->busy());
        QTRY_COMPARE(visualItems(window->contentItem(), "undoHistoryRow").size(), 1);
        QTRY_VERIFY(card->isVisible());
        blankContext();
        auto *undo = object("undoFileOperationMenuItem");
        QVERIFY(undo && undo->property("enabled").toBool());
        QVERIFY(undo->property("text").toString().contains(QStringLiteral("撤销粘贴")));
        click(qobject_cast<QQuickItem *>(undo));
        QTRY_VERIFY(!operations->busy());
        QTRY_VERIFY(!QFile::exists(fixture->path() + "/folder/sample.txt"));
        QVERIFY(QFile::exists(fixture->path() + "/sample.txt"));
        QTRY_COMPARE(visualItems(window->contentItem(), "undoHistoryRow").size(), 0);
        QTRY_VERIFY(!card->isVisible());

        QVERIFY(QFile::copy(fixture->path() + "/sample.txt", fixture->path() + "/folder/sample.txt"));
        operations->run("copy", {uri("sample.txt")}, uri("folder"));
        QTRY_VERIFY(operations->waitingConflict());
        QTRY_VERIFY(card->property("taskActive").toBool());
        QTRY_VERIFY(card->isVisible());
        QTRY_VERIFY(item("sidebarTaskProgress")->isVisible());
        auto *conflict = object("conflictDialog");
        QVERIFY(QMetaObject::invokeMethod(conflict, "close"));
        QTest::qWait(100);
        click(item("sidebarCancelTaskButton"));
        QTRY_VERIFY(!operations->busy());
        QTRY_VERIFY(!card->property("taskActive").toBool());
        QTRY_VERIFY(!card->isVisible());
        operations->run("touch", {}, uri(""), "new.txt");
        QTRY_VERIFY(!operations->busy());
        QTRY_COMPARE(visualItems(window->contentItem(), "undoHistoryRow").size(), 1);
        QTRY_VERIFY(card->isVisible());
        auto *row = visualItems(window->contentItem(), "undoHistoryRow").first();
        click(row);
        QTRY_VERIFY(!operations->busy());
        QTRY_VERIFY(!QFile::exists(fixture->path() + "/new.txt"));
        QTRY_VERIFY(!card->isVisible());
        QVERIFY(window->grabWindow().save(QString(NOOK_BINARY_DIR) + "/ui-undo-history.png"));
    }

    void undoHistoryAllowsSelectingAnyRow() {
        for (int i = 0; i < 3; ++i) {
            operations->run("touch", {}, uri(""), "selective-" + QString::number(i));
            QTRY_VERIFY(!operations->busy());
        }
        QTRY_COMPARE(visualItems(window->contentItem(), "undoHistoryRow").size(), 3);
        QTRY_VERIFY(item("undoHistoryHint")->property("text").toString().contains(QStringLiteral("任意")));
        auto expected = operations->undoHistory();
        for (int index : {1, 1, 0}) {
            QQuickItem *selected = nullptr;
            for (auto *row : visualItems(window->contentItem(), "undoHistoryRow")) {
                QVERIFY(row->isEnabled());
                if (row->property("index").toInt() == index)
                    selected = row;
            }
            QVERIFY(selected);
            const auto name = selected->property("modelData").toMap().value("detail").toString();
            QCOMPARE(selected->property("modelData"), expected.at(index));
            expected.removeAt(index);
            QTest::qWait(100);
            click(selected);
            QTRY_VERIFY(!operations->busy());
            QVERIFY2(operations->errors().isEmpty(), qPrintable(operations->errors().join("\n")));
            QVERIFY(!QFile::exists(name));
            QCOMPARE(operations->undoHistory(), expected);
            for (const auto &record : expected)
                QVERIFY(QFile::exists(record.toMap().value("detail").toString()));
        }
        QVERIFY(operations->undoHistory().isEmpty());
        QTRY_VERIFY(!item("recentActivity")->isVisible());
    }

    void undoHistoryAtSmallWindowAndHighScale() {
        for (int i = 0; i < 5; ++i) {
            operations->run("touch", {}, uri(""), "undo-long-file-name-" + QString::number(i) + ".txt");
            QTRY_VERIFY(!operations->busy());
        }
        settings->set("uiScale", 1.5);
        window->resize(920, 640);
        QTest::qWait(150);
        auto *card = item("recentActivity");
        auto *sidebar = item("sidebar");
        auto *scroll = item("undoHistoryScroll");
        QVERIFY(card && sidebar && scroll);
        QVERIFY(window->grabWindow().save(QString(NOOK_BINARY_DIR) + "/ui-undo-small.png"));
        QVERIFY2(sidebar->height() >= 80, qPrintable(QString::number(sidebar->height())));
        QVERIFY(card->height() <= 145);
        QVERIFY(scroll->property("clip").toBool());
        auto *page = scroll->property("contentItem").value<QQuickItem *>();
        QVERIFY(page);
        QVERIFY(page->property("contentHeight").toReal() > page->height());
        page->setProperty("contentY", page->property("contentHeight").toReal() - page->height());
        QTest::qWait(100);
        QQuickItem *last = nullptr;
        for (auto *row : visualItems(window->contentItem(), "undoHistoryRow"))
            if (row->property("index").toInt() == 4) last = row;
        QVERIFY(last && last->isEnabled());
        const auto center = last->mapToItem(scroll, {last->width() / 2, last->height() / 2});
        QVERIFY(scroll->boundingRect().contains(center));
        QVERIFY(window->grabWindow().save(QString(NOOK_BINARY_DIR) + "/ui-undo-small.png"));
        click(last);
        QTRY_VERIFY(!operations->busy());
        QVERIFY2(operations->errors().isEmpty(), qPrintable(operations->errors().join("\n")));
        QCOMPARE(operations->undoHistory().size(), 4);
        QVERIFY(!QFile::exists(fixture->path() + "/undo-long-file-name-0.txt"));
        QVERIFY(QFile::exists(fixture->path() + "/undo-long-file-name-4.txt"));
        QTest::qWait(100);
        click(item("clearUndoHistoryButton"));
        QVERIFY(operations->undoHistory().isEmpty());
        QTRY_VERIFY(!card->isVisible());
        QVERIFY(QFile::exists(fixture->path() + "/undo-long-file-name-4.txt"));
    }

    void undoShortcutRespectsTextEditing() {
        operations->run("touch", {}, uri(""), "shortcut.txt");
        QTRY_VERIFY(!operations->busy());
        auto *address = item("addressField");
        if (!address) {
            QTest::keyClick(window, Qt::Key_L, Qt::ControlModifier);
        } else {
            address->forceActiveFocus();
        }
        QTRY_VERIFY(root->property("editingText").toBool());
        QTest::keyClick(window, Qt::Key_Z, Qt::ControlModifier);
        QTest::qWait(100);
        QVERIFY(QFile::exists(fixture->path() + "/shortcut.txt"));
        QTest::keyClick(window, Qt::Key_Escape);
        click(entry(uri("sample.txt")));
        QTest::keyClick(window, Qt::Key_Z, Qt::ControlModifier);
        QTRY_VERIFY(!operations->busy());
        QTRY_VERIFY(!QFile::exists(fixture->path() + "/shortcut.txt"));
    }
    void taskErrorsVisibleBeforeCompletion() {
        QVERIFY(QFile::copy(fixture->path() + "/sample.txt", fixture->path() + "/folder/sample.txt"));
        operations->run("copy", {uri("missing"), uri("sample.txt")}, uri("folder"));
        QTRY_VERIFY(operations->waitingConflict());
        QVERIFY(operations->busy());
        QCOMPARE(operations->errors().size(), 1);
        QTRY_VERIFY(item("sidebarTaskErrorsButton")->isVisible());
        auto *conflict = object("conflictDialog");
        QVERIFY(QMetaObject::invokeMethod(conflict, "close"));
        QTest::qWait(100);
        QVERIFY(window->grabWindow().save(QString(NOOK_BINARY_DIR) + "/ui-task-progress.png"));
        click(item("sidebarTaskErrorsButton"));
        auto *errors = object("errorsDialog");
        QVERIFY(errors);
        QTRY_VERIFY(errors->property("visible").toBool());
        QVERIFY(QMetaObject::invokeMethod(errors, "close"));
        operations->cancel();
        QTRY_VERIFY(!operations->busy());
    }
    void indexInformationLivesInSettings() {
        click(item("settingsButton"));
        auto *panel = object("settingsDialog");
        QTRY_VERIFY(panel->property("opened").toBool());
        click(item("settingsCategory2"));
        QTest::qWait(100);
        auto *count = item("indexedCountLabel");
        QVERIFY(count);
        QVERIFY(count->isVisible());
        QCOMPARE(count->property("text").toString(), QLocale().toString(controller->search()->indexed()) + QStringLiteral(" 个项目"));
        // QML's default general format switches to exponent notation at this size.
        QQmlExpression formatted(engine->rootContext(), root.get(), "Number(1938171).toLocaleString(Qt.locale(), 'f', 0)");
        const auto million = formatted.evaluate().toString();
        QVERIFY(!formatted.hasError());
        QVERIFY(!million.contains("e", Qt::CaseInsensitive));
        QString digits;
        for (const auto ch : million) if (ch.isDigit()) digits += ch;
        QCOMPARE(digits, QStringLiteral("1938171"));
        QVERIFY(item("updateIndexButton")->isVisible());
        QVERIFY(item("indexStatusLabel")->isVisible());
        QVERIFY(window->grabWindow().save(QString(NOOK_BINARY_DIR) + "/ui-index-settings.png"));
        click(item("settingsCategory1"));
        QTest::qWait(100);
        auto *clear = item("clearUndoHistorySettingsButton");
        QVERIFY(clear);
        revealSetting(clear);
        QVERIFY(!clear->isEnabled());
        closeSettings();
    }

    void spaceAfterSaveAndToolbar() {
        click(entry(uri("sample.txt")));
        QCOMPARE(controller->left()->selectedUris(), QStringList{uri("sample.txt")});
        QSignalSpy saved(left, SIGNAL(columnsSaved()));
        QVERIFY(saved.isValid());
        click(findVisual(left, "saveColumnWidthsButton"));
        QCOMPARE(saved.count(), 1);
        const auto widths = settings->values().value("columnWidths").toList();
        QCOMPARE(widths.size(), 4);
        // Clearing only the displayed toast makes an accidental second save observable.
        root->setProperty("toastMessage", "");
        QSignalSpy preferencesChanged(settings.get(), &AppSettings::changed);
        for (int i = 0; i < 2; ++i) {
            QTest::keyClick(window, Qt::Key_Space);
            QTRY_VERIFY(controller->preview()->visible());
            QTest::keyClick(window, Qt::Key_Space);
            QTRY_VERIFY(!controller->preview()->visible());
        }
        QCOMPARE(saved.count(), 1);
        QCOMPARE(preferencesChanged.count(), 0);
        QVERIFY(root->property("toastMessage").toString().isEmpty());
        QCOMPARE(settings->values().value("columnWidths").toList(), widths);

        // A harmless toolbar button must not trap Space as a button activation.
        click(item("refreshButton"));
        QTRY_VERIFY(!controller->left()->loading());
        QCOMPARE(controller->left()->selectedUris(), QStringList{uri("sample.txt")});
        QTest::keyClick(window, Qt::Key_Space);
        QTRY_VERIFY(controller->preview()->visible());
        QTest::keyClick(window, Qt::Key_Space);
        QTRY_VERIFY(!controller->preview()->visible());
        QCOMPARE(saved.count(), 1);
        QVERIFY(root->property("toastMessage").toString().isEmpty());
    }

    void spaceWhileEditing() {
        click(entry(uri("sample.txt")));
        QSignalSpy visibility(controller->preview(), &Preview::changed);
        QTest::keyClick(window, Qt::Key_L, Qt::ControlModifier);
        auto *address = item("addressField");
        QVERIFY(address);
        QTRY_VERIFY(address->hasActiveFocus());
        QTest::keyClick(window, Qt::Key_Space);
        QCOMPARE(address->property("text").toString(), QStringLiteral(" "));
        QVERIFY(!controller->preview()->visible());

        click(item("searchField"));
        QTRY_VERIFY(item("searchField")->hasActiveFocus());
        QTest::keyClick(window, Qt::Key_Space);
        QCOMPARE(item("searchField")->property("text").toString(), QStringLiteral(" "));
        QVERIFY(!controller->preview()->visible());
        // Whitespace does not switch Main to the search results model.
        QVERIFY(!root->property("searching").toBool());
        click(item("settingsButton"));
        auto *panel = object("settingsDialog");
        QVERIFY(panel);
        QTRY_VERIFY(panel->property("opened").toBool());
        click(item("settingsCategory2"));
        QTRY_COMPARE(panel->property("category").toInt(), 2);
        QTest::qWait(100); // Allow the newly visible card's layout to settle before hit testing.
        auto *roots = item("rootsInput");
        QVERIFY(roots);
        QTRY_VERIFY(roots->isVisible());
        revealSetting(roots);
        click(roots);
        QTRY_VERIFY(roots->hasActiveFocus());
        QTest::keyClick(window, Qt::Key_A, Qt::ControlModifier);
        QTest::keyClick(window, Qt::Key_Space);
        QCOMPARE(roots->property("text").toString(), QStringLiteral(" "));
        QTest::qWait(300); // Include the search debounce and shortcut delivery.
        QCOMPARE(visibility.count(), 0);
        QVERIFY(!controller->preview()->visible());
        closeSettings();
    }

    void blankMenuCreatesEmptyTextFile() {
        blankContext();
        auto *menu = object("blankContextMenu");
        auto *submenu = object("createSubmenu");
        QVERIFY(submenu);
        QVERIFY(!object("fileContextMenu")->property("visible").toBool());
        checkPopupBounds(menu);
        // Point at the submenu's real MenuItem so hover cannot close it while keyboard
        // navigation is in progress. Qt creates this item, so it has no objectName.
        QQuickItem *submenuItem = nullptr;
        for (auto *candidate : visualItems(window->contentItem(), QString{}))
            if (candidate->property("subMenu").value<QObject *>() == submenu)
                submenuItem = candidate;
        QVERIFY(submenuItem);
        // Click directly: moving and then clicking can race the submenu's hover timer.
        click(submenuItem);
        QTRY_VERIFY(submenu->property("opened").toBool());
        QTest::qWait(100); // Polish submenu delegate positions while pointer stays on its parent item.
        QTRY_VERIFY(item("newTextMenuItem"));
        auto *create = item("newTextMenuItem");
        QTest::mouseMove(window, create->mapToScene({40, create->height() / 2}).toPoint());
        click(create);
        QTRY_VERIFY(item("nameInput"));
        auto *name = item("nameInput");
        QVERIFY(name);
        QTRY_VERIFY(name->hasActiveFocus());
        QCOMPARE(root->property("inputKind").toString(), QStringLiteral("touch"));
        QTest::qWait(260); // Capture the settled dialog, not its enter transition.
        QVERIFY(window->grabWindow().save(QString(NOOK_BINARY_DIR) + "/ui-name-dialog.png"));
        QTest::keyClick(window, Qt::Key_A, Qt::ControlModifier);
        for (char character : QByteArray("created by UI.txt"))
            QTest::keyClick(window, character);
        QSignalSpy finished(operations.get(), &Operations::finished);
        QTest::keyClick(window, Qt::Key_Return);
        QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 1, 10000);
        QTRY_VERIFY(!operations->busy());
        QVERIFY2(operations->errors().isEmpty(), qPrintable(operations->errors().join('\n')));
        const QFileInfo created(fixture->path() + "/created by UI.txt");
        QVERIFY(created.isFile());
        QCOMPARE(created.size(), qint64(0));
        QTRY_VERIFY(!controller->left()->loading());
        QTRY_COMPARE(controller->left()->count(), 3);
        QVERIFY(!menu->property("visible").toBool());
    }

    void restartFromSettings() {
        click(item("settingsButton"));
        auto *panel = object("settingsDialog");
        QVERIFY(panel);
        QTRY_VERIFY(panel->property("opened").toBool());
        click(item("settingsCategory3"));
        QTRY_COMPARE(panel->property("category").toInt(), 3);
        // Running from the build tree, nothing has replaced the program.
        QVERIFY(!controller->upgraded());
        QCOMPARE(controller->installedVersion(), QCoreApplication::applicationVersion());
        auto *restart = item("restartButton");
        QVERIFY(restart);
        revealSetting(restart);
        QVERIFY(restart->isEnabled());
        QSignalSpy requested(controller.get(), &Application::restartRequested);
        click(restart);
        QCOMPARE(requested.size(), 1);
        closeSettings();
    }
    void restartKeepsTabs() {
        controller->addTab(nook::normalizeUri(fixture->path()));
        controller->setGrid(true);
        controller->setActiveTab(0);
        const auto state = controller->state();
        QCOMPARE(state["tabs"].toArray().size(), 2);
        Application next(search.get(), operations.get(), settings.get(), false, nullptr, state);
        QCOMPARE(next.tabs().size(), 2);
        QCOMPARE(next.activeTab(), 0);
        QCOMPARE(next.state(), state);
        controller->closeTab(1);
    }

    void mountSettingsRequireConfirmation() {
        QTRY_VERIFY(!controller->diskMounts()->busy());
        click(item("settingsButton"));
        auto *panel = object("settingsDialog");
        QVERIFY(panel);
        QTRY_VERIFY(panel->property("opened").toBool());
        click(item("settingsCategory4"));
        QTRY_COMPARE(panel->property("category").toInt(), 4);
        QTRY_VERIFY(!controller->diskMounts()->busy());
        QTest::qWait(100);
        auto *apply = item("applyMountsButton");
        QVERIFY(apply);
        QVERIFY(!apply->isEnabled());
        // Exercise the confirmation UI with an in-memory choice only. Never click
        // authorization and never send any request to pkexec or the real helper.
        const QVariantMap disk{{"uuid", "0123456789ABCDEF"}, {"autoMount", false}, {"readOnly", false}};
        QVERIFY(QMetaObject::invokeMethod(panel, "changeMount", Q_ARG(QVariant, disk),
                                         Q_ARG(QVariant, true), Q_ARG(QVariant, true)));
        QTRY_VERIFY(apply->isEnabled());
        QSignalSpy backendState(controller->diskMounts(), &DiskMounts::changed);
        click(apply);
        auto *confirm = panel->findChild<QObject *>("mountConfirmDialog");
        QVERIFY(confirm);
        QTRY_VERIFY(confirm->property("opened").toBool());
        checkPopupBounds(confirm);
        QVERIFY(!controller->diskMounts()->busy());
        click(item("cancelMountApplyButton"));
        QTRY_VERIFY(!confirm->property("visible").toBool());
        QCOMPARE(backendState.count(), 0);
        closeSettings();
    }

    void archiveMenuItems() {
        // A plain file can be compressed, not extracted; the one-click name comes from it.
        click(entry(uri("sample.txt")), Qt::RightButton);
        auto *menu = object("fileContextMenu");
        QTRY_VERIFY(menu->property("opened").toBool());
        QVERIFY(item("compressMenuItem")->isVisible());
        QCOMPARE(item("compressZipMenuItem")->property("text").toString(), QStringLiteral("压缩为“sample.zip”"));
        QVERIFY(!item("extractHereMenuItem")->isVisible());
        QMetaObject::invokeMethod(menu, "close");
        QTRY_VERIFY(!menu->property("visible").toBool());

        QTemporaryDir other(QString(NOOK_BINARY_DIR) + "/archive-menu-XXXXXX");
        for (const char *name : {"相册.zip", "data.7z.001", "notes.txt"}) {
            QFile file(other.path() + "/" + name);
            QVERIFY(file.open(QIODevice::WriteOnly));
            file.write("x");
        }
        const auto at = [&](const char *name) { return QUrl::fromLocalFile(other.path() + "/" + name).toString(); };
        QVariantMap actions = controller->archiveActions({at("相册.zip")});
        QVERIFY(actions.value("extract").toBool());
        QCOMPARE(actions.value("folderName").toString(), QStringLiteral("相册"));
        actions = controller->archiveActions({at("相册.zip"), at("data.7z.001")});
        QVERIFY(actions.value("extract").toBool());
        QCOMPARE(actions.value("zipName").toString(), other.path().section('/', -1) + ".zip");
        QVERIFY(!controller->archiveActions({at("相册.zip"), at("notes.txt")}).value("extract").toBool());
        QVERIFY(!controller->archiveActions({"trash:///x"}).value("compress").toBool());
        const QStringList command = controller->archiveCommand("extract-here", {at("相册.zip")});
        QVERIFY(command.first().endsWith("nook-archive"));
        QCOMPARE(command.mid(1), (QStringList{"--extract-here", "--", other.path() + "/相册.zip"}));
    }

    void propertiesApplyRealPermissions() {
        const auto path = fixture->path() + "/sample.txt";
        QCOMPARE(::chmod(QFile::encodeName(path).constData(), 0600), 0);
        click(entry(uri("sample.txt")), Qt::RightButton);
        auto *menu = object("fileContextMenu");
        QVERIFY(menu);
        QTRY_VERIFY(menu->property("opened").toBool());
        click(item("filePropertiesMenuItem"));
        auto *dialog = object("propertiesDialog");
        QVERIFY(dialog);
        QTRY_VERIFY(dialog->property("opened").toBool());
        QTRY_VERIFY(!controller->properties()->busy());
        const auto details = controller->properties()->details();
        QCOMPARE(details.value("name").toString(), QStringLiteral("sample.txt"));
        QCOMPARE(details.value("permissions").toInt(), 0600);
        QVERIFY2(details.value("editable").toBool(),
                 qPrintable(details.value("permissionsError").toString()));
        // Drag-selecting text in the dialog selects text: it must neither start a file drag in
        // the pane underneath nor drag the scrollable page itself around.
        QQuickItem *nameText = nullptr;
        for (auto *candidate : dialog->findChildren<QQuickItem *>())
            if (QString(candidate->metaObject()->className()).startsWith("QQuickTextEdit") &&
                candidate->property("text").toString() == "sample.txt")
                nameText = candidate;
        QVERIFY(nameText);
        // A short window makes the dialog page scrollable, which is when a drag used to move it.
        const auto fullSize = window->size();
        window->resize(fullSize.width(), 480);
        QTest::qWait(150); // let the rows re-lay out after the details arrived
        const auto handlers = window->findChildren<QObject *>();
        auto fileDragActive = [&] {
            for (auto *handler : handlers)
                if (QString(handler->metaObject()->className()) == "QQuickDragHandler" &&
                    handler->property("active").toBool())
                    return true;
            return false;
        };
        bool fileDrag = false;
        const auto from = nameText->mapToScene({2, nameText->height() / 2}).toPoint();
        QTest::mousePress(window, Qt::LeftButton, Qt::NoModifier, from);
        for (int i = 1; i <= 8; ++i) {
            QTest::mouseMove(window, from + QPoint(i * 8, i * 6), 15);
            fileDrag = fileDrag || fileDragActive();
        }
        QTest::mouseRelease(window, Qt::LeftButton, Qt::NoModifier, from + QPoint(64, 48));
        auto *page = dialog->property("contentItem").value<QQuickItem *>()->property("contentItem").value<QQuickItem *>();
        QVERIFY(page);
        QVERIFY(page->property("contentHeight").toReal() > page->height());
        QCOMPARE(page->property("contentY").toReal(), 0.0);
        // Starting the drag just beside the text (on the label) must not drag the page either.
        const auto label = nameText->mapToScene({-30, nameText->height() / 2}).toPoint();
        QTest::mousePress(window, Qt::LeftButton, Qt::NoModifier, label);
        for (int i = 1; i <= 8; ++i)
            QTest::mouseMove(window, label + QPoint(i * 4, -i * 12), 15);
        QTest::mouseRelease(window, Qt::LeftButton, Qt::NoModifier, label + QPoint(32, -96));
        QTest::qWait(50);
        QCOMPARE(page->property("contentY").toReal(), 0.0);
        QVERIFY2(!fileDrag, "text selection in the properties dialog started a file drag");
        QVERIFY(!nameText->property("selectedText").toString().isEmpty());
        window->resize(fullSize);
        QTest::qWait(100);
        auto *grid = item("permissionGrid");
        QVERIFY(grid);
        QVERIFY(grid->isVisible());
        // Permission grid index 5 is group-read (0040); exercise its actual checkbox.
        auto *groupRead = item("permissionBit5");
        QVERIFY(groupRead);
        QVERIFY(!groupRead->property("checked").toBool());
        // Extra metadata rows (e.g. Hidden) can put the permission grid below the viewport.
        // Scroll it into view instead of clicking a clipped checkbox through the footer.
        page->setProperty("contentY", qMax(0.0, page->property("contentHeight").toReal() - page->height()));
        QTest::qWait(100);
        click(groupRead);
        QTRY_COMPARE(dialog->property("draftMode").toInt(), 0640);
        QCOMPARE(fileMode(path), 0600); // A draft must not mutate the file.
        auto *apply = item("applyPermissionsButton");
        QVERIFY(apply);
        QTRY_VERIFY(apply->isEnabled());
        QSignalSpy applied(controller->properties(), &FileProperties::permissionsApplied);
        click(apply);
        QTRY_COMPARE_WITH_TIMEOUT(applied.count(), 1, 10000);
        QTRY_VERIFY(!controller->properties()->busy());
        QCOMPARE(fileMode(path), 0640);
        QCOMPARE(controller->properties()->details().value("permissions").toInt(), 0640);
        QTRY_VERIFY(!apply->isEnabled());
        click(item("closePropertiesButton"));
        QTRY_VERIFY(!dialog->property("visible").toBool());
    }

    void scaledWorkspaceAndPopups_data() {
        QTest::addColumn<double>("scale");
        QTest::newRow("125-percent") << 1.25;
        QTest::newRow("75-percent") << 0.75;
    }

    void scaledWorkspaceAndPopups() {
        QFETCH(double, scale);
        settings->set("uiScale", scale);
        auto *workspace = item("scaledWorkspace");
        QVERIFY(workspace);
        QTRY_COMPARE(workspace->scale(), scale);
        QTRY_VERIFY(qAbs(workspace->width() * scale - window->width()) < 1);
        QTRY_VERIFY(qAbs(workspace->height() * scale - window->height()) < 1);
        const auto scene = workspace->mapRectToScene(workspace->boundingRect());
        QVERIFY(qAbs(scene.width() - window->width()) < 1);
        QVERIFY(qAbs(scene.height() - window->height()) < 1);
        QTest::qWait(100); // QtQuick layouts polish asynchronously after a scale change.
        for (int i = 0; i < 2; ++i) {
            click(item("settingsButton"));
            auto *panel = object("settingsDialog");
            QVERIFY(panel);
            QTRY_VERIFY(panel->property("opened").toBool());
            auto *content = panel->property("contentItem").value<QQuickItem *>();
            QVERIFY(content);
            const auto mapped = content->mapRectToScene(content->boundingRect());
            QVERIFY(qAbs(mapped.width() / content->width() - scale) < 0.01);
            QVERIFY(qAbs(mapped.height() / content->height() - scale) < 0.01);
            checkPopupBounds(panel);
            closeSettings(); // Uses scene-mapped pointer coordinates at each scale.
        }
        blankContext(); // Near the bottom-right; must clamp in logical, not physical pixels.
        checkPopupBounds(object("blankContextMenu"));
        QTest::keyClick(window, Qt::Key_Escape);
        QTRY_VERIFY(!object("blankContextMenu")->property("visible").toBool());
        click(entry(uri("sample.txt")), Qt::RightButton);
        QTRY_VERIFY(object("fileContextMenu")->property("opened").toBool());
        checkPopupBounds(object("fileContextMenu"));
        QTest::keyClick(window, Qt::Key_Escape);
    }

    void middleClickClosesTab() {
        controller->addTab(uri("folder"));
        QTRY_COMPARE(controller->tabs().size(), 2);
        QTest::qWait(50);
        const auto cards = visualItems(window->contentItem(), "tabCard");
        QCOMPARE(cards.size(), 2);
        QQuickItem *second = cards[0]->mapToScene({0, 0}).x() > cards[1]->mapToScene({0, 0}).x() ? cards[0] : cards[1];
        // Away from the close button at the right end of the tab.
        QTest::mouseClick(window, Qt::MiddleButton, Qt::NoModifier, second->mapToScene({30, second->height() / 2}).toPoint());
        QTRY_COMPARE(controller->tabs().size(), 1);
    }

    // Dragging a tab sideways moves it among the others; the strip itself stays put.
    void dragReordersTabs() {
        controller->addTab(uri("folder"));
        controller->addTab(nook::parentUri(nook::normalizeUri(fixture->path())));
        QTRY_COMPARE(controller->tabs().size(), 3);
        controller->setActiveTab(0);
        QTest::qWait(50);
        auto ordered = [this] {
            auto cards = visualItems(window->contentItem(), "tabCard");
            std::sort(cards.begin(), cards.end(), [](QQuickItem *a, QQuickItem *b) {
                return a->mapToScene({0, 0}).x() < b->mapToScene({0, 0}).x();
            });
            return cards;
        };
        auto uris = [this] {
            QStringList list;
            for (const auto &tab : controller->tabs())
                list << tab.toMap().value("uri").toString();
            return list;
        };
        const QStringList before = uris();
        const auto cards = ordered();
        QCOMPARE(cards.size(), 3);
        const QPointF start = cards[0]->mapToScene({30, cards[0]->height() / 2});
        const qreal step = cards[1]->mapToScene({0, 0}).x() - cards[0]->mapToScene({0, 0}).x();
        QTest::mousePress(window, Qt::LeftButton, Qt::NoModifier, start.toPoint());
        // Across the second tab, onto the third.
        for (int i = 1; i <= 20; ++i) {
            QTest::mouseMove(window, (start + QPointF(step * 2.2 * i / 20, 0)).toPoint());
            QTest::qWait(10);
        }
        QTest::mouseRelease(window, Qt::LeftButton, Qt::NoModifier, (start + QPointF(step * 2.2, 0)).toPoint());
        QTRY_COMPARE(uris(), (QStringList{before[1], before[2], before[0]}));
        QCOMPARE(controller->activeTab(), 2);
        auto *strip = cards[0]->parentItem()->parentItem()->parentItem(); // card, tab, content, list
        QCOMPARE(strip->property("contentX").toReal(), strip->property("originX").toReal());
        // Back in place on the strip after letting go.
        QTRY_COMPARE(ordered()[2]->property("x").toReal(), 0.0);
        controller->closeTab(2);
        controller->closeTab(1);
    }

    // The window's ✕ sits in the top-right corner, beside the toolbar buttons, and closes it.
    void closeButtonClosesWindow() {
        auto *close = item("windowCloseButton");
        auto *settingsButton = item("settingsButton");
        QVERIFY(close && settingsButton);
        const auto rect = close->mapRectToScene(close->boundingRect());
        const auto beside = settingsButton->mapRectToScene(settingsButton->boundingRect());
        QVERIFY2(window->width() - rect.right() < 24, qPrintable(QString::number(window->width() - rect.right())));
        QVERIFY(rect.top() < 30);
        QCOMPARE(rect.height(), beside.height());
        QVERIFY(rect.left() > beside.right());
        click(close);
        QTRY_VERIFY(!window->isVisible());
    }

    // Delete goes straight to the trash; only permanent deletion asks, in nook's own dialog.
    void trashAsksNothing() {
        auto *dialog = object("deleteDialog");
        QVERIFY(dialog);
        controller->left()->selectUri(uri("sample.txt"));
        QTRY_COMPARE(controller->left()->selectedCount(), 1);
        window->requestActivate();
        QTest::keyClick(window, Qt::Key_Delete);
        QTRY_VERIFY_WITH_TIMEOUT(!QFileInfo::exists(fixture->path() + "/sample.txt"), 10000);
        QVERIFY(!dialog->property("visible").toBool());
        QTRY_VERIFY(!operations->busy());
        QVERIFY2(operations->errors().isEmpty(), qPrintable(operations->errors().join("\n")));

        controller->left()->selectUri(uri("folder"));
        QTRY_COMPARE(controller->left()->selectedCount(), 1);
        QTest::keyClick(window, Qt::Key_Delete, Qt::ShiftModifier);
        QTRY_VERIFY(dialog->property("opened").toBool());
        QVERIFY(dialog->property("danger").toBool());
        QVERIFY(dialog->property("message").toString().contains("folder"));
        checkPopupBounds(dialog);
        QTest::keyClick(window, Qt::Key_Escape);
        QTRY_VERIFY(!dialog->property("visible").toBool());
        QVERIFY(QFileInfo::exists(fixture->path() + "/folder"));
    }

    void selectionMode() {
        auto *folderRow = entry(uri("folder"));
        auto *sampleRow = entry(uri("sample.txt"));
        QVERIFY(folderRow && sampleRow);
        click(item("selectionModeButton"));
        QTRY_VERIFY(root->property("selectionMode").toBool());
        QTRY_VERIFY(item("pickMark") && item("pickMark")->isVisible());
        auto *model = controller->left();
        // A click toggles one item and keeps the others.
        click(sampleRow);
        QTRY_COMPARE(model->selectedCount(), 1);
        click(folderRow);
        QTRY_COMPARE(model->selectedCount(), 2);
        click(sampleRow);
        QTRY_COMPARE(model->selectedUris(), QStringList{uri("folder")});
        QVERIFY(item("paneCount")->property("text").toString().startsWith(QStringLiteral("已选 1 /")));
        auto *area = item("emptyAreaPointer");
        const QPoint blank = area->mapToScene({area->width() - 40, area->height() - 40}).toPoint();
        // A drag that starts on an item draws a band (no file drag); it flips what it covers.
        auto band = [&](QPoint from, QPoint to) {
            QTest::mousePress(window, Qt::LeftButton, Qt::NoModifier, from);
            for (int i = 1; i <= 8; ++i)
                QTest::mouseMove(window, from + (to - from) * i / 8);
            QTRY_VERIFY(item("selectionBand")->isVisible());
            QTest::mouseRelease(window, Qt::LeftButton, Qt::NoModifier, to);
            QTRY_VERIFY(!item("selectionBand")->isVisible());
        };
        const QPoint onSample = sampleRow->mapToScene({60, sampleRow->height() / 2}).toPoint();
        const QPoint onFolder = folderRow->mapToScene({60, folderRow->height() / 2}).toPoint();
        band(onSample, onSample + QPoint(30, 4)); // sample was not picked: now it is
        QTRY_COMPARE(model->selectedCount(), 2);
        QVERIFY(window->grabWindow().save(QString(NOOK_BINARY_DIR) + "/ui-selection-mode.png"));
        band(onSample, onFolder + QPoint(30, 0)); // both were picked: both let go
        QTRY_COMPARE(model->selectedCount(), 0);
        band(onFolder, onFolder + QPoint(30, 4));
        QTRY_COMPARE(model->selectedUris(), QStringList{uri("folder")});
        // A left click on blank space clears the picks.
        QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, blank);
        QTRY_COMPARE(model->selectedCount(), 0);
        // Esc leaves the mode and keeps the selection.
        click(sampleRow);
        click(folderRow);
        QTRY_COMPARE(model->selectedCount(), 2);
        QTest::keyClick(window, Qt::Key_Escape);
        QTRY_VERIFY(!root->property("selectionMode").toBool());
        QCOMPARE(model->selectedCount(), 2);
        QVERIFY(!item("pickMark") || !item("pickMark")->isVisible());
        // Ordinary clicks again: blank space clears.
        QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, blank);
        QTRY_COMPARE(model->selectedCount(), 0);
    }

    void detailColumnsStartBand() {
        // In the list view only the name drags files; the other cells start a band, as in Explorer.
        auto *sampleRow = entry(uri("sample.txt"));
        QVERIFY(sampleRow);
        const qreal dateX = 9 + left->property("columnsWidth").toReal() - 150;
        const QPoint start = sampleRow->mapToScene({dateX, sampleRow->height() / 2}).toPoint();
        const QPoint end = start - QPoint(0, int(sampleRow->height()));
        QTest::mousePress(window, Qt::LeftButton, Qt::NoModifier, start);
        for (int i = 1; i <= 8; ++i)
            QTest::mouseMove(window, start + (end - start) * i / 8);
        QTRY_VERIFY(item("selectionBand")->isVisible());
        QTest::mouseRelease(window, Qt::LeftButton, Qt::NoModifier, end);
        QTRY_COMPARE(controller->left()->selectedCount(), 2);
        QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, start); // a click there still selects the row
        QTRY_COMPARE(controller->left()->selectedUris(), QStringList{uri("sample.txt")});
    }

    void dropOnPathSegment() {
        QVERIFY(QDir().mkpath(fixture->path() + "/folder/inner"));
        for (const auto &name : {"one.txt", "two.txt"}) {
            QFile file(fixture->path() + "/folder/inner/" + name);
            QVERIFY(file.open(QIODevice::WriteOnly));
        }
        controller->navigate(uri("folder/inner"));
        QTRY_VERIFY(!controller->left()->loading());
        QTRY_VERIFY(crumb("folder"));
        const QString rootTitle = QFileInfo(fixture->path()).fileName();
        QTRY_VERIFY(crumb(rootTitle));
        // Dropped promptly on a segment further up: moved there, and the view stays put.
        auto *probe = dragProbe({uri("folder/inner/one.txt")});
        QVERIFY(probe);
        hoverAt(probe, crumb(rootTitle));
        QTRY_VERIFY(root->property("dropHintText").toString().startsWith(QStringLiteral("移动到")));
        QVERIFY(QMetaObject::invokeMethod(probe, "release"));
        QTRY_VERIFY(QFile::exists(fixture->path() + "/one.txt"));
        QVERIFY(!QFile::exists(fixture->path() + "/folder/inner/one.txt"));
        QCOMPARE(controller->activePane()->uri(), uri("folder/inner"));
        QCOMPARE(root->property("dropHintText").toString(), QString());
        delete probe;
        // Resting on a segment opens it; the trail holds still meanwhile (the deeper segment
        // stays), and dropping there then moves into it.
        probe = dragProbe({uri("folder/inner/two.txt")});
        hoverAt(probe, crumb("folder"));
        QTRY_COMPARE(controller->activePane()->uri(), uri("folder"));
        QVERIFY(crumb("inner"));
        QVERIFY(QMetaObject::invokeMethod(probe, "release"));
        QTRY_VERIFY(QFile::exists(fixture->path() + "/folder/two.txt"));
        QTRY_VERIFY(!crumb("inner")); // released: the trail follows the location again
        delete probe;
    }

    void fileDragRunsToTheEnd() {
        // The drag starts a moment after it is requested (the offscreen platform then ends it at
        // once): requesting it must not cancel it, and its end must come back without a script error.
        QSignalSpy warnings(engine.get(), &QQmlEngine::warnings);
        root->setProperty("springMoved", true);
        QVERIFY(QMetaObject::invokeMethod(root.get(), "startFileDrag", Q_ARG(QVariant, QStringList{uri("sample.txt")}),
                                          Q_ARG(QVariant, QStringLiteral("file"))));
        auto *source = item("fileDragSource");
        QVERIFY(source);
        QCOMPARE(root->property("springOrigin").toMap().value("uri").toString(), controller->activePane()->uri());
        QTRY_VERIFY(root->property("springOrigin").isNull() || !root->property("springOrigin").isValid());
        QVERIFY(!QQmlProperty::read(source, "Drag.active", qmlContext(source)).toBool());
        QVERIFY2(warnings.isEmpty(), qPrintable(warnings.isEmpty() ? QString() : warnings.first().first().value<QList<QQmlError>>().first().toString()));
    }

    void restingOnEllipsisClimbs() {
        QVERIFY(QDir().mkpath(fixture->path() + "/folder/inner"));
        controller->navigate(uri("folder/inner"));
        QTRY_VERIFY(!controller->left()->loading());
        const auto trail = controller->activePane()->breadcrumbs();
        QVERIFY(trail.size() > 2 && trail[1].toMap()["title"].toString() == QStringLiteral("…"));
        const QString first = trail[1].toMap()["uri"].toString();
        QTRY_VERIFY(crumb(QStringLiteral("…")));
        auto *probe = dragProbe({uri("sample.txt")});
        QVERIFY(probe);
        hoverAt(probe, crumb(QStringLiteral("…")));
        QTRY_COMPARE(controller->activePane()->uri(), first);
        QTRY_VERIFY(first.startsWith(controller->activePane()->uri()) && controller->activePane()->uri() != first);
        QVERIFY(QMetaObject::invokeMethod(probe, "abandon")); // never drop outside the fixture
        delete probe;
    }

    void hoverOpensFoldersAndPlaces() {
        QVERIFY(QDir().mkpath(fixture->path() + "/folder/deeper/deepest"));
        settings->set("quickAccess", QStringList{uri("folder/deeper")});
        settings->set("quickAccessOrder", QStringList{uri("folder/deeper")});
        auto *probe = dragProbe({uri("sample.txt")});
        QVERIFY(probe);
        // A folder in the list opens after a moment.
        hoverAt(probe, entry(uri("folder")), {-200, 0});
        QTRY_COMPARE(controller->activePane()->uri(), uri("folder"));
        QTRY_VERIFY(entry(uri("folder/deeper")));
        // The pointer now rests on "deeper" in the new listing: nothing opens until it moves.
        hoverAt(probe, entry(uri("folder/deeper")), {-200, 0});
        QTest::qWait(1200);
        QCOMPARE(controller->activePane()->uri(), uri("folder"));
        hoverAt(probe, entry(uri("folder/deeper")), {-180, 2});
        QTRY_COMPARE(controller->activePane()->uri(), uri("folder/deeper"));
        // A quick-access place opens the same way (here: back to the start, then the place).
        controller->navigate(fixture->path());
        QTRY_VERIFY(!controller->left()->loading());
        QQuickItem *place = nullptr;
        QTRY_VERIFY([&] {
            for (auto *row : visualItems(item("sidebar"), "quickPlace"))
                if (row->property("entry").toMap().value("uri").toString() == uri("folder/deeper"))
                    place = row;
            return place != nullptr;
        }());
        hoverAt(probe, place, {0, 1});
        QTRY_COMPARE(controller->activePane()->uri(), uri("folder/deeper"));
        QVERIFY(QMetaObject::invokeMethod(probe, "release")); // dropped on the place itself
        QTRY_VERIFY(QFile::exists(fixture->path() + "/folder/deeper/sample.txt"));
        delete probe;
        // A cancelled drag goes back to where it started.
        root->setProperty("springOrigin", QVariantMap{{"tab", 0}, {"side", 0}, {"uri", uri("folder")}});
        root->setProperty("springMoved", true);
        QVERIFY(QMetaObject::invokeMethod(root.get(), "fileDragFinished", Q_ARG(QVariant, int(Qt::IgnoreAction))));
        QTRY_COMPARE(controller->activePane()->uri(), uri("folder"));
    }

    void keyboardNeverSavesColumns() {
        // Saving column widths is only ever a deliberate click; keys used to browse folders
        // (Enter, Backspace, Space) must never reach that button.
        auto *save = item("saveColumnWidthsButton");
        QVERIFY(save);
        QSignalSpy saved(left, SIGNAL(columnsSaved()));
        click(save);
        QCOMPARE(saved.count(), 1);
        root->setProperty("toastMessage", "");
        // Even if Tab once reached the header, clicking a row gives the keys back to the list.
        click(entry(uri("folder")));
        for (int i = 0; i < 40 && window->activeFocusItem() != save; ++i)
            QTest::keyClick(window, Qt::Key_Tab);
        for (int round = 0; round < 2; ++round) {
            click(entry(uri("folder")));
            QCOMPARE(controller->left()->selectedUris(), QStringList{uri("folder")});
            QTest::keyClick(window, Qt::Key_Return);
            QTRY_COMPARE(controller->left()->uri(), uri("folder"));
            QTRY_VERIFY(!controller->left()->loading());
            QTest::keyClick(window, Qt::Key_Backspace);
            QTRY_COMPARE(controller->left()->uri(), nook::normalizeUri(fixture->path()));
            QTRY_VERIFY(!controller->left()->loading());
            QTRY_VERIFY(entry(uri("folder")));
        }
        QTest::keyClick(window, Qt::Key_Space);
        QTest::keyClick(window, Qt::Key_Space);
        QTest::qWait(50);
        QCOMPARE(saved.count(), 1);
        QVERIFY2(root->property("toastMessage").toString().isEmpty(), qPrintable(root->property("toastMessage").toString()));
    }

    void backspaceGoesUp() {
        const auto top = controller->left()->uri();
        controller->left()->navigate(uri("folder"));
        QTRY_VERIFY(!controller->left()->loading());
        QCOMPARE(controller->left()->uri(), uri("folder"));
        // Typing in a field: Backspace edits the text and must not navigate.
        auto *address = item("addressField");
        QVERIFY(address);
        address->forceActiveFocus();
        QTRY_VERIFY(address->hasActiveFocus());
        QTest::keyClick(window, Qt::Key_End);
        QTest::keyClick(window, Qt::Key_Backspace);
        QTest::qWait(50);
        QCOMPARE(controller->left()->uri(), uri("folder"));
        QTest::keyClick(window, Qt::Key_Escape);
        // With the file area focused it goes up one level.
        click(item("emptyAreaPointer"));
        QTest::keyClick(window, Qt::Key_Backspace);
        QTRY_COMPARE(controller->left()->uri(), top);
    }

    void textContextMenuIsThemed_data() {
        QTest::addColumn<QString>("field");
        QTest::newRow("search") << "searchField";
        QTest::newRow("address") << "addressField";
    }

    void textContextMenuIsThemed() {
        QFETCH(QString, field);
        auto *address = item(field);
        QVERIFY(address);
        if (address->property("text").toString().isEmpty())
            address->setProperty("text", "nook"); // an empty field has nothing to select
        address->forceActiveFocus();
        QMetaObject::invokeMethod(address, "selectAll");
        click(address, Qt::RightButton);
        auto *menu = qobject_cast<QObject *>(address->findChild<QObject *>("textContextMenu"));
        QVERIFY(menu);
        QTRY_VERIFY(menu->property("opened").toBool());
        QTest::qWait(100);
        checkPopupBounds(menu);
        // Opens beside the pointer, not somewhere else on screen.
        auto *frame = menu->property("contentItem").value<QQuickItem *>()->parentItem();
        const auto pointer = address->mapToScene({address->width() / 2, address->height() / 2});
        const auto rect = frame->mapRectToScene(frame->boundingRect());
        // (It may be pushed left to stay inside the window, but still covers the pointer.)
        QVERIFY2(pointer.x() >= rect.left() - 2 && pointer.x() <= rect.right() && qAbs(rect.top() - pointer.y()) < 40,
                 qPrintable(QString("menu at %1,%2; pointer %3,%4").arg(rect.left()).arg(rect.top()).arg(pointer.x()).arg(pointer.y())));
        // Right-clicking keeps the selection, so Copy is available.
        auto *copy = item("textMenuCopy");
        QVERIFY(copy);
        QVERIFY(copy->isEnabled());
        auto *selectAll = item("textMenuSelectAll");
        QVERIFY(selectAll);
        QCOMPARE(selectAll->property("text").toString(), QStringLiteral("全选"));
        click(selectAll);
        QTRY_VERIFY(!menu->property("visible").toBool());
        QCOMPARE(address->property("selectedText").toString(), address->property("text").toString());
        QTest::keyClick(window, Qt::Key_Escape);
        if (field == "searchField")
            address->setProperty("text", "");
    }

    void staticHoverEligibility() {
        QVERIFY(!controller->preview()->hoverEligible(
            {{"fileUri", uri("folder")}, {"folder", true}, {"fileName", "folder"}, {"iconName", "folder"}}));
        QVERIFY(!controller->preview()->hoverEligible({{"fileUri", uri("movie.mp4")},
                                                       {"folder", false},
                                                       {"fileName", "movie.mp4"},
                                                       {"iconName", "video"}}));
        // MIME metadata must still exclude video disguised with a static extension.
        QVERIFY(!controller->preview()->hoverEligible({{"fileUri", uri("movie.txt")},
                                                       {"folder", false},
                                                       {"fileName", "movie.txt"},
                                                       {"mimeType", "video/mp4"}}));
    }

    void selectAllFrames() {
        if (qEnvironmentVariableIsEmpty("NOOK_FRAME_DUMP"))
            QSKIP("diagnostic only");
        settings->set("animations", true);
        settings->set("theme", qEnvironmentVariable("NOOK_FRAME_THEME", "light"));
        for (int i = 0; i < 8; ++i) {
            QFile file(fixture->path() + QStringLiteral("/frame-%1.txt").arg(i));
            QVERIFY(file.open(QIODevice::WriteOnly));
        }
        controller->left()->refresh();
        QTRY_VERIFY(!controller->left()->loading());
        QTest::qWait(500);
        click(entry(uri("frame-0.txt")));
        QTest::qWait(400);
        const QString out = qEnvironmentVariable("NOOK_FRAME_DUMP");
        window->grabWindow().save(out + "/f-before.png");
        QTest::keyClick(window, Qt::Key_A, Qt::ControlModifier);
        QElapsedTimer t; t.start();
        for (int i = 0; i < 12; ++i) {
            window->grabWindow().save(out + QStringLiteral("/f-%1-%2ms.png").arg(i, 2, 10, QChar('0')).arg(t.elapsed()));
            QTest::qWait(16);
        }
        settings->set("animations", false);
    }
    // Frames of the list motions (view morph, re-sort reflow, entering a folder) for a look.
    void motionFrames() {
        if (qEnvironmentVariableIsEmpty("NOOK_FRAME_DUMP"))
            QSKIP("diagnostic only");
        settings->set("animations", true);
        settings->set("theme", qEnvironmentVariable("NOOK_FRAME_THEME", "light"));
        for (int i = 0; i < 10; ++i) {
            QFile file(fixture->path() + QStringLiteral("/motion-%1.txt").arg(i));
            QVERIFY(file.open(QIODevice::WriteOnly));
            file.write(QByteArray((i * 7919) % 10 * 1000, 'x'));
        }
        controller->left()->refresh();
        QTRY_VERIFY(!controller->left()->loading());
        QTest::qWait(600);
        const QString out = qEnvironmentVariable("NOOK_FRAME_DUMP");
        auto frames = [&](const QString &name, const std::function<void()> &act) {
            act();
            QElapsedTimer t;
            t.start();
            for (int i = 0; i < 10; ++i) {
                window->grabWindow().save(out + QStringLiteral("/%1-%2-%3ms.png").arg(name).arg(i, 2, 10, QChar('0')).arg(t.elapsed()));
                QTest::qWait(40);
            }
            QTest::qWait(500);
        };
        frames("morph-grid", [&] { controller->setGrid(true); });
        frames("morph-list", [&] { controller->setGrid(false); });
        frames("reflow", [&] { controller->left()->sortBy("size", true); });
        frames("preview-open", [&] { controller->preview()->open(uri("motion-3.txt")); });
        frames("preview-close", [&] { controller->preview()->close(); });
        frames("menu", [&] { click(entry(uri("motion-2.txt")), Qt::RightButton); });
        frames("menu-again", [&] { click(entry(uri("motion-6.txt")), Qt::RightButton); });
        QTest::keyClick(window, Qt::Key_Escape);
        frames("enter", [&] { controller->left()->navigate(uri("folder")); });
        settings->set("animations", false);
    }
    // Every settings page, for a look at the layout.
    void settingsShots() {
        if (qEnvironmentVariableIsEmpty("NOOK_FRAME_DUMP"))
            QSKIP("diagnostic only");
        const QString out = qEnvironmentVariable("NOOK_FRAME_DUMP");
        settings->set("theme", qEnvironmentVariable("NOOK_FRAME_THEME", "light"));
        auto *panel = object("settingsDialog");
        QVERIFY(panel);
        QMetaObject::invokeMethod(panel, "open");
        QTRY_VERIFY(panel->property("opened").toBool());
        for (int page = 0; page < 5; ++page) {
            panel->setProperty("category", page);
            QTest::qWait(250);
            window->grabWindow().save(out + QStringLiteral("/settings-%1.png").arg(page));
            auto *scroll = item("settingsScroll");
            auto *flick = scroll ? scroll->property("contentItem").value<QQuickItem *>() : nullptr;
            if (flick && flick->property("contentHeight").toReal() > flick->height() + 4) {
                flick->setProperty("contentY", flick->property("contentHeight").toReal() - flick->height());
                QTest::qWait(150);
                window->grabWindow().save(out + QStringLiteral("/settings-%1-end.png").arg(page));
            }
        }
        QMetaObject::invokeMethod(panel, "close");
    }
    void rubberBandSelection() {
        auto *list = item("fileList");
        QVERIFY(list);
        const QPoint start = list->mapToScene({300, list->height() - 40}).toPoint();
        const QPoint end = list->mapToScene({140, 8}).toPoint();
        QTest::mousePress(window, Qt::LeftButton, Qt::NoModifier, start);
        for (int i = 1; i <= 10; ++i)
            QTest::mouseMove(window, start + (end - start) * i / 10);
        QTRY_VERIFY(item("selectionBand") && item("selectionBand")->isVisible());
        QTest::mouseRelease(window, Qt::LeftButton, Qt::NoModifier, end);
        QTRY_COMPARE(controller->left()->selectedCount(), 2);
        QVERIFY(!item("selectionBand")->isVisible());
        // A band that only reaches the second row selects just that row.
        const QPoint second = list->mapToScene({140, 43 + 20}).toPoint();
        QTest::mousePress(window, Qt::LeftButton, Qt::NoModifier, start);
        for (int i = 1; i <= 10; ++i)
            QTest::mouseMove(window, start + (second - start) * i / 10);
        QTest::mouseRelease(window, Qt::LeftButton, Qt::NoModifier, second);
        QTRY_COMPARE(controller->left()->selectedCount(), 1);
        QCOMPARE(controller->left()->selectedUris(), QStringList{uri("sample.txt")});
        QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, start);
        QTRY_COMPARE(controller->left()->selectedCount(), 0);
    }
    void undoHoveredRowLeavesNoResidue_data() {
        QTest::addColumn<bool>("grid");
        QTest::addColumn<bool>("animations");
        QTest::addColumn<bool>("openTip");
        for (bool grid : {false, true})
            for (bool animations : {false, true})
                for (bool openTip : {false, true})
                    QTest::newRow(qPrintable(QString("%1-%2-%3").arg(grid ? "grid" : "list", animations ? "animated" : "instant", openTip ? "opened" : "pending")))
                        << grid << animations << openTip;
    }
    void undoHoveredRowLeavesNoResidue() {
        QFETCH(bool, grid);
        QFETCH(bool, animations);
        QFETCH(bool, openTip);
        settings->set("animations", animations);
        controller->setGrid(grid);
        operations->run("touch", {}, uri(""), "hover-undo.txt");
        QTRY_VERIFY(!operations->busy());
        QTRY_VERIFY(entry(uri("hover-undo.txt")));
        QTest::qWait(350);
        QPointer<QQuickItem> removedRow = entry(uri("hover-undo.txt"));
        click(removedRow);
        const auto pointer = removedRow->mapToScene({80, removedRow->height() / 2}).toPoint();
        QTest::mouseMove(window, pointer);
        auto *tip = removedRow->findChild<QObject *>("fileEntryToolTip");
        QVERIFY(tip);
        QPointer<QObject> removedTip = tip;
        if (openTip)
            QTRY_VERIFY_WITH_TIMEOUT(tip->property("opened").toBool(), 2500);
        else
            QVERIFY(!tip->property("opened").toBool());
        QCOMPARE(root->property("hoveredEntry").toMap().value("fileUri").toString(), uri("hover-undo.txt"));

        // Leave the cursor on the disappearing file throughout undo and its remove animation.
        operations->undo();
        QTRY_VERIFY(!operations->busy());
        QTRY_COMPARE(controller->left()->count(), 2);
        QVERIFY(!removedRow || !removedRow->property("live").toBool());
        QVERIFY(!removedTip || !removedTip->property("visible").toBool());
        QTest::qWait(400); // No mouse movement: retirement must cancel hover itself.
        QTest::mouseMove(window, pointer + QPoint(1, 0));
        QTest::qWait(1100); // Past the tooltip delay; moving over an old row cannot resurrect it.
        QVERIFY(!QFile::exists(fixture->path() + "/hover-undo.txt"));
        QVERIFY(!removedTip || !removedTip->property("visible").toBool());
        QVERIFY(root->property("hoveredEntry").toMap().value("fileUri").toString() != uri("hover-undo.txt"));
        // A pooled or transition-retained delegate must not be an interactive ghost row.
        QVERIFY(!removedRow || !removedRow->isVisible() || !removedRow->isEnabled() ||
                removedRow->property("fileUri").toString() != uri("hover-undo.txt"));
        for (auto *highlight : visualItems(left, "selectionHighlight"))
            QVERIFY(!highlight->property("shown").toBool());
        if (removedRow) {
            auto *spotlight = findVisual(removedRow, "fileEntrySpotlight");
            QVERIFY(spotlight && !spotlight->isVisible() && !spotlight->property("active").toBool());
        }

        // Reuse must restore normal input/hover rather than leaving future rows disabled.
        operations->run("touch", {}, uri(""), "hover-reused.txt");
        QTRY_VERIFY(!operations->busy());
        QTRY_VERIFY(entry(uri("hover-reused.txt")));
        QTest::qWait(350);
        auto *reused = entry(uri("hover-reused.txt"));
        QVERIFY(reused->isEnabled());
        click(reused);
        QTest::mouseMove(window, reused->mapToScene({80, reused->height() / 2}).toPoint());
        auto *reusedTip = reused->findChild<QObject *>("fileEntryToolTip");
        QVERIFY(reusedTip);
        QTRY_VERIFY_WITH_TIMEOUT(reusedTip->property("opened").toBool(), 2500);
        settings->set("animations", false);
    }
    void rowTransitionsSettle() {
        // With motion on, in-place refreshes animate rows in and out; pooled delegates must
        // never be left faded or shrunk once the transitions finish.
        settings->set("animations", true);
        for (int round = 0; round < 3; ++round) {
            for (int i = 0; i < 6; ++i) {
                QFile file(fixture->path() + QStringLiteral("/row-%1-%2.txt").arg(round).arg(i));
                QVERIFY(file.open(QIODevice::WriteOnly));
            }
            if (round > 0)
                for (int i = 0; i < 6; i += 2)
                    QVERIFY(QFile::remove(fixture->path() + QStringLiteral("/row-%1-%2.txt").arg(round - 1).arg(i)));
            controller->left()->refresh();
            QTRY_VERIFY(!controller->left()->loading());
            QTest::qWait(450);
        }
        QCOMPARE(controller->left()->count(), 2 + 6 + 3 + 3);
        auto *list = item("fileList");
        QVERIFY(list);
        int checked = 0;
        for (int i = 0; i < controller->left()->count(); ++i) {
            QQuickItem *row = nullptr;
            QVERIFY(QMetaObject::invokeMethod(list, "itemAtIndex", Q_RETURN_ARG(QQuickItem *, row), Q_ARG(int, i)));
            if (!row)
                continue; // outside the instantiated window
            ++checked;
            QVERIFY2(qFuzzyCompare(row->opacity(), 1.0), qPrintable(row->property("fileName").toString()));
            QCOMPARE(row->scale(), 1.0);
        }
        QVERIFY(checked >= 10);
        settings->set("animations", false);
    }
    void sidebarDragPersistsQuickOrder() {
        for (const auto &name : {"folder/alpha", "folder/beta"})
            QVERIFY(QDir().mkdir(fixture->path() + "/" + name));
        const QStringList quick{uri("folder/alpha"), uri("folder/beta")};
        settings->set("quickAccess", quick);
        // Put the test targets at the top irrespective of the user's standard folders.
        settings->set("quickAccessOrder", quick);
        auto *sidebar = item("sidebar");
        QVERIFY(sidebar);
        auto quickRow = [&](const QString &fileUri) -> QQuickItem * {
            for (auto *row : visualItems(sidebar, "quickPlace"))
                if (row->property("entry").toMap().value("uri").toString() == fileUri)
                    return row;
            return nullptr;
        };
        QTRY_VERIFY(quickRow(quick[0]) && quickRow(quick[1]));
        // Wait for read-only disk discovery, without mounting or activating any disk.
        QTRY_VERIFY(controller->findChildren<QProcess *>().isEmpty());
        const auto disks = placeUris(controller->places(), true);
        QVERIFY(!disks.isEmpty());
        QCOMPARE(visualItems(sidebar, "diskPlace").size(), disks.size());
        for (auto *row : visualItems(sidebar, "quickPlace"))
            QVERIFY(!row->property("entry").toMap().value("disk").toBool());
        for (auto *row : visualItems(sidebar, "diskPlace"))
            QVERIFY(row->property("entry").toMap().value("disk").toBool());
        auto *source = quickRow(quick[1]);
        auto *target = quickRow(quick[0]);
        const auto start = source->mapToScene({source->width() / 2, source->height() / 2}).toPoint();
        const auto end = target->mapToScene({target->width() / 2, target->height() / 2}).toPoint();
        QTest::mousePress(window, Qt::LeftButton, Qt::NoModifier, start);
        for (int step = 1; step <= 8; ++step)
            QTest::mouseMove(window, start + (end - start) * step / 8, 25);
        QTest::mouseRelease(window, Qt::LeftButton, Qt::NoModifier, end);
        QTRY_VERIFY(settings->values().value("quickAccessOrder").toStringList().indexOf(quick[1]) <
                    settings->values().value("quickAccessOrder").toStringList().indexOf(quick[0]));
        const auto order = placeUris(controller->places(), false);
        QCOMPARE(order.mid(0, 2), (QStringList{quick[1], quick[0]}));
        QCOMPARE(settings->values().value("quickAccess").toStringList(), quick);
        // Reordering refreshes places and starts asynchronous, read-only disk discovery again.
        QTRY_COMPARE(placeUris(controller->places(), true), disks);
        AppSettings reloaded;
        QCOMPARE(reloaded.values().value("quickAccessOrder").toStringList(), order);
        controller->refreshPlaces();
        QTRY_VERIFY(controller->findChildren<QProcess *>().isEmpty());
        QCOMPARE(placeUris(controller->places(), false), order);
        QCOMPARE(placeUris(controller->places(), true), disks);
    }

    void thumbnailFallbacks() {
        QFile malformed(fixture->path() + "/malformed.png");
        QVERIFY(malformed.open(QIODevice::WriteOnly));
        malformed.write("not a PNG\n");
        malformed.close();
        QImage transparent(32, 32, QImage::Format_ARGB32);
        transparent.fill(Qt::transparent);
        QVERIFY(transparent.save(fixture->path() + "/transparent.png"));
        controller->left()->refresh();
        QTRY_VERIFY(!controller->left()->loading());
        QTRY_COMPARE(controller->left()->count(), 4);
        controller->setGrid(true);
        QTRY_VERIFY(entry(uri("malformed.png")) && entry(uri("transparent.png")));
        for (const auto &name : {"malformed.png", "transparent.png"}) {
            auto *row = entry(uri(name));
            auto *fallback = findVisual(row, "gridFallbackIcon");
            auto *thumbnail = findVisual(row, "gridThumbnail");
            QVERIFY(fallback && thumbnail);
            QTRY_VERIFY(fallback->isVisible());
            QVERIFY(fallback->width() > 0 && fallback->height() > 0);
            QVERIFY(fallback->opacity() > 0);
            QVERIFY(!thumbnail->property("source").toUrl().isEmpty());
            // QtQuick Image.Loading == 2. Either decoded image or provider failure is allowed.
            QTRY_VERIFY_WITH_TIMEOUT(thumbnail->property("status").toInt() != 2, 15000);
            QVERIFY(fallback->isVisible());
        }
    }
};

int main(int argc, char **argv) {
    // Must precede QGuiApplication/QSettings: never read or overwrite personal preferences.
    QTemporaryDir env(QString(NOOK_BINARY_DIR) + "/followup-env-XXXXXX");
    if (!env.isValid())
        return 1;
    qputenv("XDG_CONFIG_HOME", QFile::encodeName(env.path() + "/config"));
    qputenv("XDG_DATA_HOME", QFile::encodeName(env.path() + "/data"));
    qputenv("XDG_CACHE_HOME", QFile::encodeName(env.path() + "/cache"));
    qputenv("GIO_USE_VFS", "local");
    // Do not inherit a portal platform theme that starts desktop services in the test session.
    if (qEnvironmentVariableIsEmpty("NOOK_REAL_THEME")) qputenv("QT_QPA_PLATFORMTHEME", "generic");
    QGuiApplication app(argc, argv);
    app.setApplicationName("nook-followup-UI-test");
    FollowupUiTest test;
    const int result = QTest::qExec(&test, argc, argv);
    QThreadPool::globalInstance()->waitForDone();
    return result;
}
#include "followup_ui_test.moc"
