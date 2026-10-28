#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "fileproperties.h"
#include "files.h"
#include <QDateTime>
#include <QFile>
#include <QFutureWatcher>
#include <QLocale>
#include <QPointer>
#include <QtConcurrentRun>
#include <algorithm>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <functional>
#include <gio/gio.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <vector>

namespace {
template <typename T> struct Unref {
    void operator()(T *p) const {
        if (p)
            g_object_unref(p);
    }
};
template <typename T> using GPtr = std::unique_ptr<T, Unref<T>>;
using Cancel = std::shared_ptr<GCancellable>;
struct Fd {
    int value = -1;
    explicit Fd(int fd = -1) : value(fd) {}
    ~Fd() {
        if (value >= 0)
            ::close(value);
    }
    Fd(const Fd &) = delete;
    Fd &operator=(const Fd &) = delete;
};
struct Failure {
    QString code, message;
};
Failure systemFailure(int number) {
    QString code = QStringLiteral("ioError"), label = QStringLiteral("Unable to access file");
    switch (number) {
    case EACCES:
    case EPERM:
        code = QStringLiteral("permissionDenied");
        label = QStringLiteral("Permission denied");
        break;
    case EROFS:
        code = QStringLiteral("readOnly");
        label = QStringLiteral("Read-only filesystem");
        break;
    case ELOOP:
    case EOPNOTSUPP:
    case ENOSYS:
        code = QStringLiteral("notSupported");
        label = QStringLiteral("Not supported (symlinks and special files cannot be changed)");
        break;
    case ENOENT:
    case ENOTDIR:
        code = QStringLiteral("notFound");
        label = QStringLiteral("File not found");
        break;
    }
    return {code, label + QStringLiteral(": ") + QString::fromLocal8Bit(std::strerror(number))};
}
Failure gioFailure(GError *error) {
    if (!error)
        return {QStringLiteral("ioError"), QStringLiteral("Unable to inspect file")};
    QString code = QStringLiteral("ioError");
    if (g_error_matches(error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED))
        code = QStringLiteral("permissionDenied");
    else if (g_error_matches(error, G_IO_ERROR, G_IO_ERROR_READ_ONLY))
        code = QStringLiteral("readOnly");
    else if (g_error_matches(error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED))
        code = QStringLiteral("notSupported");
    else if (g_error_matches(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND))
        code = QStringLiteral("notFound");
    else if (g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        code = QStringLiteral("cancelled");
    return {code, QString::fromUtf8(error->message)};
}
Failure replaced() {
    return {QStringLiteral("replaced"),
            QStringLiteral("The selected file was replaced; inspect it again before changing permissions")};
}
bool regularOrDirectory(mode_t mode) {
    return S_ISREG(mode) || S_ISDIR(mode);
}
bool ownsFile(uid_t uid) {
    return ::geteuid() == 0 || ::geteuid() == uid;
}
bool sameFile(const struct stat &a, const struct stat &b) {
    return a.st_dev == b.st_dev && a.st_ino == b.st_ino && (a.st_mode & S_IFMT) == (b.st_mode & S_IFMT);
}
bool canPreserveSetgid(const struct stat &st) {
    if (!(st.st_mode & S_ISGID) || st.st_gid == ::getegid())
        return true;
    const int count = ::getgroups(0, nullptr);
    if (count < 0)
        return false;
    std::vector<gid_t> groups(count);
    if (::getgroups(count, groups.data()) != count)
        return false;
    // Conservatively refuse foreign-group SGID, even for privileged callers: some kernels
    // silently clear it without CAP_FSETID. Never discover that by mutating the file first.
    return std::find(groups.begin(), groups.end(), st.st_gid) != groups.end();
}
Failure setgidFailure() {
    return {"notSupported", "Cannot safely preserve setgid for a group outside the current user's groups"};
}
bool supportsEmptyPathChmod() {
#ifdef SYS_fchmodat2
    // A non-mutating feature probe; EBADF means the syscall and flags were recognized.
    return ::syscall(SYS_fchmodat2, -1, "", 0, AT_EMPTY_PATH) == -1 && errno == EBADF;
#else
    return false;
#endif
}
int chmodPathFd(int fd, mode_t mode) {
#ifdef SYS_fchmodat2
    return int(::syscall(SYS_fchmodat2, fd, "", mode, AT_EMPTY_PATH));
#else
    Q_UNUSED(fd);
    Q_UNUSED(mode);
    errno = ENOSYS;
    return -1;
#endif
}
struct Target {
    QByteArray path;
    // Pin the inspected inode, even if unlinked, so dev/ino cannot be recycled before Apply.
    Fd pin;
    // Keep a readable descriptor when possible, permitting restoration even after chmod 0000.
    Fd access;
    struct stat identity{};
};
struct Result {
    QVariantMap details;
    std::shared_ptr<Target> target;
    Failure failure;
    bool applied = false, hiddenApplied = false;
};
void permissionFailure(QVariantMap &details, const Failure &failure) {
    details["editable"] = false;
    details["permissionsErrorCode"] = failure.code;
    details["permissionsError"] = failure.message;
}
QString textAttribute(GFileInfo *info, const char *attribute) {
    const char *text = g_file_info_get_attribute_string(info, attribute);
    return text ? QString::fromUtf8(text) : QString();
}
void timestamp(QVariantMap &details, GFileInfo *info, const char *attribute, const QString &key) {
    if (!g_file_info_has_attribute(info, attribute))
        return;
    const auto seconds = static_cast<qint64>(g_file_info_get_attribute_uint64(info, attribute));
    details[key] = seconds;
    details[key + "Text"] = QDateTime::fromSecsSinceEpoch(seconds).toLocalTime().toString("yyyy-MM-dd HH:mm");
}
Result inspectFile(const QString &input, GCancellable *cancel) {
    Result result;
    auto &details = result.details;
    details = {{"uri", input}, {"permissionsSupported", false}, {"editable", false}};
    if (input.isEmpty() || input.contains(QChar::Null)) {
        result.failure = {QStringLiteral("invalidUri"), QStringLiteral("Select a file or directory first")};
        return result;
    }
    GPtr<GFile> file;
    if (input.startsWith('/')) {
        const auto bytes = QFile::encodeName(input);
        if (QFile::decodeName(bytes) != input) {
            result.failure = {QStringLiteral("invalidUri"),
                              QStringLiteral("The path cannot be encoded losslessly; use its original URI")};
            return result;
        }
        file.reset(g_file_new_for_path(bytes.constData()));
    } else {
        // Do not convert a file URI through QUrl::toLocalFile(): that loses non-UTF-8 bytes.
        const auto bytes = input.toUtf8();
        char *scheme = g_uri_parse_scheme(bytes.constData());
        if (!scheme) {
            result.failure = {QStringLiteral("invalidUri"),
                              QStringLiteral("An absolute path or URI is required")};
            return result;
        }
        g_free(scheme);
        file.reset(g_file_new_for_uri(bytes.constData()));
    }
    char *uri = g_file_get_uri(file.get());
    details["uri"] = QString::fromUtf8(uri);
    g_free(uri);
    char *rawPath = g_file_get_path(file.get());
    const QByteArray path = rawPath ? QByteArray(rawPath) : QByteArray();
    g_free(rawPath);
    details["path"] = QFile::decodeName(path);

    GError *error = nullptr;
    GPtr<GFileInfo> info(g_file_query_info(
        file.get(),
        "standard::display-name,standard::name,standard::type,standard::is-symlink,standard::size,"
        "time::modified,time::created,owner::user,owner::group,unix::mode,unix::uid,unix::gid,"
        "unix::device,unix::inode",
        G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, cancel, &error));
    if (!info) {
        result.failure = gioFailure(error);
        g_clear_error(&error);
        return result;
    }
    const auto type = g_file_info_get_file_type(info.get());
    const bool symlink = g_file_info_get_is_symlink(info.get()) || type == G_FILE_TYPE_SYMBOLIC_LINK;
    const bool supportedType = !symlink && (type == G_FILE_TYPE_REGULAR || type == G_FILE_TYPE_DIRECTORY);
    const bool hasMode = g_file_info_has_attribute(info.get(), G_FILE_ATTRIBUTE_UNIX_MODE);
    const auto mode = hasMode ? g_file_info_get_attribute_uint32(info.get(), G_FILE_ATTRIBUTE_UNIX_MODE) : 0;
    const auto size = static_cast<qint64>(g_file_info_get_size(info.get()));
    details["name"] = QString::fromUtf8(g_file_info_get_display_name(info.get()));
    const QByteArray baseName = path.mid(path.lastIndexOf('/') + 1);
    details["hidden"] = !path.isEmpty() && nook::hiddenFile(path);
    details["hiddenByName"] = baseName.startsWith('.');
    details["canHide"] = !path.isEmpty() && path != "/";
    details["type"] = symlink                         ? "symlink"
                      : type == G_FILE_TYPE_REGULAR   ? "file"
                      : type == G_FILE_TYPE_DIRECTORY ? "directory"
                                                      : "other";
    details["size"] = size;
    details["sizeText"] = nook::formatSize(size);
    details["permissions"] = int(mode & 0777);
    details["owner"] = textAttribute(info.get(), G_FILE_ATTRIBUTE_OWNER_USER);
    details["group"] = textAttribute(info.get(), G_FILE_ATTRIBUTE_OWNER_GROUP);
    if (details["owner"].toString().isEmpty() &&
        g_file_info_has_attribute(info.get(), G_FILE_ATTRIBUTE_UNIX_UID))
        details["owner"] =
            QString::number(g_file_info_get_attribute_uint32(info.get(), G_FILE_ATTRIBUTE_UNIX_UID));
    if (details["group"].toString().isEmpty() &&
        g_file_info_has_attribute(info.get(), G_FILE_ATTRIBUTE_UNIX_GID))
        details["group"] =
            QString::number(g_file_info_get_attribute_uint32(info.get(), G_FILE_ATTRIBUTE_UNIX_GID));
    timestamp(details, info.get(), G_FILE_ATTRIBUTE_TIME_MODIFIED, "modified");
    timestamp(details, info.get(), G_FILE_ATTRIBUTE_TIME_CREATED, "created");
    details["permissionsSupported"] = hasMode && supportedType && !path.isEmpty();
    if (!details["permissionsSupported"].toBool()) {
        permissionFailure(
            details,
            {"notSupported",
             "Not supported: only local regular files and directories have editable Unix permissions"});
        return result;
    }
    // A missing identity/owner attribute must not become a zero-valued, apparently valid identity.
    for (const char *attribute :
         {G_FILE_ATTRIBUTE_UNIX_DEVICE, G_FILE_ATTRIBUTE_UNIX_INODE, G_FILE_ATTRIBUTE_UNIX_UID}) {
        if (!g_file_info_has_attribute(info.get(), attribute)) {
            permissionFailure(
                details, {"notSupported", "Not supported: the file's identity and owner cannot be verified"});
            return result;
        }
    }
    auto target = std::make_shared<Target>();
    target->path = path;
    target->pin.value = ::open(path.constData(), O_PATH | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
    if (target->pin.value < 0 || ::fstat(target->pin.value, &target->identity) != 0) {
        permissionFailure(details, systemFailure(errno));
        return result;
    }
    const auto &st = target->identity;
    if (!regularOrDirectory(st.st_mode) ||
        quint64(st.st_dev) != g_file_info_get_attribute_uint32(info.get(), G_FILE_ATTRIBUTE_UNIX_DEVICE) ||
        quint64(st.st_ino) != g_file_info_get_attribute_uint64(info.get(), G_FILE_ATTRIBUTE_UNIX_INODE) ||
        st.st_uid != g_file_info_get_attribute_uint32(info.get(), G_FILE_ATTRIBUTE_UNIX_UID) ||
        (st.st_mode & (S_IFMT | 07777)) != (mode & (S_IFMT | 07777))) {
        result.failure = replaced();
        permissionFailure(details, result.failure);
        return result;
    }
    struct statvfs fs{};
    if (::fstatvfs(target->pin.value, &fs) != 0) {
        permissionFailure(details, systemFailure(errno));
        return result;
    }
    target->access.value = ::open(path.constData(), O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
    if (target->access.value >= 0) {
        struct stat accessIdentity{};
        if (::fstat(target->access.value, &accessIdentity) != 0 || !sameFile(st, accessIdentity)) {
            result.failure = replaced();
            permissionFailure(details, result.failure);
            return result;
        }
    } else if (!supportsEmptyPathChmod()) {
        // Older kernels cannot safely chmod an initially unreadable file through O_PATH.
        permissionFailure(
            details,
            {"permissionDenied",
             "Permission denied: this kernel requires a readable descriptor to change permissions safely"});
        return result;
    }
    details["readOnly"] = bool(fs.f_flag & ST_RDONLY);
    details["editable"] = true;
    if (fs.f_flag & ST_RDONLY)
        permissionFailure(details, systemFailure(EROFS));
    else if (!ownsFile(st.st_uid))
        permissionFailure(
            details,
            {"permissionDenied", "Permission denied: only the file's owner or root may change permissions"});
    else if (!canPreserveSetgid(st))
        permissionFailure(details, setgidFailure());
    result.target = std::move(target);
    return result;
}
Result changePermissions(const std::shared_ptr<Target> &target, QVariantMap details, int mode,
                         GCancellable *cancel) {
    Result result;
    result.details = std::move(details);
    result.target = target;
    if (g_cancellable_is_cancelled(cancel))
        return result;
    // O_PATH validates replacements without opening devices for I/O. Mutation uses the
    // retained readable fd, or fchmodat2(AT_EMPTY_PATH) for an initially unreadable inode.
    // Neither route follows a symlink, uses /proc/self/fd, or falls back to pathname chmod.
    Fd fd(::open(target->path.constData(), O_PATH | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC));
    if (fd.value < 0) {
        result.failure = systemFailure(errno);
        return result;
    }
    struct stat st{};
    if (::fstat(fd.value, &st) != 0) {
        result.failure = systemFailure(errno);
        return result;
    }
    if (!regularOrDirectory(st.st_mode) || !sameFile(st, target->identity)) {
        result.failure = replaced();
        permissionFailure(result.details, result.failure);
        result.target.reset();
        return result;
    }
    if (!ownsFile(st.st_uid)) {
        result.failure = {"permissionDenied", "Permission denied: the current user is not the file's owner"};
        permissionFailure(result.details, result.failure);
        return result;
    }
    struct statvfs fs{};
    if (::fstatvfs(fd.value, &fs) != 0) {
        result.failure = systemFailure(errno);
        return result;
    }
    if (fs.f_flag & ST_RDONLY) {
        result.failure = systemFailure(EROFS);
        result.details["readOnly"] = true;
        permissionFailure(result.details, result.failure);
        return result;
    }
    if (!canPreserveSetgid(st)) {
        result.failure = setgidFailure();
        permissionFailure(result.details, result.failure);
        return result;
    }
    if (g_cancellable_is_cancelled(cancel))
        return result;
    // Read the special bits again at Apply time, not from stale inspection metadata.
    const mode_t wanted = (st.st_mode & 07000) | mode;
    const int status =
        target->access.value >= 0 ? ::fchmod(target->access.value, wanted) : chmodPathFd(fd.value, wanted);
    if (status != 0) {
        result.failure = systemFailure(errno);
        return result;
    }
    // Cancellation cannot undo this mutation. Always report a completed chmod for refresh/toast.
    result.applied = true;
    if (::fstat(fd.value, &st) != 0) {
        result.failure = systemFailure(errno);
        return result;
    }
    result.details["permissions"] = int(st.st_mode & 0777);
    if ((st.st_mode & 07777) != wanted)
        result.failure = {
            "permissionDenied",
            "Permission change was not fully retained by the filesystem (including special bits)"};
    return result;
}
} // namespace

struct FileProperties::Private {
    struct Job {
        QFutureWatcher<Result> *watcher;
        Cancel cancel;
    };
    QVariantMap details;
    std::shared_ptr<Target> target;
    std::vector<Job> jobs;
    quint64 generation = 0;
    bool busy = false;

    void cancelJobs() {
        for (const auto &job : jobs)
            g_cancellable_cancel(job.cancel.get());
    }
    void start(FileProperties *owner, std::function<Result(GCancellable *)> work) {
        const auto current = ++generation;
        Cancel cancel(g_cancellable_new(), Unref<GCancellable>{});
        auto *watcher = new QFutureWatcher<Result>(owner);
        jobs.push_back({watcher, cancel});
        QObject::connect(watcher, &QFutureWatcher<Result>::finished, owner, [this, owner, watcher, current] {
            const Result result = watcher->result();
            jobs.erase(std::remove_if(jobs.begin(), jobs.end(),
                                      [watcher](const Job &job) { return job.watcher == watcher; }),
                       jobs.end());
            watcher->deleteLater();
            QPointer<FileProperties> guard(owner);
            if (current == generation) {
                details = result.details;
                target = result.target;
                busy = false;
                if (!result.failure.message.isEmpty()) {
                    details["error"] = result.failure.message;
                    details["errorCode"] = result.failure.code;
                }
                Q_EMIT owner->changed();
            }
            if (!guard)
                return;
            if (result.applied)
                Q_EMIT owner->permissionsApplied();
            if (guard && result.hiddenApplied)
                Q_EMIT owner->hiddenApplied(result.details.value("hidden").toBool());
            if (guard && current == generation && !result.failure.message.isEmpty())
                Q_EMIT owner->error(result.failure.message);
        });
        watcher->setFuture(
            QtConcurrent::run([work = std::move(work), cancel] { return work(cancel.get()); }));
        busy = true;
        Q_EMIT owner->changed();
    }
};

FileProperties::FileProperties(QObject *parent) : QObject(parent), d(std::make_unique<Private>()) {}
FileProperties::~FileProperties() {
    d->cancelJobs();
    for (const auto &job : d->jobs) {
        job.watcher->disconnect(this);
        job.watcher->waitForFinished();
        delete job.watcher;
    }
}
QVariantMap FileProperties::details() const {
    return d->details;
}
bool FileProperties::busy() const {
    return d->busy;
}
void FileProperties::inspect(const QString &uri) {
    d->cancelJobs();
    d->target.reset();
    d->details.clear();
    d->start(this, [uri](GCancellable *cancel) { return inspectFile(uri, cancel); });
}
void FileProperties::applyPermissions(int mode) {
    Failure failure;
    if (mode < 0 || (mode & ~0777) != 0)
        failure = {"invalidMode", "Invalid permissions: only rwx bits (0000 through 0777) are accepted"};
    else if (d->busy)
        failure = {"busy", "Wait for the current properties operation to finish"};
    else if (!d->target || !d->details.value("editable").toBool())
        failure = {
            d->details.value("permissionsErrorCode", "notSupported").toString(),
            d->details
                .value("permissionsError", "Not supported: inspect an editable local file or directory first")
                .toString()};
    if (!failure.message.isEmpty()) {
        d->details["error"] = failure.message;
        d->details["errorCode"] = failure.code;
        QPointer<FileProperties> guard(this);
        const auto generation = d->generation;
        Q_EMIT changed();
        if (guard && generation == d->generation)
            Q_EMIT error(failure.message);
        return;
    }
    d->details.remove("error");
    d->details.remove("errorCode");
    d->start(this, [target = d->target, details = d->details, mode](GCancellable *cancel) {
        return changePermissions(target, details, mode, cancel);
    });
}
void FileProperties::setHidden(bool hidden) {
    const auto uri = d->details.value("uri").toString();
    if (d->busy || uri.isEmpty() || !d->details.value("canHide").toBool())
        return;
    d->cancelJobs();
    d->target.reset();
    d->start(this, [uri, hidden](GCancellable *cancel) {
        QString message, next;
        const bool ok = nook::setHidden(uri, hidden, &message, &next);
        // Re-inspect (possibly under the new name) so permissions keep a fresh, pinned target.
        Result result = inspectFile(ok ? next : uri, cancel);
        result.hiddenApplied = ok;
        if (!ok)
            result.failure = {QStringLiteral("hiddenFailed"), message};
        return result;
    });
}
