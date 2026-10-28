#include "operations.h"
#include "files.h"
#include <QCoreApplication>
#include <QUuid>
#include <cstdlib>
#include <cstring>

namespace {
struct FileRef {
    GFile *p;
    explicit FileRef(GFile *value) : p(value) {}
    ~FileRef() {
        if (p)
            g_object_unref(p);
    }
    operator GFile *() const {
        return p;
    }
};
QString fileLabel(GFile *file) {
    char *name = g_file_get_parse_name(file);
    QString result = QString::fromUtf8(name);
    g_free(name);
    return result;
}
bool descendant(GFile *source, GFile *destination) {
    char *a = g_file_get_path(source), *b = g_file_get_path(destination);
    char *ra = a ? realpath(a, nullptr) : nullptr;
    char *rb = b ? realpath(b, nullptr) : nullptr;
    bool result = false;
    if (ra && rb) {
        QByteArray x(ra), y(rb);
        result = x == y || y.startsWith(x + '/');
    }
    free(ra);
    free(rb);
    g_free(a);
    g_free(b);
    return result;
}
} // namespace
Operations::Operations(QObject *parent) : QObject(parent) {
    m_cancel = g_cancellable_new();
}
Operations::~Operations() {
    cancel();
    if (m_thread.joinable())
        m_thread.join();
    g_object_unref(m_cancel);
}
void Operations::cancel() {
    m_cancelled = true;
    g_cancellable_cancel(m_cancel);
    m_condition.notify_all();
}
void Operations::resolveConflict(int choice) {
    {
        std::lock_guard lock(m_mutex);
        m_choice = choice;
    }
    m_waiting = false;
    Q_EMIT conflictChanged();
    m_condition.notify_all();
}
void Operations::report(QString message) {
    auto now = std::chrono::steady_clock::now();
    if (now - m_lastReport < std::chrono::milliseconds(100))
        return;
    m_lastReport = now;
    QMetaObject::invokeMethod(
        this,
        [this, message] {
            m_status = message;
            Q_EMIT changed();
        },
        Qt::QueuedConnection);
}
int Operations::conflict(const QString &name, bool folder) {
    {
        std::lock_guard lock(m_mutex);
        m_choice = -1;
    }
    QMetaObject::invokeMethod(
        this,
        [this, name, folder] {
            if (m_cancelled)
                return;
            m_conflictName = name;
            m_conflictFolder = folder;
            m_waiting = true;
            Q_EMIT conflictChanged();
        },
        Qt::QueuedConnection);
    std::unique_lock lock(m_mutex);
    m_condition.wait(lock, [this] { return m_choice >= 0 || m_cancelled; });
    return m_cancelled ? 0 : m_choice;
}
bool Operations::removeTree(GFile *file, GError **error) {
    GFileInfo *info =
        g_file_query_info(file, "standard::type", G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, m_cancel, error);
    if (!info)
        return false;
    bool directory = g_file_info_get_file_type(info) == G_FILE_TYPE_DIRECTORY;
    g_object_unref(info);
    if (directory) {
        auto *iter = g_file_enumerate_children(file, "standard::name", G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
                                               m_cancel, error);
        if (!iter)
            return false;
        bool success = true;
        while (!m_cancelled) {
            auto *childInfo = g_file_enumerator_next_file(iter, m_cancel, error);
            if (!childInfo)
                break;
            FileRef child(g_file_enumerator_get_child(iter, childInfo));
            g_object_unref(childInfo);
            if (!removeTree(child, error)) {
                success = false;
                break;
            }
        }
        g_object_unref(iter);
        if (!success || (error && *error) || m_cancelled)
            return false;
    }
    return g_file_delete(file, m_cancel, error);
}
bool Operations::transfer(GFile *source, GFile *destination, bool move, GError **error) {
    if (m_cancelled)
        return false;
    if (g_file_equal(source, destination)) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "源与目标相同，未执行操作");
        return false;
    }
    auto *info = g_file_query_info(source, "standard::type,unix::mode", G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
                                   m_cancel, error);
    if (!info)
        return false;
    auto type = g_file_info_get_file_type(info);
    const bool directory = type == G_FILE_TYPE_DIRECTORY;
    const bool hasMode = g_file_info_has_attribute(info, "unix::mode");
    const guint32 mode = g_file_info_get_attribute_uint32(info, "unix::mode") & 0777;
    g_object_unref(info);
    if (!directory && type != G_FILE_TYPE_REGULAR && type != G_FILE_TYPE_SYMBOLIC_LINK) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED, "不复制设备、管道或套接字");
        return false;
    }
    FileRef parent(g_file_get_parent(destination));
    if (directory && parent.p && descendant(source, parent)) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "不能将文件夹放入自身或其子目录");
        return false;
    }
    FileRef target(G_FILE(g_object_ref(destination)));
    bool overwrite = false;
    if (g_file_query_exists(target, m_cancel)) {
        int choice = conflict(fileLabel(target),
                              directory || g_file_query_file_type(target, G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
                                                                  m_cancel) == G_FILE_TYPE_DIRECTORY);
        if (choice == 0)
            return false;
        if (choice == 1) {
            if (directory || g_file_query_file_type(target, G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, m_cancel) ==
                                 G_FILE_TYPE_DIRECTORY) {
                g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_EXISTS,
                                    "文件夹不自动合并或替换，请选择保留两者");
                return false;
            }
            overwrite = true;
        } else {
            char *base = g_file_get_basename(destination);
            for (int suffix = 2; suffix < 100000; ++suffix) {
                QByteArray name = QByteArray(base) + " (" + QByteArray::number(suffix) + ")";
                FileRef candidate(g_file_get_child(parent, name.constData()));
                if (!g_file_query_exists(candidate, m_cancel)) {
                    g_object_unref(target.p);
                    target.p = G_FILE(g_object_ref(candidate.p));
                    break;
                }
            }
            g_free(base);
        }
    }
    auto flags =
        static_cast<GFileCopyFlags>(G_FILE_COPY_NOFOLLOW_SYMLINKS | (overwrite ? G_FILE_COPY_OVERWRITE : 0));
    if (move) {
        GError *moveError = nullptr;
        if (g_file_move(source, target, static_cast<GFileCopyFlags>(flags | G_FILE_COPY_NO_FALLBACK_FOR_MOVE),
                        m_cancel, nullptr, nullptr, &moveError))
            return true;
        if (!g_error_matches(moveError, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED) &&
            !g_error_matches(moveError, G_IO_ERROR, G_IO_ERROR_WOULD_RECURSE)) {
            g_propagate_error(error, moveError);
            return false;
        }
        g_clear_error(&moveError);
    }
    if (directory) {
        if (!g_file_make_directory(target, m_cancel, error))
            return false;
        if (hasMode && !g_file_set_attribute_uint32(target, "unix::mode", mode | 0700,
                                                    G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, m_cancel, error))
            return false;
        auto *iter = g_file_enumerate_children(source, "standard::name", G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
                                               m_cancel, error);
        if (!iter)
            return false;
        bool success = true;
        while (!m_cancelled) {
            auto *childInfo = g_file_enumerator_next_file(iter, m_cancel, error);
            if (!childInfo)
                break;
            FileRef child(g_file_enumerator_get_child(iter, childInfo));
            FileRef dest(g_file_get_child(target, g_file_info_get_name(childInfo)));
            g_object_unref(childInfo);
            if (!transfer(child, dest, false, error)) {
                success = false;
                break;
            }
        }
        g_object_unref(iter);
        if (!success || (error && *error) || m_cancelled)
            return false;
        if (hasMode && !g_file_set_attribute_uint32(target, "unix::mode", mode,
                                                    G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, m_cancel, error))
            return false;
        if (move)
            return removeTree(source, error);
        return true;
    }
    // Commit a completed copy from an exclusive temporary name; cancellation
    // never deletes the existing destination or a previously completed item.
    auto temporary = QByteArray(".nook-copy-") + QUuid::createUuid().toByteArray(QUuid::Id128) + ".part";
    FileRef temp(g_file_get_child(parent, temporary.constData()));
    report(QStringLiteral("正在处理 %1").arg(fileLabel(source)));
    bool success = g_file_copy(
        source, temp, G_FILE_COPY_NOFOLLOW_SYMLINKS, m_cancel,
        [](goffset current, goffset total, gpointer context) {
            auto *self = static_cast<Operations *>(context);
            self->report(QStringLiteral("正在复制 · %1 / %2")
                             .arg(nook::formatSize(current), nook::formatSize(total)));
        },
        this, error);
    if (success)
        success =
            g_file_move(temp, target, static_cast<GFileCopyFlags>(flags | G_FILE_COPY_NO_FALLBACK_FOR_MOVE),
                        m_cancel, nullptr, nullptr, error);
    if (!success) {
        if (!error || !*error || !g_error_matches(*error, G_IO_ERROR, G_IO_ERROR_EXISTS))
            g_file_delete(temp, nullptr, nullptr);
        return false;
    }
    return !move || g_file_delete(source, m_cancel, error);
}
void Operations::run(const QString &kind, const QStringList &uris, const QString &destination,
                     const QString &name) {
    if (m_busy)
        return;
    if ((kind == "mkdir" || kind == "touch" || kind == "rename") && !nook::validName(name)) {
        m_errors = {QStringLiteral("名称无效：不能包含 /、空字符，或超过文件系统名称长度")};
        Q_EMIT changed();
        return;
    }
    if (m_thread.joinable())
        m_thread.join();
    m_busy = true;
    m_cancelled = false;
    g_cancellable_reset(m_cancel);
    m_errors.clear();
    m_status = QStringLiteral("正在准备…");
    Q_EMIT changed();
    m_thread = std::thread([this, kind, uris, destination, name] {
        QStringList failures;
        int completed = 0, skipped = 0;
        auto record = [&](const QString &uri, GError *error) {
            if (error) {
                failures.append(nook::localPath(uri) + ": " + QString::fromUtf8(error->message));
                g_error_free(error);
            }
        };
        if (kind == "mkdir" || kind == "touch") {
            GError *error = nullptr;
            FileRef child(g_file_new_for_uri(nook::childUri(destination, name).toUtf8().constData()));
            bool ok = false;
            if (kind == "mkdir")
                ok = g_file_make_directory(child, m_cancel, &error);
            else if (auto *stream = g_file_create(child, G_FILE_CREATE_NONE, m_cancel, &error)) {
                ok = g_output_stream_close(G_OUTPUT_STREAM(stream), m_cancel, &error);
                g_object_unref(stream);
            }
            if (ok)
                ++completed;
            else
                record(destination, error);
        } else
            for (const auto &uri : uris) {
                if (m_cancelled)
                    break;
                FileRef source(g_file_new_for_uri(uri.toUtf8().constData()));
                GError *error = nullptr;
                bool ok = false;
                report(QStringLiteral("正在处理 %1").arg(fileLabel(source)));
                if (kind == "trash")
                    ok = g_file_trash(source, m_cancel, &error);
                else if (kind == "delete")
                    ok = removeTree(source, &error);
                else if (kind == "restore") {
                    GFileInfo *info = g_file_query_info(
                        source, "trash::orig-path", G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, m_cancel, &error);
                    if (info) {
                        const char *path = g_file_info_get_attribute_byte_string(info, "trash::orig-path");
                        if (path) {
                            FileRef target(g_file_new_for_path(path));
                            ok = transfer(source, target, true, &error);
                        } else
                            g_set_error_literal(&error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                                                "无法确定回收站项目的原始位置");
                        g_object_unref(info);
                    }
                } else {
                    FileRef parent(g_file_new_for_uri(
                        (kind == "rename" ? nook::parentUri(uri) : destination).toUtf8().constData()));
                    char *base = g_file_get_basename(source);
                    QByteArray raw = kind == "rename" ? name.toUtf8() : QByteArray(base);
                    FileRef target(g_file_get_child(parent, raw.constData()));
                    g_free(base);
                    ok = transfer(source, target, kind == "move" || kind == "rename", &error);
                }
                if (ok)
                    ++completed;
                else if (!error && !m_cancelled)
                    ++skipped;
                record(uri, error);
            }
        bool cancelled = m_cancelled;
        QMetaObject::invokeMethod(
            this,
            [this, failures, completed, skipped, cancelled] {
                m_busy = false;
                m_waiting = false;
                m_errors = failures;
                m_status = QStringLiteral("%1 · 完成 %2 项，跳过 %3 项，错误 %4 项")
                               .arg(cancelled ? "已取消" : "任务结束")
                               .arg(completed)
                               .arg(skipped)
                               .arg(failures.size());
                Q_EMIT changed();
                Q_EMIT conflictChanged();
                Q_EMIT finished();
            },
            Qt::QueuedConnection);
    });
}
