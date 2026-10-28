#include "application.h"
#include "filechooser.h"
#include "preview.h"
#include <QDBusMetaType>
#include <QGuiApplication>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickWindow>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QThreadPool>
#include <QtTest>
#include <memory>

using nook::portal::ChooserRequest;
using nook::portal::Choice;
using nook::portal::ChoiceOption;
using nook::portal::Filter;
using nook::portal::FilterRule;

namespace {
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
QByteArray pathBytes(const QString &path) {
    return QFile::encodeName(path) + '\0';
}
} // namespace

// The portal's file chooser: what applications ask for, and what the dialog answers.
class ChooserTest : public QObject {
    Q_OBJECT
  private:
    // Declared in lifetime order: QML is destroyed before its controller and fixtures.
    std::unique_ptr<QTemporaryDir> fixture;
    std::unique_ptr<AppSettings> settings;
    std::unique_ptr<SearchIndex> search;
    std::unique_ptr<Operations> operations;
    std::unique_ptr<Application> controller;
    std::unique_ptr<ChooserRequest> request;
    std::unique_ptr<QQmlEngine> engine;
    std::unique_ptr<QObject> root;
    QQuickWindow *window = nullptr;

    QString path(const QString &name) const {
        return fixture->path() + "/" + name;
    }
    QString uri(const QString &name) const {
        return nook::normalizeUri(path(name));
    }
    QVariantMap options(QVariantMap extra = {}) const {
        extra.insert("current_folder", pathBytes(fixture->path()));
        return extra;
    }
    QList<Filter> textFilters() const {
        return {Filter{"文本文档", {FilterRule{0, "*.md"}, FilterRule{0, "*.txt"}}},
                Filter{"图片", {FilterRule{1, "image/*"}}}};
    }
    void open(const QString &mode, const QVariantMap &opts) {
        request = std::make_unique<ChooserRequest>(mode, "test.app", QString(), opts, QString());
        controller = std::make_unique<Application>(search.get(), operations.get(), settings.get(), false);
        controller->setRemembersSession(false);
        engine = std::make_unique<QQmlEngine>();
        engine->addImageProvider("thumb", new ThumbnailProvider);
        QQmlComponent component(engine.get(), QUrl::fromLocalFile(QString(NOOK_SOURCE_DIR) + "/qml/FileChooser.qml"));
        QVERIFY2(component.isReady(), qPrintable(component.errorString()));
        root.reset(component.createWithInitialProperties(
            {{"controller", QVariant::fromValue(controller.get())}, {"request", QVariant::fromValue(request.get())}}));
        QVERIFY2(root, qPrintable(component.errorString()));
        window = qobject_cast<QQuickWindow *>(root.get());
        QVERIFY(window);
        QVERIFY(QTest::qWaitForWindowExposed(window));
        QTRY_COMPARE(controller->left()->uri(), uri({}));
        QTRY_VERIFY(!controller->left()->loading());
    }
    QQuickItem *item(const QString &name) const {
        return findVisual(window->contentItem(), name);
    }
    QStringList listed() const {
        QStringList names;
        auto *model = controller->left();
        for (int row = 0; row < model->count(); ++row)
            names << model->entry(row).value("fileName").toString();
        return names;
    }
    void accept() {
        QMetaObject::invokeMethod(root.get(), "accept");
    }

  private Q_SLOTS:
    void init() {
        fixture = std::make_unique<QTemporaryDir>(QString(NOOK_BINARY_DIR) + "/chooser-fixture-XXXXXX");
        QVERIFY(fixture->isValid());
        for (const auto &name : {"notes.md", "todo.txt", "photo.png", "report.pdf"}) {
            QFile file(path(name));
            QVERIFY(file.open(QIODevice::WriteOnly));
            file.write("x");
        }
        QVERIFY(QDir().mkdir(path("folder")));
        settings = std::make_unique<AppSettings>();
        settings->store().clear();
        settings->set("startFolder", QDir::homePath());
        settings->set("roots", QStringList{fixture->path()});
        settings->set("theme", "dark");
        settings->set("animations", false);
        settings->set("hoverPreview", false);
        settings->set("uiScale", 1.0);
        search = std::make_unique<SearchIndex>();
        operations = std::make_unique<Operations>();
    }
    void cleanup() {
        root.reset();
        engine.reset();
        request.reset();
        controller.reset();
        operations.reset();
        search.reset();
        settings.reset();
        QThreadPool::globalInstance()->waitForDone();
        fixture.reset();
    }

    // The shapes xdg-desktop-portal sends and expects back.
    void wireSignatures() {
        QCOMPARE(QByteArray(QDBusMetaType::typeToSignature(QMetaType::fromType<QList<Filter>>())), QByteArray("a(sa(us))"));
        QCOMPARE(QByteArray(QDBusMetaType::typeToSignature(QMetaType::fromType<Filter>())), QByteArray("(sa(us))"));
        QCOMPARE(QByteArray(QDBusMetaType::typeToSignature(QMetaType::fromType<QList<Choice>>())), QByteArray("a(ssa(ss)s)"));
        QCOMPARE(QByteArray(QDBusMetaType::typeToSignature(QMetaType::fromType<QList<nook::portal::ChoiceValue>>())), QByteArray("a(ss)"));
    }
    void readsTheRequest() {
        const QVariantMap opts{
            {"accept_label", "_Upload"},
            {"multiple", true},
            {"filters", QVariant::fromValue(textFilters())},
            {"current_filter", QVariant::fromValue(Filter{"图片", {FilterRule{1, "image/*"}}})},
            {"choices", QVariant::fromValue(QList<Choice>{
                            Choice{"enc", "编码", {ChoiceOption{"utf8", "UTF-8"}, ChoiceOption{"gbk", "GBK"}}, "gbk"}})},
            {"current_file", pathBytes(path("todo.txt"))}};
        ChooserRequest open("open", "app", "标题", opts, QString());
        QCOMPARE(open.property("acceptLabel").toString(), QString("Upload"));
        QVERIFY(open.property("multiple").toBool());
        QCOMPARE(open.property("currentFilter").toInt(), 1);
        const auto filters = open.filters();
        QCOMPARE(filters.size(), 2);
        QCOMPARE(filters[0].toMap().value("label").toString(), QString("文本文档 (*.md;*.txt)"));
        QCOMPARE(filters[0].toMap().value("extension").toString(), QString("md"));
        QCOMPARE(filters[1].toMap().value("label").toString(), QString("图片"));
        QCOMPARE(open.choices().value(0).toMap().value("initial").toString(), QString("gbk"));
        // Only SaveFile names a file; its folder still opens.
        QCOMPARE(open.property("currentFolder").toString(), uri({}));

        ChooserRequest save("save", "app", {}, {{"current_file", pathBytes(path("todo.txt"))}}, QString());
        QCOMPARE(save.property("currentName").toString(), QString("todo.txt"));
        QCOMPARE(save.resolve(save.property("currentFolder").toString(), "a/b.txt"), uri("a/b.txt"));
        QVERIFY(save.isFolder(uri("folder")));
        QVERIFY(save.exists(uri("todo.txt")));

        ChooserRequest files("saveFiles", "app", {}, {{"files", QVariant::fromValue(QList<QByteArray>{
                                                             pathBytes("/elsewhere/todo.txt"), pathBytes("/x/new.txt")})}},
                             QString());
        QCOMPARE(files.targetsIn(uri({})), (QStringList{uri("todo (1).txt"), uri("new.txt")}));

        QSignalSpy finished(&save, &ChooserRequest::finished);
        save.cancel();
        save.cancel();
        QCOMPARE(finished.size(), 1);
        QCOMPARE(finished[0][0].toUInt(), 1u);
    }

    void openFiltersAndPicksFiles() {
        open("open", options({{"multiple", true}, {"filters", QVariant::fromValue(textFilters())}}));
        QTRY_COMPARE(listed(), (QStringList{"folder", "notes.md", "todo.txt"}));
        QSignalSpy finished(request.get(), &ChooserRequest::finished);
        controller->left()->selectRows({1, 2});
        auto *name = item("chooserName");
        QVERIFY(name);
        QCOMPARE(name->property("text").toString(), QString("\"notes.md\" \"todo.txt\""));
        // The type list switches what is shown.
        root->setProperty("filterIndex", 1);
        QTRY_COMPARE(listed(), (QStringList{"folder", "photo.png"}));
        root->setProperty("filterIndex", 0);
        QTRY_COMPARE(listed().size(), 3);
        controller->left()->selectRows({1, 2});
        accept();
        QCOMPARE(finished.size(), 1);
        QCOMPARE(finished[0][0].toUInt(), 0u);
        const auto results = finished[0][1].toMap();
        QCOMPARE(results.value("uris").toStringList(), (QStringList{uri("notes.md"), uri("todo.txt")}));
        QCOMPARE(results.value("current_filter").value<Filter>().name, QString("文本文档"));
    }

    void openTypedName() {
        open("open", options());
        QSignalSpy finished(request.get(), &ChooserRequest::finished);
        auto *name = item("chooserName");
        QVERIFY(name);
        // A folder's name enters it; a file's name opens it.
        name->setProperty("text", "folder");
        accept();
        QTRY_COMPARE(controller->left()->uri(), uri("folder"));
        QCOMPARE(finished.size(), 0);
        name->setProperty("text", "../report.pdf");
        accept();
        QCOMPARE(finished.size(), 1);
        QCOMPARE(finished[0][1].toMap().value("uris").toStringList(), QStringList{uri("report.pdf")});
    }

    void saveAddsExtensionAndConfirmsOverwrite() {
        open("save", options({{"current_name", "draft"}, {"filters", QVariant::fromValue(textFilters())}}));
        QSignalSpy finished(request.get(), &ChooserRequest::finished);
        auto *name = item("chooserName");
        QVERIFY(name);
        QCOMPARE(name->property("text").toString(), QString("draft"));
        accept();
        QCOMPARE(finished.size(), 1);
        QCOMPARE(finished[0][1].toMap().value("uris").toStringList(), QStringList{uri("draft.md")});
    }

    void saveTypeChangesExtension() {
        open("save", options({{"current_name", "notes.md"},
                              {"filters", QVariant::fromValue(QList<Filter>{
                                              Filter{"Markdown", {FilterRule{0, "*.md"}}},
                                              Filter{"文本", {FilterRule{0, "*.txt"}}}})}}));
        QSignalSpy finished(request.get(), &ChooserRequest::finished);
        auto *name = item("chooserName");
        QVERIFY(name);
        root->setProperty("filterIndex", 1);
        QCOMPARE(name->property("text").toString(), QString("notes.txt"));
        root->setProperty("filterIndex", 0);
        QCOMPARE(name->property("text").toString(), QString("notes.md"));
        // notes.md exists: nothing is answered until the replacement is confirmed.
        accept();
        QCOMPARE(finished.size(), 0);
        auto *dialog = root->findChild<QObject *>("chooserOverwriteDialog");
        QVERIFY(dialog);
        QTRY_VERIFY(dialog->property("opened").toBool());
        auto *replace = item("chooserReplace");
        if (!replace)
            replace = findVisual(qobject_cast<QQuickItem *>(dialog->property("contentItem").value<QObject *>()),
                                 "chooserReplace");
        QVERIFY(replace);
        QMetaObject::invokeMethod(replace, "clicked");
        QCOMPARE(finished.size(), 1);
        QCOMPARE(finished[0][1].toMap().value("uris").toStringList(), QStringList{uri("notes.md")});
        QCOMPARE(finished[0][1].toMap().value("current_filter").value<Filter>().name, QString("Markdown"));
    }

    void foldersAndChoices() {
        open("open", options({{"directory", true},
                              {"choices", QVariant::fromValue(QList<Choice>{Choice{"ro", "只读", {}, "false"}})}}));
        QTRY_COMPARE(listed(), QStringList{"folder"});
        QSignalSpy finished(request.get(), &ChooserRequest::finished);
        // Nothing selected: the folder shown is the answer.
        accept();
        QCOMPARE(finished.size(), 1);
        const auto results = finished[0][1].toMap();
        QCOMPARE(results.value("uris").toStringList(), QStringList{uri({})});
        const auto choices = results.value("choices").value<QList<nook::portal::ChoiceValue>>();
        QCOMPARE(choices.size(), 1);
        QCOMPARE(choices[0].value, QString("false"));
    }

    void escapeCancels() {
        open("open", options());
        QSignalSpy finished(request.get(), &ChooserRequest::finished);
        window->requestActivate();
        QTest::keyClick(window, Qt::Key_Escape);
        QTRY_COMPARE(finished.size(), 1);
        QCOMPARE(finished[0][0].toUInt(), 1u);
    }
};

int main(int argc, char **argv) {
    // Must precede QGuiApplication/QSettings: never read or overwrite personal preferences.
    QTemporaryDir env(QString(NOOK_BINARY_DIR) + "/chooser-env-XXXXXX");
    if (!env.isValid())
        return 1;
    qputenv("XDG_CONFIG_HOME", QFile::encodeName(env.path() + "/config"));
    qputenv("XDG_DATA_HOME", QFile::encodeName(env.path() + "/data"));
    qputenv("XDG_CACHE_HOME", QFile::encodeName(env.path() + "/cache"));
    qputenv("GIO_USE_VFS", "local");
    qputenv("QT_QPA_PLATFORMTHEME", "generic");
    QGuiApplication app(argc, argv);
    app.setApplicationName("nook-chooser-test");
    nook::portal::registerTypes();
    ChooserTest test;
    const int result = QTest::qExec(&test, argc, argv);
    QThreadPool::globalInstance()->waitForDone();
    return result;
}
#include "chooser_test.moc"
