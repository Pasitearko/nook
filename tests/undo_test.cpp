#include "operations.h"
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QUrl>
#include <QtTest>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace {
QString fixtureTemplate() {
    return QCoreApplication::applicationDirPath() + "/undo-fixture-XXXXXX";
}
QString uri(const QString &path) {
    return QUrl::fromLocalFile(path).toString();
}
void put(const QString &path, const QByteArray &data = "original") {
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly));
    QCOMPARE(file.write(data), data.size());
}
QByteArray read(const QString &path) {
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray{};
}
} // namespace
// Fixture data must share the home filesystem with its isolated XDG Trash.
// /tmp may be a system tmpfs whose trash operation GIO intentionally refuses.
class UndoTest : public QObject {
    Q_OBJECT
  private Q_SLOTS:
    void copyAndUndo() {
        QTemporaryDir dir(fixtureTemplate());
        const auto source = dir.filePath("中文.txt"), dest = dir.filePath("dest");
        QVERIFY(QDir().mkdir(dest));
        put(source);
        Operations op;
        op.run("copy", {uri(source)}, uri(dest));
        QTRY_VERIFY(!op.busy());
        QVERIFY2(op.errors().isEmpty(), qPrintable(op.errors().join("\n")));
        QCOMPARE(op.undoHistory().size(), 1);
        QVERIFY(op.undoText().contains(QStringLiteral("撤销粘贴“中文.txt”")));
        op.undo();
        QTRY_VERIFY(!op.busy());
        QVERIFY2(op.errors().isEmpty(), qPrintable(op.errors().join("\n")));
        QVERIFY(!QFile::exists(dest + "/中文.txt"));
        QCOMPARE(read(source), QByteArray("original"));
        QVERIFY(!op.canUndo());
    }
    void moveAndRename() {
        QTemporaryDir dir(fixtureTemplate());
        const auto source = dir.filePath("a"), dest = dir.filePath("dest");
        QVERIFY(QDir().mkdir(dest));
        put(source);
        Operations op;
        op.run("move", {uri(source)}, uri(dest));
        QTRY_VERIFY(!op.busy());
        QVERIFY(!QFile::exists(source));
        QVERIFY(op.undoText().contains(QStringLiteral("撤销移动")));
        op.run("rename", {uri(dest + "/a")}, {}, "b");
        QTRY_VERIFY(!op.busy());
        QCOMPARE(op.undoHistory().size(), 2);
        op.undo();
        QTRY_VERIFY(!op.busy());
        QVERIFY2(op.errors().isEmpty(), qPrintable(op.errors().join("\n")));
        QCOMPARE(read(dest + "/a"), QByteArray("original"));
        op.undo();
        QTRY_VERIFY(!op.busy());
        QVERIFY2(op.errors().isEmpty(), qPrintable(op.errors().join("\n")));
        QCOMPARE(read(source), QByteArray("original"));
        QVERIFY(!QFile::exists(dest + "/a"));
    }
    void createInsideCreatedDirectory() {
        QTemporaryDir dir(fixtureTemplate());
        Operations op;
        op.run("mkdir", {}, uri(dir.path()), "folder");
        QTRY_VERIFY(!op.busy());
        op.run("touch", {}, uri(dir.filePath("folder")), "file");
        QTRY_VERIFY(!op.busy());
        op.undo();
        QTRY_VERIFY(!op.busy());
        QVERIFY2(op.errors().isEmpty(), qPrintable(op.errors().join("\n")));
        op.undo();
        QTRY_VERIFY(!op.busy());
        QVERIFY2(op.errors().isEmpty(), qPrintable(op.errors().join("\n")));
        QVERIFY(!QFile::exists(dir.filePath("folder")));
    }
    void keepBothUsesActualDestination() {
        QTemporaryDir dir(fixtureTemplate());
        QVERIFY(QDir().mkdir(dir.filePath("dest")));
        put(dir.filePath("a"), "new");
        put(dir.filePath("dest/a"), "old");
        Operations op;
        op.run("copy", {uri(dir.filePath("a"))}, uri(dir.filePath("dest")));
        QTRY_VERIFY(op.waitingConflict());
        op.resolveConflict(2);
        QTRY_VERIFY(!op.busy());
        QVERIFY2(op.errors().isEmpty(), qPrintable(op.errors().join("\n")));
        QCOMPARE(read(dir.filePath("dest/a (2)")), QByteArray("new"));
        op.undo();
        QTRY_VERIFY(!op.busy());
        QVERIFY2(op.errors().isEmpty(), qPrintable(op.errors().join("\n")));
        QCOMPARE(read(dir.filePath("dest/a")), QByteArray("old"));
        QVERIFY(!QFile::exists(dir.filePath("dest/a (2)")));
    }
    void replacementAndPermanentDeletionAreNotRecorded() {
        QTemporaryDir dir(fixtureTemplate());
        QVERIFY(QDir().mkdir(dir.filePath("dest")));
        put(dir.filePath("a"), "new");
        put(dir.filePath("dest/a"), "old");
        Operations op;
        op.run("copy", {uri(dir.filePath("a"))}, uri(dir.filePath("dest")));
        QTRY_VERIFY(op.waitingConflict());
        op.resolveConflict(1);
        QTRY_VERIFY(!op.busy());
        QVERIFY2(op.errors().isEmpty(), qPrintable(op.errors().join("\n")));
        QVERIFY(op.undoHistory().isEmpty());
        QVERIFY(op.status().contains(QStringLiteral("不支持安全撤销")));
        QCOMPARE(read(dir.filePath("dest/a")), QByteArray("new"));
        op.run("delete", {uri(dir.filePath("a"))});
        QTRY_VERIFY(!op.busy());
        QVERIFY(op.undoHistory().isEmpty());
    }
    void changedFilesBlockUndo_data() {
        QTest::addColumn<bool>("directory");
        QTest::newRow("file-content") << false;
        QTest::newRow("directory-extra-child") << true;
    }
    void changedFilesBlockUndo() {
        QFETCH(bool, directory);
        QTemporaryDir dir(fixtureTemplate());
        Operations op;
        op.run(directory ? "mkdir" : "touch", {}, uri(dir.path()), "new");
        QTRY_VERIFY(!op.busy());
        put(dir.filePath(directory ? "new/external" : "new"), "important external change");
        op.undo();
        QTRY_VERIFY(!op.busy());
        QVERIFY(!op.errors().isEmpty());
        QCOMPARE(op.undoHistory().size(), 1);
        QCOMPARE(read(dir.filePath(directory ? "new/external" : "new")),
                 QByteArray("important external change"));
    }
    void occupiedOriginalBlocksEntireBatch() {
        QTemporaryDir dir(fixtureTemplate());
        QVERIFY(QDir().mkdir(dir.filePath("dest")));
        put(dir.filePath("a"));
        put(dir.filePath("b"));
        Operations op;
        op.run("move", {uri(dir.filePath("a")), uri(dir.filePath("b"))}, uri(dir.filePath("dest")));
        QTRY_VERIFY(!op.busy());
        put(dir.filePath("a"), "unrelated");
        op.undo();
        QTRY_VERIFY(!op.busy());
        QCOMPARE(op.completedItems(), 0);
        QVERIFY(!op.errors().isEmpty());
        QCOMPARE(read(dir.filePath("a")), QByteArray("unrelated"));
        QVERIFY(!QFile::exists(dir.filePath("b")));
        QVERIFY(QFile::exists(dir.filePath("dest/b")));
        QVERIFY(QFile::remove(dir.filePath("a")));
        op.undo();
        QTRY_VERIFY(!op.busy());
        QVERIFY2(op.errors().isEmpty(), qPrintable(op.errors().join("\n")));
        QCOMPARE(read(dir.filePath("a")), QByteArray("original"));
        QCOMPARE(read(dir.filePath("b")), QByteArray("original"));
    }
    void parentReplacementAndDanglingLinkBlockUndo() {
        QTemporaryDir dir(fixtureTemplate());
        QVERIFY(QDir().mkdir(dir.filePath("dest")));
        put(dir.filePath("a"));
        Operations op;
        op.run("move", {uri(dir.filePath("a"))}, uri(dir.filePath("dest")));
        QTRY_VERIFY(!op.busy());
        QVERIFY(::symlink("missing", QFile::encodeName(dir.filePath("a")).constData()) == 0);
        op.undo();
        QTRY_VERIFY(!op.busy());
        QVERIFY(!op.errors().isEmpty());
        QVERIFY(QFile::remove(dir.filePath("a")));
        QVERIFY(QDir().rename(dir.filePath("dest"), dir.filePath("old-dest")));
        QVERIFY(QDir().mkdir(dir.filePath("dest")));
        QVERIFY(QFile::rename(dir.filePath("old-dest/a"), dir.filePath("dest/a")));
        op.undo();
        QTRY_VERIFY(!op.busy());
        QVERIFY(!op.errors().isEmpty());
        QVERIFY(QFile::exists(dir.filePath("dest/a")));
    }
    void trashRoundTripAndSameBasename() {
        QTemporaryDir dir(fixtureTemplate());
        const auto path = dir.filePath("same name 中文.txt");
        Operations op;
        put(path, "first");
        op.run("trash", {uri(path)});
        QTRY_VERIFY(!op.busy());
        QVERIFY2(op.errors().isEmpty(), qPrintable(op.errors().join("\n")));
        QCOMPARE(op.undoHistory().size(), 1);
        put(path, "second");
        op.run("trash", {uri(path)});
        QTRY_VERIFY(!op.busy());
        QVERIFY2(op.errors().isEmpty(), qPrintable(op.errors().join("\n")));
        QCOMPARE(op.undoHistory().size(), 2);
        QVERIFY(op.undoText().contains(QStringLiteral("撤销删除")));
        op.undo();
        QTRY_VERIFY(!op.busy());
        QVERIFY2(op.errors().isEmpty(), qPrintable(op.errors().join("\n")));
        QCOMPARE(read(path), QByteArray("second"));
        op.undo();
        QTRY_VERIFY(!op.busy());
        QVERIFY(!op.errors().isEmpty());
        QCOMPARE(read(path), QByteArray("second"));
        QVERIFY(QFile::remove(path));
        op.undo();
        QTRY_VERIFY(!op.busy());
        QVERIFY2(op.errors().isEmpty(), qPrintable(op.errors().join("\n")));
        QCOMPARE(read(path), QByteArray("first"));
    }
    void symlinkDoesNotTouchTarget() {
        QTemporaryDir dir(fixtureTemplate());
        put(dir.filePath("target"));
        QVERIFY(::symlink("target", QFile::encodeName(dir.filePath("link")).constData()) == 0);
        Operations op;
        op.run("trash", {uri(dir.filePath("link"))});
        QTRY_VERIFY(!op.busy());
        QCOMPARE(op.undoHistory().size(), 1);
        op.undo();
        QTRY_VERIFY(!op.busy());
        QVERIFY2(op.errors().isEmpty(), qPrintable(op.errors().join("\n")));
        QVERIFY(QFileInfo(dir.filePath("link")).isSymLink());
        QCOMPARE(read(dir.filePath("target")), QByteArray("original"));
    }
    void sessionLimitAndClearDoNotDelete() {
        QTemporaryDir dir(fixtureTemplate());
        Operations op;
        for (int i = 0; i < 7; ++i) {
            op.run("touch", {}, uri(dir.path()), QString::number(i));
            QTRY_VERIFY(!op.busy());
        }
        QCOMPARE(op.undoHistory().size(), 5);
        for (int i = 6; i >= 2; --i) {
            QVERIFY(op.undoText().contains(QString::number(i)));
            op.undo();
            QTRY_VERIFY(!op.busy());
            QVERIFY2(op.errors().isEmpty(), qPrintable(op.errors().join("\n")));
        }
        QVERIFY(!op.canUndo());
        QVERIFY(QFile::exists(dir.filePath("0")));
        op.run("touch", {}, uri(dir.path()), "clear");
        QTRY_VERIFY(!op.busy());
        op.clearUndoHistory();
        QVERIFY(!op.canUndo());
        QVERIFY(QFile::exists(dir.filePath("clear")));
        Operations anotherSession;
        QVERIFY(!anotherSession.canUndo());
    }
    void atomicRestoreNeverReplacesLateDestination() {
        QTemporaryDir dir(fixtureTemplate());
        put(dir.filePath("current"), "recover me");
        const auto item = nook::undoItem(dir.filePath("current"), dir.filePath("original"), nullptr);
        QVERIFY(!item.fingerprint.isEmpty());
        // Simulate another process creating the destination after preflight.
        put(dir.filePath("original"), "unrelated late arrival");
        GError *error = nullptr;
        QVERIFY(!nook::undoMove(item, &error));
        QVERIFY(error != nullptr);
        g_clear_error(&error);
        QCOMPARE(read(dir.filePath("current")), QByteArray("recover me"));
        QCOMPARE(read(dir.filePath("original")), QByteArray("unrelated late arrival"));
        QVERIFY(QFile::remove(dir.filePath("original")));
        QVERIFY(nook::undoMove(item, &error));
        QVERIFY(error == nullptr);
        QCOMPARE(read(dir.filePath("original")), QByteArray("recover me"));
    }
    void lossyFilenameNeverRecordsAnUnrelatedObject() {
        QTemporaryDir dir(fixtureTemplate());
        QVERIFY(QDir().mkdir(dir.filePath("dest")));
        const QByteArray raw = QFile::encodeName(dir.path()) + "/raw-\xff";
        const QByteArray target = QFile::encodeName(dir.filePath("dest")) + "/raw-\xff";
        const int fd = ::open(raw.constData(), O_CREAT | O_WRONLY | O_EXCL, 0600);
        QVERIFY(fd >= 0);
        QCOMPARE(::write(fd, "raw", 3), 3);
        ::close(fd);
        char *encoded = g_filename_to_uri(raw.constData(), nullptr, nullptr);
        QVERIFY(encoded != nullptr);
        const auto rawUri = QString::fromUtf8(encoded);
        g_free(encoded);
        QVERIFY(nook::undoLocalPath(rawUri).isEmpty());
        const auto lookalike = dir.filePath(QString::fromUtf8("dest/raw-\xef\xbf\xbd"));
        put(lookalike, "unrelated unicode name");
        Operations op;
        op.run("copy", {rawUri}, uri(dir.filePath("dest")));
        QTRY_VERIFY(!op.busy());
        QVERIFY2(op.errors().isEmpty(), qPrintable(op.errors().join("\n")));
        QVERIFY(op.undoHistory().isEmpty());
        struct stat st{};
        QVERIFY(::lstat(target.constData(), &st) == 0);
        op.undo();
        QCOMPARE(read(lookalike), QByteArray("unrelated unicode name"));
        QVERIFY(::unlink(target.constData()) == 0);
        op.run("move", {rawUri}, uri(dir.filePath("dest")));
        QTRY_VERIFY(!op.busy());
        QVERIFY2(op.errors().isEmpty(), qPrintable(op.errors().join("\n")));
        QVERIFY(op.undoHistory().isEmpty());
        QCOMPARE(read(lookalike), QByteArray("unrelated unicode name"));
        // A tree containing a byte-only name is also rejected, never aliased.
        QVERIFY(nook::undoFingerprint(dir.filePath("dest"), nullptr).isEmpty());
    }
    void crossFilesystemMoveIsExplicitlyUntracked() {
        QTemporaryDir dir(fixtureTemplate()), other;
        struct stat a{}, b{};
        QVERIFY(::stat(QFile::encodeName(dir.path()).constData(), &a) == 0);
        QVERIFY(::stat(QFile::encodeName(other.path()).constData(), &b) == 0);
        if (a.st_dev == b.st_dev)
            QSKIP("No second filesystem available for cross-volume assertion");
        put(dir.filePath("a"));
        Operations op;
        op.run("move", {uri(dir.filePath("a"))}, uri(other.path()));
        QTRY_VERIFY(!op.busy());
        QVERIFY2(op.errors().isEmpty(), qPrintable(op.errors().join("\n")));
        QCOMPARE(read(other.filePath("a")), QByteArray("original"));
        QVERIFY(!QFile::exists(dir.filePath("a")));
        QVERIFY(op.undoHistory().isEmpty());
        QVERIFY(op.status().contains(QStringLiteral("不支持安全撤销")));
    }
    void overlappingSelections_data() {
        QTest::addColumn<QString>("kind");
        QTest::addColumn<bool>("childFirst");
        for (const auto &kind : {QString("move"), QString("trash")}) {
            QTest::newRow(qPrintable(kind + "-child-first")) << kind << true;
            QTest::newRow(qPrintable(kind + "-parent-first")) << kind << false;
        }
    }
    void overlappingSelections() {
        QFETCH(QString, kind);
        QFETCH(bool, childFirst);
        QTemporaryDir dir(fixtureTemplate());
        QVERIFY(QDir().mkpath(dir.filePath("P/sub")));
        QVERIFY(QDir().mkdir(dir.filePath("dest")));
        put(dir.filePath("P/sub/x"));
        QStringList sources{uri(dir.filePath("P/sub/x")), uri(dir.filePath("P/sub")), uri(dir.filePath("P"))};
        if (!childFirst)
            std::reverse(sources.begin(), sources.end());
        Operations op;
        op.run(kind, sources, uri(dir.filePath("dest")));
        QTRY_VERIFY(!op.busy());
        QVERIFY2(op.errors().isEmpty(), qPrintable(op.errors().join("\n")));
        QCOMPARE(op.completedItems(), 1);
        QCOMPARE(op.undoHistory().size(), 1);
        QVERIFY(!QFile::exists(dir.filePath("P")));
        op.undo();
        QTRY_VERIFY(!op.busy());
        QVERIFY2(op.errors().isEmpty(), qPrintable(op.errors().join("\n")));
        QCOMPARE(read(dir.filePath("P/sub/x")), QByteArray("original"));
    }
    void completedItemsSurviveFailureAndCancellation() {
        QTemporaryDir dir(fixtureTemplate());
        QVERIFY(QDir().mkdir(dir.filePath("dest")));
        put(dir.filePath("good"));
        put(dir.filePath("conflict"));
        put(dir.filePath("dest/conflict"), "old");
        Operations op;
        op.run("copy",
               {uri(dir.filePath("missing")), uri(dir.filePath("good")), uri(dir.filePath("conflict"))},
               uri(dir.filePath("dest")));
        QTRY_VERIFY(op.waitingConflict());
        op.cancel();
        QTRY_VERIFY(!op.busy());
        QCOMPARE(op.completedItems(), 1);
        QCOMPARE(op.undoHistory().size(), 1);
        op.undo();
        QTRY_VERIFY(!op.busy());
        QVERIFY2(op.errors().isEmpty(), qPrintable(op.errors().join("\n")));
        QVERIFY(!QFile::exists(dir.filePath("dest/good")));
        QCOMPARE(read(dir.filePath("dest/conflict")), QByteArray("old"));
    }
};
int main(int argc, char **argv) {
    QTemporaryDir env(QDir::currentPath() + "/undo-env-XXXXXX");
    if (!env.isValid())
        return 1;
    qputenv("XDG_DATA_HOME", QFile::encodeName(env.filePath("data")));
    qputenv("XDG_CONFIG_HOME", QFile::encodeName(env.filePath("config")));
    qputenv("XDG_CACHE_HOME", QFile::encodeName(env.filePath("cache")));
    qputenv("GIO_USE_VFS", "local");
    QCoreApplication app(argc, argv);
    UndoTest test;
    return QTest::qExec(&test, argc, argv);
}
#include "undo_test.moc"
