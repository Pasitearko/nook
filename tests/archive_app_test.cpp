#include "application.h"
#include "archiveapp.h"
#include "archivedocument.h"
#include "archivemodel.h"
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
};

QTEST_MAIN(ArchiveAppTest)
#include "archive_app_test.moc"
