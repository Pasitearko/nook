#include "application.h"
#include "archiveapp.h"
#include "archivedocument.h"
#include "archivemodel.h"
#include "passwordstore.h"
#include "taskqueue.h"
#include <QDir>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickWindow>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>
#include <archive.h>
#include <archive_entry.h>
#include <clocale>
#include <memory>

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

QVariantMap file(const QString &path, qint64 size, qint64 packed = -1, bool encrypted = false) {
    QVariantMap entry{{"path", path}, {"type", "file"}, {"size", double(size)}, {"mtime", 1700000000.0}};
    if (packed >= 0)
        entry.insert("packed", double(packed));
    if (encrypted)
        entry.insert("encrypted", true);
    return entry;
}

QVariantMap folder(const QString &path) {
    return {{"path", path}, {"type", "dir"}, {"size", 0.0}, {"mtime", 1700000000.0}};
}

QStringList names(const ArchiveModel &model) {
    QStringList result;
    for (int row = 0; row < model.count(); ++row)
        result << model.data(model.index(row), ArchiveModel::NameRole).toString();
    return result;
}

// A ZIP whose names are stored as the given bytes (no UTF-8 flag), optionally AES encrypted.
void writeZip(const QString &target, const QList<QPair<QByteArray, QByteArray>> &files, const QByteArray &password = {}) {
    const QByteArray locale = setlocale(LC_CTYPE, nullptr);
    setlocale(LC_CTYPE, "C");
    struct archive *a = archive_write_new();
    archive_write_set_format_zip(a);
    if (!password.isEmpty()) {
        archive_write_set_options(a, "zip:encryption=aes256");
        archive_write_set_passphrase(a, password.constData());
    }
    archive_write_open_filename(a, QFile::encodeName(target).constData());
    for (const auto &[name, data] : files) {
        archive_entry *entry = archive_entry_new();
        archive_entry_copy_pathname(entry, name.constData());
        archive_entry_set_filetype(entry, AE_IFREG);
        archive_entry_set_perm(entry, 0644);
        archive_entry_set_mtime(entry, 1700000000, 0);
        archive_entry_set_size(entry, data.size());
        archive_write_header(a, entry);
        archive_write_data(a, data.constData(), size_t(data.size()));
        archive_entry_free(entry);
    }
    archive_write_close(a);
    archive_write_free(a);
    setlocale(LC_CTYPE, locale.constData());
}

QString read(const QString &path) {
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? QString::fromUtf8(file.readAll()) : QString();
}

void put(const QString &path, const QByteArray &data) {
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write(data);
}

// The application as the tests use it: no keyring, no notifications.
struct TestApp {
    AppSettings settings;
    ArchiveApp app{&settings, [](const QString &) {}};
    TestApp() {
        app.passwords()->setMemoryOnly(true);
        app.setPref("notify", false);
        app.setPref("autoTryPasswords", false);
        app.setPref("concurrency", 1);
    }
    TaskQueue *queue() { return app.queue(); }
};

QString status(TaskQueue *queue, int id) { return queue->task(id).value("status").toString(); }

} // namespace

// nook解压缩's browsing side: the tree model, a document backed by the worker, and the window.
class ArchiveAppTest : public QObject {
    Q_OBJECT
    std::unique_ptr<QTemporaryDir> fixture;

  private Q_SLOTS:
    void initTestCase() {
        fixture = std::make_unique<QTemporaryDir>(QString(NOOK_BINARY_DIR) + "/archive-app-fixture-XXXXXX");
        QVERIFY(fixture->isValid());
        qputenv("XDG_CONFIG_HOME", QFile::encodeName(fixture->path() + "/config"));
        // The trash goes where the fixtures are (same filesystem), never the real one.
        qputenv("XDG_DATA_HOME", QFile::encodeName(fixture->path() + "/data"));
        QDir().mkpath(fixture->path() + "/data");
    }

    void modelTreeAndNavigation() {
        ArchiveModel model;
        // "docs" is never listed on its own; it still appears, with its size summed.
        model.reset({folder("src"), file("src/main.c", 100, 40), file("src/util/str.c", 50, 20),
                     file("docs/guide 10.md", 30, 10), file("docs/guide 2.md", 20, 10), file("README", 5, 5)},
                    "demo.zip");
        QCOMPARE(names(model), (QStringList{"docs", "src", "README"}));
        QCOMPARE(model.fileCount(), 5);
        QCOMPARE(model.folderCount(), 3);
        QCOMPARE(model.totalSize(), 205.0);
        QVERIFY(model.packedKnown());
        QCOMPARE(model.packedSize(), 85.0);
        QCOMPARE(model.data(model.index(1), ArchiveModel::BytesRole).toDouble(), 150.0);
        QVERIFY(!model.canUp());

        model.activate(0);
        QCOMPARE(model.folder(), QStringLiteral("docs"));
        // Numbers sort as numbers.
        QCOMPARE(names(model), (QStringList{"guide 2.md", "guide 10.md"}));
        QCOMPARE(model.breadcrumbs().size(), 2);
        model.up();
        QCOMPARE(model.folder(), QString());
        QCOMPARE(model.currentRow(), 0); // the folder just left is selected
        model.back();
        QCOMPARE(model.folder(), QStringLiteral("docs"));
        QVERIFY(model.canForward());
        model.forward();
        QCOMPARE(model.folder(), QString());

        model.openFolder("src/util");
        QCOMPARE(names(model), (QStringList{"str.c"}));
        model.openFolder("");
        model.sortBy("size", true);
        QCOMPARE(names(model), (QStringList{"src", "docs", "README"}));
        model.sortBy("name", false);

        // A search covers the whole package and shows where each match is.
        model.setQuery("guide");
        QCOMPARE(model.count(), 2);
        QCOMPARE(model.data(model.index(0), ArchiveModel::LocationRole).toString(), QStringLiteral("docs"));
        model.setQuery("");
        QCOMPARE(model.count(), 3);
    }

    void modelSelection() {
        ArchiveModel model;
        model.reset({file("a", 1), file("b", 1), file("c", 1), file("d", 1), file("nested.tar.gz", 1)}, "x.zip");
        model.click(1, 0);
        model.click(3, Qt::ShiftModifier);
        QCOMPARE(model.selectedPaths(), (QStringList{"b", "c", "d"}));
        model.click(2, Qt::ControlModifier);
        QCOMPARE(model.selectedPaths(), (QStringList{"b", "d"}));
        model.selectAll();
        QCOMPARE(model.selectedCount(), 5);
        model.clearSelection();
        QCOMPARE(model.selectedCount(), 0);
        // Packages inside the package get the package icon.
        QCOMPARE(model.data(model.index(4), ArchiveModel::IconRole).toString(), QStringLiteral("archive"));
    }

    void modelUnsafeNames() {
        ArchiveModel model;
        // The worker flags names that would climb out; they stay one row, never a folder chain.
        QVariantMap evil = file("../../evil", 1);
        evil.insert("unsafe", true);
        model.reset({evil, file("ok", 1)}, "x.zip");
        QCOMPARE(model.count(), 2);
        const int row = names(model).indexOf("../../evil");
        QVERIFY(row >= 0);
        QVERIFY(model.data(model.index(row), ArchiveModel::UnsafeRole).toBool());
        QCOMPARE(model.data(model.index(row), ArchiveModel::IconRole).toString(), QStringLiteral("file"));
    }

    void documentLegacyNames() {
        const QString zip = fixture->path() + "/legacy.zip";
        // "中文" in GBK, as Windows tools of the time wrote it.
        writeZip(zip, {{QByteArray("\xd6\xd0\xce\xc4/\xb1\xa8\xb8\xe6.txt"), QByteArray(4000, 'x')}});
        ArchiveDocument document;
        QSignalSpy opened(&document, &ArchiveDocument::opened);
        document.open(QUrl::fromLocalFile(zip).toString());
        QTRY_COMPARE_WITH_TIMEOUT(document.state(), QStringLiteral("ready"), 10000);
        QCOMPARE(opened.size(), 1);
        QVERIFY(document.legacyNames());
        QCOMPARE(document.formatLabel(), QStringLiteral("ZIP"));
        QVERIFY(!document.readOnly());
        QCOMPARE(names(*document.model()), (QStringList{"中文"}));
        document.model()->activate(0);
        QCOMPARE(names(*document.model()), (QStringList{"报告.txt"}));
        // ZIP entries carry their compressed size (from 7-Zip's listing) where 7-Zip is present.
        if (!QStandardPaths::findExecutable("7z").isEmpty())
            QVERIFY(document.model()->packedKnown());

        document.setEncoding("Big5");
        QTRY_COMPARE_WITH_TIMEOUT(document.state(), QStringLiteral("ready"), 10000);
        QVERIFY(!names(*document.model()).contains("中文"));
        document.setEncoding("auto");
        QTRY_COMPARE_WITH_TIMEOUT(document.state(), QStringLiteral("ready"), 10000);
        QCOMPARE(document.encoding(), QStringLiteral("GB18030"));
    }

    void documentMissingAndDamaged() {
        ArchiveDocument document;
        document.open(fixture->path() + "/nothing-here.zip");
        QTRY_COMPARE_WITH_TIMEOUT(document.state(), QStringLiteral("error"), 10000);
        QVERIFY(!document.error().isEmpty());
        const QString broken = fixture->path() + "/broken.zip";
        QFile out(broken);
        QVERIFY(out.open(QIODevice::WriteOnly));
        out.write("PK\x03\x04 definitely not a zip");
        out.close();
        document.open(broken);
        QTRY_COMPARE_WITH_TIMEOUT(document.state(), QStringLiteral("error"), 10000);
    }

    void windowBrowsesPackage() {
        const QString zip = fixture->path() + "/browse.zip";
        writeZip(zip, {{"top/inner.txt", "hello"}, {"top/deep/leaf.md", "leaf"}, {"readme.txt", "hi"}});
        AppSettings settings;
        ArchiveApp app(&settings, [](const QString &) {});
        ArchiveDocument document;
        QQmlEngine engine;
        QQmlComponent component(&engine, QUrl::fromLocalFile(QString(NOOK_SOURCE_DIR) + "/qml/ArchiveMain.qml"));
        QVERIFY2(component.isReady(), qPrintable(component.errorString()));
        std::unique_ptr<QObject> root(component.createWithInitialProperties(
            {{"app", QVariant::fromValue(&app)}, {"document", QVariant::fromValue(&document)}}));
        QVERIFY2(root, qPrintable(component.errorString()));
        auto *window = qobject_cast<QQuickWindow *>(root.get());
        QVERIFY(window);
        window->show();
        QVERIFY(QTest::qWaitForWindowExposed(window));

        auto *empty = findVisual(window->contentItem(), "archiveEmpty");
        QVERIFY(empty && empty->isVisible());
        document.open(zip);
        QTRY_COMPARE_WITH_TIMEOUT(document.state(), QStringLiteral("ready"), 10000);
        QTRY_VERIFY(!empty->isVisible());
        auto *list = findVisual(window->contentItem(), "archiveFileList");
        QVERIFY(list);
        QTRY_COMPARE(list->property("count").toInt(), 2);
        QVERIFY(findVisual(window->contentItem(), "archiveFormat")->property("text").toString().startsWith("ZIP"));

        // Enter opens the selected folder.
        document.model()->click(0, 0);
        list->forceActiveFocus();
        QTest::keyClick(window, Qt::Key_Return);
        QTRY_COMPARE(document.model()->folder(), QStringLiteral("top"));
        QTest::keyClick(window, Qt::Key_Up, Qt::AltModifier);
        QTRY_COMPARE(document.model()->folder(), QString());

        // The search box searches the whole package.
        auto *search = findVisual(window->contentItem(), "archiveSearch");
        QVERIFY(search);
        search->setProperty("text", "leaf");
        QTRY_COMPARE(document.model()->query(), QStringLiteral("leaf"));
        QTRY_COMPARE(list->property("count").toInt(), 1);
        QVERIFY(window->grabWindow().save(QString(NOOK_BINARY_DIR) + "/archive-app-search.png"));
        search->setProperty("text", "");
        QTRY_COMPARE(list->property("count").toInt(), 2);

        auto *summary = findVisual(window->contentItem(), "archiveSummary");
        QVERIFY(summary);
        QVERIFY(summary->property("text").toString().contains("3 个文件"));
        QVERIFY(window->grabWindow().save(QString(NOOK_BINARY_DIR) + "/archive-app-browse.png"));
    }

    void windowAsksForPassword() {
        const QString zip = fixture->path() + "/secret.zip";
        writeZip(zip, {{"plain.txt", "visible name, hidden data"}}, "s3cret");
        AppSettings settings;
        ArchiveApp app(&settings, [](const QString &) {});
        ArchiveDocument document;
        QQmlEngine engine;
        QQmlComponent component(&engine, QUrl::fromLocalFile(QString(NOOK_SOURCE_DIR) + "/qml/ArchiveMain.qml"));
        std::unique_ptr<QObject> root(component.createWithInitialProperties(
            {{"app", QVariant::fromValue(&app)}, {"document", QVariant::fromValue(&document)}}));
        QVERIFY2(root, qPrintable(component.errorString()));
        auto *window = qobject_cast<QQuickWindow *>(root.get());
        window->show();
        QVERIFY(QTest::qWaitForWindowExposed(window));
        document.open(zip);
        QTRY_COMPARE_WITH_TIMEOUT(document.state(), QStringLiteral("ready"), 10000);
        // ZIP names are not encrypted: the listing needs no password, but the entry is marked.
        QVERIFY(!document.passwordRequested());
        QVERIFY(document.model()->data(document.model()->index(0), ArchiveModel::EncryptedRole).toBool());
        QVERIFY(document.formatLabel().contains("已加密"));
    }

    // Phase 3: the task queue.
    void queueExtractAsksAboutExistingNames() {
        TestApp t;
        const QString zip = fixture->path() + "/two.zip";
        writeZip(zip, {{"a.txt", "new a"}, {"b.txt", "new b"}});
        const QString target = fixture->path() + "/conflict-target";
        put(target + "/a.txt", "old a");
        QSignalSpy attention(t.queue(), &TaskQueue::attention);
        const int id = t.queue()->start({{"op", "extract"}, {"archive", zip}, {"destination", target}, {"subfolder", "never"}, {"conflict", "ask"}}, {}, nullptr);
        QTRY_COMPARE_WITH_TIMEOUT(status(t.queue(), id), QStringLiteral("conflict"), 10000);
        QCOMPARE(attention.size(), 1);
        const QVariantMap conflict = t.queue()->task(id).value("conflict").toMap();
        QCOMPARE(conflict.value("path").toString(), QStringLiteral("a.txt"));
        QCOMPARE(t.queue()->attentionCount(), 1);
        t.queue()->resolveConflict(id, "rename", true);
        QTRY_COMPARE_WITH_TIMEOUT(status(t.queue(), id), QStringLiteral("done"), 10000);
        QCOMPARE(read(target + "/a.txt"), QStringLiteral("old a"));
        QCOMPARE(read(target + "/a (2).txt"), QStringLiteral("new a"));
        QCOMPARE(read(target + "/b.txt"), QStringLiteral("new b"));
        const QVariantMap task = t.queue()->task(id);
        QVERIFY(task.value("message").toString().startsWith("已解压 2 个项目"));
        QVERIFY(task.value("targetIsFolder").toBool());
        QCOMPARE(t.queue()->activeCount(), 0);
    }

    void queueSmartFolderAndDeleteCreated() {
        TestApp t;
        const QString zip = fixture->path() + "/smart.zip";
        writeZip(zip, {{"one.txt", "1"}, {"two.txt", "2"}});
        const QString target = fixture->path() + "/smart-target";
        QDir().mkpath(target);
        const int id = t.queue()->start({{"op", "extract"}, {"archive", zip}, {"destination", target}, {"subfolder", "smart"}}, {}, nullptr);
        QTRY_COMPARE_WITH_TIMEOUT(status(t.queue(), id), QStringLiteral("done"), 10000);
        // Two top-level items: they go into a folder named after the package.
        QCOMPARE(read(target + "/smart/one.txt"), QStringLiteral("1"));
        const QVariantMap task = t.queue()->task(id);
        QCOMPARE(task.value("target").toString(), target + "/smart");
        QVERIFY(!task.value("targetIsFolder").toBool());
        QVERIFY(task.value("canDeleteCreated").toBool());
        t.queue()->deleteCreated(id);
        QVERIFY(!QFileInfo::exists(target + "/smart"));
        QVERIFY(!t.queue()->task(id).value("canDeleteCreated").toBool());
        QVERIFY(t.queue()->task(id).value("message").toString().contains("回收站"));
    }

    void queuePasswordsSessionAndKeyring() {
        TestApp t;
        const QString zip = fixture->path() + "/locked.zip";
        writeZip(zip, {{"secret.txt", "classified"}}, "right");
        const QString target = fixture->path() + "/locked-target";
        QDir().mkpath(target);
        const int id = t.queue()->start({{"op", "extract"}, {"archive", zip}, {"destination", target}, {"subfolder", "never"}}, {}, nullptr);
        QTRY_COMPARE_WITH_TIMEOUT(status(t.queue(), id), QStringLiteral("password"), 10000);
        QVERIFY(!t.queue()->task(id).value("passwordWrong").toBool());
        t.queue()->submitPassword(id, "wrong", "");
        QTRY_COMPARE_WITH_TIMEOUT(status(t.queue(), id), QStringLiteral("password"), 10000);
        QTRY_VERIFY(t.queue()->task(id).value("passwordWrong").toBool());
        t.queue()->submitPassword(id, "right", "工作资料");
        QTRY_COMPARE_WITH_TIMEOUT(status(t.queue(), id), QStringLiteral("done"), 10000);
        QCOMPARE(read(target + "/secret.txt"), QStringLiteral("classified"));
        // Kept under the chosen name, and remembered for this session.
        QCOMPARE(t.app.passwords()->entries().size(), 1);
        QCOMPARE(t.app.passwords()->entries().first().toMap().value("name").toString(), QStringLiteral("工作资料"));
        QCOMPARE(t.queue()->sessionPassword(zip), QStringLiteral("right"));
        const int again = t.queue()->start({{"op", "test"}, {"archive", zip}}, {}, nullptr);
        QTRY_COMPARE_WITH_TIMEOUT(status(t.queue(), again), QStringLiteral("done"), 10000);
        QVERIFY(t.queue()->task(again).value("message").toString().startsWith("测试通过"));

        // Another package with the same password: tried silently once that is switched on.
        const QString other = fixture->path() + "/locked-too.zip";
        writeZip(other, {{"x.txt", "x"}}, "right");
        t.app.passwords()->save("别的", "nope");
        t.app.setPref("autoTryPasswords", true);
        QSignalSpy attention(t.queue(), &TaskQueue::attention);
        const int silent = t.queue()->start({{"op", "test"}, {"archive", other}}, {}, nullptr);
        QTRY_COMPARE_WITH_TIMEOUT(status(t.queue(), silent), QStringLiteral("done"), 10000);
        QCOMPARE(attention.size(), 0);
    }

    void queueOrderConcurrencyAndCancel() {
        TestApp t;
        const QString zip = fixture->path() + "/order.zip";
        writeZip(zip, {{"f.txt", QByteArray(1000, 'f')}});
        const int a = t.queue()->start({{"op", "test"}, {"archive", zip}}, {}, nullptr);
        const int b = t.queue()->start({{"op", "test"}, {"archive", zip}}, {}, nullptr);
        const int c = t.queue()->start({{"op", "test"}, {"archive", zip}}, {}, nullptr);
        QCOMPARE(status(t.queue(), a), QStringLiteral("running"));
        QCOMPARE(status(t.queue(), b), QStringLiteral("queued"));
        QVERIFY(t.queue()->task(c).value("canMoveUp").toBool());
        QVERIFY(!t.queue()->task(b).value("canMoveUp").toBool());
        t.queue()->move(c, -1);
        QCOMPARE(t.queue()->data(t.queue()->index(1), TaskQueue::IdRole).toInt(), c);
        t.queue()->cancel(b);
        QCOMPARE(status(t.queue(), b), QStringLiteral("cancelled"));
        QTRY_COMPARE_WITH_TIMEOUT(t.queue()->activeCount(), 0, 10000);
        QCOMPARE(status(t.queue(), c), QStringLiteral("done"));
        QCOMPARE(t.queue()->finishedCount(), 3);
        t.queue()->clearFinished();
        QCOMPARE(t.queue()->count(), 0);

        t.app.setPref("concurrency", 2);
        const int d = t.queue()->start({{"op", "test"}, {"archive", zip}}, {}, nullptr);
        const int e = t.queue()->start({{"op", "test"}, {"archive", zip}}, {}, nullptr);
        QCOMPARE(status(t.queue(), d), QStringLiteral("running"));
        QCOMPARE(status(t.queue(), e), QStringLiteral("running"));
        QTRY_COMPARE_WITH_TIMEOUT(t.queue()->activeCount(), 0, 10000);
    }

    void compressChoicesBecomeRequests() {
        TestApp t;
        const QString base = fixture->path() + "/compress";
        put(base + "/报告.pdf", "pdf");
        put(base + "/照片/1.jpg", "jpg");
        put(base + "/notes", "plain");
        ArchiveApp &app = t.app;
        QCOMPARE(app.compressDefaults({base + "/报告.pdf"}).value("name").toString(), QStringLiteral("报告"));
        QCOMPARE(app.compressDefaults({base + "/照片"}).value("name").toString(), QStringLiteral("照片"));
        QCOMPARE(app.compressDefaults({base + "/notes"}).value("name").toString(), QStringLiteral("notes"));
        QCOMPARE(app.compressDefaults({base + "/报告.pdf", base + "/照片"}).value("name").toString(), QStringLiteral("compress"));
        QVERIFY(app.compressDefaults({base + "/报告.pdf"}).value("single").toBool());
        QCOMPARE(app.levelFor("zip", 3), 5);
        QCOMPARE(app.levelFor("tar.zst", 5), 19);
        QCOMPARE(app.levelFor("tar.bz2", 0), 1);
        QCOMPARE(app.levelFor("tar", 3), -1);
        put(base + "/out.tar.gz", "x");
        QCOMPARE(app.freeName(base, "out.tar.gz"), QStringLiteral("out (2).tar.gz"));

        const QVariantMap choices{{"name", "合集"}, {"folder", base}, {"format", "7z"}, {"levelStep", 3},
                                  {"password", "pw"}, {"encryptNames", true}, {"volumeSize", 1048576.0}};
        const QVariantList one = app.compressRequests({base + "/报告.pdf", base + "/照片"}, choices);
        QCOMPARE(one.size(), 1);
        const QVariantMap request = one.first().toMap();
        QCOMPARE(request.value("output").toString(), base + "/合集.7z");
        QCOMPARE(request.value("password").toString(), QStringLiteral("pw"));
        QCOMPARE(request.value("options").toMap().value("level").toInt(), 5);
        QVERIFY(request.value("options").toMap().value("encryptNames").toBool());
        QCOMPARE(request.value("options").toMap().value("volumeSize").toDouble(), 1048576.0);

        QVariantMap separate = choices;
        separate.insert("format", "zip");
        separate.insert("separate", true);
        separate.insert("password", "");
        const QVariantList two = app.compressRequests({base + "/报告.pdf", base + "/照片"}, separate);
        QCOMPARE(two.size(), 2);
        QCOMPARE(two[0].toMap().value("output").toString(), base + "/报告.zip");
        QCOMPARE(two[1].toMap().value("output").toString(), base + "/照片.zip");
        QVERIFY(!two[0].toMap().contains("password"));
        QCOMPARE(two[0].toMap().value("options").toMap().value("zipEncryption").toString(), QStringLiteral("AES256"));

        // Single-file formats keep the whole file name: notes → notes.gz.
        QVariantMap stream = choices;
        stream.insert("format", "gz");
        stream.insert("name", "");
        QCOMPARE(app.compressRequests({base + "/notes"}, stream).first().toMap().value("output").toString(), base + "/notes.gz");
        QVERIFY(!app.compressRequests({base + "/notes"}, stream).first().toMap().contains("password"));

        // Run the separate ones, moving the sources to the trash afterwards.
        QList<int> ids;
        for (const auto &r : two)
            ids << t.queue()->start(r.toMap(), {{"deleteSources", true}}, nullptr);
        for (int id : ids)
            QTRY_COMPARE_WITH_TIMEOUT(status(t.queue(), id), QStringLiteral("done"), 15000);
        QVERIFY(QFileInfo(base + "/报告.zip").size() > 0);
        QVERIFY(QFileInfo(base + "/照片.zip").size() > 0);
        QVERIFY(!QFileInfo::exists(base + "/报告.pdf"));
        QVERIFY(!QFileInfo::exists(base + "/照片"));
        QVERIFY(t.queue()->task(ids.first()).value("message").toString().contains("回收站"));
    }

    void composeWindowCompressesAndCloses() {
        TestApp t;
        const QString base = fixture->path() + "/compose";
        put(base + "/a.txt", "aaa");
        put(base + "/b.txt", "bbb");
        QQmlEngine engine;
        QQmlComponent component(&engine, QUrl::fromLocalFile(QString(NOOK_SOURCE_DIR) + "/qml/ArchiveCompose.qml"));
        QVERIFY2(component.isReady(), qPrintable(component.errorString()));
        std::unique_ptr<QObject> root(component.createWithInitialProperties(
            {{"app", QVariant::fromValue(&t.app)}, {"items", QStringList{base + "/a.txt", base + "/b.txt"}}}));
        QVERIFY2(root, qPrintable(component.errorString()));
        auto *window = qobject_cast<QQuickWindow *>(root.get());
        QVERIFY(window);
        QVERIFY(QTest::qWaitForWindowExposed(window));
        auto *name = findVisual(window->contentItem(), "archiveComposeName");
        QVERIFY(name);
        QCOMPARE(name->property("text").toString(), QStringLiteral("compose"));
        name->setProperty("text", "两个文件");
        QVERIFY(window->grabWindow().save(QString(NOOK_BINARY_DIR) + "/archive-app-compose.png"));
        auto *start = findVisual(window->contentItem(), "archiveComposeStart");
        QVERIFY(start && start->isEnabled());
        QMetaObject::invokeMethod(start, "clicked");
        QTRY_COMPARE_WITH_TIMEOUT(t.queue()->count(), 1, 5000);
        QTRY_VERIFY_WITH_TIMEOUT(!window->isVisible(), 15000);
        QVERIFY(QFileInfo(base + "/两个文件.zip").size() > 0);
        QCOMPARE(status(t.queue(), t.queue()->data(t.queue()->index(0), TaskQueue::IdRole).toInt()), QStringLiteral("done"));
    }

    void mainWindowExtractsAndAsks() {
        TestApp t;
        const QString zip = fixture->path() + "/ui-extract.zip";
        writeZip(zip, {{"only/x.txt", "fresh"}});
        const QString target = fixture->path() + "/ui-target";
        put(target + "/only/x.txt", "stale");
        ArchiveDocument document;
        t.app.attach(&document);
        QQmlEngine engine;
        QQmlComponent component(&engine, QUrl::fromLocalFile(QString(NOOK_SOURCE_DIR) + "/qml/ArchiveMain.qml"));
        QVERIFY2(component.isReady(), qPrintable(component.errorString()));
        std::unique_ptr<QObject> root(component.createWithInitialProperties(
            {{"app", QVariant::fromValue(&t.app)}, {"document", QVariant::fromValue(&document)}}));
        QVERIFY2(root, qPrintable(component.errorString()));
        auto *window = qobject_cast<QQuickWindow *>(root.get());
        window->show();
        QVERIFY(QTest::qWaitForWindowExposed(window));
        document.open(zip);
        QTRY_COMPARE_WITH_TIMEOUT(document.state(), QStringLiteral("ready"), 10000);

        auto *button = findVisual(window->contentItem(), "archiveExtract");
        QVERIFY(button);
        QMetaObject::invokeMethod(button, "clicked");
        auto *destination = findVisual(window->contentItem(), "archiveExtractDestination");
        QTRY_VERIFY(destination && destination->isVisible());
        QCOMPARE(destination->property("text").toString(), fixture->path());
        destination->setProperty("text", target);
        QTest::qWait(400); // let the dialog finish appearing
        QVERIFY(window->grabWindow().save(QString(NOOK_BINARY_DIR) + "/archive-app-extract.png"));
        QMetaObject::invokeMethod(findVisual(window->contentItem(), "archiveExtractStart"), "clicked");
        QTRY_COMPARE_WITH_TIMEOUT(t.queue()->count(), 1, 5000);
        const int id = t.queue()->data(t.queue()->index(0), TaskQueue::IdRole).toInt();
        // The existing file is asked about in this window.
        QTRY_COMPARE_WITH_TIMEOUT(status(t.queue(), id), QStringLiteral("conflict"), 10000);
        QQuickItem *overwrite = nullptr;
        QTRY_VERIFY((overwrite = findVisual(window->contentItem(), "archiveConflictOverwrite")) && overwrite->isVisible());
        QTest::qWait(400);
        QVERIFY(window->grabWindow().save(QString(NOOK_BINARY_DIR) + "/archive-app-conflict.png"));
        QMetaObject::invokeMethod(overwrite, "clicked");
        QTRY_COMPARE_WITH_TIMEOUT(status(t.queue(), id), QStringLiteral("done"), 10000);
        QCOMPARE(read(target + "/only/x.txt"), QStringLiteral("fresh"));
        auto *drawer = findVisual(window->contentItem(), "archiveTaskCard");
        QTRY_VERIFY(drawer && drawer->isVisible());
        QTest::qWait(300);
        QVERIFY(window->grabWindow().save(QString(NOOK_BINARY_DIR) + "/archive-app-tasks.png"));
        // The rule chosen in the dialog is remembered.
        QCOMPARE(t.app.prefs().value("subfolder").toString(), QStringLiteral("smart"));
    }

    void orphanTaskGetsItsOwnWindow() {
        TestApp t;
        QQmlEngine engine;
        std::vector<std::unique_ptr<QObject>> windows;
        QStringList opened;
        t.app.setWindowFactory([&](const QString &type, const QVariantMap &properties) -> QObject * {
            opened << type;
            QVariantMap initial = properties;
            initial.insert("app", QVariant::fromValue(&t.app));
            QQmlComponent component(&engine, QUrl::fromLocalFile(QString(NOOK_SOURCE_DIR) + "/qml/" + type + ".qml"));
            if (!component.isReady())
                qWarning() << component.errorString();
            windows.emplace_back(component.createWithInitialProperties(initial));
            return windows.back().get();
        });
        const QString zip = fixture->path() + "/orphan.zip";
        writeZip(zip, {{"o.txt", "new"}});
        const QString target = fixture->path() + "/orphan-target";
        put(target + "/o.txt", "old");
        // Started by no window (as from nook's menu, or after its window closed).
        const int id = t.queue()->start({{"op", "extract"}, {"archive", zip}, {"destination", target}, {"subfolder", "never"}}, {}, nullptr);
        QTRY_COMPARE_WITH_TIMEOUT(opened, QStringList{"ArchiveTaskWindow"}, 10000);
        auto *window = qobject_cast<QQuickWindow *>(windows.back().get());
        QVERIFY(window);
        QVERIFY(QTest::qWaitForWindowExposed(window));
        QVERIFY(t.queue()->ownedBy(id, window));
        QQuickItem *skip = nullptr;
        QTRY_VERIFY((skip = findVisual(window->contentItem(), "archiveConflictSkip")) && skip->isVisible());
        QTest::qWait(300);
        QVERIFY(window->grabWindow().save(QString(NOOK_BINARY_DIR) + "/archive-app-task-window.png"));
        QMetaObject::invokeMethod(skip, "clicked");
        QTRY_COMPARE_WITH_TIMEOUT(status(t.queue(), id), QStringLiteral("done"), 10000);
        QCOMPARE(read(target + "/o.txt"), QStringLiteral("old"));
        // Done: the little window goes away by itself.
        QTRY_VERIFY_WITH_TIMEOUT(!window->isVisible(), 5000);
    }

    void settingsManageSavedPasswords() {
        TestApp t;
        t.app.passwords()->save("网盘", "abc123");
        ArchiveDocument document;
        QQmlEngine engine;
        QQmlComponent component(&engine, QUrl::fromLocalFile(QString(NOOK_SOURCE_DIR) + "/qml/ArchiveMain.qml"));
        std::unique_ptr<QObject> root(component.createWithInitialProperties(
            {{"app", QVariant::fromValue(&t.app)}, {"document", QVariant::fromValue(&document)}}));
        QVERIFY2(root, qPrintable(component.errorString()));
        auto *window = qobject_cast<QQuickWindow *>(root.get());
        window->show();
        QVERIFY(QTest::qWaitForWindowExposed(window));
        QMetaObject::invokeMethod(findVisual(window->contentItem(), "archiveSettingsButton"), "clicked");
        QQuickItem *name = nullptr;
        QTRY_VERIFY((name = findVisual(window->contentItem(), "archiveNewPasswordName")) && name->isVisible());
        name->setProperty("text", "公司");
        findVisual(window->contentItem(), "archiveNewPassword")->setProperty("text", "s3cret");
        QMetaObject::invokeMethod(findVisual(window->contentItem(), "archiveAddPassword"), "clicked");
        QCOMPARE(t.app.passwords()->entries().size(), 2);
        QCOMPARE(t.app.passwords()->entries().first().toMap().value("name").toString(), QStringLiteral("公司"));
        QTest::qWait(400);
        QVERIFY(window->grabWindow().save(QString(NOOK_BINARY_DIR) + "/archive-app-settings.png"));
        t.app.passwords()->remove("网盘");
        QCOMPARE(t.app.passwords()->entries().size(), 1);
    }
};

QTEST_MAIN(ArchiveAppTest)
#include "archive_app_test.moc"
