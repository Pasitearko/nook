#include "preview.h"
#include "storage.h"
#include "files.h"
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QFontDatabase>
#include <QImageReader>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMimeDatabase>
#include <QPointer>
#include <QQuickTextureFactory>
#include <QRegularExpression>
#include <QSaveFile>
#include <QStandardPaths>
#include <QSyntaxHighlighter>
#include <QTextDocument>
#include <QUrl>
#include <QtConcurrent>
#include <signal.h>
#include <sys/resource.h>
#include <unistd.h>

namespace {
QByteArray command(const QString &program, const QStringList &args, int timeout, QString *error) {
    QProcess process;
    if (program == "/usr/bin/ffmpeg") {
        process.setChildProcessModifier([] {
            // max_alloc is per allocation; bound aggregate decoder/reference-frame memory as well.
            const struct rlimit memory{768ull * 1024 * 1024, 768ull * 1024 * 1024};
            if (::setrlimit(RLIMIT_AS, &memory) != 0)
                ::_exit(1);
        });
    }
    QByteArray output, diagnostic;
    QElapsedTimer elapsed;
    elapsed.start();
    process.start(program, args, QIODevice::ReadOnly);
    if (!process.waitForStarted(1000)) {
        *error = QStringLiteral("预览工具缺失：%1").arg(program);
        return {};
    }
    // Drain while running; truncating only after waitForFinished allows unlimited QProcess buffering.
    do {
        process.waitForFinished(40);
        output += process.readAllStandardOutput();
        diagnostic += process.readAllStandardError();
        if (output.size() > 1024 * 1024 || diagnostic.size() > 64 * 1024 || elapsed.elapsed() > timeout) {
            process.kill();
            process.waitForFinished(1000);
            *error = QStringLiteral("预览处理超时或输出超过限制：%1").arg(program);
            return {};
        }
    } while (process.state() != QProcess::NotRunning);
    if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0 ||
        (program == "/usr/bin/ffmpeg" && !diagnostic.trimmed().isEmpty())) {
        *error = QStringLiteral("无法读取此文件：%1").arg(QString::fromUtf8(diagnostic).left(600));
        return {};
    }
    return output;
}
bool completeImage(const QString &path, const QByteArray &format) {
    // Some Qt plugins salvage truncated files without setting reader.error() (notably libjpeg).
    // Check container terminators/declared lengths before accepting such a salvaged opaque image.
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return false;
    const auto header = file.read(12);
    if (format == "jpeg" || format == "jpg") {
        if (file.size() < 4 || !header.startsWith("\xff\xd8") || !file.seek(file.size() - 2))
            return false;
        return file.read(2) == QByteArray::fromHex("ffd9");
    }
    if (format == "png") {
        if (file.size() < 20 || !file.seek(file.size() - 12))
            return false;
        return file.read(12) == QByteArray::fromHex("0000000049454e44ae426082");
    }
    if (format == "gif") {
        return file.size() > 12 && file.seek(file.size() - 1) && file.read(1) == QByteArray(";");
    }
    if (format == "webp") {
        if (header.size() != 12 || !header.startsWith("RIFF") || header.mid(8) != "WEBP")
            return false;
        quint32 length = 0;
        for (int i = 0; i < 4; ++i)
            length |= quint32(quint8(header[4 + i])) << (8 * i);
        return qint64(length) + 8 == file.size();
    }
    return true;
}
bool visibleImage(const QImage &image) {
    if (image.isNull())
        return false;
    if (!image.hasAlphaChannel())
        return true;
    const auto rgba = image.convertToFormat(QImage::Format_ARGB32);
    for (int y = 0; y < rgba.height(); ++y) {
        const auto *pixels = reinterpret_cast<const QRgb *>(rgba.constScanLine(y));
        for (int x = 0; x < rgba.width(); ++x)
            if (qAlpha(pixels[x]) != 0)
                return true;
    }
    return false;
}
QImage cachedImage(const QString &path, const QSize &bound) {
    QImageReader reader(path);
    const auto size = reader.size();
    if (!size.isValid() || size.width() > bound.width() || size.height() > bound.height() ||
        !completeImage(path, reader.format()))
        return {};
    auto image = reader.read();
    return reader.error() == QImageReader::UnknownError && visibleImage(image) ? image : QImage();
}
class SafeDocument : public QTextDocument {
  protected:
    QVariant loadResource(int, const QUrl &) override {
        return {};
    }
};
class CodeHighlighter : public QSyntaxHighlighter {
  public:
    explicit CodeHighlighter(QTextDocument *doc) : QSyntaxHighlighter(doc) {}

  protected:
    void highlightBlock(const QString &line) override {
        const QList<QPair<QRegularExpression, QColor>> rules = {
            {QRegularExpression("\\b(?:class|struct|int|bool|void|auto|const|return|if|else|for|while|def|"
                                "import|from|fn|let|pub|use|function|var|true|false|null|None)\\b"),
             QColor("#709dc2")},
            {QRegularExpression("\"(?:[^\"\\\\]|\\\\.)*\"|'(?:[^'\\\\]|\\\\.)*'"), QColor("#b29967")},
            {QRegularExpression("//[^\\n]*|#[^\\n]*"), QColor("#769489")}};
        for (const auto &rule : rules) {
            auto matches = rule.first.globalMatch(line);
            while (matches.hasNext()) {
                const auto match = matches.next();
                setFormat(match.capturedStart(), match.capturedLength(), rule.second);
            }
        }
    }
};
QString csvHtml(const QString &text) {
    QString html = "<table cellspacing='0' cellpadding='6' border='1'>";
    QString cell;
    QStringList row;
    bool quoted = false;
    int rows = 0;
    auto endRow = [&] {
        row.append(cell);
        cell.clear();
        html += "<tr>";
        for (const auto &value : row.mid(0, 30))
            html += "<td>" + value.toHtmlEscaped() + "</td>";
        html += "</tr>";
        row.clear();
        ++rows;
    };
    for (int i = 0; i < text.size() && rows < 300; ++i) {
        QChar ch = text[i];
        if (ch == '"') {
            if (quoted && i + 1 < text.size() && text[i + 1] == '"') {
                cell += '"';
                ++i;
            } else
                quoted = !quoted;
        } else if (ch == ',' && !quoted) {
            row.append(cell);
            cell.clear();
        } else if (ch == '\n' && !quoted)
            endRow();
        else if (ch != '\r')
            cell += ch;
    }
    if ((!cell.isEmpty() || !row.isEmpty()) && rows < 300)
        endRow();
    return html + "</table><p>最多显示 300 行、30 列。</p>";
}
QString cacheDirectory() {
    QString dir = nook::storageDirectory(nook::Storage::Cache) + "/previews";
    QDir().mkpath(dir);
    QFile::setPermissions(dir, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
    return dir;
}
void trimCache(const QString &directory) {
    auto files = QDir(directory).entryInfoList(QDir::Files, QDir::Time);
    qint64 total = 0;
    int count = 0;
    for (const auto &f : files) {
        total += f.size();
        if (++count > 2048 || total > 128ll * 1024 * 1024)
            QFile::remove(f.absoluteFilePath());
    }
}
} // namespace
QVariantMap makePreview(const QString &uri, int page, bool thumbnail) {
    QString path = nook::localPath(uri);
    QFileInfo info(path);
    QVariantMap result{{"title", info.fileName()},
                       {"subtitle", nook::formatSize(info.size())},
                       {"mode", "info"},
                       {"text", ""},
                       {"pages", 1}};
    if (path.isEmpty() || !info.exists()) {
        result["error"] = QStringLiteral("文件不存在、挂载盘离线或该位置不支持预览");
        return result;
    }
    if (info.isDir()) {
        result["text"] = QStringLiteral("文件夹 · 双击打开查看内容");
        return result;
    }
    if (!info.isFile()) {
        result["error"] = QStringLiteral("仅预览普通文件，设备和特殊文件不会被读取");
        return result;
    }
    const QString kind = nook::iconFor(info.fileName(), false), ext = info.suffix().toLower();
    QString cache = cacheDirectory();
    QByteArray key = (uri + '|' + QString::number(info.size()) + '|' +
                      QString::number(info.lastModified().toMSecsSinceEpoch()) + '|' + QString::number(page) +
                      (thumbnail ? "|thumb-v2" : "|full-v2"))
                         .toUtf8();
    QString output = cache + '/' +
                     QString::fromLatin1(QCryptographicHash::hash(key, QCryptographicHash::Sha256).toHex()) +
                     ".png";
    QString error;
    QImageReader reader(path);
    // PDFs always go through poppler (first page only, no file-size cap). A PDF image plugin
    // (QtPdf, shipped with qt6-webengine) would otherwise claim them as "images" and reject
    // large documents or ones whose page size it cannot report.
    if (ext != "pdf" && (kind == "image" || reader.canRead())) {
        if (!QFile::exists(output)) {
            reader.setAutoTransform(true);
            QSize dimensions = reader.size();
            if (!dimensions.isValid() || dimensions.width() > 40000 || dimensions.height() > 40000 ||
                info.size() > 256ll * 1024 * 1024 ||
                qint64(dimensions.width()) * dimensions.height() > 100000000 ||
                (!reader.supportsOption(QImageIOHandler::ScaledSize) &&
                 qint64(dimensions.width()) * dimensions.height() > 24000000)) {
                result["error"] = QStringLiteral("图片损坏或尺寸超过预览限制");
                return result;
            }
            reader.setScaledSize(
                dimensions.scaled(thumbnail ? QSize(256, 256) : QSize(1400, 1000), Qt::KeepAspectRatio));
            // QImageReader can clear format() after read(); validate the container before decoding.
            if (thumbnail && !completeImage(path, reader.format())) {
                result["error"] = QStringLiteral("图片文件不完整或容器损坏");
                return result;
            }
            auto image = reader.read();
            if (image.isNull() || reader.error() != QImageReader::UnknownError ||
                (thumbnail && !visibleImage(image))) {
                result["error"] =
                    QStringLiteral("图片损坏、不透明内容为空或格式不受支持：%1").arg(reader.errorString());
                return result;
            }
            QSaveFile file(output);
            if (!file.open(QIODevice::WriteOnly) || !image.save(&file, "PNG") || !file.commit()) {
                result["error"] = QStringLiteral("无法写入预览缓存");
                return result;
            }
            result["subtitle"] = QStringLiteral("%1 × %2 · %3")
                                     .arg(dimensions.width())
                                     .arg(dimensions.height())
                                     .arg(nook::formatSize(info.size()));
        }
        result["mode"] = "image";
        result["image"] = QUrl::fromLocalFile(output).toString();
    } else if (ext == "pdf") {
        auto metadata = command("/usr/bin/pdfinfo", {path}, 2500, &error);
        auto match = QRegularExpression("Pages:\\s+(\\d+)").match(QString::fromUtf8(metadata));
        int pages = match.hasMatch() ? match.captured(1).toInt() : 1;
        result["pages"] = pages;
        if (page < 1 || page > pages)
            error = QStringLiteral("页码超出文档范围");
        if (error.isEmpty() && !QFile::exists(output))
            command("/usr/bin/pdftoppm",
                    {"-f", QString::number(page), "-l", QString::number(page), "-singlefile", "-scale-to",
                     thumbnail ? "256" : "1400", "-png", path, output.chopped(4)},
                    8000, &error);
        // poppler's stderr is a wall of parser diagnostics; say what it means instead.
        if (error.startsWith(QStringLiteral("无法读取此文件")))
            error = QStringLiteral("无法渲染此 PDF：文件可能已损坏、已加密，或并不是有效的 PDF");
        result["mode"] = "image";
        result["image"] = QUrl::fromLocalFile(output).toString();
        result["subtitle"] = QStringLiteral("PDF · 第 %1 / %2 页").arg(page).arg(pages);
    } else if (kind == "video" || kind == "music") {
        // Grid posters need no metadata probe: one bounded decode of the first video frame.
        if (!thumbnail) {
            auto data =
                command("/usr/bin/ffprobe",
                        {"-v", "error", "-protocol_whitelist", "file,pipe", "-show_entries",
                         "format=duration,format_name:stream=codec_name,width,height", "-of", "json", path},
                        4000, &error);
            auto json = QJsonDocument::fromJson(data).object();
            result["text"] = QString::fromUtf8(QJsonDocument(json).toJson(QJsonDocument::Indented));
        }
        if (kind == "video" && error.isEmpty()) {
            if (!QFile::exists(output)) {
                auto bytes = command("/usr/bin/ffmpeg",
                                     {"-v",
                                      "error",
                                      "-nostdin",
                                      "-xerror",
                                      "-max_alloc",
                                      "67108864",
                                      "-threads",
                                      "1",
                                      "-filter_threads",
                                      "1",
                                      "-protocol_whitelist",
                                      "file,pipe",
                                      "-probesize",
                                      "1048576",
                                      "-analyzeduration",
                                      "1000000",
                                      "-max_pixels",
                                      "24000000",
                                      "-err_detect",
                                      "explode",
                                      "-i",
                                      path,
                                      "-map",
                                      "0:v:0",
                                      "-an",
                                      "-sn",
                                      "-dn",
                                      "-frames:v",
                                      "1",
                                      "-vf",
                                      thumbnail ? "scale=256:256:force_original_aspect_ratio=decrease"
                                                : "scale=640:360:force_original_aspect_ratio=decrease",
                                      "-threads",
                                      "1",
                                      "-c:v",
                                      "png",
                                      "-f",
                                      "image2pipe",
                                      "pipe:1"},
                                     4000, &error);
                const auto image = QImage::fromData(bytes, "PNG");
                if (error.isEmpty() && !visibleImage(image))
                    error = QStringLiteral("无法提取视频画面");
                if (error.isEmpty()) {
                    QSaveFile file(output);
                    if (!file.open(QIODevice::WriteOnly) || !image.save(&file, "PNG") || !file.commit())
                        error = QStringLiteral("无法写入预览缓存");
                }
            }
            if (error.isEmpty()) {
                result["mode"] = "image";
                result["image"] = QUrl::fromLocalFile(output).toString();
            }
        }
    } else if (!thumbnail) {
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly))
            error = file.errorString();
        else {
            QByteArray bytes = file.read(256 * 1024);
            bool truncated = info.size() > bytes.size();
            if (bytes.contains('\0') && !bytes.startsWith("\xFF\xFE") && !bytes.startsWith("\xFE\xFF"))
                result["text"] = QStringLiteral("此格式暂不提供内容预览，可使用系统应用打开。");
            else {
                QString text;
                if (bytes.startsWith("\xFF\xFE") || bytes.startsWith("\xFE\xFF")) {
                    bool le = bytes.startsWith("\xFF\xFE");
                    for (int i = 2; i + 1 < bytes.size(); i += 2) {
                        auto a = quint8(bytes[i]), b = quint8(bytes[i + 1]);
                        text += QChar(le ? a | (b << 8) : (a << 8) | b);
                    }
                } else
                    text = QString::fromUtf8(bytes);
                if (ext == "md" || ext == "markdown") {
                    SafeDocument doc;
                    doc.setMarkdown(text,
                                    QTextDocument::MarkdownFeatures(QTextDocument::MarkdownDialectGitHub) |
                                        QTextDocument::MarkdownFeatures(QTextDocument::MarkdownNoHTML));
                    QString html = doc.toHtml();
                    html.remove(
                        QRegularExpression("<img\\b[^>]*>", QRegularExpression::CaseInsensitiveOption));
                    result["mode"] = "rich";
                    result["text"] = html;
                } else if (ext == "csv") {
                    result["mode"] = "rich";
                    result["text"] = csvHtml(text);
                } else if (ext == "json") {
                    QJsonParseError parse;
                    auto doc = QJsonDocument::fromJson(bytes, &parse);
                    result["mode"] = "text";
                    result["text"] =
                        parse.error == QJsonParseError::NoError ? QString::fromUtf8(doc.toJson()) : text;
                    if (parse.error != QJsonParseError::NoError)
                        result["notice"] = QStringLiteral("JSON 不完整或格式有误，显示原文");
                } else if (kind == "code") {
                    SafeDocument doc;
                    doc.setDefaultFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
                    doc.setPlainText(text);
                    CodeHighlighter highlighter(&doc);
                    highlighter.rehighlight();
                    result["mode"] = "rich";
                    result["text"] = doc.toHtml();
                } else {
                    result["mode"] = "text";
                    result["text"] = text;
                }
                if (truncated)
                    result["notice"] = QStringLiteral("仅显示前 256 KB，完整文件可用系统应用打开");
            }
        }
    }
    if (thumbnail) {
        if (result.value("image").toString().isEmpty())
            error = error.isEmpty() ? QStringLiteral("此格式不支持缩略图") : error;
        else if (cachedImage(output, QSize(256, 256)).isNull()) {
            QFile::remove(output);
            error = QStringLiteral("缩略图损坏或完全透明");
        }
    }
    if (!error.isEmpty()) {
        result["error"] = error;
        result.remove("image");
        result["mode"] = "info";
    }
    trimCache(cache);
    return result;
}
bool Preview::hoverEligible(const QVariantMap &entry) const {
    if (entry.value("folder").toBool() || entry.value("directory").toBool() ||
        entry.value("isDirectory").toBool())
        return false;
    // Explicit MIME/type metadata always wins, even if the filename suggests text or an image.
    bool supportedMime = false;
    static const auto imageMimes = QImageReader::supportedMimeTypes();
    for (const auto &key : {"mimeType", "mime", "contentType", "kind"}) {
        const auto mime = entry.value(key).toString().section(';', 0, 0).trimmed().toLower();
        if (mime.startsWith("video/") || mime.startsWith("audio/") || mime == "inode/directory" ||
            mime == "application/ogg")
            return false;
        supportedMime = supportedMime || mime.startsWith("text/") || mime == "application/pdf" ||
                        mime == "application/json" || mime == "application/xml" ||
                        mime == "application/javascript" || imageMimes.contains(mime.toLatin1());
    }
    const auto icon = entry.value("iconName").toString();
    if (icon == "folder" || icon == "video" || icon == "music" || icon == "audio")
        return false;
    QString name = entry.value("fileName").toString();
    if (name.isEmpty())
        name = QUrl(entry.value("fileUri", entry.value("uri")).toString()).fileName();
    const auto kind = nook::iconFor(name, false);
    if (kind == "video" || kind == "music")
        return false;
    if (kind == "image" || kind == "code" || supportedMime)
        return true;
    const auto ext = QFileInfo(name).suffix().toLower();
    if (QStringList{"txt", "md", "markdown", "csv", "log", "pdf"}.contains(ext))
        return true;
    return QMimeDatabase().mimeTypeForFile(name, QMimeDatabase::MatchExtension).name().startsWith("text/");
}
Preview::Preview(QObject *parent) : QObject(parent) {}
Preview::~Preview() {
    stopProcess();
}
void Preview::stopProcess() {
    if (!m_process)
        return;
    auto *old = m_process;
    m_process = nullptr;
    old->disconnect(this);
    if (old->state() != QProcess::NotRunning) {
        connect(old, &QProcess::finished, old, &QObject::deleteLater);
        if (old->processId() > 0)
            ::kill(-old->processId(), SIGKILL);
        old->kill();
    } else
        old->deleteLater();
}
void Preview::close() {
    ++m_generation;
    stopProcess();
    m_content.clear();
    m_visible = false;
    m_loading = false;
    Q_EMIT changed();
}
void Preview::open(const QString &uri, int page) {
    stopProcess();
    const auto generation = ++m_generation;
    m_uri = uri;
    m_page = page;
    m_visible = true;
    m_loading = true;
    m_content = {{"title", nook::displayName(uri)}};
    Q_EMIT changed();
    auto *process = new QProcess(qApp);
    m_process = process;
    auto *timer = new QTimer(process);
    timer->setSingleShot(true);
    timer->setInterval(12000);
    auto buffer = std::make_shared<QByteArray>();
    connect(process, &QProcess::readyReadStandardOutput, this, [this, process, buffer] {
        *buffer += process->readAllStandardOutput();
        if (buffer->size() > 2 * 1024 * 1024)
            process->kill();
    });
    connect(timer, &QTimer::timeout, process, [process] {
        if (process->processId() > 0)
            ::kill(-process->processId(), SIGKILL);
        process->kill();
    });
    connect(process, &QProcess::errorOccurred, this, [this, generation](QProcess::ProcessError error) {
        if (generation == m_generation && error == QProcess::FailedToStart) {
            m_loading = false;
            m_content["error"] = QStringLiteral("无法启动预览进程");
            Q_EMIT changed();
        }
    });
    connect(process, &QProcess::finished, this,
            [this, process, timer, buffer, generation](int code, QProcess::ExitStatus status) {
                timer->stop();
                *buffer += process->readAllStandardOutput();
                if (generation == m_generation) {
                    auto doc = QJsonDocument::fromJson(*buffer);
                    if (code == 0 && status == QProcess::NormalExit && doc.isObject())
                        m_content = doc.object().toVariantMap();
                    else
                        m_content["error"] = QStringLiteral("预览失败或超时；主窗口可继续使用");
                    m_loading = false;
                    m_process = nullptr;
                    Q_EMIT changed();
                }
                process->deleteLater();
            });
    process->setProcessEnvironment(nook::workerEnvironment());
    process->start(m_executable.isEmpty() ? QCoreApplication::applicationFilePath() : m_executable,
                   {"--preview-worker", uri, QString::number(page)});
    timer->start();
}
namespace {
class ThumbnailResponse : public QQuickImageResponse {
  public:
    std::shared_ptr<std::atomic_bool> cancelled = std::make_shared<std::atomic_bool>(false);
    QImage image;
    void cancel() override {
        *cancelled = true;
    }
    QQuickTextureFactory *textureFactory() const override {
        return cancelled->load() || image.isNull() ? nullptr
                                                   : QQuickTextureFactory::textureFactoryForImage(image);
    }
    QString errorString() const override {
        return cancelled->load() || image.isNull() ? QStringLiteral("Thumbnail unavailable") : QString();
    }
};
} // namespace
ThumbnailProvider::ThumbnailProvider(QString workerExecutable) : m_executable(std::move(workerExecutable)) {
    m_pool.setMaxThreadCount(2);
    m_pool.setExpiryTimeout(5000);
}
ThumbnailProvider::~ThumbnailProvider() {
    m_pool.waitForDone();
}
QQuickImageResponse *ThumbnailProvider::requestImageResponse(const QString &id, const QSize &) {
    auto *response = new ThumbnailResponse;
    // Split BEFORE decoding: the URI can contain an encoded literal '|', '%' or '#'.
    // QML's QUrl may encode the delimiter as %7C. A pipe within the URI is already
    // URI-escaped then encodeURIComponent-escaped (%257C), so it cannot be confused with this.
    // Everything after the delimiter is an opaque QML cache revision, not part of the URI.
    auto separator = id.indexOf('|');
    const auto encodedSeparator = id.indexOf("%7C", 0, Qt::CaseInsensitive);
    if (encodedSeparator >= 0 && (separator < 0 || encodedSeparator < separator))
        separator = encodedSeparator;
    const QString uri = QUrl::fromPercentEncoding(id.left(separator < 0 ? id.size() : separator).toUtf8());
    auto cancelled = response->cancelled;
    const QString executable = m_executable.isEmpty() ? QCoreApplication::applicationFilePath() : m_executable;
    (void)QtConcurrent::run(&m_pool, [response, uri, cancelled, executable] {
        QImage image;
        if (!cancelled->load()) {
            QProcess process;
            QByteArray output, diagnostic;
            QElapsedTimer elapsed;
            elapsed.start();
            process.setProcessEnvironment(nook::workerEnvironment());
            process.start(executable, {"--thumbnail-worker", uri});
            bool valid = process.waitForStarted(1000);
            while (valid) {
                process.waitForFinished(40);
                output += process.readAllStandardOutput();
                diagnostic += process.readAllStandardError();
                valid = !cancelled->load() && elapsed.elapsed() < 7000 && output.size() <= 64 * 1024 &&
                        diagnostic.size() <= 64 * 1024;
                if (!valid || process.state() == QProcess::NotRunning)
                    break;
            }
            if (process.state() != QProcess::NotRunning) {
                // Workers establish a process group; kill ffmpeg/pdf children as well on cancellation.
                if (process.processId() > 0)
                    ::kill(-process.processId(), SIGKILL);
                process.kill();
                process.waitForFinished(1000);
            }
            if (valid && !cancelled->load() && process.exitStatus() == QProcess::NormalExit &&
                process.exitCode() == 0) {
                const auto result = QJsonDocument::fromJson(output).object();
                const QUrl imageUrl(result["image"].toString());
                if (result["error"].toString().isEmpty() && result["mode"].toString() == "image" &&
                    imageUrl.isLocalFile())
                    image = cachedImage(imageUrl.toLocalFile(), QSize(256, 256));
            }
        }
        response->image = cancelled->load() ? QImage() : image;
        Q_EMIT response->finished();
    });
    return response;
}
