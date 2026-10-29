#include "application.h"
#include "archiveapp.h"
#include "archivedocument.h"
#include "archivemodel.h"
#include "filesessions.h"
#include "passwordstore.h"
#include "taskqueue.h"
#include <QClipboard>
#include <QDir>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickWindow>
#include <QRandomGenerator>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>
#include <archive.h>
#include <archive_entry.h>
#include <clocale>
#include <memory>

namespace {

std::unique_ptr<QTemporaryDir> environment;

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

// What a package holds under a name (read with libarchive, so independent of the worker).
QByteArray entryData(const QString &archivePath, const QString &name) {
    struct archive *a = archive_read_new();
    archive_read_support_format_all(a);
    archive_read_support_filter_all(a);
    QByteArray data;
    if (archive_read_open_filename(a, QFile::encodeName(archivePath).constData(), 65536) == ARCHIVE_OK) {
        archive_entry *entry;
        while (archive_read_next_header(a, &entry) == ARCHIVE_OK) {
            const char *path = archive_entry_pathname_utf8(entry);
            if (path && QString::fromUtf8(path) == name) {
                char buffer[8192];
                la_ssize_t got;
                while ((got = archive_read_data(a, buffer, sizeof buffer)) > 0)
                    data.append(buffer, got);
                break;
            }
        }
    }
    archive_read_free(a);
    return data;
}

// A main window on document, shown.
std::unique_ptr<QObject> mainWindow(QQmlEngine &engine, ArchiveApp &app, ArchiveDocument &document) {
    QQmlComponent component(&engine, QUrl::fromLocalFile(QString(NOOK_SOURCE_DIR) + "/qml/ArchiveMain.qml"));
    if (!component.isReady())
        qWarning() << component.errorString();
    std::unique_ptr<QObject> root(component.createWithInitialProperties(
        {{"app", QVariant::fromValue(&app)}, {"document", QVariant::fromValue(&document)}}));
    if (auto *window = qobject_cast<QQuickWindow *>(root.get()))
        window->show();
    return root;
}

QQuickItem *shown(QQuickWindow *window, const QString &name) {
    QQuickItem *item = findVisual(window->contentItem(), name);
    return item && item->isVisible() ? item : nullptr;
}

} // namespace

// nook解压缩's browsing side: the tree model, a document backed by the worker, and the window.
class ArchiveAppTest : public QObject {
    Q_OBJECT
    std::unique_ptr<QTemporaryDir> fixture;

  private Q_SLOTS:
    void initTestCase() {
        fixture = std::move(environment);
        QVERIFY(fixture->isValid());
        qputenv("XDG_CONFIG_HOME", QFile::encodeName(fixture->path() + "/config"));
        // The trash goes where the fixtures are (same filesystem), never the real one.
        qputenv("XDG_DATA_HOME", QFile::encodeName(fixture->path() + "/data"));
        QDir().mkpath(fixture->path() + "/data");
        // Files opened from packages are copied here.
        qputenv("XDG_CACHE_HOME", QFile::encodeName(fixture->path() + "/cache"));
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
        // Written back, the names would change encoding: converting to a UTF-8 ZIP comes first.
        QVERIFY(document.readOnly());
        QVERIFY(document.readOnlyReason().contains("UTF-8"));
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
        QQmlComponent component(&engine,
                                QUrl::fromLocalFile(QString(NOOK_SOURCE_DIR) + "/qml/ArchiveMain.qml"));
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
        QQmlComponent component(&engine,
                                QUrl::fromLocalFile(QString(NOOK_SOURCE_DIR) + "/qml/ArchiveMain.qml"));
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
        const int id = t.queue()->start(
            {{"op", "extract"}, {"archive", zip}, {"destination", target}, {"subfolder", "never"}}, {},
            nullptr);
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
        QQmlComponent component(&engine,
                                QUrl::fromLocalFile(QString(NOOK_SOURCE_DIR) + "/qml/ArchiveCompose.qml"));
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
        QQmlComponent component(&engine,
                                QUrl::fromLocalFile(QString(NOOK_SOURCE_DIR) + "/qml/ArchiveMain.qml"));
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
            QQmlComponent component(&engine,
                                    QUrl::fromLocalFile(QString(NOOK_SOURCE_DIR) + "/qml/" + type + ".qml"));
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
        const int id = t.queue()->start(
            {{"op", "extract"}, {"archive", zip}, {"destination", target}, {"subfolder", "never"}}, {},
            nullptr);
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

    void defaultAssociationUiIsExplicitAndRestorable() {
        TestApp t;
        t.app.setPref("defaultBannerDismissed", false);
        t.app.clearRecent();
        ArchiveDocument document;
        QQmlEngine engine;
        auto root = mainWindow(engine, t.app, document);
        auto *window = qobject_cast<QQuickWindow *>(root.get());
        QVERIFY(window);
        QVERIFY(QTest::qWaitForWindowExposed(window));
        QTRY_VERIFY(shown(window, "archiveDefaultBanner"));
        auto *defaults = t.app.defaults();
        QCOMPARE(defaults->defaultFor("application/zip"), QStringLiteral("archive-test-other.desktop"));
        const QString list = defaults->mimeappsPath();
        const QString original = read(list);
        QMetaObject::invokeMethod(shown(window, "archiveDefaultBannerSet"), "clicked");
        QTRY_VERIFY(shown(window, "archiveDefaultConfirm"));
        QCOMPARE(read(list), original); // opening and cancelling never takes over
        QMetaObject::invokeMethod(shown(window, "archiveDefaultCancel"), "clicked");
        QTRY_VERIFY(!shown(window, "archiveDefaultConfirm"));
        QCOMPARE(read(list), original);
        QMetaObject::invokeMethod(shown(window, "archiveDefaultBannerSet"), "clicked");
        QTRY_VERIFY(shown(window, "archiveDefaultConfirm"));
        QCOMPARE(findVisual(window->contentItem(), "archiveDefaultTypes")->property("count").toInt(),
                 defaults->typeCount());
        QTest::qWait(250);
        QVERIFY(window->grabWindow().save(QString(NOOK_BINARY_DIR) + "/archive-app-default-confirm.png"));
        QMetaObject::invokeMethod(shown(window, "archiveDefaultConfirm"), "clicked");
        QTRY_COMPARE(defaults->state(), QStringLiteral("all"));
        QTRY_VERIFY(!shown(window, "archiveDefaultBanner"));
        QVERIFY(defaults->canRestore());
        QMetaObject::invokeMethod(findVisual(window->contentItem(), "archiveSettingsButton"), "clicked");
        QTRY_VERIFY(shown(window, "archiveRestoreDefaults"));
        QVERIFY(!findVisual(window->contentItem(), "archiveSetDefault")->isEnabled());
        QTest::qWait(250);
        QVERIFY(window->grabWindow().save(QString(NOOK_BINARY_DIR) + "/archive-app-default-settings.png"));
        QMetaObject::invokeMethod(shown(window, "archiveRestoreDefaults"), "clicked");
        QTRY_VERIFY(shown(window, "archiveDefaultConfirm"));
        QCOMPARE(defaults->state(), QStringLiteral("all")); // restore also asks first
        QMetaObject::invokeMethod(shown(window, "archiveDefaultConfirm"), "clicked");
        QTRY_VERIFY(!defaults->canRestore());
        QCOMPARE(defaults->defaultFor("application/zip"), QStringLiteral("archive-test-other.desktop"));
        QTRY_VERIFY(shown(window, "archiveDefaultBanner"));

        // The close button hides only this window; "don't ask" is persisted separately.
        QMetaObject::invokeMethod(shown(window, "archiveDefaultBannerClose"), "clicked");
        QTRY_VERIFY(!shown(window, "archiveDefaultBanner"));
        root.reset();
        root = mainWindow(engine, t.app, document);
        window = qobject_cast<QQuickWindow *>(root.get());
        QVERIFY(window);
        QTRY_VERIFY(shown(window, "archiveDefaultBanner"));
        auto *dismiss = shown(window, "archiveDefaultDismiss");
        QVERIFY(dismiss);
        dismiss->setProperty("checked", true);
        QMetaObject::invokeMethod(dismiss, "toggled");
        QTRY_VERIFY(!shown(window, "archiveDefaultBanner"));
        QCOMPARE(t.app.prefs().value("defaultBannerDismissed").toBool(), true);
        // The permanent entry remains accessible even when the banner is dismissed.
        QMetaObject::invokeMethod(findVisual(window->contentItem(), "archiveSettingsButton"), "clicked");
        QTRY_VERIFY(shown(window, "archiveSetDefault"));
        QVERIFY(shown(window, "archiveSetDefault")->isEnabled());
        t.app.setPref("defaultBannerDismissed", false);
    }

    void defaultBannerOnlyInEmptyWindow() {
        TestApp t;
        t.app.setPref("defaultBannerDismissed", false);
        ArchiveDocument document;
        QQmlEngine engine;
        auto root = mainWindow(engine, t.app, document);
        auto *window = qobject_cast<QQuickWindow *>(root.get());
        QVERIFY(window);
        QTRY_VERIFY(shown(window, "archiveDefaultBanner"));
        const QString zip = fixture->path() + "/banner.zip";
        writeZip(zip, {{"hello.txt", "hello"}});
        document.open(zip);
        QTRY_COMPARE_WITH_TIMEOUT(document.state(), QStringLiteral("ready"), 10000);
        QVERIFY(!shown(window, "archiveDefaultBanner"));
        document.close();
        QTRY_VERIFY(shown(window, "archiveDefaultBanner"));
    }

    void settingsManageSavedPasswords() {
        TestApp t;
        t.app.passwords()->save("网盘", "abc123");
        ArchiveDocument document;
        QQmlEngine engine;
        QQmlComponent component(&engine,
                                QUrl::fromLocalFile(QString(NOOK_SOURCE_DIR) + "/qml/ArchiveMain.qml"));
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

    // Phase 4: what nook's right-click menu runs.
    void nookMenuActionsUseProgressWindows() {
        TestApp t;
        QQmlEngine engine;
        std::vector<std::unique_ptr<QObject>> windows;
        t.app.setWindowFactory([&](const QString &type, const QVariantMap &properties) -> QObject * {
            QVariantMap initial = properties;
            initial.insert("app", QVariant::fromValue(&t.app));
            QQmlComponent component(&engine,
                                    QUrl::fromLocalFile(QString(NOOK_SOURCE_DIR) + "/qml/" + type + ".qml"));
            if (!component.isReady())
                qWarning() << component.errorString();
            windows.emplace_back(component.createWithInitialProperties(initial));
            return windows.back().get();
        });
        const QString base = fixture->path() + "/menu";
        put(base + "/旅行/1.jpg", "one");
        put(base + "/旅行/2.jpg", "two");
        put(base + "/旅行.zip", "taken"); // the one-click name is taken: it gets a number

        // "压缩为 旅行.zip"
        QList<int> ids = t.app.compressNow({base + "/旅行"});
        QCOMPARE(ids.size(), 1);
        QCOMPARE(windows.size(), size_t(1));
        auto *progress = qobject_cast<QQuickWindow *>(windows.back().get());
        QVERIFY(progress && t.queue()->ownedBy(ids.first(), progress));
        QTRY_COMPARE_WITH_TIMEOUT(status(t.queue(), ids.first()), QStringLiteral("done"), 15000);
        QVERIFY(QFileInfo(base + "/旅行 (2).zip").size() > 100);
        QCOMPARE(read(base + "/旅行.zip"), QStringLiteral("taken"));
        QTRY_VERIFY_WITH_TIMEOUT(!progress->isVisible(), 5000);

        // "解压到当前文件夹" on two packages (one split into volumes, chosen by every part): each
        // goes into a folder of its own, the split one once.
        const QString first = base + "/甲.zip";
        writeZip(first, {{"a.txt", "A"}});
        TestApp maker;
        QByteArray noise(300000, 0);
        for (auto &byte : noise)
            byte = char(QRandomGenerator::global()->generate());
        put(base + "/大/blob.bin", noise);
        const int split = maker.queue()->start({{"op", "create"}, {"format", "7z"}, {"output", base + "/乙.7z"},
                                                {"items", QStringList{base + "/大"}},
                                                {"options", QVariantMap{{"volumeSize", 100 * 1024.0}, {"level", 0}}}}, {}, nullptr);
        QTRY_COMPARE_WITH_TIMEOUT(status(maker.queue(), split), QStringLiteral("done"), 15000);
        QStringList parts = QDir(base).entryList({"乙.7z.*"}, QDir::Files, QDir::Name);
        QVERIFY2(parts.size() > 1, qPrintable(QDir(base).entryList(QDir::Files).join(", ") + " / " + maker.queue()->task(split).value("message").toString()));
        QStringList chosen{first};
        for (const auto &part : parts)
            chosen << base + "/" + part;
        QCOMPARE(t.app.firstParts(chosen).size(), 2);
        put(base + "/out/keep.txt", "k");
        ids = t.app.extractInto(chosen, base + "/out");
        QCOMPARE(ids.size(), 2);
        progress = qobject_cast<QQuickWindow *>(windows.back().get());
        QVERIFY(QTest::qWaitForWindowExposed(progress));
        QTest::qWait(200);
        QVERIFY(progress->grabWindow().save(QString(NOOK_BINARY_DIR) + "/archive-app-progress.png"));
        for (int id : ids)
            QTRY_COMPARE_WITH_TIMEOUT(status(t.queue(), id), QStringLiteral("done"), 15000);
        QCOMPARE(read(base + "/out/甲/a.txt"), QStringLiteral("A"));
        QCOMPARE(QFileInfo(base + "/out/乙/大/blob.bin").size(), 300000);
        QTRY_VERIFY_WITH_TIMEOUT(!progress->isVisible(), 5000);

        // A single package "here" follows the folder rule (smart): one top-level item, no extra folder.
        ids = t.app.extractInto({first}, {});
        QTRY_COMPARE_WITH_TIMEOUT(status(t.queue(), ids.first()), QStringLiteral("done"), 15000);
        QCOMPARE(read(base + "/a.txt"), QStringLiteral("A"));
    }

    void progressWindowStaysOnFailure() {
        TestApp t;
        QQmlEngine engine;
        std::vector<std::unique_ptr<QObject>> windows;
        t.app.setWindowFactory([&](const QString &type, const QVariantMap &properties) -> QObject * {
            QVariantMap initial = properties;
            initial.insert("app", QVariant::fromValue(&t.app));
            QQmlComponent component(&engine,
                                    QUrl::fromLocalFile(QString(NOOK_SOURCE_DIR) + "/qml/" + type + ".qml"));
            windows.emplace_back(component.createWithInitialProperties(initial));
            return windows.back().get();
        });
        const QString broken = fixture->path() + "/broken-menu.zip";
        put(broken, "PK\x03\x04 not really");
        const QList<int> ids = t.app.extractInto({broken}, {});
        auto *progress = qobject_cast<QQuickWindow *>(windows.back().get());
        QTRY_COMPARE_WITH_TIMEOUT(status(t.queue(), ids.first()), QStringLiteral("failed"), 15000);
        QTest::qWait(900);
        QVERIFY(progress->isVisible());
        QVERIFY(progress->grabWindow().save(QString(NOOK_BINARY_DIR) + "/archive-app-failed.png"));
    }

    // Deleting asks first; renaming and adding change the package and the window shows it again.
    void mainWindowChangesPackage() {
        if (QStandardPaths::findExecutable("7z").isEmpty())
            QSKIP("ZIP packages are changed with 7-Zip");
        TestApp t;
        const QString zip = fixture->path() + "/edit-ui.zip";
        writeZip(zip, {{"docs/a.txt", "aaa"}, {"docs/b.txt", "bbb"}, {"keep.txt", "keep"}});
        ArchiveDocument document;
        t.app.attach(&document);
        QQmlEngine engine;
        auto root = mainWindow(engine, t.app, document);
        auto *window = qobject_cast<QQuickWindow *>(root.get());
        QVERIFY(window && QTest::qWaitForWindowExposed(window));
        document.open(zip);
        QTRY_COMPARE_WITH_TIMEOUT(document.state(), QStringLiteral("ready"), 10000);
        QVERIFY(!document.readOnly());
        ArchiveModel *model = document.model();
        model->openFolder("docs");
        QCOMPARE(names(*model), (QStringList{"a.txt", "b.txt"}));

        // Delete: a red confirmation, then the package without it.
        model->click(0, 0);
        QMetaObject::invokeMethod(findVisual(window->contentItem(), "archiveDelete"), "clicked");
        QQuickItem *confirm = nullptr;
        QTRY_VERIFY((confirm = shown(window, "archiveDeleteConfirm")));
        QTest::qWait(300);
        QVERIFY(window->grabWindow().save(QString(NOOK_BINARY_DIR) + "/archive-app-delete.png"));
        QMetaObject::invokeMethod(confirm, "clicked");
        QTRY_COMPARE_WITH_TIMEOUT(names(*model), (QStringList{"b.txt"}), 15000);
        QCOMPARE(model->folder(), QStringLiteral("docs"));

        // Rename: the name without its extension is selected; a taken name is refused.
        model->click(0, 0);
        QMetaObject::invokeMethod(findVisual(window->contentItem(), "archiveRename"), "clicked");
        QQuickItem *field = nullptr;
        QTRY_VERIFY((field = shown(window, "archiveRenameField")));
        QCOMPARE(field->property("selectedText").toString(), QStringLiteral("b"));
        field->setProperty("text", "c.txt");
        QTest::qWait(300);
        QVERIFY(window->grabWindow().save(QString(NOOK_BINARY_DIR) + "/archive-app-rename.png"));
        QMetaObject::invokeMethod(findVisual(window->contentItem(), "archiveRenameConfirm"), "clicked");
        QTRY_COMPARE_WITH_TIMEOUT(names(*model), (QStringList{"c.txt"}), 15000);

        // Add into the folder shown; the same name again is asked about.
        const QString incoming = fixture->path() + "/edit-ui-in/new.txt";
        put(incoming, "new");
        QMetaObject::invokeMethod(window, "addPaths", Q_ARG(QVariant, QVariant(QStringList{incoming})));
        QTRY_COMPARE_WITH_TIMEOUT(names(*model), (QStringList{"c.txt", "new.txt"}), 15000);
        put(incoming, "newer");
        QMetaObject::invokeMethod(window, "addPaths", Q_ARG(QVariant, QVariant(QStringList{incoming})));
        QQuickItem *overwrite = nullptr;
        QTRY_VERIFY_WITH_TIMEOUT((overwrite = shown(window, "archiveConflictOverwrite")), 15000);
        QTest::qWait(300);
        QVERIFY(window->grabWindow().save(QString(NOOK_BINARY_DIR) + "/archive-app-add-conflict.png"));
        QMetaObject::invokeMethod(overwrite, "clicked");
        QTRY_COMPARE_WITH_TIMEOUT(t.queue()->activeCount(), 0, 15000);
        QCOMPARE(entryData(zip, "docs/new.txt"), QByteArray("newer"));
        QCOMPARE(entryData(zip, "docs/c.txt"), QByteArray("bbb"));
        QCOMPARE(entryData(zip, "keep.txt"), QByteArray("keep"));
        QVERIFY(entryData(zip, "docs/a.txt").isEmpty());
    }

    // A file opened from the package: each save asks; closing with an edit not written back asks too.
    void openedFileIsWrittenBack() {
        if (QStandardPaths::findExecutable("7z").isEmpty())
            QSKIP("ZIP packages are changed with 7-Zip");
        TestApp t;
        QString opened;
        t.app.files()->setOpener([&](const QString &path) { opened = path; });
        const QString zip = fixture->path() + "/open-ui.zip";
        writeZip(zip, {{"notes/todo.txt", "one"}});
        ArchiveDocument document;
        t.app.attach(&document);
        QQmlEngine engine;
        auto root = mainWindow(engine, t.app, document);
        auto *window = qobject_cast<QQuickWindow *>(root.get());
        QVERIFY(window && QTest::qWaitForWindowExposed(window));
        document.open(zip);
        QTRY_COMPARE_WITH_TIMEOUT(document.state(), QStringLiteral("ready"), 10000);
        document.model()->openFolder("notes");
        QMetaObject::invokeMethod(window, "activate", Q_ARG(QVariant, 0));
        QTRY_VERIFY_WITH_TIMEOUT(!opened.isEmpty(), 10000);
        QVERIFY(opened.startsWith(fixture->path() + "/cache/nook-archive/"));
        QCOMPARE(read(opened), QStringLiteral("one"));
        // The extraction was the window's work, not a task in the list.
        QTRY_COMPARE(t.queue()->count(), 0);

        put(opened, "two two");
        QQuickItem *accept = nullptr;
        QTRY_VERIFY_WITH_TIMEOUT((accept = shown(window, "archiveModifiedAccept")), 5000);
        QTest::qWait(300);
        QVERIFY(window->grabWindow().save(QString(NOOK_BINARY_DIR) + "/archive-app-modified.png"));
        QMetaObject::invokeMethod(accept, "clicked");
        QTRY_COMPARE_WITH_TIMEOUT(entryData(zip, "notes/todo.txt"), QByteArray("two two"), 15000);
        QTRY_COMPARE(t.app.files()->unsavedCount(), 0);

        QTRY_VERIFY(!shown(window, "archiveModifiedDecline"));
        put(opened, "three three three");
        QTRY_COMPARE_WITH_TIMEOUT(t.app.files()->unsavedCount(), 1, 5000);
        QQuickItem *decline = nullptr;
        QTRY_VERIFY_WITH_TIMEOUT((decline = shown(window, "archiveModifiedDecline")), 5000);
        QMetaObject::invokeMethod(decline, "clicked");
        QCOMPARE(t.app.files()->unsavedCount(), 1);
        window->close();
        QQuickItem *discard = nullptr;
        QTRY_VERIFY((discard = shown(window, "archiveUnsavedDiscard")));
        QVERIFY(window->isVisible());
        QTest::qWait(300);
        QVERIFY(window->grabWindow().save(QString(NOOK_BINARY_DIR) + "/archive-app-unsaved.png"));
        QMetaObject::invokeMethod(discard, "clicked");
        QTRY_VERIFY(!window->isVisible());
        QVERIFY(!QFileInfo::exists(opened));
        QCOMPARE(entryData(zip, "notes/todo.txt"), QByteArray("two two"));
    }

    // A package that cannot change: its edits can be saved as a copy. Dragging out extracts first.
    void readOnlyCopyAndDragExport() {
        TestApp t;
        QString opened;
        FileSessions *files = t.app.files();
        files->setOpener([&](const QString &path) { opened = path; });
        const QString zip = fixture->path() + "/legacy-edit.zip";
        writeZip(zip, {{QByteArray("\xd6\xd0\xce\xc4/\xb1\xa8\xb8\xe6.txt"), "report"}});
        QObject owner;
        QSignalSpy modified(files, &FileSessions::modified);
        files->open(&owner, zip, "中文/报告.txt", "GB18030", true);
        QTRY_VERIFY_WITH_TIMEOUT(!opened.isEmpty(), 10000);
        QCOMPARE(QFileInfo(opened).fileName(), QStringLiteral("报告.txt"));
        put(opened, "edited");
        QTRY_COMPARE_WITH_TIMEOUT(modified.size(), 1, 5000);
        const int id = modified.first().first().toInt();
        QVERIFY(files->ownedBy(id, &owner));
        QVERIFY(files->session(id).value("readOnly").toBool());
        files->writeBack(id); // refused: nothing starts
        QCOMPARE(t.queue()->activeCount(), 0);
        const QString saved = fixture->path() + "/legacy-saved";
        QDir().mkpath(saved);
        put(saved + "/报告.txt", "already here");
        const QString copy = files->saveCopy(id, saved);
        QCOMPARE(copy, saved + "/报告 (2).txt");
        QCOMPARE(read(copy), QStringLiteral("edited"));
        QCOMPARE(files->unsavedCount(), 0);

        QSignalSpy ready(files, &FileSessions::dragReady);
        const int token = files->exportForDrag(&owner, zip, {"中文"}, {}, "GB18030");
        QVERIFY(token > 0);
        QTRY_COMPARE_WITH_TIMEOUT(ready.size(), 1, 10000);
        QCOMPARE(ready.first().at(0).toInt(), token);
        const QStringList urls = ready.first().at(1).toStringList();
        QCOMPARE(urls.size(), 1);
        const QString dragged = QUrl(urls.first()).toLocalFile();
        QVERIFY(dragged.endsWith("/中文"));
        QCOMPARE(read(dragged + "/报告.txt"), QStringLiteral("report"));
        QTRY_COMPARE(t.queue()->count(), 0);
    }

    // Converting keeps the original and writes names as UTF-8: the copy can then be changed.
    void composeConvertsLegacyNames() {
        TestApp t;
        const QString zip = fixture->path() + "/convert-ui.zip";
        writeZip(zip, {{QByteArray("\xd6\xd0\xce\xc4/\xb1\xa8\xb8\xe6.txt"), "report"}});
        QQmlEngine engine;
        QQmlComponent component(&engine,
                                QUrl::fromLocalFile(QString(NOOK_SOURCE_DIR) + "/qml/ArchiveCompose.qml"));
        QVERIFY2(component.isReady(), qPrintable(component.errorString()));
        std::unique_ptr<QObject> root(component.createWithInitialProperties(
            {{"app", QVariant::fromValue(&t.app)}, {"items", QStringList()}, {"convertFrom", zip}, {"encoding", "GB18030"}}));
        QVERIFY2(root, qPrintable(component.errorString()));
        auto *window = qobject_cast<QQuickWindow *>(root.get());
        QVERIFY(window && QTest::qWaitForWindowExposed(window));
        // Next to the original, under a name that does not replace it.
        QCOMPARE(findVisual(window->contentItem(), "archiveComposeName")->property("text").toString(), QStringLiteral("convert-ui (2)"));
        QCOMPARE(findVisual(window->contentItem(), "archiveComposeFormat")->property("currentText").toString(), QStringLiteral("ZIP"));
        QVERIFY(!shown(window, "archiveComposeItems"));
        QVERIFY(!shown(window, "archiveComposeDelete"));
        QVERIFY(window->grabWindow().save(QString(NOOK_BINARY_DIR) + "/archive-app-convert.png"));
        QMetaObject::invokeMethod(findVisual(window->contentItem(), "archiveComposeStart"), "clicked");
        QTRY_VERIFY_WITH_TIMEOUT(!window->isVisible(), 20000);

        const QString converted = fixture->path() + "/convert-ui (2).zip";
        QCOMPARE(entryData(converted, "中文/报告.txt"), QByteArray("report"));
        QVERIFY(QFileInfo::exists(zip));
        ArchiveDocument document;
        document.open(converted);
        QTRY_COMPARE_WITH_TIMEOUT(document.state(), QStringLiteral("ready"), 10000);
        QVERIFY(!document.legacyNames());
        QVERIFY(!document.readOnly());
        QCOMPARE(names(*document.model()), (QStringList{"中文"}));

        // The worker refuses to write over the package it reads.
        QVariantMap request = t.app.convertRequest(zip, {{"name", "convert-ui"}, {"folder", fixture->path()}, {"format", "zip"}, {"levelStep", 3}}, "GB18030");
        QCOMPARE(request.value("output").toString(), zip);
        const int id = t.queue()->start(request, {}, nullptr);
        QTRY_COMPARE_WITH_TIMEOUT(status(t.queue(), id), QStringLiteral("failed"), 10000);
        QVERIFY(QFileInfo(zip).size() > 0);
    }
    // A drag from blank space or a row's size/date/type cells selects what the band covers
    // (Ctrl adds to the selection); a click on blank space clears it. The info dialog's values
    // can be selected and copied.
    void bandSelectionAndCopyableInfo() {
        TestApp t;
        const QString zip = fixture->path() + "/band.zip";
        QList<QPair<QByteArray, QByteArray>> entries;
        for (int i = 0; i < 8; ++i)
            entries << qMakePair(QByteArray("file-") + QByteArray::number(i) + ".txt", QByteArray(100 + i, 'x'));
        writeZip(zip, entries);
        ArchiveDocument document;
        t.app.attach(&document);
        QQmlEngine engine;
        auto root = mainWindow(engine, t.app, document);
        auto *window = qobject_cast<QQuickWindow *>(root.get());
        QVERIFY(window && QTest::qWaitForWindowExposed(window));
        document.open(zip);
        QTRY_COMPARE_WITH_TIMEOUT(document.state(), QStringLiteral("ready"), 10000);
        ArchiveModel *model = document.model();
        QTRY_COMPARE(model->count(), 8);
        auto *list = findVisual(window->contentItem(), "archiveFileList");
        auto *band = findVisual(window->contentItem(), "archiveSelectionBand");
        QVERIFY(list && band);
        QTest::qWait(200);
        const qreal rowHeight = 40;
        // In the size column (past the name), in the middle of a row.
        const auto sizeCell = [&](int row) { return list->mapToScene({list->width() - 400, row * rowHeight + rowHeight / 2}).toPoint(); };
        const QPoint blank = list->mapToScene({list->width() - 200, list->height() - 20}).toPoint();
        const auto drag = [&](QPoint from, QPoint to, Qt::KeyboardModifiers modifiers = Qt::NoModifier) {
            QTest::mousePress(window, Qt::LeftButton, modifiers, from);
            for (int i = 1; i <= 8; ++i)
                QTest::mouseMove(window, from + (to - from) * i / 8);
            QTRY_VERIFY(band->isVisible());
            QTest::mouseRelease(window, Qt::LeftButton, modifiers, to);
            QTRY_VERIFY(!band->isVisible());
        };
        // From the blank space below the rows up into row 5: rows 5–7.
        drag(blank, sizeCell(5));
        QTRY_COMPARE(model->selectedRows(), (QList<int>{5, 6, 7}));
        // From a row's size cell: rows 1–2 only.
        drag(sizeCell(1), sizeCell(2));
        QTRY_COMPARE(model->selectedRows(), (QList<int>{1, 2}));
        // Ctrl adds rows 4–5.
        drag(sizeCell(4), sizeCell(5), Qt::ControlModifier);
        QTRY_COMPARE(model->selectedRows(), (QList<int>{1, 2, 4, 5}));
        // A click in a size cell selects that row; a click on blank space clears.
        QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, sizeCell(3));
        QTRY_COMPARE(model->selectedRows(), QList<int>{3});
        QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, blank);
        QTRY_COMPARE(model->selectedCount(), 0);
        // The name still selects its row (and is where files are dragged out from).
        QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, list->mapToScene({60, 6 * rowHeight + rowHeight / 2}).toPoint());
        QTRY_COMPARE(model->selectedRows(), QList<int>{6});

        // Info: the location can be selected with the mouse and copied.
        QTest::keyClick(window, Qt::Key_I, Qt::ControlModifier);
        QQuickItem *value = nullptr;
        QTRY_VERIFY((value = shown(window, "archiveInfoValue")));
        QVERIFY(value->property("selectByMouse").toBool());
        QVERIFY(value->property("readOnly").toBool());
        QCOMPARE(value->property("text").toString(), fixture->path());
        QTest::qWait(300);
        const QPoint from = value->mapToScene({2, 8}).toPoint();
        const QPoint to = value->mapToScene({value->width() - 2, value->height() - 4}).toPoint();
        QTest::mousePress(window, Qt::LeftButton, Qt::NoModifier, from);
        for (int i = 1; i <= 10; ++i)
            QTest::mouseMove(window, from + (to - from) * i / 10);
        QTest::mouseRelease(window, Qt::LeftButton, Qt::NoModifier, to);
        QTRY_VERIFY(!value->property("selectedText").toString().isEmpty());
        QGuiApplication::clipboard()->clear();
        QTest::keyClick(window, Qt::Key_C, Qt::ControlModifier);
        QTRY_VERIFY(!QGuiApplication::clipboard()->text().isEmpty());
        QCOMPARE(QGuiApplication::clipboard()->text(), value->property("selectedText").toString());
        QVERIFY(window->grabWindow().save(QString(NOOK_BINARY_DIR) + "/archive-app-info-select.png"));
    }
};

// Set XDG before QApplication/GIO is first used. Association writes must never reach the user's
// desktop, even when this test binary is launched directly rather than through ctest.
int main(int argc, char **argv) {
    environment = std::make_unique<QTemporaryDir>(QString(NOOK_BINARY_DIR) + "/archive-app-fixture-XXXXXX");
    if (!environment->isValid())
        return 1;
    const QString base = environment->path();
    qputenv("XDG_CONFIG_HOME", QFile::encodeName(base + "/config"));
    qputenv("XDG_CONFIG_DIRS", QFile::encodeName(base + "/sysconfig"));
    qputenv("XDG_DATA_HOME", QFile::encodeName(base + "/data"));
    qputenv("XDG_DATA_DIRS", "/usr/share");
    qputenv("XDG_CURRENT_DESKTOP", "nook-test");
    qputenv("XDG_CACHE_HOME", QFile::encodeName(base + "/cache"));
    QString desktop = read(QString(NOOK_SOURCE_DIR) + "/packaging/local.nook.archive.desktop");
    desktop.replace("Exec=nook-archive %U", "Exec=true %U");
    put(base + "/data/applications/local.nook.archive.desktop", desktop.toUtf8());
    put(base + "/data/applications/archive-test-other.desktop",
        "[Desktop Entry]\nType=Application\nName=Test Archive Manager\nExec=true "
        "%U\nMimeType=application/zip;\n");
    put(base + "/config/mimeapps.list",
        "[Default Applications]\napplication/zip=archive-test-other.desktop\n");
    QGuiApplication app(argc, argv);
    ArchiveAppTest test;
    return QTest::qExec(&test, argc, argv);
}
#include "archive_app_test.moc"
