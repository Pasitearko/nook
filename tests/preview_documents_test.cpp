// Integration: qt_add_executable(nook-preview-documents-tests tests/preview_documents_test.cpp)
// target_link_libraries(... PRIVATE nook_core Qt6::Test PkgConfig::LIBARCHIVE)
// add_test(NAME preview-documents COMMAND nook-preview-documents-tests)
// Native fixtures need no office suite, archive executable or network access.
#include "preview.h"
#include "previewdocuments.h"
#include <QFile>
#include <QJsonDocument>
#include <QTextStream>
#include <unistd.h>
#include <QTemporaryDir>
#include <QUrl>
#include <QtTest>
#include <archive.h>
#include <archive_entry.h>

class PreviewDocumentsTest : public QObject {
    Q_OBJECT
    struct Member {
        QByteArray name;
        QByteArray data;
        bool directory = false;
        QByteArray link = {};
    };
    static void package(const QString &path, const QList<Member> &members, bool zip = true) {
        auto *writer = archive_write_new();
        QVERIFY(writer);
        if (zip)
            QCOMPARE(archive_write_set_format_zip(writer), ARCHIVE_OK);
        else {
            QCOMPARE(archive_write_set_format_pax_restricted(writer), ARCHIVE_OK);
            QCOMPARE(archive_write_add_filter_gzip(writer), ARCHIVE_OK);
        }
        QCOMPARE(archive_write_open_filename(writer, QFile::encodeName(path).constData()), ARCHIVE_OK);
        for (const auto &member : members) {
            auto *entry = archive_entry_new();
            archive_entry_set_pathname(entry, member.name.constData());
            archive_entry_set_filetype(entry, member.directory ? AE_IFDIR : member.link.isEmpty() ? AE_IFREG : AE_IFLNK);
            archive_entry_set_perm(entry, 0644);
            archive_entry_set_size(entry, member.data.size());
            if (!member.link.isEmpty())
                archive_entry_set_symlink(entry, member.link.constData());
            QCOMPARE(archive_write_header(writer, entry), ARCHIVE_OK);
            if (!member.data.isEmpty())
                QCOMPARE(archive_write_data(writer, member.data.constData(), member.data.size()), member.data.size());
            archive_entry_free(entry);
        }
        QCOMPARE(archive_write_close(writer), ARCHIVE_OK);
        QCOMPARE(archive_write_free(writer), ARCHIVE_OK);
    }
    static QByteArray word(const QByteArray &text) {
        return "<w:document xmlns:w='http://schemas.openxmlformats.org/wordprocessingml/2006/main'><w:body><w:p><w:r><w:t>" +
               text + "</w:t></w:r></w:p></w:body></w:document>";
    }
    static QVariantMap preview(const QString &path, int page = 1) {
        return makePreview(QUrl::fromLocalFile(path).toString(), page);
    }
    static void failed(const QVariantMap &result) {
        QVERIFY2(!result.value("error").toString().isEmpty(), qPrintable(result.value("text").toString()));
        QVERIFY(result.value("text").toString().isEmpty());
    }
    static QList<Member> workbook(const QByteArray &target = "/xl/worksheets/sheet1.xml") {
        return {{"xl/workbook.xml", "<workbook xmlns:r='urn:relationships'><sheets><sheet name='Sheet A' r:id='s1'/><sheet name='Sheet B' r:id='s2'/></sheets></workbook>"},
                {"xl/_rels/workbook.xml.rels", "<Relationships><Relationship Id='s1' Type='urn:worksheet/worksheet' Target='" + target + "'/><Relationship Id='s2' Type='urn:worksheet/worksheet' Target='worksheets/sheet2.xml'/></Relationships>"},
                {"xl/sharedStrings.xml", "<sst><si><t>&lt;img src='file:///etc/passwd'&gt;</t></si></sst>"},
                {"xl/worksheets/sheet1.xml", "<worksheet><sheetData><row r='1'><c r='A1' t='s'><v>0</v></c><c r='C1' t='inlineStr'><is><t>Inline</t></is></c><c r='D1'><f>HYPERLINK(&quot;https://invalid&quot;)</f><v>42</v></c></row><row r='100'><c r='B100'><v>123</v></c></row></sheetData></worksheet>"},
                {"xl/worksheets/sheet2.xml", "<worksheet><sheetData><row r='1'><c r='A1' t='b'><v>1</v></c></row></sheetData></worksheet>"}};
    }
  private Q_SLOTS:
    void docxNative() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const auto path = dir.filePath("report.docx");
        package(path, {{"word/document.xml", word("Hello &amp; 世界")},
                       {"word/_rels/document.xml.rels", "<not-even-valid"}});
        const auto result = preview(path);
        QVERIFY(!result.contains("error"));
        QCOMPARE(result.value("mode").toString(), QString("text"));
        QCOMPARE(result.value("text").toString(), QString::fromUtf8("Hello & 世界"));
        QVERIFY(!result.value("notice").toString().isEmpty());
    }
    void wordRejectsEntitiesAndMalformedXml() {
        QTemporaryDir dir;
        const auto path = dir.filePath("bad.docx");
        for (const auto &xml : {QByteArray("<!DOCTYPE document [<!ENTITY secret SYSTEM 'file:///etc/passwd'>]><document><t>&secret;</t></document>"),
                                QByteArray("<!DOCTYPE document [<!ENTITY a 'expansion'>]><document><t>&a;</t></document>"),
                                QByteArray("<document><t>truncated"),
                                QByteArray(70, '<')}) {
            package(path, {{"word/document.xml", xml}});
            failed(preview(path));
        }
        QByteArray deep;
        for (int i = 0; i < 70; ++i) deep += "<p>";
        for (int i = 0; i < 70; ++i) deep += "</p>";
        package(path, {{"word/document.xml", deep}});
        failed(preview(path));
    }
    void boundedMembers() {
        QTemporaryDir dir;
        const auto path = dir.filePath("huge.docx");
        package(path, {{"word/document.xml", word(QByteArray(3 * 1024 * 1024, 'x'))}});
        failed(preview(path));
        package(path, {{"../word/document.xml", word("unsafe")}});
        failed(preview(path));
        package(path, {{"word/document.xml", word("first")}, {"word/document.xml", word("duplicate")}});
        failed(preview(path));
    }
    void xlsxSheetsEscapingSparseCellsAndCachedFormula() {
        QTemporaryDir dir;
        const auto path = dir.filePath("book.xlsx");
        package(path, workbook());
        const auto first = preview(path);
        QVERIFY2(!first.contains("error"), qPrintable(first.value("error").toString()));
        QCOMPARE(first.value("mode").toString(), QString("rich"));
        QCOMPARE(first.value("pages").toInt(), 2);
        const auto html = first.value("text").toString();
        QVERIFY(html.contains("&lt;img"));
        QVERIFY(!html.contains("<img"));
        QVERIFY(html.contains("<td></td><td>Inline</td><td>42</td>"));
        QVERIFY(html.contains("2–99"));
        QVERIFY(html.contains("<th>100</th>"));
        QVERIFY(!html.contains("HYPERLINK"));
        const auto second = preview(path, 2);
        QVERIFY(!second.contains("error"));
        QVERIFY(second.value("text").toString().contains("TRUE"));
        QVERIFY(second.value("subtitle").toString().contains("Sheet B"));
        failed(preview(path, 3));
        package(path, workbook("https://example.invalid/book.xml"));
        failed(preview(path));
        package(path, workbook("../../../outside.xml"));
        failed(preview(path));
    }
    void epubSpineAndNoResources() {
        QTemporaryDir dir;
        const auto path = dir.filePath("book.epub");
        QList<Member> members{{"META-INF/container.xml", "<container><rootfiles><rootfile full-path='OPS/package.opf'/></rootfiles></container>"},
                              {"OPS/package.opf", "<package><manifest><item id='b' href='b.xhtml' media-type='application/xhtml+xml'/><item id='a' href='a%20chapter.xhtml' media-type='application/xhtml+xml'/></manifest><spine><itemref idref='a'/><itemref idref='b'/></spine></package>"},
                              {"OPS/b.xhtml", "<html><body><p>Second chapter</p></body></html>"},
                              {"OPS/a chapter.xhtml", "<html><head><title>Hidden</title><style>HiddenCSS</style></head><body><h1>First chapter</h1><script>HiddenJS</script><img src='file:///etc/passwd'/><a href='https://example.invalid'>Visible link text</a></body></html>"}};
        package(path, members);
        const auto first = preview(path);
        QVERIFY(!first.contains("error"));
        QCOMPARE(first.value("pages").toInt(), 2);
        QCOMPARE(first.value("mode").toString(), QString("text"));
        const auto text = first.value("text").toString();
        QVERIFY(text.contains("First chapter"));
        QVERIFY(text.contains("Visible link text"));
        QVERIFY(!text.contains("Hidden"));
        QVERIFY(!text.contains("file:"));
        QVERIFY(preview(path, 2).value("text").toString().contains("Second chapter"));
        failed(preview(path, 0));
        members[3].data = "<!DOCTYPE html SYSTEM 'https://example.invalid/entity'><html><body>Unsafe DTD</body></html>";
        package(path, members);
        failed(preview(path));
    }
    void archiveHierarchyNoExtractionAndUnsafeNames() {
        QTemporaryDir dir;
        const auto path = dir.filePath("tree.zip");
        package(path, {{"z/file.txt", "payload"}, {"a/deep/child.txt", "payload"}, {"a.txt", "payload"},
                       {"../escape.txt", "escape"}, {"/absolute.txt", "escape"}, {"C:/drive.txt", "escape"},
                       {"link", {}, false, "/etc/passwd"}, {"link/child.txt", "not-followed"}});
        const auto result = preview(path);
        QVERIFY(!result.contains("error"));
        const auto text = result.value("text").toString();
        QVERIFY(text.contains("▸ a/\n  ▸ deep/\n      child.txt"));
        QVERIFY(text.indexOf("child.txt") < text.indexOf("a.txt"));
        QVERIFY(!text.contains("escape.txt"));
        QVERIFY(!text.contains("absolute.txt"));
        QVERIFY(!text.contains("drive.txt"));
        QVERIFY(text.contains("路径类型冲突"));
        QVERIFY(result.value("notice").toString().contains("3"));
        QCOMPARE(QDir(dir.path()).entryList(QDir::Files), QStringList{"tree.zip"});
        package(path, {{"target", {}, false, "/etc/passwd"}});
        QVERIFY(preview(path).value("text").toString().contains("链接，不跟随"));
    }
    void archiveLimitsAndCompressedTar() {
        QTemporaryDir dir;
        auto path = dir.filePath("limited.zip");
        QList<Member> members;
        for (int i = 0; i < 2100; ++i)
            members << Member{QByteArray::number(i) + ".txt", {}};
        package(path, members);
        const auto limited = preview(path);
        QVERIFY(!limited.contains("error"));
        QVERIFY(limited.value("notice").toString().contains("不完整"));
        QVERIFY(limited.value("text").toString().size() <= 128 * 1024);
        path = dir.filePath("tree.tar.gz");
        package(path, {{"parent/child.txt", "Hello"}}, false);
        const auto tar = preview(path);
        QVERIFY2(!tar.contains("error"), qPrintable(tar.value("error").toString()));
        QVERIFY(tar.value("text").toString().contains("▸ parent/\n    child.txt"));
    }
    void standaloneCompressionAndStreamLimit() {
        QTemporaryDir dir;
        const auto rawPath = dir.filePath("notes.txt.gz");
        auto *writer = archive_write_new();
        QCOMPARE(archive_write_set_format_raw(writer), ARCHIVE_OK);
        QCOMPARE(archive_write_add_filter_gzip(writer), ARCHIVE_OK);
        QCOMPARE(archive_write_open_filename(writer, QFile::encodeName(rawPath).constData()), ARCHIVE_OK);
        auto *entry = archive_entry_new();
        archive_entry_set_pathname(entry, "notes.txt");
        archive_entry_set_filetype(entry, AE_IFREG);
        archive_entry_set_size(entry, 5);
        QCOMPARE(archive_write_header(writer, entry), ARCHIVE_OK);
        QCOMPARE(archive_write_data(writer, "Hello", 5), 5);
        archive_entry_free(entry);
        QCOMPARE(archive_write_free(writer), ARCHIVE_OK);
        const auto raw = preview(rawPath);
        QVERIFY2(!raw.contains("error"), qPrintable(raw.value("error").toString()));
        QVERIFY(raw.value("text").toString().contains("notes.txt"));
        const auto bombPath = dir.filePath("bounded.tar.gz");
        package(bombPath, {{"huge.txt", QByteArray(33 * 1024 * 1024, 'x')}, {"not-scanned.txt", "after"}}, false);
        const auto bomb = preview(bombPath);
        QVERIFY(!bomb.contains("error"));
        QVERIFY(bomb.value("notice").toString().contains("不完整"));
        QVERIFY(!bomb.value("text").toString().contains("not-scanned"));
    }
    void subprocessRoundtrip() {
        QTemporaryDir dir;
        const auto path = dir.filePath("worker.docx");
        package(path, {{"word/document.xml", word("Worker text")}});
        Preview controller;
        controller.setWorkerExecutable(QCoreApplication::applicationFilePath());
        controller.open(QUrl::fromLocalFile(path).toString());
        QTRY_VERIFY_WITH_TIMEOUT(!controller.loading(), 5000);
        QVERIFY2(!controller.content().contains("error"), qPrintable(controller.content().value("error").toString()));
        QCOMPARE(controller.content().value("text").toString(), QString("Worker text"));
        controller.close();
        QVERIFY(!controller.visible());
    }
    void legacyFallbackAndOtherFormats() {
        QTemporaryDir dir;
        for (const auto &extension : {"doc", "xls"}) {
            const auto path = dir.filePath(QString("legacy.") + extension);
            QFile file(path);
            QVERIFY(file.open(QIODevice::WriteOnly));
            file.write("not a legacy document");
            file.close();
            const auto result = preview(path);
            QCOMPARE(result.value("mode").toString(), QString("text"));
            QVERIFY(result.value("text").toString().contains("LibreOffice"));
        }
        QVariantMap map;
        QVERIFY(!nook::documentPreview(dir.filePath("plain.txt"), 1, &map));
        QVERIFY(map.isEmpty());
    }
};
int main(int argc, char **argv) {
    const bool worker = argc > 1 && QByteArray(argv[1]) == "--preview-worker";
    if (worker)
        ::setpgid(0, 0);
    QCoreApplication application(argc, argv);
    if (worker) {
        const auto result = makePreview(argc > 2 ? QString::fromLocal8Bit(argv[2]) : QString(),
                                        argc > 3 ? QByteArray(argv[3]).toInt() : 1);
        QTextStream(stdout) << QJsonDocument::fromVariant(result).toJson(QJsonDocument::Compact) << Qt::endl;
        return 0;
    }
    PreviewDocumentsTest test;
    return QTest::qExec(&test, argc, argv);
}
#include "preview_documents_test.moc"
