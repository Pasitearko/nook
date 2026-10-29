#include "worker.h"
#include "../archivenames.h"
#include <QDir>
#include <QFileInfo>
#include <QHash>
#include <QRandomGenerator>
#include <QSet>
#include <archive.h>
#include <archive_entry.h>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>

// Changing a package: every change is made to a copy in a private folder next to it, the copy
// is checked, and only then does it replace the original (one rename), so a failure, a
// cancellation or a crash leaves the original as it was. ZIP and 7z are changed by 7-Zip,
// tar and compressed tar are rewritten through libarchive.
namespace nook::archive::worker {
namespace {

QJsonObject fail(const QString &code, const QString &message) {
    return {{"event", "done"}, {"ok", false}, {"error", failure(code, message)}};
}
QJsonObject cancelled() { return {{"event", "done"}, {"ok", false}, {"cancelled", true}}; }
QString errorText() { return QString::fromLocal8Bit(std::strerror(errno)); }

enum class Kind { Zip, SevenZip, Tar, Other };

Kind kindOf(const QString &path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return Kind::Other;
    const QByteArray magic = file.read(6);
    if (magic.startsWith(QByteArray("7z\xBC\xAF\x27\x1C", 6)))
        return Kind::SevenZip;
    if (magic.startsWith("PK\x03\x04") || magic.startsWith("PK\x05\x06"))
        return Kind::Zip;
    struct archive *a = archive_read_new();
    archive_read_support_filter_all(a);
    archive_read_support_format_tar(a);
    archive_read_support_format_gnutar(a);
    archive_entry *entry = nullptr;
    Kind kind = Kind::Other;
    if (archive_read_open_filename(a, QFile::encodeName(path).constData(), 1 << 16) == ARCHIVE_OK) {
        const int status = archive_read_next_header(a, &entry);
        if ((status == ARCHIVE_OK || status == ARCHIVE_WARN || status == ARCHIVE_EOF) &&
            (archive_format(a) & ARCHIVE_FORMAT_BASE_MASK) == ARCHIVE_FORMAT_TAR)
            kind = Kind::Tar;
    }
    archive_read_free(a);
    return kind;
}

// A private folder in dirFd ("prefix" + random); empty with errno on failure.
QByteArray makeStage(int dirFd, const char *prefix) {
    for (int attempt = 0; attempt < 32; ++attempt) {
        const QByteArray name = prefix + QByteArray::number(QRandomGenerator::global()->generate(), 16);
        if (::mkdirat(dirFd, name.constData(), 0700) == 0)
            return name;
        if (errno != EEXIST)
            return {};
    }
    return {};
}

struct StageGuard {
    int fd = -1;
    QByteArray name;
    ~StageGuard() {
        if (!name.isEmpty())
            removeTree(fd, name);
        if (fd >= 0)
            ::close(fd);
    }
};

// Copies source to target (reflinked where the file system can), keeping mode and times.
bool copyFile(const QString &source, const QString &target, bool followLinks, QString *error) {
    const int in = ::open(QFile::encodeName(source).constData(), O_RDONLY | O_CLOEXEC | (followLinks ? 0 : O_NOFOLLOW));
    if (in < 0) {
        *error = errorText();
        return false;
    }
    struct stat st{};
    ::fstat(in, &st);
    const int out = ::open(QFile::encodeName(target).constData(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (out < 0) {
        *error = errorText();
        ::close(in);
        return false;
    }
    auto &channel = Channel::instance();
    bool ok = true;
    for (;;) {
        if (channel.cancelled()) {
            ok = false;
            break;
        }
        const ssize_t copied = ::copy_file_range(in, nullptr, out, nullptr, 8 << 20, 0);
        if (copied == 0)
            break;
        if (copied < 0) {
            if (errno == EXDEV || errno == ENOSYS || errno == EINVAL) {
                char buffer[1 << 16];
                ssize_t got;
                while ((got = ::read(in, buffer, sizeof buffer)) > 0)
                    if (::write(out, buffer, size_t(got)) != got) {
                        got = -1;
                        break;
                    }
                ok = got == 0;
            } else {
                ok = false;
            }
            if (!ok)
                *error = errorText();
            break;
        }
    }
    ::fchmod(out, st.st_mode & 0777);
    const struct timespec times[2] = {st.st_atim, st.st_mtim};
    ::futimens(out, times);
    if (::close(out) != 0 && ok) {
        ok = false;
        *error = errorText();
    }
    ::close(in);
    return ok;
}

struct Existing {
    bool dir = false;
    qint64 size = 0;
    qint64 mtime = 0;
};

// The package's paths, with the folders that are only implied by the paths inside them.
QHash<QString, Existing> indexOf(const QList<QPair<QString, Existing>> &entries) {
    QHash<QString, Existing> index;
    for (const auto &[path, entry] : entries) {
        index.insert(path, entry);
        for (qsizetype slash = path.lastIndexOf(u'/'); slash > 0; slash = path.lastIndexOf(u'/', slash - 1)) {
            const QString parent = path.left(slash);
            if (index.contains(parent))
                break;
            index.insert(parent, Existing{true, 0, 0});
        }
    }
    return index;
}

QList<QPair<QString, Existing>> fromSevenZip(const QJsonArray &items) {
    QList<QPair<QString, Existing>> entries;
    for (const auto &value : items) {
        const QJsonObject item = value.toObject();
        if (item.value("unsafe").toBool())
            continue;
        entries << qMakePair(item.value("path").toString(),
                             Existing{item.value("type").toString() == "dir", qint64(item.value("size").toDouble()),
                                      qint64(item.value("mtime").toDouble())});
    }
    return entries;
}

QString entryPath(archive_entry *entry) {
    const char *utf8 = archive_entry_pathname_utf8(entry);
    const QString raw = utf8 ? QString::fromUtf8(utf8) : QString::fromLocal8Bit(archive_entry_pathname(entry));
    return safeComponents(raw, false).join(u'/');
}

struct TarReader {
    struct archive *a = nullptr;
    explicit TarReader(const QString &path) {
        a = archive_read_new();
        archive_read_support_filter_all(a);
        archive_read_support_format_tar(a);
        archive_read_support_format_gnutar(a);
        if (archive_read_open_filename(a, QFile::encodeName(path).constData(), 1 << 16) != ARCHIVE_OK) {
            archive_read_free(a);
            a = nullptr;
        }
    }
    ~TarReader() {
        if (a)
            archive_read_free(a);
    }
};

bool tarEntries(const QString &path, QList<QPair<QString, Existing>> *entries, QString *error) {
    TarReader reader(path);
    if (!reader.a) {
        *error = "无法读取压缩包";
        return false;
    }
    archive_entry *entry = nullptr;
    int status;
    while ((status = archive_read_next_header(reader.a, &entry)) == ARCHIVE_OK || status == ARCHIVE_WARN) {
        const QString name = entryPath(entry);
        if (!name.isEmpty())
            *entries << qMakePair(name, Existing{archive_entry_filetype(entry) == AE_IFDIR, archive_entry_size(entry),
                                                 archive_entry_mtime(entry)});
        archive_read_data_skip(reader.a);
        if (Channel::instance().cancelled())
            return false;
    }
    if (status != ARCHIVE_EOF) {
        *error = QString::fromUtf8(archive_error_string(reader.a));
        return false;
    }
    return true;
}

// Reads every byte of a tar package back: the rewritten copy must open and decompress.
bool tarVerify(const QString &path) {
    TarReader reader(path);
    if (!reader.a)
        return false;
    archive_entry *entry = nullptr;
    int status;
    char buffer[1 << 16];
    while ((status = archive_read_next_header(reader.a, &entry)) == ARCHIVE_OK || status == ARCHIVE_WARN) {
        la_ssize_t got;
        while ((got = archive_read_data(reader.a, buffer, sizeof buffer)) > 0) {
        }
        if (got < 0)
            return false;
    }
    return status == ARCHIVE_EOF;
}

// What adding will put into the package.
struct Planned {
    QString source;
    QString path; // inside the package
    enum Type { Directory, File, Link } type = File;
    struct stat st{};
    QString target; // links
};

struct AddPlan {
    QList<Planned> items;
    QStringList replaced;  // existing entries (and what is under them) that make way
    int skipped = 0;
    bool cancelled = false;
};

bool under(const QString &path, const QString &root) {
    return path == root || (path.size() > root.size() && path.startsWith(root) && path.at(root.size()) == u'/');
}

QString join(const QString &folder, const QString &name) { return folder.isEmpty() ? name : folder + u'/' + name; }

void planAdd(const QString &source, const QString &folder, QString name, Kind kind, const QHash<QString, Existing> &index,
             QSet<QString> &taken, Conflicts &conflicts, AddPlan &plan) {
    auto &channel = Channel::instance();
    if (channel.cancelled()) {
        plan.cancelled = true;
        return;
    }
    struct stat st{};
    if (::lstat(QFile::encodeName(source).constData(), &st) != 0) {
        warn("read", source, "无法读取：" + errorText());
        return;
    }
    QString target;
    if (S_ISLNK(st.st_mode)) {
        char buffer[4096];
        const ssize_t length = ::readlink(QFile::encodeName(source).constData(), buffer, sizeof buffer);
        target = length > 0 ? QFile::decodeName(QByteArray(buffer, int(length))) : QString();
        // ZIP keeps what a link points to (the same rule as creating one).
        if (kind == Kind::Zip) {
            struct stat followed{};
            if (::stat(QFile::encodeName(source).constData(), &followed) != 0 || !S_ISREG(followed.st_mode)) {
                warn("unsafe-link", source, "链接目标不是文件，已跳过");
                return;
            }
            st = followed;
        }
    } else if (!S_ISDIR(st.st_mode) && !S_ISREG(st.st_mode)) {
        warn("special", source, "设备文件、管道和套接字不会加入压缩包");
        return;
    }
    QString path = join(folder, name);
    const bool directory = S_ISDIR(st.st_mode);
    const auto existing = index.constFind(path);
    // Folders merge; anything else that is already there is a conflict.
    if (existing != index.constEnd() && !(directory && existing->dir)) {
        struct stat old{};
        old.st_mode = existing->dir ? S_IFDIR : S_IFREG;
        old.st_size = existing->size;
        old.st_mtim.tv_sec = existing->mtime;
        const Choice choice = conflicts.resolve(path, old, st);
        if (channel.cancelled()) {
            plan.cancelled = true;
            return;
        }
        if (choice == Choice::Skip) {
            ++plan.skipped;
            return;
        }
        if (choice == Choice::Rename) {
            for (int number = 2;; ++number) {
                const QString candidate = numberedName(name, number, directory);
                if (!index.contains(join(folder, candidate)) && !taken.contains(join(folder, candidate))) {
                    name = candidate;
                    break;
                }
            }
            path = join(folder, name);
        } else {
            plan.replaced << path;
        }
    }
    taken.insert(path);
    Planned item;
    item.source = source;
    item.path = path;
    item.st = st;
    item.type = directory ? Planned::Directory : S_ISLNK(st.st_mode) ? Planned::Link : Planned::File;
    item.target = target;
    plan.items << item;
    if (directory) {
        const QDir dir(source);
        for (const auto &child : dir.entryList(QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot, QDir::Name)) {
            planAdd(dir.filePath(child), path, child, kind, index, taken, conflicts, plan);
            if (plan.cancelled)
                return;
        }
    }
}

// Lays the planned items out under root as they will be named inside the package.
bool materialize(const QString &root, const QString &folder, const QList<Planned> &items, QString *error) {
    if (!folder.isEmpty() && !QDir().mkpath(root + u'/' + folder)) {
        *error = "无法准备要添加的文件";
        return false;
    }
    for (const auto &item : items) {
        const QString target = root + u'/' + item.path;
        if (item.type == Planned::Directory) {
            if (::mkdir(QFile::encodeName(target).constData(), 0700) != 0 && errno != EEXIST) {
                *error = errorText();
                return false;
            }
        } else if (item.type == Planned::Link) {
            if (::symlink(QFile::encodeName(item.target).constData(), QFile::encodeName(target).constData()) != 0) {
                *error = errorText();
                return false;
            }
            const struct timespec times[2] = {item.st.st_atim, item.st.st_mtim};
            ::utimensat(AT_FDCWD, QFile::encodeName(target).constData(), times, AT_SYMLINK_NOFOLLOW);
        } else if (!copyFile(item.source, target, true, error)) {
            if (error->isEmpty())
                *error = "已取消";
            return false;
        }
    }
    // Folder times last: filling a folder changes its time.
    for (auto it = items.crbegin(); it != items.crend(); ++it)
        if (it->type == Planned::Directory) {
            ::chmod(QFile::encodeName(root + u'/' + it->path).constData(), (it->st.st_mode & 0777) | 0700);
            const struct timespec times[2] = {it->st.st_atim, it->st.st_mtim};
            ::utimensat(AT_FDCWD, QFile::encodeName(root + u'/' + it->path).constData(), times, 0);
        }
    return true;
}

// Rewrites a tar package from source to target: dropped paths go, renamed ones get their new
// name, and added items are appended at the end.
QJsonObject tarRewrite(const QString &source, const QString &target, const std::function<bool(const QString &)> &drop,
                       const std::function<QString(const QString &)> &rename, const QList<Planned> &added) {
    auto &channel = Channel::instance();
    TarReader reader(source);
    if (!reader.a)
        return fail("corrupt", "无法读取压缩包");
    struct archive *w = archive_write_new();
    struct Closer {
        struct archive *w;
        ~Closer() { archive_write_free(w); }
    } closer{w};
    archive_write_set_format_pax_restricted(w);
    archive_write_set_bytes_in_last_block(w, 1);
    archive_entry *entry = nullptr;
    int status = archive_read_next_header(reader.a, &entry);
    // The filters are known once the first header is read: write the same compression.
    for (int i = archive_filter_count(reader.a) - 1; i >= 0; --i) {
        const int code = archive_filter_code(reader.a, i);
        if (code != ARCHIVE_FILTER_NONE)
            archive_write_add_filter(w, code);
    }
    if (archive_write_open_filename(w, QFile::encodeName(target).constData()) != ARCHIVE_OK)
        return fail("destination", QString::fromUtf8(archive_error_string(w)));
    const qint64 total = std::max<qint64>(1, QFileInfo(source).size());
    char buffer[1 << 16];
    for (; status == ARCHIVE_OK || status == ARCHIVE_WARN; status = archive_read_next_header(reader.a, &entry)) {
        if (channel.cancelled())
            return cancelled();
        const QString path = entryPath(entry);
        if (path.isEmpty() || drop(path)) {
            archive_read_data_skip(reader.a);
            continue;
        }
        const QString renamed = rename(path);
        if (renamed != path)
            archive_entry_set_pathname_utf8(entry, renamed.toUtf8().constData());
        if (const char *link = archive_entry_hardlink(entry)) {
            const QString linked = safeComponents(QString::fromUtf8(link), false).join(u'/');
            if (drop(linked)) {
                warn("hardlink", path, "硬链接指向的文件已删除，此项一并删除");
                archive_read_data_skip(reader.a);
                continue;
            }
            if (rename(linked) != linked)
                archive_entry_set_hardlink_utf8(entry, rename(linked).toUtf8().constData());
        }
        if (archive_write_header(w, entry) < ARCHIVE_WARN)
            return fail("failed", QString::fromUtf8(archive_error_string(w)));
        la_ssize_t got;
        while ((got = archive_read_data(reader.a, buffer, sizeof buffer)) > 0)
            if (archive_write_data(w, buffer, size_t(got)) < 0)
                return fail("failed", QString::fromUtf8(archive_error_string(w)));
        if (got < 0)
            return fail("corrupt", QString::fromUtf8(archive_error_string(reader.a)));
        channel.progress({{"phase", "update"}, {"fraction", std::min(0.95, double(archive_filter_bytes(reader.a, -1)) / total)}});
    }
    if (status != ARCHIVE_EOF)
        return fail("corrupt", QString::fromUtf8(archive_error_string(reader.a)));
    for (const auto &item : added) {
        if (channel.cancelled())
            return cancelled();
        archive_entry *out = archive_entry_new();
        archive_entry_copy_stat(out, &item.st);
        archive_entry_set_pathname_utf8(out, item.path.toUtf8().constData());
        archive_entry_set_uid(out, 0);
        archive_entry_set_gid(out, 0);
        if (item.type == Planned::Link) {
            archive_entry_set_size(out, 0);
            archive_entry_set_symlink_utf8(out, item.target.toUtf8().constData());
        } else if (item.type == Planned::Directory) {
            archive_entry_set_size(out, 0);
        }
        if (archive_write_header(w, out) < ARCHIVE_WARN) {
            archive_entry_free(out);
            return fail("failed", QString::fromUtf8(archive_error_string(w)));
        }
        archive_entry_free(out);
        if (item.type == Planned::File) {
            QFile file(item.source);
            if (!file.open(QIODevice::ReadOnly))
                return fail("read", "无法读取：" + item.source);
            qint64 got;
            while ((got = file.read(buffer, sizeof buffer)) > 0)
                if (archive_write_data(w, buffer, size_t(got)) < 0)
                    return fail("failed", QString::fromUtf8(archive_error_string(w)));
        }
        channel.progress({{"phase", "update"}, {"current", item.path}});
    }
    if (archive_write_close(w) != ARCHIVE_OK)
        return fail(QString::fromUtf8(archive_error_string(w)).contains("space", Qt::CaseInsensitive) ? "no-space" : "failed",
                    QString::fromUtf8(archive_error_string(w)));
    return {};
}

QStringList paths(const QJsonArray &values) {
    QStringList list;
    for (const auto &value : values) {
        const QString path = safeComponents(value.toString(), false).join(u'/');
        if (!path.isEmpty() && !list.contains(path))
            list << path;
    }
    return list;
}

} // namespace

QJsonObject modify(const Request &request) {
    auto &channel = Channel::instance();
    const QJsonObject &json = request.json;
    const QString op = request.op;
    const QString archive = request.archive;
    const QFileInfo info(archive);
    if (!info.isFile())
        return fail("not-found", "找不到压缩包");
    if (volumesOf(archive).split)
        return fail("unsupported", "分卷压缩包不能直接修改，可以先转换为单个压缩包");
    const Kind kind = kindOf(archive);
    if (kind == Kind::Other)
        return fail("unsupported", "这种格式只能查看和解压；可以转换为 ZIP 或 7z 后再修改");

    StageGuard stage;
    stage.fd = ::open(QFile::encodeName(info.absolutePath()).constData(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (stage.fd < 0)
        return fail("destination", "无法打开压缩包所在的文件夹：" + errorText());
    struct statvfs space{};
    if (::fstatvfs(stage.fd, &space) == 0 && qint64(space.f_bavail) * qint64(space.f_frsize) < info.size() + (64ll << 20))
        return fail("no-space", "磁盘空间不足：修改时需要先复制一份压缩包");
    stage.name = makeStage(stage.fd, ".nook-modify-");
    if (stage.name.isEmpty())
        return fail("destination", "无法写入压缩包所在的文件夹：" + errorText());
    const QString stagePath = info.absolutePath() + u'/' + QFile::decodeName(stage.name);
    channel.send({{"event", "staging"}, {"path", stagePath}});
    const QString copy = stagePath + u'/' + info.fileName();
    channel.progress({{"phase", "copy"}, {"fraction", 0.0}}, true);
    QString error;
    if (!copyFile(archive, copy, true, &error))
        return channel.cancelled() ? cancelled() : fail("failed", "无法复制压缩包：" + error);

    // What is in it now.
    QList<QPair<QString, Existing>> entries;
    std::optional<QString> password;
    if (request.hasPassword && !request.password.isEmpty())
        password = request.password;
    bool headerEncrypted = false;
    if (kind == Kind::Tar) {
        if (!tarEntries(copy, &entries, &error))
            return channel.cancelled() ? cancelled() : fail("corrupt", "无法读取压缩包：" + error);
    } else {
        Request listing = request;
        listing.hasPassword = password.has_value();
        listing.password = password.value_or(QString());
        const EditListing edit = sevenZipEditListing(listing, copy, op == "add");
        if (!edit.ok)
            return edit.done;
        entries = fromSevenZip(edit.entries);
        password = edit.password;
        headerEncrypted = edit.headerEncrypted;
    }
    const QHash<QString, Existing> index = indexOf(entries);
    const auto descendants = [&](const QStringList &roots) {
        QStringList all;
        for (const auto &[path, entry] : std::as_const(entries))
            for (const auto &root : roots)
                if (under(path, root)) {
                    all << path;
                    break;
                }
        return all;
    };
    QStringList passwordSwitches;
    if (password) {
        passwordSwitches << "-p";
        if (kind == Kind::Zip)
            passwordSwitches << "-mem=AES256";
        if (headerEncrypted)
            passwordSwitches << "-mhe=on";
    }
    const QStringList base = sevenZipSwitches() + QStringList{"-spd", "-bsp2", "-y"};

    int changed = 0, skipped = 0;
    if (op == "delete") {
        const QStringList roots = paths(json.value("entries").toArray());
        for (const auto &root : roots)
            if (!index.contains(root))
                return fail("not-found", "压缩包里没有“" + root + "”");
        if (roots.isEmpty())
            return fail("options", "没有要删除的项目");
        const QStringList doomed = descendants(roots);
        changed = int(roots.size());
        if (kind == Kind::Tar) {
            const QString rewritten = stagePath + u"/.rewrite";
            QJsonObject result = tarRewrite(copy, rewritten, [&](const QString &path) {
                return std::any_of(roots.begin(), roots.end(), [&](const QString &root) { return under(path, root); });
            }, [](const QString &path) { return path; }, {});
            if (!result.isEmpty())
                return result;
            ::rename(QFile::encodeName(rewritten).constData(), QFile::encodeName(copy).constData());
        } else if (!doomed.isEmpty()) {
            QJsonObject result = sevenZipEdit(QStringList{"d"} + base + passwordSwitches + QStringList{copy}, doomed, {}, "update", password);
            if (!result.isEmpty())
                return result;
        }
    } else if (op == "rename") {
        const QString from = safeComponents(json.value("from").toString(), false).join(u'/');
        const QString to = safeComponents(json.value("to").toString(), false).join(u'/');
        if (from.isEmpty() || to.isEmpty() || !index.contains(from))
            return fail("not-found", "压缩包里没有“" + json.value("from").toString() + "”");
        if (from == to)
            return {{"event", "done"}, {"ok", true}, {"changed", 0}};
        if (under(to, from))
            return fail("options", "不能把文件夹移动到它自己里面");
        if (index.contains(to))
            return fail("exists", "已有同名项目“" + to.section(u'/', -1) + "”");
        const auto renamed = [&](const QString &path) { return under(path, from) ? to + path.mid(from.size()) : path; };
        changed = 1;
        if (kind == Kind::Tar) {
            const QString rewritten = stagePath + u"/.rewrite";
            QJsonObject result = tarRewrite(copy, rewritten, [](const QString &) { return false; }, renamed, {});
            if (!result.isEmpty())
                return result;
            ::rename(QFile::encodeName(rewritten).constData(), QFile::encodeName(copy).constData());
        } else {
            QStringList pairs;
            for (const auto &path : descendants({from}))
                pairs << path << renamed(path);
            if (!pairs.isEmpty()) {
                QJsonObject result = sevenZipEdit(QStringList{"rn"} + base + passwordSwitches + QStringList{copy} + pairs, {}, {}, "update", password);
                if (!result.isEmpty())
                    return result;
            } else {
                return fail("unsupported", "这个文件夹在压缩包里没有单独的条目，请重命名其中的项目");
            }
        }
    } else if (op == "add") {
        const QString folder = safeComponents(json.value("prefix").toString(), false).join(u'/');
        if (!folder.isEmpty() && index.contains(folder) && !index.value(folder).dir)
            return fail("options", "“" + folder + "”不是文件夹");
        Conflicts conflicts;
        conflicts.policy = json.value("conflict").toString("ask");
        AddPlan plan;
        QSet<QString> taken;
        for (const auto &value : json.value("items").toArray()) {
            const QString source = QFileInfo(value.toString()).absoluteFilePath();
            if (!QFileInfo(source).exists() && !QFileInfo(source).isSymLink())
                return fail("not-found", "找不到要添加的项目：" + source);
            planAdd(source, folder, QFileInfo(source).fileName(), kind, index, taken, conflicts, plan);
            if (plan.cancelled)
                return cancelled();
        }
        skipped = plan.skipped;
        changed = int(std::count_if(plan.items.begin(), plan.items.end(), [&](const Planned &item) {
            return !item.path.contains(u'/') || item.path.section(u'/', 0, -2) == folder;
        }));
        if (plan.items.isEmpty())
            return {{"event", "done"}, {"ok", true}, {"changed", 0}, {"skipped", skipped}};
        // Entries of another type in the way (a file where a folder goes) are removed first.
        QStringList mismatched;
        for (const auto &path : std::as_const(plan.replaced))
            if (index.contains(path) && index.value(path).dir != (std::find_if(plan.items.begin(), plan.items.end(), [&](const Planned &item) { return item.path == path; })->type == Planned::Directory))
                mismatched << path;
        if (kind == Kind::Tar) {
            const QString rewritten = stagePath + u"/.rewrite";
            QJsonObject result = tarRewrite(copy, rewritten, [&](const QString &path) {
                return std::any_of(plan.replaced.begin(), plan.replaced.end(), [&](const QString &root) {
                    return mismatched.contains(root) ? under(path, root) : path == root;
                });
            }, [](const QString &path) { return path; }, plan.items);
            if (!result.isEmpty())
                return result;
            ::rename(QFile::encodeName(rewritten).constData(), QFile::encodeName(copy).constData());
        } else {
            if (!mismatched.isEmpty()) {
                QJsonObject result = sevenZipEdit(QStringList{"d"} + base + passwordSwitches + QStringList{copy}, descendants(mismatched), {}, "update", password);
                if (!result.isEmpty())
                    return result;
            }
            const QString root = stagePath + u"/add";
            if (::mkdir(QFile::encodeName(root).constData(), 0700) != 0 || !materialize(root, folder, plan.items, &error))
                return channel.cancelled() ? cancelled() : fail("failed", "无法准备要添加的文件：" + error);
            QStringList top;
            if (!folder.isEmpty())
                top << folder.section(u'/', 0, 0);
            else
                for (const auto &item : std::as_const(plan.items))
                    if (!item.path.contains(u'/'))
                        top << item.path;
            QStringList arguments{"a"};
            arguments << base << passwordSwitches;
            if (kind == Kind::SevenZip)
                arguments << "-snl";
            arguments << copy;
            QJsonObject result = sevenZipEdit(arguments, top, root, "update", password);
            if (!result.isEmpty())
                return result;
        }
    } else {
        return fail("request", "未知的请求：" + op);
    }

    // Only a copy that reads back completely replaces the original.
    channel.progress({{"phase", "test"}, {"fraction", 0.0}}, true);
    if (kind == Kind::Tar) {
        if (!tarVerify(copy))
            return fail("test-failed", "修改后的压缩包没有通过校验，原压缩包未改动");
    } else {
        QJsonObject result = sevenZipVerify(copy, password);
        if (!result.isEmpty())
            return result;
    }
    if (channel.cancelled())
        return cancelled();
    struct stat original{};
    if (::stat(QFile::encodeName(archive).constData(), &original) == 0)
        ::chmod(QFile::encodeName(copy).constData(), original.st_mode & 07777 & ~mode_t(S_ISUID | S_ISGID));
    if (::rename(QFile::encodeName(copy).constData(), QFile::encodeName(archive).constData()) != 0)
        return fail("failed", "无法替换原压缩包：" + errorText());
    channel.progress({{"phase", "commit"}, {"fraction", 1.0}}, true);
    QJsonObject done{{"event", "done"}, {"ok", true}, {"changed", changed}, {"skipped", skipped}};
    if (password)
        done.insert("passwordUsed", true);
    return done;
}

QJsonObject convert(const Request &request) {
    auto &channel = Channel::instance();
    const QJsonObject &json = request.json;
    const QFileInfo output(json.value("output").toString());
    // The original stays: replacing it would take what is being read.
    if (output.absoluteFilePath() == QFileInfo(request.archive).absoluteFilePath()
        || (output.exists() && output.canonicalFilePath() == QFileInfo(request.archive).canonicalFilePath()))
        return fail("same", "不能把压缩包转换到它自己：请换个名称或位置");
    StageGuard stage;
    stage.fd = ::open(QFile::encodeName(output.absolutePath()).constData(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (stage.fd < 0)
        return fail("destination", "无法打开保存位置：" + errorText());
    stage.name = makeStage(stage.fd, ".nook-convert-");
    if (stage.name.isEmpty())
        return fail("destination", "无法写入保存位置：" + errorText());
    const QString stagePath = output.absolutePath() + u'/' + QFile::decodeName(stage.name);
    channel.send({{"event", "staging"}, {"path", stagePath}});

    // Everything out (names decoded the way the window showed them), then into the new format.
    Request extract = request;
    extract.op = "extract";
    QJsonObject extractJson{{"op", "extract"}, {"archive", request.archive}, {"destination", stagePath},
                            {"subfolder", "never"}, {"conflict", "overwrite"}, {"encoding", request.encoding}};
    extract.json = extractJson;
    bool fallback = false;
    QJsonObject done;
    if (backendFor(request.archive) == Backend::Libarchive)
        done = libarchiveExtract(extract, &fallback);
    else
        fallback = true;
    if (fallback)
        done = sevenZipExtract(extract);
    if (!done.value("ok").toBool())
        return done;
    if (channel.cancelled())
        return cancelled();
    QStringList items;
    for (const auto &name : QDir(stagePath).entryList(QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot, QDir::Name))
        items << stagePath + u'/' + name;
    if (items.isEmpty())
        return fail("options", "压缩包是空的，没有可转换的内容");

    Request create = request;
    create.op = "create";
    create.json = QJsonObject{{"op", "create"},
                              {"format", json.value("format")},
                              {"output", output.absoluteFilePath()},
                              {"items", QJsonArray::fromStringList(items)},
                              {"options", json.value("options")},
                              {"conflict", json.value("conflict").toString("ask")},
                              {"testAfter", json.value("testAfter").toBool()}};
    create.hasPassword = json.contains("newPassword") && !json.value("newPassword").toString().isEmpty();
    create.password = json.value("newPassword").toString();
    return worker::create(create);
}

} // namespace nook::archive::worker
