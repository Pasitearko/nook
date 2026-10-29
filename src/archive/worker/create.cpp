#include "worker.h"
#include "../archivenames.h"
#include <QFileInfo>
#include <QRandomGenerator>
#include <archive.h>
#include <archive_entry.h>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <fnmatch.h>
#include <sys/stat.h>
#include <unistd.h>

namespace nook::archive::worker {
namespace {

const QStringList SevenZipFormats{"7z", "zip"};
const QStringList TarFormats{"tar", "tar.gz", "tar.bz2", "tar.xz", "tar.zst"};
const QStringList StreamFormats{"gz", "bz2", "xz", "zst"};

QJsonObject fail(const QString &code, const QString &message) {
    return {{"event", "done"}, {"ok", false}, {"error", failure(code, message)}};
}

qint64 totalSize(const QStringList &items) {
    namespace fs = std::filesystem;
    qint64 total = 0;
    for (const auto &item : items) {
        std::error_code error;
        const fs::path root(item.toStdString());
        const auto status = fs::symlink_status(root, error);
        if (fs::is_regular_file(status)) {
            total += qint64(fs::file_size(root, error));
            continue;
        }
        if (!fs::is_directory(status))
            continue;
        for (fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, error), end;
             !error && it != end; it.increment(error)) {
            if (it->is_regular_file(error) && !it->is_symlink(error))
                total += qint64(it->file_size(error));
        }
    }
    return std::max<qint64>(total, 1);
}

bool excluded(const QJsonArray &patterns, const QString &relative) {
    const QByteArray name = QFileInfo(relative).fileName().toUtf8();
    const QByteArray path = relative.toUtf8();
    for (const auto &pattern : patterns) {
        const QByteArray glob = pattern.toString().toUtf8();
        if (!glob.isEmpty() && (::fnmatch(glob.constData(), name.constData(), 0) == 0 ||
                                ::fnmatch(glob.constData(), path.constData(), FNM_PATHNAME) == 0))
            return true;
    }
    return false;
}

class Writer {
  public:
    ~Writer() {
        if (m_archive)
            archive_write_free(m_archive);
    }
    QJsonObject open(const QString &format, const QJsonObject &options, const QString &path) {
        m_archive = archive_write_new();
        const bool stream = StreamFormats.contains(format);
        if (stream)
            archive_write_set_format_raw(m_archive);
        else
            archive_write_set_format_pax_restricted(m_archive);
        const QString filter = stream ? format : format.section(u'.', 1);
        int low = 0, high = 9;
        if (filter == "gz")
            archive_write_add_filter_gzip(m_archive);
        else if (filter == "bz2")
            archive_write_add_filter_bzip2(m_archive), low = 1;
        else if (filter == "xz")
            archive_write_add_filter_xz(m_archive);
        else if (filter == "zst")
            archive_write_add_filter_zstd(m_archive), low = 1, high = 22;
        if (!filter.isEmpty() && options.contains("level")) {
            const int level = options.value("level").toInt(-1);
            if (level < low || level > high)
                return fail("options", QStringLiteral("压缩等级应在 %1～%2 之间").arg(low).arg(high));
            archive_write_set_filter_option(m_archive, nullptr, "compression-level", QByteArray::number(level).constData());
        }
        if ((filter == "xz" || filter == "zst") && options.contains("threads")) {
            const int threads = options.value("threads").toInt(0);
            if (threads >= 1 && threads <= 256)
                archive_write_set_filter_option(m_archive, nullptr, "threads", QByteArray::number(threads).constData());
        }
        if (archive_write_open_filename(m_archive, path.toLocal8Bit().constData()) != ARCHIVE_OK)
            return fail("io", QString::fromUtf8(archive_error_string(m_archive)));
        return {};
    }
    ::archive *handle() const { return m_archive; }
    QJsonObject close() {
        if (archive_write_close(m_archive) != ARCHIVE_OK)
            return fail(errno == ENOSPC ? "no-space" : "io", QString::fromUtf8(archive_error_string(m_archive)));
        return {};
    }

  private:
    ::archive *m_archive = nullptr;
};

QJsonObject libarchiveCreate(const Request &request, const QString &temporaryPath) {
    auto &channel = Channel::instance();
    const QJsonObject &json = request.json;
    const QString format = json.value("format").toString();
    const QJsonObject options = json.value("options").toObject();
    QStringList items;
    for (const auto &item : json.value("items").toArray())
        items << QFileInfo(item.toString()).absoluteFilePath();
    const qint64 total = totalSize(items);
    qint64 done = 0;
    Writer writer;
    if (auto error = writer.open(format, options, temporaryPath); !error.isEmpty())
        return error;
    QByteArray buffer(1 << 17, Qt::Uninitialized);
    const auto copy = [&](::archive *source, int fd, const QString &name) -> QJsonObject {
        for (;;) {
            const la_ssize_t read = source ? archive_read_data(source, buffer.data(), size_t(buffer.size()))
                                           : ::read(fd, buffer.data(), size_t(buffer.size()));
            if (read == 0)
                return {};
            if (read < 0) {
                if (!source && errno == EINTR)
                    continue;
                warn("read", name, source ? QString::fromUtf8(archive_error_string(source)) : QString::fromLocal8Bit(std::strerror(errno)));
                return {};
            }
            if (archive_write_data(writer.handle(), buffer.constData(), size_t(read)) < 0)
                return fail(errno == ENOSPC ? "no-space" : "io", QString::fromUtf8(archive_error_string(writer.handle())));
            done += read;
            if (channel.cancelled())
                return {{"event", "done"}, {"ok", false}, {"cancelled", true}};
            channel.progress({{"phase", "create"}, {"fraction", std::min(1.0, double(done) / total)}, {"current", name}});
        }
    };

    if (StreamFormats.contains(format)) {
        const QString source = items.value(0);
        const int fd = ::open(source.toLocal8Bit().constData(), O_RDONLY | O_CLOEXEC);
        struct stat st{};
        if (fd < 0 || ::fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) {
            if (fd >= 0)
                ::close(fd);
            return fail("options", "单文件压缩格式只能压缩一个文件");
        }
        archive_entry *entry = archive_entry_new();
        archive_entry_set_pathname(entry, QFileInfo(source).fileName().toUtf8().constData());
        archive_entry_set_filetype(entry, AE_IFREG);
        archive_entry_set_perm(entry, st.st_mode & 0777);
        archive_entry_set_size(entry, st.st_size);
        archive_entry_set_mtime(entry, st.st_mtim.tv_sec, st.st_mtim.tv_nsec);
        archive_write_header(writer.handle(), entry);
        archive_entry_free(entry);
        QJsonObject result = copy(nullptr, fd, QFileInfo(source).fileName());
        ::close(fd);
        if (!result.isEmpty())
            return result;
        if (auto error = writer.close(); !error.isEmpty())
            return error;
        return {{"event", "done"}, {"ok", true}};
    }

    const QJsonArray patterns = options.value("exclude").toArray();
    ::archive *disk = archive_read_disk_new();
    struct DiskFree {
        ::archive *a;
        ~DiskFree() { archive_read_free(a); }
    } diskFree{disk};
    archive_read_disk_set_standard_lookup(disk);
    archive_read_disk_set_symlink_physical(disk); // a link is stored as a link
    for (const auto &item : std::as_const(items)) {
        const QString base = QFileInfo(item).fileName();
        const QByteArray encoded = item.toLocal8Bit();
        if (archive_read_disk_open(disk, encoded.constData()) != ARCHIVE_OK) {
            warn("read", item, QString::fromUtf8(archive_error_string(disk)));
            continue;
        }
        for (;;) {
            archive_entry *entry = archive_entry_new();
            const int status = archive_read_next_header2(disk, entry);
            if (status == ARCHIVE_EOF) {
                archive_entry_free(entry);
                break;
            }
            if (status < ARCHIVE_WARN) {
                warn("read", QString::fromLocal8Bit(archive_entry_sourcepath(entry)), QString::fromUtf8(archive_error_string(disk)));
                archive_entry_free(entry);
                if (status == ARCHIVE_FATAL)
                    break;
                continue;
            }
            const QString source = QString::fromLocal8Bit(archive_entry_sourcepath(entry));
            const QString relative = base + source.mid(item.size());
            if (source != item && excluded(patterns, relative)) {
                archive_entry_free(entry);
                continue; // not descending skips an excluded folder's contents too
            }
            archive_read_disk_descend(disk);
            archive_entry_set_pathname(entry, relative.toUtf8().constData());
            const int written = archive_write_header(writer.handle(), entry);
            if (written < ARCHIVE_WARN) {
                const QString message = QString::fromUtf8(archive_error_string(writer.handle()));
                archive_entry_free(entry);
                if (written == ARCHIVE_FATAL)
                    return fail(errno == ENOSPC ? "no-space" : "io", message);
                warn("write", relative, message);
                continue;
            }
            QJsonObject result;
            if (archive_entry_filetype(entry) == AE_IFREG && archive_entry_size(entry) > 0)
                result = copy(disk, -1, relative);
            archive_entry_free(entry);
            if (!result.isEmpty())
                return result;
        }
        archive_read_close(disk);
    }
    if (auto error = writer.close(); !error.isEmpty())
        return error;
    return {{"event", "done"}, {"ok", true}};
}

QString renamed(const QString &file, const QString &output, int number) {
    if (number == 1)
        return file;
    if (file.startsWith(output))
        return numberedName(output, number, false) + file.mid(output.size());
    return numberedName(file, number, false);
}

}

QJsonObject create(const Request &request) {
    auto &channel = Channel::instance();
    const QJsonObject &json = request.json;
    const QString format = json.value("format").toString();
    if (!SevenZipFormats.contains(format) && !TarFormats.contains(format) && !StreamFormats.contains(format))
        return fail("options", "不支持创建此格式：" + format);
    const QFileInfo output(json.value("output").toString());
    const QString name = output.fileName();
    if (name.isEmpty() || name == u"." || name == u"..")
        return fail("options", "压缩包名称无效");
    QStringList items;
    for (const auto &item : json.value("items").toArray()) {
        struct stat st{};
        const QString path = QFileInfo(item.toString()).absoluteFilePath();
        if (::lstat(path.toLocal8Bit().constData(), &st) != 0)
            return fail("not-found", "找不到要压缩的项目：" + path);
        items << path;
    }
    if (items.isEmpty())
        return fail("options", "没有要压缩的项目");
    if (StreamFormats.contains(format) && items.size() != 1)
        return fail("options", "单文件压缩格式只能压缩一个文件，多个项目请使用 tar." + format);

    const QString directory = output.absolutePath();
    const int outputFd = ::open(directory.toLocal8Bit().constData(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (outputFd < 0)
        return fail("destination", "无法打开保存位置：" + QString::fromLocal8Bit(std::strerror(errno)));
    struct Closer {
        int fd;
        QByteArray temporary;
        ~Closer() {
            if (!temporary.isEmpty())
                removeTree(fd, temporary);
            ::close(fd);
        }
    } closer{outputFd, {}};
    for (int attempt = 0; attempt < 32 && closer.temporary.isEmpty(); ++attempt) {
        const QByteArray candidate = ".nook-create-" + QByteArray::number(QRandomGenerator::global()->generate(), 16);
        if (::mkdirat(outputFd, candidate.constData(), 0700) == 0)
            closer.temporary = candidate;
        else if (errno != EEXIST)
            break;
    }
    if (closer.temporary.isEmpty())
        return fail("destination", "无法写入保存位置：" + QString::fromLocal8Bit(std::strerror(errno)));
    const QString temporaryDir = directory + u'/' + QString::fromUtf8(closer.temporary);
    channel.send({{"event", "staging"}, {"path", temporaryDir}});
    const QString temporaryPath = temporaryDir + u'/' + name;

    QJsonObject result = SevenZipFormats.contains(format) ? sevenZipCreate(request, temporaryPath)
                                                          : libarchiveCreate(request, temporaryPath);
    if (!result.value("ok").toBool())
        return result;

    const int temporaryFd = ::openat(outputFd, closer.temporary.constData(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    QStringList files;
    if (temporaryFd >= 0) {
        const std::filesystem::path root(temporaryDir.toStdString());
        std::error_code error;
        for (const auto &entry : std::filesystem::directory_iterator(root, error))
            files << QString::fromStdString(entry.path().filename().string());
        files.sort();
    }
    if (files.isEmpty()) {
        if (temporaryFd >= 0)
            ::close(temporaryFd);
        return fail("failed", "没有生成压缩包");
    }
    if (json.value("testAfter").toBool()) {
        Request test = request;
        test.op = "test";
        test.archive = temporaryDir + u'/' + files.first();
        bool fallback = false;
        QJsonObject tested = backendFor(test.archive) == Backend::SevenZip ? sevenZipTest(test) : libarchiveTest(test, &fallback);
        if (fallback)
            tested = sevenZipTest(test);
        if (!tested.value("ok").toBool()) {
            ::close(temporaryFd);
            if (tested.value("cancelled").toBool())
                return tested;
            return {{"event", "done"}, {"ok", false}, {"error", failure("test-failed", "压缩完成，但校验没有通过")},
                    {"errors", tested.value("errors")}};
        }
    }

    // Commit the whole set under one name: a volume set must not end up half renamed.
    Conflicts conflicts;
    conflicts.policy = json.value("conflict").toString("ask");
    int number = 1;
    for (;; ++number) {
        bool clash = false;
        struct stat existing{}, incoming{};
        QString clashing;
        for (const auto &file : std::as_const(files)) {
            if (::fstatat(outputFd, encodePath(renamed(file, name, number)).constData(), &existing, AT_SYMLINK_NOFOLLOW) == 0) {
                clash = true;
                clashing = renamed(file, name, number);
                ::fstatat(temporaryFd, encodePath(file).constData(), &incoming, AT_SYMLINK_NOFOLLOW);
                break;
            }
        }
        if (!clash)
            break;
        if (number > 1)
            continue;
        const Choice choice = conflicts.resolve(clashing, existing, incoming);
        if (choice == Choice::Skip) {
            ::close(temporaryFd);
            return {{"event", "done"}, {"ok", false}, {"skipped", 1}, {"cancelled", true}};
        }
        if (choice == Choice::Overwrite && !S_ISDIR(existing.st_mode)) {
            number = 0; // overwrite under the requested names
            break;
        }
    }
    QStringList created;
    for (const auto &file : std::as_const(files)) {
        const QString target = renamed(file, name, std::max(number, 1));
        const int moved = number == 0
                              ? ::renameat(temporaryFd, encodePath(file).constData(), outputFd, encodePath(target).constData())
                              : ::renameat2(temporaryFd, encodePath(file).constData(), outputFd, encodePath(target).constData(), RENAME_NOREPLACE);
        if (moved != 0) {
            ::close(temporaryFd);
            return fail("io", QString::fromLocal8Bit(std::strerror(errno)));
        }
        created << target;
    }
    ::close(temporaryFd);
    return {{"event", "done"}, {"ok", true}, {"created", QJsonArray::fromStringList(created)},
            {"output", directory + u'/' + created.first()}, {"destination", directory}};
}

}
