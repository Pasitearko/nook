#include "previewdocuments.h"
#include "archive/archivenames.h"
#include "storage.h"
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QMap>
#include <QProcess>
#include <QUrl>
#include <QXmlStreamReader>
#include <archive.h>
#include <archive_entry.h>
#include <algorithm>
#include <memory>
#include <sys/resource.h>
#include <unistd.h>

namespace nook {
namespace {
constexpr qint64 MemberLimit = 2 * 1024 * 1024;
constexpr qint64 PackageLimit = 16 * 1024 * 1024;
constexpr int TextLimit = 128 * 1024;
constexpr int EntryLimit = 4096;
constexpr int NodeLimit = 2048;
const QString LimitMessage = QStringLiteral("文件损坏、已加密或超过安全预览限制；请用系统应用打开。");

// Stricter than the extraction worker's safeComponents: previews do not normalize absolute,
// traversal, drive, control-character or excessively deep names into apparently safe entries.
QString memberPath(QString path) {
    path.replace(u'\\', u'/');
    if (path.isEmpty() || path.size() > 1024 || path.startsWith(u'/') || path.contains(u':'))
        return {};
    for (const QChar ch : path)
        if (ch.category() == QChar::Other_Control || ch.category() == QChar::Other_Format ||
            ch == QChar::LineSeparator || ch == QChar::ParagraphSeparator)
            return {};
    QStringList parts;
    for (const auto &part : path.split(u'/')) {
        if (part == u"..")
            return {};
        if (!part.isEmpty() && part != u".")
            parts << part;
    }
    return parts.size() <= 32 ? parts.join(u'/') : QString();
}

class Reader {
  public:
    QFile file;
    QByteArray buffer = QByteArray(64 * 1024, Qt::Uninitialized);
    QElapsedTimer clock;
    qint64 input = 0;
    std::unique_ptr<::archive, decltype(&archive_read_free)> handle{archive_read_new(), archive_read_free};
    bool open(const QString &path, bool zip) {
        file.setFileName(path);
        clock.start();
        if (!file.open(QIODevice::ReadOnly) || file.size() > 512ll * 1024 * 1024)
            return false;
        // No external-program filters. ZIP documents must really be ZIP packages.
        if (zip)
            archive_read_support_format_zip_seekable(handle.get());
        else {
            // ARCHIVE_WARN means a helper-program fallback was registered. Never open that reader.
            if (archive_read_support_filter_gzip(handle.get()) != ARCHIVE_OK ||
                archive_read_support_filter_bzip2(handle.get()) != ARCHIVE_OK ||
                archive_read_support_filter_xz(handle.get()) != ARCHIVE_OK ||
                archive_read_support_filter_zstd(handle.get()) != ARCHIVE_OK ||
                archive_read_support_filter_lz4(handle.get()) != ARCHIVE_OK ||
                archive_read_support_filter_lzma(handle.get()) != ARCHIVE_OK ||
                archive_read_support_filter_compress(handle.get()) != ARCHIVE_OK ||
                archive_read_support_filter_lzip(handle.get()) != ARCHIVE_OK)
                return false;
            archive_read_support_format_all(handle.get());
            archive_read_support_format_raw(handle.get());
        }
        archive_read_set_seek_callback(handle.get(), seek);
        return archive_read_open2(handle.get(), this, nullptr, read, skip, nullptr) == ARCHIVE_OK;
    }
    bool bounded() const { return clock.elapsed() <= 3500 && input <= 128ll * 1024 * 1024; }

  private:
    static la_ssize_t read(::archive *, void *data, const void **out) {
        auto &self = *static_cast<Reader *>(data);
        if (!self.bounded())
            return -1;
        const auto n = self.file.read(self.buffer.data(), self.buffer.size());
        if (n > 0)
            self.input += n;
        *out = self.buffer.constData();
        return n;
    }
    static la_int64_t skip(::archive *, void *data, la_int64_t count) {
        auto &self = *static_cast<Reader *>(data);
        if (!self.bounded() || count < 0)
            return 0;
        const auto before = self.file.pos();
        const auto step = std::min<qint64>(count, self.file.size() - before);
        return self.file.seek(before + step) ? step : 0;
    }
    static la_int64_t seek(::archive *, void *data, la_int64_t offset, int whence) {
        auto &self = *static_cast<Reader *>(data);
        if (!self.bounded())
            return -1;
        const qint64 base = whence == SEEK_SET ? 0 : whence == SEEK_CUR ? self.file.pos() : self.file.size();
        if (offset < -base || offset > self.file.size() - base)
            return -1;
        return self.file.seek(base + offset) ? base + offset : -1;
    }
};

using Members = QMap<QString, QByteArray>;
bool collect(const QString &path, const QString &kind, Members *members) {
    Reader reader;
    if (!reader.open(path, true))
        return false;
    qint64 total = 0;
    int entries = 0;
    archive_entry *entry = nullptr;
    int status;
    while ((status = archive_read_next_header(reader.handle.get(), &entry)) == ARCHIVE_OK) {
        if (++entries > EntryLimit || !reader.bounded())
            return false;
        const char *raw = archive_entry_pathname_utf8(entry);
        if (!raw)
            raw = archive_entry_pathname(entry);
        const QString name = memberPath(QString::fromUtf8(raw ? raw : ""));
        const bool wanted = kind == "docx" ? name == "word/document.xml"
                          : kind == "xlsx" ? name == "xl/sharedStrings.xml" || name == "xl/workbook.xml" ||
                                name == "xl/_rels/workbook.xml.rels" ||
                                (name.startsWith("xl/worksheets/") && name.endsWith(".xml"))
                          : name == "META-INF/container.xml" || name.endsWith(".opf") ||
                                name.endsWith(".xhtml") || name.endsWith(".html") || name.endsWith(".htm");
        if (!wanted)
            continue;
        if (archive_entry_filetype(entry) != AE_IFREG || archive_entry_hardlink(entry) ||
            archive_entry_symlink(entry) || archive_entry_is_encrypted(entry) || members->contains(name) ||
            members->size() >= 128 || archive_entry_size(entry) > MemberLimit)
            return false;
        QByteArray bytes;
        char block[16384];
        la_ssize_t count;
        while ((count = archive_read_data(reader.handle.get(), block, sizeof(block))) > 0) {
            total += count;
            if (bytes.size() + count > MemberLimit || total > PackageLimit || !reader.bounded())
                return false;
            bytes.append(block, count);
        }
        if (count < 0)
            return false;
        members->insert(name, bytes);
    }
    return status == ARCHIVE_EOF && !members->isEmpty();
}

// No DOM, DTDs, entity declarations, external entities, URLs, CSS or HTML resource loading.
// Even benign EPUB DOCTYPE declarations are refused rather than permitting entity expansion.
class Xml {
  public:
    QXmlStreamReader reader;
    int depth = 0;
    int tokens = 0;
    bool bad = false;
    explicit Xml(const QByteArray &bytes) : reader(bytes) { reader.setEntityExpansionLimit(1); }
    bool next() {
        if (bad || reader.atEnd())
            return false;
        const auto token = reader.readNext();
        if (++tokens > 200000 || token == QXmlStreamReader::DTD ||
            token == QXmlStreamReader::EntityReference ||
            (token == QXmlStreamReader::StartElement && ++depth > 64)) {
            bad = true;
            return false;
        }
        if (token == QXmlStreamReader::EndElement)
            --depth;
        return !reader.hasError();
    }
    bool ok() const { return !bad && !reader.hasError(); }
};

QString wordText(const QByteArray &bytes, bool *ok) {
    Xml xml(bytes);
    QString text;
    bool inText = false;
    while (xml.next()) {
        const auto name = xml.reader.name();
        if (xml.reader.isStartElement()) {
            if (name == u"t")
                inText = true;
            else if (name == u"tab")
                text += u'\t';
            else if (name == u"br" || name == u"cr")
                text += u'\n';
        } else if (xml.reader.isCharacters() && inText)
            text += xml.reader.text();
        else if (xml.reader.isEndElement()) {
            if (name == u"t")
                inText = false;
            else if (name == u"p" || name == u"tr")
                text += u'\n';
            else if (name == u"tc")
                text += u'\t';
        }
        if (text.size() > TextLimit) {
            *ok = false;
            return {};
        }
    }
    *ok = xml.ok();
    return text.trimmed();
}

// Resolve only relative in-package references. Never turn links into local/network resources.
QString resolve(const QString &base, const QString &reference) {
    const QUrl url(reference);
    if (!url.isValid() || !url.isRelative() || !url.authority().isEmpty() ||
        reference.contains(u'\\') || url.hasQuery())
        return {};
    QString decoded = QUrl::fromPercentEncoding(url.path(QUrl::FullyEncoded).toUtf8());
    if (decoded.isEmpty() || decoded.contains(u':') || decoded.startsWith("//") || decoded.contains(u'\\'))
        return {};
    // A single leading slash is an OOXML package-root reference, never a filesystem path.
    const bool root = decoded.startsWith(u'/');
    if (root)
        decoded.remove(0, 1);
    const QString joined = QDir::cleanPath((root ? QString() : QFileInfo(base).path() + u'/') + decoded);
    return memberPath(joined);
}

QString epubText(const Members &members, int page, QVariantMap *result, bool *ok) {
    QString package;
    Xml container(members.value("META-INF/container.xml"));
    while (container.next())
        if (container.reader.isStartElement() && container.reader.name() == u"rootfile" && package.isEmpty())
            package = memberPath(container.reader.attributes().value("full-path").toString());
    if (!container.ok() || !members.contains(package)) {
        *ok = false;
        return {};
    }
    QMap<QString, QString> manifest;
    QStringList spine;
    Xml opf(members.value(package));
    while (opf.next()) {
        if (!opf.reader.isStartElement())
            continue;
        const auto attributes = opf.reader.attributes();
        if (opf.reader.name() == u"item" &&
            attributes.value("media-type") == u"application/xhtml+xml")
            manifest.insert(attributes.value("id").toString(), resolve(package, attributes.value("href").toString()));
        else if (opf.reader.name() == u"itemref" && attributes.value("linear") != u"no")
            spine << attributes.value("idref").toString();
    }
    if (!opf.ok() || spine.isEmpty() || spine.size() > 128 || page < 1 || page > spine.size()) {
        *ok = false;
        return {};
    }
    result->insert("pages", spine.size());
    result->insert("subtitle", QStringLiteral("EPUB · 第 %1 / %2 章").arg(page).arg(spine.size()));
    const auto chapter = manifest.value(spine.at(page - 1));
    if (chapter.isEmpty() || !members.contains(chapter)) {
        *ok = false;
        return {};
    }
    Xml html(members.value(chapter));
    QString text;
    int hidden = 0;
    while (html.next()) {
        const auto name = html.reader.name();
        if (html.reader.isStartElement()) {
            if (hidden || name == u"script" || name == u"style" || name == u"head")
                ++hidden;
            else if (name == u"br")
                text += u'\n';
        } else if (html.reader.isEndElement()) {
            if (hidden)
                --hidden;
            else if (name == u"p" || name == u"div" || name == u"li" || name == u"h1" ||
                     name == u"h2" || name == u"h3" || name == u"tr")
                text += u'\n';
        } else if (html.reader.isCharacters() && !hidden)
            text += html.reader.text();
        if (text.size() > TextLimit) {
            *ok = false;
            return {};
        }
    }
    *ok = html.ok();
    return text.trimmed();
}

QStringList sharedStrings(const QByteArray &bytes, bool *ok) {
    if (bytes.isEmpty())
        return {};
    Xml xml(bytes);
    QStringList strings;
    QString current;
    bool inText = false;
    while (xml.next()) {
        if (xml.reader.isStartElement() && xml.reader.name() == u"si")
            current.clear();
        else if (xml.reader.isStartElement() && xml.reader.name() == u"t")
            inText = true;
        else if (xml.reader.isCharacters() && inText)
            current += xml.reader.text();
        else if (xml.reader.isEndElement()) {
            if (xml.reader.name() == u"t")
                inText = false;
            else if (xml.reader.name() == u"si")
                strings << current;
        }
        if (strings.size() > 20000 || current.size() > TextLimit) {
            *ok = false;
            return {};
        }
    }
    *ok = xml.ok();
    return strings;
}

QString excelHtml(const Members &members, int page, QVariantMap *result, bool *ok) {
    QMap<QString, QString> relationships;
    Xml rels(members.value("xl/_rels/workbook.xml.rels"));
    while (rels.next()) {
        if (rels.reader.isStartElement() && rels.reader.name() == u"Relationship") {
            const auto a = rels.reader.attributes();
            if (a.value("TargetMode") != u"External" && a.value("Type").endsWith(u"/worksheet"))
                relationships.insert(a.value("Id").toString(), resolve("xl/workbook.xml", a.value("Target").toString()));
        }
    }
    QStringList paths, names;
    Xml workbook(members.value("xl/workbook.xml"));
    while (workbook.next()) {
        if (!workbook.reader.isStartElement() || workbook.reader.name() != u"sheet")
            continue;
        const auto a = workbook.reader.attributes();
        // Attribute local name also supports strict OOXML's relationship namespace.
        QString id;
        for (const auto &attribute : a)
            if (attribute.name() == u"id")
                id = attribute.value().toString();
        paths << relationships.value(id);
        names << a.value("name").toString();
    }
    if (!rels.ok() || !workbook.ok() || paths.isEmpty() || paths.size() > 128 ||
        page < 1 || page > paths.size() || !members.contains(paths.at(page - 1))) {
        *ok = false;
        return {};
    }
    result->insert("pages", paths.size());
    result->insert("subtitle", QStringLiteral("Excel · %1 · %2 / %3").arg(names.at(page - 1)).arg(page).arg(paths.size()));
    auto strings = sharedStrings(members.value("xl/sharedStrings.xml"), ok);
    if (!*ok)
        return {};
    Xml sheet(members.value(paths.at(page - 1)));
    QString html = "<table border='1' cellspacing='0' cellpadding='4'>";
    QString value, type, reference;
    int rows = 0, cells = 0, lastColumn = 0, lastRow = 0;
    bool capture = false, inCell = false, limited = false;
    while (sheet.next()) {
        const auto name = sheet.reader.name();
        if (sheet.reader.isStartElement()) {
            if (name == u"row") {
                bool valid = false;
                int row = sheet.reader.attributes().value("r").toInt(&valid);
                if (!valid)
                    row = lastRow + 1;
                if (++rows > 300 || row > 300) { limited = true; break; }
                if (row <= lastRow) { *ok = false; return {}; }
                if (row > lastRow + 1)
                    html += QStringLiteral("<tr><td colspan='31'>… 行 %1–%2 …</td></tr>").arg(lastRow + 1).arg(row - 1);
                cells = lastColumn = 0;
                lastRow = row;
                html += QStringLiteral("<tr><th>%1</th>").arg(row);
            } else if (name == u"c") {
                if (++cells > 10000) { *ok = false; return {}; }
                inCell = true;
                value.clear();
                type = sheet.reader.attributes().value("t").toString();
                reference = sheet.reader.attributes().value("r").toString();
            } else if (inCell && (name == u"v" || name == u"t"))
                capture = true;
        } else if (sheet.reader.isCharacters() && capture)
            value += sheet.reader.text();
        else if (sheet.reader.isEndElement()) {
            if (name == u"v" || name == u"t")
                capture = false;
            else if (name == u"c") {
                inCell = false;
                int column = 0;
                for (const QChar ch : reference) {
                    if (ch < u'A' || ch > u'Z')
                        break;
                    column = std::min(1000, column * 26 + ch.unicode() - 'A' + 1);
                }
                if (!column)
                    column = lastColumn + 1;
                if (column > 30) { limited = true; continue; }
                while (++lastColumn < column)
                    html += "<td></td>";
                if (type == "s") {
                    bool number = false;
                    const int index = value.toInt(&number);
                    if (!number || index < 0 || index >= strings.size()) { *ok = false; return {}; }
                    value = strings.at(index);
                } else if (type == "b")
                    value = value == "1" ? "TRUE" : "FALSE";
                html += "<td>" + value.toHtmlEscaped() + "</td>";
                lastColumn = column;
            } else if (name == u"row")
                html += "</tr>";
        }
        if (html.size() > TextLimit || value.size() > TextLimit) { *ok = false; return {}; }
    }
    *ok = sheet.ok();
    if (limited)
        result->insert("notice", QStringLiteral("仅显示前 300 行、30 列；公式仅显示已有缓存值，不执行公式。"));
    return html + "</table>";
}

QString archiveTree(const QString &path, QVariantMap *result, bool *ok) {
    Reader reader;
    if (!reader.open(path, false)) { *ok = false; return {}; }
    // A sorted path map produces a real depth-first hierarchy, including implicit parent folders.
    QMap<QString, QString> nodes;
    int entries = 0, skipped = 0;
    bool limited = false;
    qint64 scanned = 0;
    archive_entry *entry = nullptr;
    int status = ARCHIVE_EOF;
    while ((status = archive_read_next_header(reader.handle.get(), &entry)) == ARCHIVE_OK) {
        if (++entries > EntryLimit || !reader.bounded()) { limited = true; break; }
        const bool rawStream = archive_format(reader.handle.get()) == ARCHIVE_FORMAT_RAW;
        if (rawStream && archive_filter_count(reader.handle.get()) <= 1) { *ok = false; return {}; }
        const char *raw = archive_entry_pathname(entry);
        const QByteArray bytes = rawStream ? archive::stemOf(QFileInfo(path).fileName()).toUtf8()
                                         : raw ? QByteArray(raw) : QByteArray();
        bool decoded = false;
        const auto name = memberPath(archive::decodeName(bytes, archive::isUtf8(bytes) ? "UTF-8" : archive::fallbackEncoding(), &decoded));
        if (name.isEmpty() || !decoded) {
            ++skipped;
        } else {
            QString prefix;
            const auto parts = name.split(u'/');
            for (int i = 0; i < parts.size(); ++i) {
                prefix += (prefix.isEmpty() ? "" : "/") + parts.at(i);
                if (nodes.size() >= NodeLimit || (nodes.size() + 1) * (name.size() + 8) > TextLimit) {
                    limited = true;
                    break;
                }
                const QString type = i + 1 < parts.size() || archive_entry_filetype(entry) == AE_IFDIR ? "dir"
                                   : archive_entry_symlink(entry) || archive_entry_hardlink(entry) ? "link"
                                   : archive_entry_filetype(entry) == AE_IFREG ? "file" : "special";
                if (!nodes.contains(prefix))
                    nodes[prefix] = type;
                else if (nodes.value(prefix) != type)
                    nodes[prefix] = "conflict";
            }
        }
        if (limited)
            break;
        // ZIP payloads can be skipped without inflation. Stream-compressed formats cannot:
        // discard through bounded reads rather than archive_read_data_skip's unbounded inflate.
        if ((archive_format(reader.handle.get()) & ARCHIVE_FORMAT_BASE_MASK) != ARCHIVE_FORMAT_ZIP) {
            char block[16384];
            la_ssize_t count;
            while ((count = archive_read_data(reader.handle.get(), block, sizeof(block))) > 0) {
                scanned += count;
                if (scanned > 32ll * 1024 * 1024 || !reader.bounded()) { limited = true; break; }
            }
            if (count < 0) { *ok = false; return {}; }
            if (limited)
                break;
        }
    }
    if (status != ARCHIVE_EOF && !limited) { *ok = false; return {}; }
    // Sorting a component list (not raw paths) keeps siblings like a.txt after a/children.
    auto paths = nodes.keys();
    std::sort(paths.begin(), paths.end(), [](const QString &a, const QString &b) {
        const auto left = a.split(u'/'), right = b.split(u'/');
        return std::lexicographical_compare(left.begin(), left.end(), right.begin(), right.end());
    });
    QString text;
    for (const auto &name : paths) {
        const auto type = nodes.value(name);
        text += QString(name.count(u'/') * 2, u' ') + (type == "dir" ? "▸ " : "  ") + name.section(u'/', -1) +
                (type == "dir" ? "/" : type == "link" ? QStringLiteral(" [链接，不跟随]") :
                 type == "special" ? QStringLiteral(" [特殊条目]") :
                 type == "conflict" ? QStringLiteral(" [路径类型冲突，不跟随]") : QString()) + u'\n';
        if (text.size() > TextLimit) { text.truncate(TextLimit); limited = true; break; }
    }
    if (text.isEmpty())
        text = QStringLiteral("（空目录树）");
    result->insert("notice", QStringLiteral("只列目录，不解压、不跟随链接。%1%2")
                   .arg(limited ? QStringLiteral("已达到预览限制，目录树不完整。") : QString())
                   .arg(skipped ? QStringLiteral("已隐藏 %1 个不安全路径。").arg(skipped) : QString()));
    *ok = true;
    return text;
}
QString legacyText(const QString &path, const QString &ext, bool *ok) {
    const QString tool = ext == "doc" ? QStringLiteral("/usr/bin/antiword") : QStringLiteral("/usr/bin/xls2csv");
    if (!QFileInfo(tool).isExecutable()) {
        *ok = false;
        return {};
    }
    QProcess process;
    process.setProcessEnvironment(workerEnvironment());
    process.setChildProcessModifier([] {
        const struct rlimit memory{256ull * 1024 * 1024, 256ull * 1024 * 1024};
        const struct rlimit cpu{2, 2}, files{0, 0}, descriptors{64, 64};
        if (::setrlimit(RLIMIT_AS, &memory) || ::setrlimit(RLIMIT_CPU, &cpu) ||
            ::setrlimit(RLIMIT_FSIZE, &files) || ::setrlimit(RLIMIT_NOFILE, &descriptors))
            ::_exit(1);
    });
    const QStringList arguments = ext == "doc" ? QStringList{"-m", "UTF-8.txt", QFileInfo(path).absoluteFilePath()}
                                                : QStringList{"-d", "utf-8", QFileInfo(path).absoluteFilePath()};
    process.start(tool, arguments, QIODevice::ReadOnly);
    if (!process.waitForStarted(500)) { *ok = false; return {}; }
    QElapsedTimer clock;
    clock.start();
    QByteArray output, diagnostic;
    do {
        process.waitForFinished(20);
        output += process.readAllStandardOutput();
        diagnostic += process.readAllStandardError();
        if (output.size() > TextLimit || diagnostic.size() > 16384 || clock.elapsed() > 2000) {
            process.kill();
            process.waitForFinished(500);
            *ok = false;
            return {};
        }
    } while (process.state() != QProcess::NotRunning);
    *ok = process.exitStatus() == QProcess::NormalExit && process.exitCode() == 0 && !output.trimmed().isEmpty();
    return *ok ? QString::fromUtf8(output) : QString();
}
} // namespace

bool documentPreview(const QString &path, int page, QVariantMap *result) {
    const QString ext = QFileInfo(path).suffix().toLower();
    const bool native = ext == "docx" || ext == "xlsx" || ext == "epub";
    if (!native && ext != "doc" && ext != "xls" && !archive::looksLikeArchive(QFileInfo(path).fileName()))
        return false;
    result->insert("mode", "text");
    if (ext == "doc" || ext == "xls") {
        // Optional text-only converters, never a full office suite with macros/profile writes.
        bool converted = false;
        QString text;
        if (QFileInfo(path).size() <= 32ll * 1024 * 1024)
            text = legacyText(path, ext, &converted);
        result->insert("notice", QStringLiteral("旧格式通过可选工具转换为纯文本；不执行宏，转换受时间、内存和输出限制。"));
        result->insert("text", converted ? text : ext == "doc"
            ? QStringLiteral("无法安全预览旧版 Word (.doc)。可选安装 antiword 后重试；或用 LibreOffice/Word 打开并另存为 .docx。转换工具缺失、失败或超过限制时不会继续处理。")
            : QStringLiteral("无法安全预览旧版 Excel (.xls)。可选安装 catdoc（提供 xls2csv）后重试；或用 LibreOffice/Excel 打开并另存为 .xlsx 或 CSV。转换工具缺失、失败或超过限制时不会继续处理。"));
        return true;
    }
    bool ok = true;
    QString text;
    if (native) {
        Members members;
        ok = collect(path, ext, &members);
        if (ok) {
            result->insert("notice", QStringLiteral("安全文本预览：不加载图片、外部链接、样式或宏；内容限 2 MiB/条目、16 MiB/文档。"));
            if (ext == "docx") {
                ok = members.contains("word/document.xml");
                if (ok)
                    text = wordText(members.value("word/document.xml"), &ok);
            } else if (ext == "xlsx") {
                text = excelHtml(members, page, result, &ok);
                result->insert("mode", "rich");
            } else
                text = epubText(members, page, result, &ok);
        }
    } else
        text = archiveTree(path, result, &ok);
    if (!ok) {
        result->insert("error", LimitMessage);
        result->insert("mode", "info");
        result->insert("text", "");
    } else
        result->insert("text", text.isEmpty() ? QStringLiteral("（无可显示的文本）") : text);
    return true;
}
} // namespace nook
