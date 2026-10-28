// Standalone QtTest target: link nook_core and Qt6::Test, with AUTOMOC enabled.
// This executable also implements --thumbnail-worker, just like the application, so provider
// tests exercise the actual asynchronous subprocess/JSON/cache path (not a mocked response).
#include "files.h"
#include "preview.h"
#include <QFile>
#include <QGuiApplication>
#include <QImage>
#include <QImageReader>
#include <QJsonDocument>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQuickTextureFactory>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTextStream>
#include <QtTest>
#include <unistd.h>

class ThumbnailTest : public QObject {
    Q_OBJECT
    static QString uri(const QString &path) {
        return nook::normalizeUri(path);
    }
    static void put(const QString &path, const QByteArray &bytes) {
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        QCOMPARE(file.write(bytes), bytes.size());
    }
    static QString id(const QString &path, const QString &revision = "1232026-01-01 12:00") {
        return QString::fromLatin1(QUrl::toPercentEncoding(uri(path))) + '|' + revision;
    }
    static void unavailable(const QVariantMap &result) {
        QVERIFY2(!result.value("error").toString().isEmpty(), qPrintable(result.value("image").toString()));
        QVERIFY(result.value("image").toString().isEmpty());
    }
    static void responseUnavailable(ThumbnailProvider &provider, const QString &source, bool cancel = false) {
        auto *response = provider.requestImageResponse(source, QSize(500, 500));
        QSignalSpy done(response, &QQuickImageResponse::finished);
        if (cancel)
            response->cancel();
        QTRY_COMPARE_WITH_TIMEOUT(done.size(), 1, 10000);
        QVERIFY(!response->errorString().isEmpty());
        auto *texture = response->textureFactory();
        QVERIFY(texture == nullptr);
        delete texture;
        delete response;
    }
  private Q_SLOTS:
    void hoverMetadataOnly() {
        Preview preview;
        for (const auto &name : {"hello.txt", "test.cpp", "test.ts", "test.md", "test.markdown", "test.json",
                                 "test.csv", "test.pdf", "test.png", "test.yaml", "README"}) {
            QVariantMap entry{{"fileName", name}, {"fileUri", "file:///nonexistent/" + QString(name)}};
            if (QString(name) == "README")
                entry["mimeType"] = "text/plain";
            QVERIFY2(preview.hoverEligible(entry), name);
            entry["mimeType"] = "video/mp4";
            QVERIFY(!preview.hoverEligible(entry));
            entry["mimeType"] = "audio/flac; codecs=flac";
            QVERIFY(!preview.hoverEligible(entry));
            entry["mimeType"] = "text/plain";
            entry["folder"] = true;
            QVERIFY(!preview.hoverEligible(entry));
        }
        for (const auto &name : {"test.mp4", "test.mkv", "test.mp3", "test.opus", "test.docx", "test.epub",
                                 "unknown.nook-unsupported"})
            QVERIFY2(!preview.hoverEligible({{"fileName", name}}), name);
        QVERIFY(!preview.hoverEligible({{"fileName", "test.txt"}, {"iconName", "music"}}));
        QVERIFY(!preview.hoverEligible({{"fileName", "test.txt"}, {"contentType", "inode/directory"}}));
        QVERIFY(preview.hoverEligible({{"fileName", "unknown"}, {"mimeType", "application/pdf"}}));
        QVERIFY(!nook::iconFor("unknown.nook-unsupported", false).isEmpty());
        QCOMPARE(nook::iconFor("test.mp4", false), QString("video"));
        QCOMPARE(nook::iconFor("test.opus", false), QString("music"));
        // A PDF image plugin (QtPdf from qt6-webengine) lists "pdf" as a readable image format;
        // PDFs must still be documents so they take the poppler preview path.
        for (const auto &format : QImageReader::supportedImageFormats())
            if (format != "pdf")
                QCOMPARE(nook::iconFor("picture." + QString::fromLatin1(format), false), QString("image"));
        QCOMPARE(nook::iconFor("book.pdf", false), QString("document"));
        QCOMPARE(nook::iconFor("BOOK.PDF", false), QString("document"));
    }
    void imageAndEncodedSource() {
        QTemporaryDir dir;
        const auto path = dir.filePath("中文 | % # ?.png");
        QImage image(640, 480, QImage::Format_ARGB32);
        image.fill(Qt::red);
        QVERIFY(image.save(path));
        const auto result = makePreview(uri(path), 1, true);
        QVERIFY(result.value("error").toString().isEmpty());
        QImage cached(QUrl(result.value("image").toString()).toLocalFile());
        QCOMPARE(cached.size(), QSize(256, 192));
        ThumbnailProvider provider;
        for (const auto &revision : {QString("6401720000000"), QString("6402026-01-01 12:00")}) {
            auto *response = provider.requestImageResponse(id(path, revision), QSize(1024, 1024));
            QSignalSpy done(response, &QQuickImageResponse::finished);
            QTRY_COMPARE_WITH_TIMEOUT(done.size(), 1, 10000);
            QVERIFY2(response->errorString().isEmpty(), qPrintable(response->errorString()));
            auto *texture = response->textureFactory();
            QVERIFY(texture);
            QCOMPARE(texture->image().size(), QSize(256, 192));
            QCOMPARE(texture->image().pixelColor(100, 100), QColor(Qt::red));
            delete texture;
            delete response;
        }
        responseUnavailable(provider, id(path), true);
        // Cached files must be validated, not accepted merely because they exist.
        put(QUrl(result.value("image").toString()).toLocalFile(), "broken cache");
        unavailable(makePreview(uri(path), 1, true));
    }
    void qmlUrlAndFallbackContract() {
        QTemporaryDir dir;
        const auto path = dir.filePath("literal %23 # ? | 中文.png");
        QImage pixels(32, 32, QImage::Format_ARGB32);
        pixels.fill(Qt::blue);
        QVERIFY(pixels.save(path));
        QQmlEngine engine;
        engine.addImageProvider("thumb", new ThumbnailProvider);
        QQmlComponent component(&engine);
        component.setData("import QtQuick\nImage { width: 64; height: 64 }", QUrl());
        QScopedPointer<QObject> image(component.create());
        QVERIFY2(image, qPrintable(component.errorString()));
        image->setProperty("source", "image://thumb/" + id(path));
        QTRY_COMPARE_WITH_TIMEOUT(image->property("status").toInt(), 1, 10000); // Image.Ready
        pixels.fill(Qt::transparent);
        const auto transparent = dir.filePath("transparent.png");
        QVERIFY(pixels.save(transparent));
        image->setProperty("source", "image://thumb/" + id(transparent));
        QTRY_COMPARE_WITH_TIMEOUT(image->property("status").toInt(), 3, 10000); // Image.Error, not Ready
    }
    void cacheEntryBudget() {
        const auto cache = QStandardPaths::writableLocation(QStandardPaths::CacheLocation) + "/previews";
        QVERIFY(QDir().mkpath(cache));
        for (int i = 0; i < 2050; ++i)
            put(cache + '/' + QString::number(i) + ".png", "x");
        QTemporaryDir dir;
        QImage image(2, 2, QImage::Format_RGB32);
        image.fill(Qt::white); // Real opaque solid-color images are valid, not fake placeholders.
        const auto path = dir.filePath("solid.png");
        QVERIFY(image.save(path));
        QVERIFY(makePreview(uri(path), 1, true).value("error").toString().isEmpty());
        QVERIFY(QDir(cache).entryList(QDir::Files).size() <= 2048);
    }
    void fallbackForInvalidAndTransparent() {
        QTemporaryDir dir;
        ThumbnailProvider provider;
        const auto transparent = dir.filePath("transparent.png");
        QImage image(32, 32, QImage::Format_ARGB32);
        image.fill(Qt::transparent);
        QVERIFY(image.save(transparent));
        put(dir.filePath("broken.png"), "not a PNG");
        put(dir.filePath("broken.mp4"), "not a video");
        put(dir.filePath("unknown.nook-unsupported"), "plain data");
        for (const auto &name :
             {"transparent.png", "broken.png", "broken.mp4", "unknown.nook-unsupported", "missing.png"}) {
            const auto path = dir.filePath(name);
            unavailable(makePreview(uri(path), 1, true));
            responseUnavailable(provider, id(path));
        }
        responseUnavailable(provider, id(dir.path()));
    }
    void truncatedContainers() {
        QTemporaryDir dir;
        for (const auto &format : {QByteArray("JPEG"), QByteArray("PNG"), QByteArray("WEBP")}) {
            const auto path = dir.filePath("truncated." + QString::fromLatin1(format).toLower());
            QImage image(100, 100, QImage::Format_RGB32);
            image.fill(Qt::green);
            if (!image.save(path, format.constData()))
                continue;
            QFile file(path);
            QVERIFY(file.open(QIODevice::ReadWrite));
            QVERIFY(file.resize(file.size() - (format == "JPEG" ? 100 : 8)));
            file.close();
            unavailable(makePreview(uri(path), 1, true));
            ThumbnailProvider provider;
            responseUnavailable(provider, id(path));
        }
    }
    void videoFirstFrame() {
        if (!QFile::exists("/usr/bin/ffmpeg"))
            QSKIP("ffmpeg is optional; absence is covered by the fallback path");
        QTemporaryDir dir;
        // A real, single-frame YUV4MPEG stream; no fixture-generation subprocess needed.
        // The .mp4 name exercises filename eligibility; ffmpeg probes the actual container.
        const auto path = dir.filePath("first | % # 中文.mp4");
        put(path, QByteArray("YUV4MPEG2 W16 H16 F1:1 Ip A1:1 C420jpeg\nFRAME\n") + QByteArray(256, char(90)) +
                      QByteArray(64, char(128)) + QByteArray(64, char(128)));
        const auto result = makePreview(uri(path), 1, true);
        QVERIFY2(result.value("error").toString().isEmpty(), qPrintable(result.value("error").toString()));
        QImage poster(QUrl(result.value("image").toString()).toLocalFile());
        QVERIFY(!poster.isNull());
        QVERIFY(poster.width() <= 256 && poster.height() <= 256);
        ThumbnailProvider provider;
        auto *response = provider.requestImageResponse(id(path), QSize(256, 256));
        QSignalSpy done(response, &QQuickImageResponse::finished);
        QTRY_COMPARE_WITH_TIMEOUT(done.size(), 1, 10000);
        QVERIFY2(response->errorString().isEmpty(), qPrintable(response->errorString()));
        auto *texture = response->textureFactory();
        QVERIFY(texture);
        delete texture;
        delete response;
        // Oversized decoder input must fail, not allocate according to this header.
        const auto huge = dir.filePath("huge.mp4");
        put(huge, "YUV4MPEG2 W100000 H100000 F1:1 Ip A1:1 C420jpeg\nFRAME\n");
        unavailable(makePreview(uri(huge), 1, true));
    }
};

int main(int argc, char **argv) {
    const bool worker = argc > 1 && QByteArray(argv[1]) == "--thumbnail-worker";
    QTemporaryDir cache;
    if (worker)
        ::setpgid(0, 0);
    else
        qputenv("XDG_CACHE_HOME", cache.path().toUtf8());
    QGuiApplication app(argc, argv);
    app.setApplicationName("NookThumbnailTest");
    QImageReader::setAllocationLimit(96);
    if (worker) {
        QTextStream(stdout) << QJsonDocument::fromVariant(
                                   makePreview(QString::fromLocal8Bit(argv[2]), 1, true))
                                   .toJson(QJsonDocument::Compact)
                            << Qt::endl;
        return 0;
    }
    ThumbnailTest test;
    return QTest::qExec(&test, argc, argv);
}
#include "thumbnail_test.moc"
