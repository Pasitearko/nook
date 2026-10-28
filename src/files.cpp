#include "files.h"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImageReader>
#include <QLocale>
#include <QMimeDatabase>
#include <QSaveFile>
#include <QStorageInfo>
#include <QUrl>
#include <gio/gio.h>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <sys/stat.h>
#include <unicode/translit.h>

namespace nook {
QString normalizeUri(const QString &input) {
    QByteArray value = input.toUtf8();
    GFile *file = input.startsWith("file:") || input.startsWith("trash:")
                      ? g_file_new_for_uri(value.constData())
                      : g_file_new_for_path(
                            QFile::encodeName(QDir::cleanPath(input.startsWith('~')
                                                                  ? QDir::homePath() + input.mid(1)
                                                                  : QFileInfo(input).absoluteFilePath()))
                                .constData());
    char *uri = g_file_get_uri(file);
    QString result = QString::fromUtf8(uri);
    g_free(uri);
    g_object_unref(file);
    return result;
}
QString localPath(const QString &uri) {
    GFile *file = g_file_new_for_uri(uri.toUtf8().constData());
    char *path = g_file_get_path(file);
    QString result = path ? QFile::decodeName(path) : QString();
    g_free(path);
    g_object_unref(file);
    return result;
}
QString parentUri(const QString &uri) {
    GFile *file = g_file_new_for_uri(uri.toUtf8().constData());
    GFile *parent = g_file_get_parent(file);
    char *value = parent ? g_file_get_uri(parent) : nullptr;
    QString result = value ? QString::fromUtf8(value) : uri;
    g_free(value);
    if (parent)
        g_object_unref(parent);
    g_object_unref(file);
    return result;
}
QString childUri(const QString &uri, const QString &name) {
    GFile *file = g_file_new_for_uri(uri.toUtf8().constData());
    GFile *child = g_file_get_child(file, name.toUtf8().constData());
    char *value = g_file_get_uri(child);
    QString result = QString::fromUtf8(value);
    g_free(value);
    g_object_unref(child);
    g_object_unref(file);
    return result;
}
QString displayName(const QString &uri) {
    if (uri.startsWith("trash:"))
        return QStringLiteral("回收站");
    auto path = localPath(uri);
    if (path == QDir::homePath())
        return QStringLiteral("主目录");
    if (path == "/")
        return QStringLiteral("文件系统");
    return QFileInfo(path).fileName();
}
// KB/MB/GB in steps of 1024, as Windows shows them (not KiB/MiB).
QString formatSize(qint64 bytes) {
    if (bytes < 1024)
        return QStringLiteral("%1 字节").arg(qMax<qint64>(0, bytes));
    static const char *const units[] = {"KB", "MB", "GB", "TB", "PB"};
    double value = double(bytes) / 1024;
    int unit = 0;
    while (value >= 1024 && unit < 4) {
        value /= 1024;
        ++unit;
    }
    QString number = QString::number(value, 'f', 1);
    if (number.endsWith(QLatin1String(".0")))
        number.chop(2);
    return number + ' ' + QLatin1String(units[unit]);
}
QString iconFor(const QString &name, bool directory) {
    if (directory)
        return "folder";
    QString ext = QFileInfo(name).suffix().toLower();
    // Query installed Qt plugins, including their MIME aliases (e.g. jpg/jpeg and tif/tiff).
    static const auto imageFormats = QImageReader::supportedImageFormats();
    static const auto imageMimes = QImageReader::supportedMimeTypes();
    const auto mime = QMimeDatabase().mimeTypeForFile(name, QMimeDatabase::MatchExtension);
    // Documents stay documents even when an image plugin (e.g. QtPdf's) can rasterize them.
    if (ext == "pdf" || mime.name() == "application/pdf")
        return "document";
    if (imageFormats.contains(ext.toLatin1()) || imageMimes.contains(mime.name().toLatin1()))
        return "image";
    if (QStringList{"mp4", "mkv", "mov", "webm", "avi", "m4v", "mpg", "mpeg", "ogv", "mts", "m2ts"}.contains(
            ext))
        return "video";
    if (QStringList{"mp3", "flac", "wav", "ogg", "m4a", "opus", "aac", "aiff", "wma"}.contains(ext))
        return "music";
    if (QStringList{"c",  "cc",  "cpp", "cxx",  "h",   "hh", "hpp", "rs",   "py",   "js",   "jsx",
                    "ts", "tsx", "qml", "json", "xml", "sh", "css", "html", "toml", "yaml", "yml"}
            .contains(ext))
        return "code";
    if (mime.name().startsWith("video/"))
        return "video";
    if (mime.name().startsWith("audio/"))
        return "music";
    if (QStringList{"txt", "md", "markdown", "pdf", "csv", "log", "docx", "epub"}.contains(ext) ||
        mime.name().startsWith("text/"))
        return "document";
    return "file";
}
QVariantMap entryMap(const Entry &e) {
    return {{"fileUri", e.uri},
            {"fileName", e.name},
            {"folder", e.directory},
            {"isLink", e.symlink},
            {"bytes", e.size},
            {"sizeText", e.directory ? QStringLiteral("—") : formatSize(e.size)},
            {"modified", e.modified},
            {"modifiedText",
             QDateTime::fromSecsSinceEpoch(e.modified).toLocalTime().toString("yyyy-MM-dd HH:mm")},
            {"kind", e.kind},
            {"iconName", e.icon},
            {"fullPath", localPath(e.uri)},
            {"hidden", e.hidden},
            {"originalUri", e.originalUri}};
}
QList<Entry> enumerate(const QString &uri, bool hidden, const Cancel &cancel, QString *error,
                       const std::function<void(QList<Entry>)> &callback) {
    QList<Entry> result, chunk;
    GFile *dir = g_file_new_for_uri(uri.toUtf8().constData());
    GError *failure = nullptr;
    // GIO caches .hidden for several seconds; read it directly so a toggle shows at once.
    char *dirPath = g_file_get_path(dir);
    const bool local = dirPath != nullptr;
    const auto listed = local ? hiddenNames(QByteArray(dirPath)) : QSet<QByteArray>();
    g_free(dirPath);
    auto *iter =
        g_file_enumerate_children(dir,
                                  "standard::name,standard::display-name,standard::type,standard::size,"
                                  "standard::is-hidden,standard::is-symlink,time::modified,trash::orig-path",
                                  G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, nullptr, &failure);
    if (iter) {
        while (!cancel->load()) {
            GFileInfo *info = g_file_enumerator_next_file(iter, nullptr, &failure);
            if (!info)
                break;
            const char *rawName = g_file_info_get_name(info);
            bool isHidden = local ? (rawName && (rawName[0] == '.' || listed.contains(QByteArray(rawName))))
                                  : g_file_info_get_is_hidden(info);
            if (hidden || !isHidden) {
                GFile *file = g_file_enumerator_get_child(iter, info);
                char *value = g_file_get_uri(file);
                Entry e;
                e.uri = QString::fromUtf8(value);
                e.name = QString::fromUtf8(g_file_info_get_display_name(info));
                e.symlink = g_file_info_get_is_symlink(info);
                e.directory = g_file_info_get_file_type(info) == G_FILE_TYPE_DIRECTORY;
                if (e.symlink)
                    e.directory = g_file_query_file_type(file, G_FILE_QUERY_INFO_NONE, nullptr) ==
                                  G_FILE_TYPE_DIRECTORY;
                e.hidden = isHidden;
                e.size = g_file_info_get_size(info);
                e.modified = static_cast<qint64>(g_file_info_get_attribute_uint64(info, "time::modified"));
                e.icon = iconFor(e.name, e.directory);
                e.kind = e.directory ? QStringLiteral("文件夹")
                                     : (e.symlink ? QStringLiteral("符号链接")
                                                  : QFileInfo(e.name).suffix().toUpper());
                const char *original = g_file_info_get_attribute_byte_string(info, "trash::orig-path");
                if (original) {
                    GFile *origin = g_file_new_for_path(original);
                    char *u = g_file_get_uri(origin);
                    e.originalUri = QString::fromUtf8(u);
                    g_free(u);
                    g_object_unref(origin);
                }
                g_free(value);
                g_object_unref(file);
                if (callback) {
                    chunk.append(e);
                    if (chunk.size() == 128) {
                        callback(std::move(chunk));
                        chunk.clear();
                    }
                } else
                    result.append(e);
            }
            g_object_unref(info);
        }
        if (callback && !chunk.isEmpty() && !cancel->load())
            callback(std::move(chunk));
        g_object_unref(iter);
    }
    if (failure) {
        if (error)
            *error = QString::fromUtf8(failure->message);
        g_error_free(failure);
    }
    g_object_unref(dir);
    return result;
}
bool sidebarDataPartition(const QString &filesystem, const QString &partitionType, const QString &label) {
    const auto fs = filesystem.toLower();
    const auto type = partitionType.toLower();
    // Partition metadata takes precedence over arbitrary user-chosen volume labels.
    const auto name = label.trimmed().toLower();
    if (type.isEmpty() && QStringList{"recovery", "windows re tools", "system reserved", "reserved", "efi",
                                      "esp", "恢复", "恢复分区", "系统保留"}
                              .contains(name))
        return false;
    const bool basic = type == "ebd0a0a2-b9e5-4433-87c0-68b6b72699c7" || type == "0x7" || type == "0x07";
    if (!type.isEmpty() && !basic)
        return false;
    return fs == "ntfs" || fs == "ntfs3" || fs == "exfat" || (fs == "vfat" && basic);
}
QStringList localRoots() {
    QStringList roots;
    for (const auto &v : QStorageInfo::mountedVolumes()) {
        const QByteArray fs = v.fileSystemType();
        if (!v.isReady() || fs.startsWith("fuse") ||
            !QList<QByteArray>{"btrfs", "ext4", "ext3", "ext2", "xfs", "vfat", "exfat", "ntfs", "ntfs3",
                               "f2fs", "zfs"}
                 .contains(fs))
            continue;
        if (!excludedPath(QFile::encodeName(v.rootPath()), {}))
            roots.append(v.rootPath());
    }
    roots.removeDuplicates();
    return roots;
}
bool windowsSystemEntry(const QByteArray &name) {
    const auto lower = name.toLower();
    return lower.startsWith('$') || lower == "system volume information" || lower == "windows" ||
           lower == "recovery" || lower == "config.msi" || lower == "perflogs" || lower == "msocache";
}
bool excludedPath(const QByteArray &p, const QStringList &extra) {
    static const QList<QByteArray> prefixes = {"/proc", "/sys",     "/dev",       "/run",
                                               "/tmp",  "/var/tmp", "/var/cache", "/root"};
    for (const auto &prefix : prefixes)
        if (p == prefix || p.startsWith(prefix + '/'))
            return true;
    const auto parts = p.split('/');
    for (const auto &part : parts)
        if (part == ".cache" || part == ".snapshots" || part == ".Trash" || part.startsWith(".Trash-"))
            return true;
    for (const auto &prefix : extra) {
        auto raw = QFile::encodeName(QDir::cleanPath(prefix));
        if (!raw.isEmpty() && (p == raw || p.startsWith(raw + '/')))
            return true;
    }
    return false;
}
QString folded(const QString &text) {
    return text.normalized(QString::NormalizationForm_C).toCaseFolded();
}
QPair<QString, QString> pinyinKeys(const QString &text) {
    thread_local std::unique_ptr<icu::Transliterator> trans([] {
        UErrorCode status = U_ZERO_ERROR;
        return icu::Transliterator::createInstance("Han-Latin; Latin-ASCII; Lower", UTRANS_FORWARD, status);
    }());
    if (!trans)
        return {folded(text), {}};
    QString full, initials;
    for (auto cp : text.toUcs4()) {
        char32_t codepoint = cp;
        QString source = QString::fromUcs4(&codepoint, 1);
        icu::UnicodeString u(reinterpret_cast<const UChar *>(source.utf16()), source.size());
        trans->transliterate(u);
        QString converted = QString::fromUtf16(reinterpret_cast<const char16_t *>(u.getBuffer()), u.length());
        converted.remove(' ');
        full += converted;
        if (!converted.isEmpty())
            initials += converted.front();
    }
    return {folded(full), folded(initials)};
}
bool validName(const QString &name) {
    return !name.isEmpty() && name != "." && name != ".." && !name.contains('/') &&
           !name.contains(QChar(0)) && name.toUtf8().size() <= 255;
}
QSet<QByteArray> hiddenNames(const QByteArray &directory) {
    QSet<QByteArray> names;
    QFile file(QFile::decodeName(directory + "/.hidden"));
    if (file.size() > 1024 * 1024 || !file.open(QIODevice::ReadOnly))
        return names;
    for (auto line : file.readAll().split('\n')) {
        if (line.endsWith('\r'))
            line.chop(1);
        if (!line.isEmpty())
            names.insert(line);
    }
    return names;
}
bool hiddenFile(const QByteArray &path) {
    const int slash = path.lastIndexOf('/');
    const QByteArray name = path.mid(slash + 1);
    if (name.isEmpty())
        return false;
    return name.startsWith('.') || hiddenNames(slash > 0 ? path.left(slash) : QByteArray("/")).contains(name);
}
bool setHidden(const QString &uri, bool hide, QString *error, QString *resultUri) {
    auto fail = [error](const QString &message) {
        if (error)
            *error = message;
        return false;
    };
    if (resultUri)
        *resultUri = uri;
    GFile *file = g_file_new_for_uri(uri.toUtf8().constData());
    char *raw = g_file_get_path(file);
    g_object_unref(file);
    const QByteArray path = raw ? QByteArray(raw) : QByteArray();
    g_free(raw);
    const int slash = path.lastIndexOf('/');
    if (path.isEmpty() || slash < 0 || path == "/")
        return fail(QStringLiteral("只能修改本地文件或文件夹的隐藏状态"));
    const QByteArray directory = slash > 0 ? path.left(slash) : QByteArray("/");
    const QByteArray name = path.mid(slash + 1);
    struct stat info{};
    if (::lstat(path.constData(), &info) != 0)
        return fail(QStringLiteral("无法访问：") + QString::fromLocal8Bit(std::strerror(errno)));
    if (name.startsWith('.')) {
        if (hide)
            return true;
        // A leading dot is the name itself; un-hiding has to rename (never overwriting).
        QByteArray visible = name;
        while (visible.startsWith('.'))
            visible.remove(0, 1);
        if (visible.isEmpty())
            return fail(QStringLiteral("名称只由“.”组成，无法取消隐藏"));
        const QByteArray target = (directory == "/" ? QByteArray() : directory) + "/" + visible;
        if (::renameat2(AT_FDCWD, path.constData(), AT_FDCWD, target.constData(), RENAME_NOREPLACE) != 0)
            return fail(errno == EEXIST
                            ? QStringLiteral("已存在“%1”，无法通过去掉开头的“.”取消隐藏").arg(QFile::decodeName(visible))
                            : QStringLiteral("重命名失败：") + QString::fromLocal8Bit(std::strerror(errno)));
        if (resultUri)
            *resultUri = normalizeUri(QFile::decodeName(target));
        return true;
    }
    QSet<QByteArray> names = hiddenNames(directory);
    if (hide == names.contains(name))
        return true;
    const QString listPath = QFile::decodeName(directory + "/.hidden");
    QFile existing(listPath);
    QByteArray content;
    if (existing.open(QIODevice::ReadOnly))
        content = existing.readAll();
    existing.close();
    QByteArrayList lines;
    for (auto line : content.split('\n')) {
        if (line.endsWith('\r'))
            line.chop(1);
        if (!line.isEmpty() && line != name)
            lines.append(line);
    }
    if (hide)
        lines.append(name);
    if (lines.isEmpty()) {
        if (!QFile::remove(listPath))
            return fail(QStringLiteral("无法更新 .hidden：") + existing.errorString());
        return true;
    }
    QSaveFile save(listPath);
    if (!save.open(QIODevice::WriteOnly) || save.write(lines.join('\n') + '\n') < 0 || !save.commit())
        return fail(QStringLiteral("无法写入所在目录的 .hidden：") + save.errorString());
    return true;
}
} // namespace nook
