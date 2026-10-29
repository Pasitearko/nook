#include "worker.h"
#include "../archivenames.h"
#include <QFileInfo>
#include <QHash>
#include <QSet>
#include <algorithm>
#include <archive.h>
#include <archive_entry.h>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>

namespace nook::archive::worker {
namespace {

constexpr qint64 SpaceMargin = 64ll << 20;

struct Item {
    QByteArray raw;
    QByteArray link;
    QByteArray hardlink;
    QString path;
    QStringList components;
    QString type;
    qint64 size = -1;
    qint64 mtime = 0;
    mode_t mode = 0;
    bool encrypted = false;
};

QString classify(const QString &message) {
    if (message.contains("passphrase", Qt::CaseInsensitive))
        return "wrong-password";
    if (message.contains("No space", Qt::CaseInsensitive))
        return "no-space";
    if (message.contains("Unsupported", Qt::CaseInsensitive) || message.contains("not supported", Qt::CaseInsensitive))
        return "unsupported";
    return "corrupt";
}

QString chinese(const QString &code, const QString &message) {
    if (code == "wrong-password")
        return "密码错误";
    if (code == "no-space")
        return "磁盘空间不足";
    if (code == "unsupported")
        return "不支持的压缩方式：" + message;
    return "压缩包已损坏或不完整：" + message;
}

class Reader {
  public:
    explicit Reader(const Request &request) : m_request(request) {}
    ~Reader() {
        if (m_archive)
            archive_read_free(m_archive);
    }
    // Opens as a package, or as one compressed stream ("notes.txt.gz") when that is all it is.
    bool open(QString *error, bool *unrecognized) {
        *unrecognized = false;
        if (start(false))
            return true;
        const QString message = QString::fromUtf8(archive_error_string(m_archive));
        if (message.contains("Unrecognized", Qt::CaseInsensitive)) {
            if (start(true) && archive_filter_count(m_archive) > 1) {
                m_raw = true;
                return true;
            }
            *unrecognized = true;
        }
        if (message.contains("encrypted", Qt::CaseInsensitive))
            *unrecognized = true; // header encryption: 7-Zip can ask for the password
        *error = message;
        return false;
    }
    ::archive *handle() const { return m_archive; }
    bool raw() const { return m_raw; }
    bool gaveUp() const { return m_gaveUp; }
    bool zip() const { return (archive_format(m_archive) & ARCHIVE_FORMAT_BASE_MASK) == ARCHIVE_FORMAT_ZIP; }
    qint64 position() const { return archive_filter_bytes(m_archive, -1); }
    QJsonArray filters() const {
        QJsonArray names;
        for (int i = 0; i < archive_filter_count(m_archive); ++i) {
            const QString name = QString::fromLatin1(archive_filter_name(m_archive, i));
            if (name != "none")
                names << name;
        }
        return names;
    }
    QString format() const {
        if (m_raw)
            return "raw";
        const char *name = archive_format_name(m_archive);
        return name ? QString::fromLatin1(name) : QString();
    }
    // Reads the next header into item; 1 = entry, 0 = end, -1 = fatal (message in error).
    int next(Item &item, QString *error) {
        archive_entry *entry = nullptr;
        int status = archive_read_next_header(m_archive, &entry);
        if (status == ARCHIVE_EOF)
            return 0;
        if (status < ARCHIVE_WARN) {
            *error = QString::fromUtf8(archive_error_string(m_archive));
            if (status == ARCHIVE_FATAL)
                return -1;
        }
        item = {};
        const char *name = archive_entry_pathname(entry);
        if (!name)
            name = archive_entry_pathname_utf8(entry);
        item.raw = name ? QByteArray(name) : QByteArray();
        if (m_raw)
            item.raw = stemOf(m_request.archive).toUtf8();
        if (const char *link = archive_entry_symlink(entry))
            item.link = link;
        if (const char *hard = archive_entry_hardlink(entry))
            item.hardlink = hard;
        const mode_t type = archive_entry_filetype(entry);
        item.type = !item.hardlink.isEmpty() ? "hardlink"
                    : type == AE_IFREG       ? "file"
                    : type == AE_IFDIR       ? "dir"
                    : type == AE_IFLNK       ? "link"
                                             : "other";
        if (m_raw)
            item.type = "file";
        item.size = archive_entry_size_is_set(entry) ? archive_entry_size(entry) : -1;
        item.mtime = archive_entry_mtime_is_set(entry) ? archive_entry_mtime(entry) : 0;
        item.mode = archive_entry_perm(entry);
        item.encrypted = archive_entry_is_encrypted(entry);
        return 1;
    }

  private:
    bool start(bool raw) {
        if (m_archive)
            archive_read_free(m_archive);
        m_archive = archive_read_new();
        archive_read_support_filter_all(m_archive);
        if (raw)
            archive_read_support_format_raw(m_archive);
        else
            archive_read_support_format_all(m_archive);
        archive_read_set_passphrase_callback(m_archive, this, &Reader::passphrase);
        return archive_read_open_filename(m_archive, m_request.archive.toLocal8Bit().constData(), 1 << 16) == ARCHIVE_OK;
    }
    static const char *passphrase(::archive *, void *data) {
        auto *self = static_cast<Reader *>(data);
        if (!self->m_triedGiven && self->m_request.hasPassword) {
            self->m_triedGiven = true;
            self->m_current = self->m_request.password.toUtf8();
            return self->m_current.constData();
        }
        const auto answer = askPassword(self->m_triedGiven || self->m_asked);
        self->m_asked = true;
        if (!answer) {
            self->m_gaveUp = true;
            return nullptr;
        }
        self->m_current = answer->toUtf8();
        return self->m_current.constData();
    }

    const Request &m_request;
    ::archive *m_archive = nullptr;
    bool m_raw = false;
    bool m_triedGiven = false;
    bool m_asked = false;
    bool m_gaveUp = false;
    QByteArray m_current;
};

qint64 fileSize(const QString &path) { return QFileInfo(path).size(); }

// O_NOFOLLOW | O_DIRECTORY on a link fails with ELOOP or ENOTDIR depending on the kernel path.
QString blockedPath() {
    if (errno == ELOOP || errno == ENOTDIR)
        return "路径经过符号链接或文件，已跳过";
    return QString::fromLocal8Bit(std::strerror(errno));
}

// Names in the requested encoding; "auto" detects one for the names that are not UTF-8.
class Names {
  public:
    Names(const QString &requested, bool zip) : m_requested(requested), m_zip(zip) {}
    void detect(const QList<QByteArray> &raw) {
        m_detected = detectEncoding(raw);
        m_legacy = m_detected != "UTF-8";
    }
    QString encoding() const { return m_requested == "auto" ? m_detected : m_requested; }
    bool legacy() const { return m_legacy; }
    QString decode(const QByteArray &raw) const {
        if (m_requested != "auto")
            return decodeName(raw, m_requested);
        if (isUtf8(raw))
            return QString::fromUtf8(raw);
        return decodeName(raw, m_detected == "UTF-8" ? fallbackEncoding() : m_detected);
    }
    void fill(Item &item) const {
        const QString decoded = decode(item.raw);
        item.components = safeComponents(decoded, m_zip);
        item.path = item.components.isEmpty() ? decoded : item.components.join(u'/');
    }

  private:
    QString m_requested;
    bool m_zip;
    QString m_detected = "UTF-8";
    bool m_legacy = false;
};

QJsonObject openFailure(const QString &message) {
    return {{"event", "done"}, {"ok", false}, {"error", failure("unsupported", "无法打开压缩包：" + message)}};
}

// Only ZIP mixes encodings in practice and reading its names is cheap (the central directory).
std::optional<Names> namesFor(const Request &request, bool zip) {
    Names names(request.encoding, zip);
    if (request.encoding != "auto" || !zip)
        return names;
    Reader reader(request);
    QString error;
    bool unrecognized = false;
    if (!reader.open(&error, &unrecognized))
        return std::nullopt;
    QList<QByteArray> raw;
    Item item;
    while (reader.next(item, &error) > 0) {
        raw << item.raw;
        if (Channel::instance().cancelled())
            break;
    }
    names.detect(raw);
    return names;
}

}

Backend backendFor(const QString &path) {
    const Volumes volumes = volumesOf(path);
    if (volumes.split)
        return Backend::SevenZip;
    QFile file(volumes.first);
    if (!file.open(QIODevice::ReadOnly))
        return Backend::Libarchive;
    const QByteArray magic = file.read(8);
    if (magic.startsWith(QByteArray("7z\xBC\xAF\x27\x1C", 6)) || magic.startsWith("Rar!\x1A\x07") ||
        magic.startsWith(QByteArray("MSWIM\0\0\0", 8)))
        return Backend::SevenZip;
    return Backend::Libarchive;
}

QJsonObject libarchiveList(const Request &request, bool *fallback) {
    auto &channel = Channel::instance();
    Reader reader(request);
    QString error;
    bool unrecognized = false;
    if (!reader.open(&error, &unrecognized)) {
        *fallback = unrecognized;
        return openFailure(error);
    }
    QList<Item> items;
    Item item;
    int status;
    while ((status = reader.next(item, &error)) > 0) {
        items << item;
        if (channel.cancelled())
            return {{"event", "done"}, {"ok", false}, {"cancelled", true}};
        if (items.size() % 512 == 0)
            channel.progress({{"phase", "list"}, {"files", int(items.size())}});
    }
    if (status < 0 && items.isEmpty()) {
        *fallback = unrecognized || error.contains("Unsupported", Qt::CaseInsensitive);
        return openFailure(error);
    }
    Names names(request.encoding, reader.zip());
    QList<QByteArray> raw;
    for (const auto &entry : std::as_const(items))
        raw << entry.raw;
    names.detect(raw);
    // libarchive does not report compressed sizes; 7-Zip's listing of the same ZIP does. Both
    // follow the central directory, so entries pair up by position (checked against sizes).
    QList<qint64> packed;
    QStringList methods;
    if (reader.zip()) {
        const QJsonArray other = sevenZipEntries(request.archive);
        bool aligned = other.size() == items.size();
        for (qsizetype i = 0; aligned && i < items.size(); ++i)
            aligned = items[i].type == "dir" || qint64(other[i].toObject().value("size").toDouble()) == items[i].size;
        for (qsizetype i = 0; aligned && i < items.size(); ++i) {
            const QJsonObject entry = other[i].toObject();
            packed << (entry.contains("packed") ? qint64(entry.value("packed").toDouble()) : -1);
            methods << entry.value("method").toString();
        }
    }
    bool encrypted = false;
    qint64 total = 0;
    for (auto &entry : items) {
        names.fill(entry);
        encrypted = encrypted || entry.encrypted;
        total += std::max<qint64>(entry.size, 0);
    }
    channel.send({{"event", "info"},
                  {"backend", "libarchive"},
                  {"format", reader.format()},
                  {"filters", reader.filters()},
                  {"encoding", names.encoding()},
                  {"legacyNames", names.legacy()},
                  {"encrypted", encrypted},
                  {"headerEncrypted", false},
                  {"split", false},
                  {"stream", reader.raw()}});
    QJsonArray batch;
    for (qsizetype i = 0; i < items.size(); ++i) {
        const Item &entry = items[i];
        QJsonObject json{{"path", entry.path}, {"type", entry.type}, {"size", double(entry.size)},
                         {"mtime", double(entry.mtime)}, {"mode", int(entry.mode)}};
        if (i < packed.size() && packed[i] >= 0 && entry.type != "dir")
            json.insert("packed", double(packed[i]));
        if (i < methods.size() && !methods[i].isEmpty())
            json.insert("method", methods[i]);
        if (entry.encrypted)
            json.insert("encrypted", true);
        if (!entry.link.isEmpty())
            json.insert("link", names.decode(entry.link));
        if (!entry.hardlink.isEmpty())
            json.insert("link", names.decode(entry.hardlink));
        if (entry.components.isEmpty())
            json.insert("unsafe", true);
        batch << json;
        if (batch.size() == 1000) {
            channel.send({{"event", "entries"}, {"items", batch}});
            batch = {};
        }
    }
    if (!batch.isEmpty())
        channel.send({{"event", "entries"}, {"items", batch}});
    QJsonObject done{{"event", "done"}, {"ok", status >= 0}, {"count", int(items.size())}, {"totalSize", double(total)}};
    if (status < 0)
        done.insert("error", failure("corrupt", chinese("corrupt", error)));
    return done;
}

QJsonObject libarchiveTest(const Request &request, bool *fallback) {
    auto &channel = Channel::instance();
    Reader reader(request);
    QString error;
    bool unrecognized = false;
    if (!reader.open(&error, &unrecognized)) {
        *fallback = unrecognized;
        return openFailure(error);
    }
    const auto names = namesFor(request, reader.zip());
    const qint64 total = std::max<qint64>(fileSize(request.archive), 1);
    QJsonArray errors;
    int tested = 0;
    Item item;
    int status;
    QByteArray buffer(1 << 17, Qt::Uninitialized);
    while ((status = reader.next(item, &error)) > 0) {
        if (names)
            names->fill(item);
        if (item.type == "file") {
            for (;;) {
                const la_ssize_t read = archive_read_data(reader.handle(), buffer.data(), size_t(buffer.size()));
                if (read == 0)
                    break;
                if (read < 0) {
                    if (reader.gaveUp())
                        return {{"event", "done"}, {"ok", false}, {"cancelled", true}};
                    const QString message = QString::fromUtf8(archive_error_string(reader.handle()));
                    if (classify(message) == "unsupported" && !item.encrypted) {
                        *fallback = true;
                        return {};
                    }
                    errors << QJsonObject{{"path", item.path}, {"message", chinese(classify(message), message)}};
                    break;
                }
                if (channel.cancelled())
                    return {{"event", "done"}, {"ok", false}, {"cancelled", true}};
                channel.progress({{"phase", "test"}, {"fraction", std::min(1.0, double(reader.position()) / total)},
                                  {"current", item.path}});
            }
        }
        ++tested;
    }
    if (status < 0)
        errors << QJsonObject{{"path", QString()}, {"message", chinese("corrupt", error)}};
    return {{"event", "done"}, {"ok", errors.isEmpty()}, {"tested", tested}, {"errors", errors}};
}

QJsonObject libarchiveExtract(const Request &request, bool *fallback) {
    auto &channel = Channel::instance();
    const QJsonObject &json = request.json;
    const QString destination = destinationOf(json);
    const int destinationFd = ::open(destination.toLocal8Bit().constData(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (destinationFd < 0)
        return {{"event", "done"}, {"ok", false}, {"error", failure("destination", "无法打开目标文件夹：" + QString::fromLocal8Bit(std::strerror(errno)))}};
    struct Closer {
        int fd;
        ~Closer() { ::close(fd); }
    } closer{destinationFd};
    struct statvfs space{};
    const qint64 expected = qint64(json.value("expectedSize").toDouble(0));
    if (expected > 0 && ::fstatvfs(destinationFd, &space) == 0 &&
        qint64(space.f_bavail) * qint64(space.f_frsize) < expected + SpaceMargin)
        return {{"event", "done"}, {"ok", false}, {"error", failure("no-space", "磁盘空间不足，解压需要约 " + QString::number(expected >> 20) + " MB")}};

    Reader reader(request);
    QString error;
    bool unrecognized = false;
    if (!reader.open(&error, &unrecognized)) {
        *fallback = unrecognized;
        return openFailure(error);
    }
    const auto detected = namesFor(request, reader.zip());
    const Names names = detected.value_or(Names(request.encoding, reader.zip()));
    Stage stage(destinationFd);
    QString stageError;
    if (!stage.create(&stageError))
        return {{"event", "done"}, {"ok", false}, {"error", failure("destination", "无法写入目标文件夹：" + stageError)}};
    channel.send({{"event", "staging"}, {"path", stage.path(destination)}});

    QStringList selection;
    for (const auto &value : json.value("entries").toArray())
        selection << safeComponents(value.toString(), false).join(u'/');
    const QStringList prefix = safeComponents(json.value("prefix").toString(), false);
    const bool flatten = json.value("flatten").toBool();
    const qint64 total = std::max<qint64>(fileSize(request.archive), 1);
    const mode_t mask = currentUmask();

    struct DirectoryMeta {
        QStringList components;
        mode_t mode;
        qint64 mtime;
    };
    QList<DirectoryMeta> directories;
    QHash<QString, QByteArray> written; // stage path -> raw archive name
    QJsonObject fatal;
    bool cancelled = false;
    qint64 bytes = 0;
    qint64 checked = 0;
    int files = 0;
    double fraction = 0;
    QByteArray buffer;

    const auto placeName = [&](int parent, const QString &path, const QByteArray &raw, QString *name) {
        struct stat st{};
        const QByteArray encoded = encodePath(*name);
        if (::fstatat(parent, encoded.constData(), &st, AT_SYMLINK_NOFOLLOW) != 0)
            return;
        if (!S_ISDIR(st.st_mode) && written.value(path) == raw) {
            ::unlinkat(parent, encoded.constData(), 0); // the same name again: the later copy wins
            return;
        }
        // A different entry whose name decodes to the same path (or a file where a folder is).
        const QString original = *name;
        for (int number = 2; number < 10000; ++number) {
            *name = numberedName(original, number, false);
            if (::fstatat(parent, encodePath(*name).constData(), &st, AT_SYMLINK_NOFOLLOW) != 0)
                break;
        }
        warn("renamed", path, "压缩包内有重名项目，已改名为 " + *name);
    };

    Item item;
    int status;
    while ((status = reader.next(item, &error)) != 0) {
        if (status < 0) {
            fatal = failure(classify(error), chinese(classify(error), error));
            break;
        }
        if (channel.cancelled()) {
            cancelled = true;
            break;
        }
        names.fill(item);
        if (item.components.isEmpty()) {
            if (!item.path.isEmpty() && item.path != u"/" && item.path != u".")
                warn("unsafe-path", item.path, "路径试图跳出解压位置，已跳过");
            continue;
        }
        if (!selection.isEmpty() && std::none_of(selection.begin(), selection.end(), [&](const QString &chosen) {
                return item.path == chosen || item.path.startsWith(chosen + u'/');
            }))
            continue;
        QStringList components = item.components;
        if (!prefix.isEmpty()) {
            if (components.mid(0, prefix.size()) != prefix)
                continue;
            components = components.mid(prefix.size());
            if (components.isEmpty())
                continue;
        }
        if (flatten) {
            if (item.type == "dir")
                continue;
            components = {components.last()};
        }
        const QString stagePath = components.join(u'/');
        if (item.type == "dir") {
            const int fd = stage.directory(components, true);
            if (fd < 0) {
                warn("unsafe-path", item.path, blockedPath());
                continue;
            }
            ::close(fd);
            directories << DirectoryMeta{components, item.mode, item.mtime};
            continue;
        }
        if (item.type == "other") {
            warn("special", item.path, "设备文件、管道和套接字不会被解压，已跳过");
            continue;
        }
        const int parent = stage.directory(components.mid(0, components.size() - 1), true);
        if (parent < 0) {
            warn("unsafe-path", item.path, blockedPath());
            continue;
        }
        struct ParentCloser {
            int fd;
            ~ParentCloser() { ::close(fd); }
        } parentCloser{parent};
        QString name = components.last();
        placeName(parent, stagePath, item.raw, &name);
        const QByteArray encoded = encodePath(name);
        const QString placed = (components.mid(0, components.size() - 1) + QStringList{name}).join(u'/');
        if (item.type == "link") {
            if (::symlinkat(item.link.isEmpty() ? "" : names.decode(item.link).toUtf8().constData(), parent, encoded.constData()) != 0)
                warn("io", item.path, QString::fromLocal8Bit(std::strerror(errno)));
            continue;
        }
        if (item.type == "hardlink") {
            const QStringList target = safeComponents(names.decode(item.hardlink), reader.zip());
            QStringList targetComponents = target;
            if (!prefix.isEmpty() && targetComponents.mid(0, prefix.size()) == prefix)
                targetComponents = targetComponents.mid(prefix.size());
            if (flatten && !targetComponents.isEmpty())
                targetComponents = {targetComponents.last()};
            bool linked = false;
            if (!targetComponents.isEmpty() && written.contains(targetComponents.join(u'/'))) {
                const int from = stage.directory(targetComponents.mid(0, targetComponents.size() - 1), false);
                if (from >= 0) {
                    linked = ::linkat(from, encodePath(targetComponents.last()).constData(), parent, encoded.constData(), 0) == 0;
                    ::close(from);
                }
            }
            if (linked) {
                written.insert(placed, item.raw);
                continue;
            }
            if (item.size <= 0) {
                warn("hardlink", item.path, "硬链接的目标不在本次解压的内容中，已跳过");
                continue;
            }
        }
        const int fd = ::openat(parent, encoded.constData(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
        if (fd < 0) {
            warn("io", item.path, QString::fromLocal8Bit(std::strerror(errno)));
            continue;
        }
        bool ok = true;
        qint64 end = 0;
        for (;;) {
            const void *block = nullptr;
            size_t length = 0;
            la_int64_t offset = 0;
            const int read = archive_read_data_block(reader.handle(), &block, &length, &offset);
            if (read == ARCHIVE_EOF)
                break;
            if (read < ARCHIVE_WARN) {
                ok = false;
                if (reader.gaveUp()) {
                    cancelled = true;
                    break;
                }
                const QString message = QString::fromUtf8(archive_error_string(reader.handle()));
                const QString code = classify(message);
                if (code == "unsupported" && !item.encrypted && written.isEmpty()) {
                    ::close(fd);
                    *fallback = true;
                    return {};
                }
                if (read == ARCHIVE_FATAL)
                    fatal = failure(code, chinese(code, message));
                else
                    channel.send({{"event", "error"}, {"path", item.path}, {"code", code}, {"message", chinese(code, message)}});
                break;
            }
            const char *data = static_cast<const char *>(block);
            size_t done = 0;
            while (done < length) {
                const ssize_t put = ::pwrite(fd, data + done, length - done, off_t(offset + la_int64_t(done)));
                if (put < 0 && errno == EINTR)
                    continue;
                if (put <= 0) {
                    ok = false;
                    fatal = failure(errno == ENOSPC ? "no-space" : "io",
                                    errno == ENOSPC ? "磁盘空间不足" : QString::fromLocal8Bit(std::strerror(errno)));
                    break;
                }
                done += size_t(put);
            }
            if (!ok)
                break;
            end = std::max<qint64>(end, offset + qint64(length));
            bytes += qint64(length);
            if (bytes - checked > (32ll << 20)) {
                checked = bytes;
                if (::fstatvfs(fd, &space) == 0 && qint64(space.f_bavail) * qint64(space.f_frsize) < SpaceMargin) {
                    ok = false;
                    fatal = failure("no-space", "磁盘空间不足");
                    break;
                }
            }
            if (channel.cancelled()) {
                ok = false;
                cancelled = true;
                break;
            }
            fraction = std::max(fraction, std::min(1.0, double(reader.position()) / total));
            channel.progress({{"phase", "extract"}, {"fraction", fraction}, {"bytes", double(bytes)},
                              {"files", files}, {"current", item.path}});
        }
        if (ok) {
            if (item.size > end)
                ok = ::ftruncate(fd, item.size) == 0;
            mode_t mode = item.mode & 0777;
            if (mode == 0)
                mode = 0644;
            ::fchmod(fd, mode & ~mask);
            if (item.mtime > 0) {
                const timespec times[2] = {{item.mtime, 0}, {item.mtime, 0}};
                ::futimens(fd, times);
            }
        }
        ::close(fd);
        if (!ok) {
            ::unlinkat(parent, encoded.constData(), 0);
            if (cancelled || !fatal.isEmpty())
                break;
            continue;
        }
        written.insert(placed, item.raw);
        ++files;
    }
    channel.progress({{"phase", "commit"}, {"fraction", 1.0}, {"bytes", double(bytes)}, {"files", files}}, true);
    return finishExtract(stage, json, {}, cancelled, fatal, [&] {
        std::sort(directories.begin(), directories.end(), [](const auto &a, const auto &b) {
            return a.components.size() > b.components.size();
        });
        for (const auto &meta : std::as_const(directories)) {
            const int fd = stage.directory(meta.components, false);
            if (fd < 0)
                continue;
            ::fchmod(fd, ((meta.mode & 0777) & ~mask) | 0700);
            if (meta.mtime > 0) {
                const timespec times[2] = {{meta.mtime, 0}, {meta.mtime, 0}};
                ::futimens(fd, times);
            }
            ::close(fd);
        }
    });
}

}
