#include "worker.h"
#include "../archivenames.h"
#include <QFileInfo>
#include <QRandomGenerator>
#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace nook::archive::worker {
namespace {

int openDirectory(int dirFd, const QByteArray &name) {
    return ::openat(dirFd, name.constData(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
}

QList<QByteArray> namesIn(int dirFd) {
    QList<QByteArray> names;
    const int copy = openDirectory(dirFd, ".");
    if (copy < 0)
        return names;
    DIR *dir = ::fdopendir(copy);
    if (!dir) {
        ::close(copy);
        return names;
    }
    while (dirent *entry = ::readdir(dir)) {
        if (std::strcmp(entry->d_name, ".") != 0 && std::strcmp(entry->d_name, "..") != 0)
            names << QByteArray(entry->d_name);
    }
    ::closedir(dir);
    std::sort(names.begin(), names.end());
    return names;
}

// Walks components from rootFd without following links; -1 if any is missing or not a directory.
int walk(int rootFd, const QStringList &components) {
    int fd = openDirectory(rootFd, ".");
    for (const auto &component : components) {
        if (fd < 0)
            return -1;
        const int next = openDirectory(fd, encodePath(component));
        ::close(fd);
        fd = next;
    }
    return fd;
}

std::optional<QStringList> resolveInside(int rootFd, QStringList directory, const QByteArray &target, int depth) {
    if (target.isEmpty() || target.startsWith('/') || depth > 40)
        return std::nullopt;
    for (const auto &component : QString::fromUtf8(target).split(u'/')) {
        if (component.isEmpty() || component == u".")
            continue;
        if (component == u"..") {
            if (directory.isEmpty())
                return std::nullopt;
            directory.removeLast();
            continue;
        }
        const int parent = walk(rootFd, directory);
        directory << component;
        if (parent < 0)
            continue;
        struct stat st{};
        const QByteArray name = encodePath(component);
        if (::fstatat(parent, name.constData(), &st, AT_SYMLINK_NOFOLLOW) == 0 && S_ISLNK(st.st_mode)) {
            QByteArray next(PATH_MAX, '\0');
            const ssize_t length = ::readlinkat(parent, name.constData(), next.data(), size_t(next.size()));
            ::close(parent);
            if (length < 0)
                return std::nullopt;
            next.truncate(length);
            directory.removeLast();
            const auto resolved = resolveInside(rootFd, directory, next, depth + 1);
            if (!resolved)
                return std::nullopt;
            directory = *resolved;
            continue;
        }
        ::close(parent);
    }
    return directory;
}

QString joined(const QStringList &location) { return location.join(u'/'); }

}

mode_t currentUmask() {
    static const mode_t mask = [] {
        const mode_t value = ::umask(022);
        ::umask(value);
        return value;
    }();
    return mask;
}

QByteArray encodePath(const QString &path) { return path.toUtf8(); }

QStringList safeComponents(QString path, bool backslashes) {
    if (backslashes)
        path.replace(u'\\', u'/');
    QStringList components;
    for (const auto &part : path.split(u'/')) {
        if (part.isEmpty() || part == u".")
            continue;
        if (part == u"..")
            return {};
        components << part;
    }
    return components;
}

bool linkStaysInside(int rootFd, const QStringList &location, const QByteArray &target, int depth) {
    QStringList directory = location;
    if (!directory.isEmpty())
        directory.removeLast();
    return resolveInside(rootFd, directory, target, depth).has_value();
}

void removeTree(int dirFd, const QByteArray &name) {
    struct stat st{};
    if (::fstatat(dirFd, name.constData(), &st, AT_SYMLINK_NOFOLLOW) != 0)
        return;
    if (S_ISDIR(st.st_mode)) {
        if ((st.st_mode & 0700) != 0700)
            ::fchmodat(dirFd, name.constData(), st.st_mode | 0700, 0);
        const int fd = openDirectory(dirFd, name);
        if (fd >= 0) {
            for (const auto &child : namesIn(fd))
                removeTree(fd, child);
            ::close(fd);
        }
        ::unlinkat(dirFd, name.constData(), AT_REMOVEDIR);
    } else {
        ::unlinkat(dirFd, name.constData(), 0);
    }
}

Stage::~Stage() {
    if (m_fd >= 0)
        ::close(m_fd);
    if (!m_name.isEmpty())
        removeTree(m_destination, m_name);
}

bool Stage::create(QString *error) {
    for (int attempt = 0; attempt < 32; ++attempt) {
        const QByteArray name = ".nook-extract-" + QByteArray::number(QRandomGenerator::global()->generate(), 16);
        if (::mkdirat(m_destination, name.constData(), 0700) == 0) {
            m_name = name;
            m_fd = openDirectory(m_destination, name);
            if (m_fd >= 0)
                return true;
            break;
        }
        if (errno != EEXIST)
            break;
    }
    *error = QString::fromLocal8Bit(std::strerror(errno));
    return false;
}

int Stage::directory(const QStringList &components, bool create) const {
    int fd = openDirectory(m_fd, ".");
    for (const auto &component : components) {
        if (fd < 0)
            return -1;
        const QByteArray name = encodePath(component);
        int next = openDirectory(fd, name);
        if (next < 0 && errno == ENOENT && create) {
            if (::mkdirat(fd, name.constData(), 0700) == 0 || errno == EEXIST)
                next = openDirectory(fd, name);
        }
        const int saved = errno;
        ::close(fd);
        errno = saved;
        fd = next;
    }
    return fd;
}

void Stage::sanitize(const QStringList &root) {
    const int rootFd = directory(root, false);
    if (rootFd < 0)
        return;
    std::function<void(int, const QStringList &)> visit = [&](int dirFd, const QStringList &location) {
        for (const auto &name : namesIn(dirFd)) {
            struct stat st{};
            if (::fstatat(dirFd, name.constData(), &st, AT_SYMLINK_NOFOLLOW) != 0)
                continue;
            const QStringList here = location + QStringList{QString::fromUtf8(name)};
            if (S_ISLNK(st.st_mode)) {
                QByteArray target(PATH_MAX, '\0');
                const ssize_t length = ::readlinkat(dirFd, name.constData(), target.data(), size_t(target.size()));
                target.truncate(std::max<ssize_t>(length, 0));
                if (length <= 0 || !linkStaysInside(rootFd, here, target)) {
                    ::unlinkat(dirFd, name.constData(), 0);
                    warn("unsafe-link", joined(here), "符号链接指向解压位置之外，已跳过");
                }
            } else if (S_ISDIR(st.st_mode)) {
                if ((st.st_mode & 0700) != 0700)
                    ::fchmodat(dirFd, name.constData(), (st.st_mode & 0777) | 0700, 0);
                const int child = openDirectory(dirFd, name);
                if (child >= 0) {
                    visit(child, here);
                    ::close(child);
                }
            } else if (S_ISREG(st.st_mode)) {
                if (st.st_mode & 07000)
                    ::fchmodat(dirFd, name.constData(), st.st_mode & 0777, 0);
            } else {
                ::unlinkat(dirFd, name.constData(), 0);
                warn("special", joined(here), "设备文件、管道和套接字不会被解压，已跳过");
            }
        }
    };
    visit(rootFd, {});
    ::close(rootFd);
}

Stage::Result Stage::commit(const QStringList &root, const QString &subfolder, const QString &folderName,
                            Conflicts &conflicts) {
    Result result;
    const int rootFd = directory(root, false);
    if (rootFd < 0)
        return result;
    const auto names = namesIn(rootFd);
    const bool wrap = !names.isEmpty() && (subfolder == "always" || (subfolder == "smart" && names.size() > 1));
    if (!wrap) {
        const int targetFd = openDirectory(m_destination, ".");
        commitTree(rootFd, targetFd, {}, conflicts, result);
        ::close(targetFd);
        ::close(rootFd);
        return result;
    }
    QString folder = folderName;
    folder.replace(u'/', u'_');
    if (folder.isEmpty() || folder == u"." || folder == u"..")
        folder = "解压";
    for (int number = 1; number < 10000; ++number) {
        const QString candidate = number == 1 ? folder : numberedName(folder, number, true);
        const QByteArray name = encodePath(candidate);
        struct stat st{};
        if (::fstatat(m_destination, name.constData(), &st, AT_SYMLINK_NOFOLLOW) != 0) {
            if (errno != ENOENT)
                break;
            const bool whole = root.isEmpty();
            const int parent = whole ? openDirectory(m_destination, ".") : directory(root.mid(0, root.size() - 1), false);
            const QByteArray from = whole ? m_name : encodePath(root.last());
            const int moved = ::renameat2(parent, from.constData(), m_destination, name.constData(), RENAME_NOREPLACE);
            const int saved = errno;
            ::close(parent);
            if (moved == 0) {
                if (whole) {
                    // The stage itself became the folder: give it ordinary permissions and leave it.
                    ::fchmod(m_fd, 0777 & ~currentUmask());
                    ::futimens(m_fd, nullptr);
                    m_name.clear();
                }
                result.created << candidate;
                break;
            }
            if (saved == EEXIST)
                continue;
            warn("io", candidate, QString::fromLocal8Bit(std::strerror(saved)));
            break;
        }
        if (S_ISDIR(st.st_mode)) {
            const int targetFd = openDirectory(m_destination, name);
            if (targetFd >= 0) {
                commitTree(rootFd, targetFd, candidate, conflicts, result);
                ::close(targetFd);
                break;
            }
        }
    }
    ::close(rootFd);
    return result;
}

void Stage::commitTree(int sourceFd, int targetFd, const QString &relative, Conflicts &conflicts, Result &result) {
    for (const auto &name : namesIn(sourceFd)) {
        struct stat incoming{};
        if (::fstatat(sourceFd, name.constData(), &incoming, AT_SYMLINK_NOFOLLOW) != 0)
            continue;
        const QString display = QString::fromUtf8(name);
        const QString path = relative.isEmpty() ? display : relative + u'/' + display;
        for (int attempt = 0; attempt < 8; ++attempt) {
            struct stat existing{};
            if (::fstatat(targetFd, name.constData(), &existing, AT_SYMLINK_NOFOLLOW) != 0) {
                if (errno != ENOENT) {
                    warn("io", path, QString::fromLocal8Bit(std::strerror(errno)));
                    break;
                }
                if (::renameat2(sourceFd, name.constData(), targetFd, name.constData(), RENAME_NOREPLACE) == 0) {
                    result.created << path;
                    break;
                }
                if (errno == EEXIST)
                    continue;
                warn("io", path, QString::fromLocal8Bit(std::strerror(errno)));
                break;
            }
            if (S_ISDIR(incoming.st_mode) && S_ISDIR(existing.st_mode)) {
                const int from = openDirectory(sourceFd, name);
                const int to = openDirectory(targetFd, name);
                if (from >= 0 && to >= 0)
                    commitTree(from, to, path, conflicts, result);
                if (from >= 0)
                    ::close(from);
                if (to >= 0)
                    ::close(to);
                break;
            }
            Choice choice = conflicts.resolve(path, existing, incoming);
            if (choice == Choice::Skip) {
                ++result.skipped;
                break;
            }
            if (choice == Choice::Overwrite && S_ISDIR(existing.st_mode)) {
                warn("kept-folder", path, "不能用文件替换同名文件夹，已改名保存");
                choice = Choice::Rename;
            }
            if (choice == Choice::Overwrite) {
                int replaced;
                if (S_ISDIR(incoming.st_mode)) {
                    ::unlinkat(targetFd, name.constData(), 0);
                    replaced = ::renameat2(sourceFd, name.constData(), targetFd, name.constData(), RENAME_NOREPLACE);
                } else {
                    replaced = ::renameat(sourceFd, name.constData(), targetFd, name.constData());
                }
                if (replaced == 0)
                    result.created << path;
                else
                    warn("io", path, QString::fromLocal8Bit(std::strerror(errno)));
                break;
            }
            for (int number = 2; number < 10000; ++number) {
                const QString candidate = numberedName(display, number, S_ISDIR(incoming.st_mode));
                const QByteArray encoded = encodePath(candidate);
                if (::renameat2(sourceFd, name.constData(), targetFd, encoded.constData(), RENAME_NOREPLACE) == 0) {
                    result.created << (relative.isEmpty() ? candidate : relative + u'/' + candidate);
                    break;
                }
                if (errno != EEXIST) {
                    warn("io", path, QString::fromLocal8Bit(std::strerror(errno)));
                    break;
                }
            }
            break;
        }
    }
}

QString destinationOf(const QJsonObject &request) { return request.value("destination").toString(); }

QJsonObject finishExtract(Stage &stage, const QJsonObject &request, const QStringList &root,
                          bool cancelled, const QJsonObject &error, const std::function<void()> &beforeCommit) {
    stage.sanitize(root);
    if (beforeCommit)
        beforeCommit();
    Conflicts conflicts;
    conflicts.policy = cancelled ? QStringLiteral("skip") : request.value("conflict").toString("ask");
    const QString archive = request.value("archive").toString();
    const QString folder = request.value("folderName").toString(stemOf(archive));
    const auto result = stage.commit(root, request.value("subfolder").toString("smart"), folder, conflicts);
    QJsonObject done{{"event", "done"},
                     {"ok", error.isEmpty() && !cancelled},
                     {"cancelled", cancelled},
                     {"created", QJsonArray::fromStringList(result.created)},
                     {"skipped", result.skipped},
                     {"destination", destinationOf(request)}};
    if (!error.isEmpty())
        done.insert("error", error);
    return done;
}

}
