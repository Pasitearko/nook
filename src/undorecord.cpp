#include "undorecord.h"
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QStandardPaths>
#include <QUrl>
#include <algorithm>
#include <cerrno>
#include <dirent.h>
#include <fcntl.h>
#include <linux/fs.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

namespace {
bool stamp(const QString &path, struct stat &st) {
    return ::lstat(QFile::encodeName(path).constData(), &st) == 0;
}
QByteArray parentIdentity(const QString &canonical, const struct stat &st) {
    return QFile::encodeName(canonical) + ':' + QByteArray::number(st.st_dev) + ':' +
           QByteArray::number(st.st_ino);
}
bool fingerprint(const QString &path, QCryptographicHash &hash, GCancellable *cancel, int &count, int depth) {
    // Bounded, metadata-only traversal runs on the operations thread, not the UI.
    if (++count > 200000 || depth > 256 || g_cancellable_is_cancelled(cancel))
        return false;
    struct stat st{};
    if (!stamp(path, st))
        return false;
    auto number = [&](auto value) {
        hash.addData(QByteArray::number(static_cast<qlonglong>(value)));
        hash.addData(";", 1);
    };
    number(st.st_dev);
    number(st.st_ino);
    number(st.st_mode);
    // Directory mtimes change when later operations are undone. Child identities
    // detect added/removed content without invalidating a correctly restored tree.
    if (!S_ISDIR(st.st_mode)) {
        number(st.st_size);
        number(st.st_mtim.tv_sec);
        number(st.st_mtim.tv_nsec);
    }
    if (S_ISLNK(st.st_mode)) {
        QByteArray link(std::max<qint64>(st.st_size + 1, 4096), Qt::Uninitialized);
        auto size = ::readlink(QFile::encodeName(path).constData(), link.data(), link.size());
        if (size < 0 || size == link.size())
            return false;
        hash.addData(link.first(size));
    }
    if (!S_ISDIR(st.st_mode))
        return S_ISREG(st.st_mode) || S_ISLNK(st.st_mode);
    DIR *dir = ::opendir(QFile::encodeName(path).constData());
    if (!dir)
        return false;
    QList<QByteArray> names;
    errno = 0;
    while (auto *entry = ::readdir(dir)) {
        QByteArray name(entry->d_name);
        if (name != "." && name != "..") {
            if (QFile::encodeName(QFile::decodeName(name)) != name) {
                ::closedir(dir);
                return false;
            }
            names.append(name);
            if (names.size() > 200000 - count || g_cancellable_is_cancelled(cancel)) {
                ::closedir(dir);
                return false;
            }
        }
    }
    bool ok = errno == 0;
    ::closedir(dir);
    if (!ok)
        return false;
    std::sort(names.begin(), names.end());
    for (const auto &name : names) {
        hash.addData(QByteArray::number(name.size()));
        hash.addData(":", 1);
        hash.addData(name);
        if (!fingerprint(path + '/' + QFile::decodeName(name), hash, cancel, count, depth + 1))
            return false;
    }
    hash.addData("/", 1);
    return true;
}
} // namespace
namespace nook {
QByteArray undoFingerprint(const QString &path, GCancellable *cancel) {
    if (path.isEmpty())
        return {};
    QCryptographicHash hash(QCryptographicHash::Sha256);
    int count = 0;
    return fingerprint(path, hash, cancel, count, 0) ? hash.result() : QByteArray{};
}
QByteArray undoParentIdentity(const QString &path) {
    const auto parent = QFileInfo(path).dir().canonicalPath();
    struct stat st{};
    if (parent.isEmpty() || !stamp(parent, st) || !S_ISDIR(st.st_mode))
        return {};
    return parentIdentity(parent, st);
}
QString undoLocalPath(const QString &uri) {
    const QUrl url(uri);
    if (!url.isLocalFile())
        return {};
    const auto path = url.toLocalFile();
    GFile *file = g_file_new_for_uri(uri.toUtf8().constData());
    char *raw = g_file_get_path(file);
    const bool lossless = raw && QFile::encodeName(path) == QByteArray(raw);
    g_free(raw);
    g_object_unref(file);
    return lossless ? path : QString{};
}
bool undoMove(const UndoItem &item, GError **error) {
    const auto currentParent = QFileInfo(item.current).dir().canonicalPath();
    const auto originalParent = QFileInfo(item.original).dir().canonicalPath();
    const int from =
        ::open(QFile::encodeName(currentParent).constData(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    const int to = ::open(QFile::encodeName(originalParent).constData(),
                          O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    struct stat fromStat{}, toStat{};
    bool ok = false;
    int code = errno;
    if (from >= 0 && to >= 0 && ::fstat(from, &fromStat) == 0 && ::fstat(to, &toStat) == 0) {
        if (parentIdentity(currentParent, fromStat) != item.currentParent ||
            parentIdentity(originalParent, toStat) != item.originalParent)
            code = ESTALE;
        else {
            ok = ::syscall(SYS_renameat2, from,
                           QFile::encodeName(QFileInfo(item.current).fileName()).constData(), to,
                           QFile::encodeName(QFileInfo(item.original).fileName()).constData(),
                           RENAME_NOREPLACE) == 0;
            code = errno;
        }
    }
    if (from >= 0)
        ::close(from);
    if (to >= 0)
        ::close(to);
    if (!ok)
        g_set_error(error, G_IO_ERROR, g_io_error_from_errno(code),
                    "无法无覆盖地恢复文件：%s（不回退到跨盘复制或永久删除）", g_strerror(code));
    return ok;
}
bool undoPathExists(const QString &path) {
    struct stat st{};
    return stamp(path, st);
}
UndoItem undoItem(const QString &current, const QString &original, GCancellable *cancel) {
    UndoItem result{current, original, {}, undoFingerprint(current, cancel), undoParentIdentity(current), {}};
    if (!original.isEmpty())
        result.originalParent = undoParentIdentity(original);
    struct stat currentStat{}, parentStat{};
    const bool sameFilesystem =
        original.isEmpty() ||
        (stamp(current, currentStat) && stamp(QFileInfo(original).dir().canonicalPath(), parentStat) &&
         currentStat.st_dev == parentStat.st_dev);
    if (result.currentParent.isEmpty() || (!original.isEmpty() && result.originalParent.isEmpty()) ||
        !sameFilesystem)
        result.fingerprint.clear();
    return result;
}
UndoItem trashedUndoItem(const QString &original, const QByteArray &expected, GCancellable *cancel) {
    if (original.isEmpty() || expected.isEmpty())
        return {};
    QString mount = QFileInfo(original).absolutePath();
    struct stat st{}, parent{};
    if (!stamp(mount, st))
        return {};
    while (mount != "/") {
        auto up = QFileInfo(mount).dir().absolutePath();
        if (!stamp(up, parent) || parent.st_dev != st.st_dev)
            break;
        mount = up;
    }
    const auto uid = QString::number(getuid());
    const QStringList roots{QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) + "/Trash",
                            QDir(mount).filePath(".Trash/" + uid), QDir(mount).filePath(".Trash-" + uid)};
    for (const auto &root : roots) {
        QDir info(root + "/info");
        for (const auto &name : info.entryList({"*.trashinfo"}, QDir::Files | QDir::Hidden)) {
            if (g_cancellable_is_cancelled(cancel))
                return {};
            QFile file(info.filePath(name));
            if (!file.open(QIODevice::ReadOnly))
                continue;
            QString stored;
            for (const auto &line : file.readAll().split('\n'))
                if (line.startsWith("Path=")) {
                    const auto raw = QByteArray::fromPercentEncoding(line.mid(5));
                    stored =
                        QFile::encodeName(QFile::decodeName(raw)) == raw ? QFile::decodeName(raw) : QString{};
                }
            if (stored.isEmpty())
                continue;
            if (!QDir::isAbsolutePath(stored))
                stored = QDir(mount).filePath(stored);
            if (QDir::cleanPath(stored) != QDir::cleanPath(original))
                continue;
            auto candidate = QDir(root + "/files").filePath(name.chopped(10));
            // This includes device/inode and the full tree; a same-name trash
            // entry or a concurrently edited item will not pass this check.
            auto item = undoItem(candidate, original, cancel);
            if (!item.fingerprint.isEmpty() && item.fingerprint == expected) {
                item.trashInfo = file.fileName();
                return item;
            }
        }
    }
    return {};
}
} // namespace nook
