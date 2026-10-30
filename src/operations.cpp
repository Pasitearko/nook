#include "operations.h"
#include "files.h"
#include "tags.h"
#include <QCoreApplication>
#include <QFile>
#include <QFileInfo>
#include <QUrl>
#include <QUuid>
#include <algorithm>
#include <cstdlib>
#include <cstring>

namespace {
// Tags follow an item nook moves or renames, and leave with one it deletes for good (to empty).
// Called from the worker thread; the store lives on the GUI thread.
void followTags(const QString &from, const QString &to) {
    QMetaObject::invokeMethod(
        qApp,
        [from, to] {
            if (to.isEmpty())
                TagStore::instance()->removed(from);
            else
                TagStore::instance()->moved(from, to);
        },
        Qt::QueuedConnection);
}
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
// Search results/API callers can select both a folder and its descendants.
// Operate only on the outermost selections, independent of input order.
QStringList topLevelSelections(const QStringList &uris, GCancellable *cancel) {
    QStringList directories, result;
    for (const auto &uri : uris) {
        if (g_cancellable_is_cancelled(cancel))
            return {};
        FileRef file(g_file_new_for_uri(uri.toUtf8().constData()));
        if (g_file_query_file_type(file, G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, cancel) ==
            G_FILE_TYPE_DIRECTORY)
            directories.append(uri);
    }
    for (const auto &uri : uris) {
        if (g_cancellable_is_cancelled(cancel))
            return {};
        FileRef file(g_file_new_for_uri(uri.toUtf8().constData()));
        bool nested = false;
        for (const auto &directory : directories) {
            FileRef parent(g_file_new_for_uri(directory.toUtf8().constData()));
            if (g_file_has_prefix(file, parent)) {
                nested = true;
                break;
            }
        }
        if (!nested && !result.contains(uri))
            result.append(uri);
    }
    return result;
}
QString fileLabel(GFile *file) {
    char *name = g_file_get_parse_name(file);
    QString result = QString::fromUtf8(name);
    g_free(name);
    return result;
}
// A byte callback can finish before the temporary copy is committed (or its
// source removed). Reserve 1% for finalization so even a rounded percentage
// cannot show 100 before the final task notification.
double activeProgress(int completed, int total, double fraction = 0) {
    return total > 0 ? std::clamp((completed + fraction) / total, 0.0, 0.99) : -1;
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
    m_errorNotifyTimer.setSingleShot(true);
    m_errorNotifyTimer.setInterval(100);
    connect(&m_errorNotifyTimer, &QTimer::timeout, this, &Operations::errorsChanged);
}
QVariantList Operations::undoHistory() const {
    QVariantList result;
    for (const auto &record : m_undo)
        result.append(QVariantMap{{"title", record.title},
                                  {"detail", record.detail},
                                  {"kind", record.kind},
                                  {"count", record.items.size()}});
    return result;
}
QString Operations::undoText() const {
    return m_undo.isEmpty() ? QStringLiteral("撤销") : QStringLiteral("撤销") + m_undo.first().title;
}
void Operations::undo() {
    undoAt(0);
}
void Operations::undoAt(int index) {
    if (canUndo() && index >= 0 && index < m_undo.size())
        start("undo", {}, {}, {}, index);
}
void Operations::clearUndoHistory() {
    if (m_busy || m_undo.isEmpty())
        return;
    m_undo.clear();
    Q_EMIT undoChanged();
    Q_EMIT changed();
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
void Operations::report(QString message, double progress, bool force) {
    auto now = std::chrono::steady_clock::now();
    if (!force && now - m_lastReport < std::chrono::milliseconds(100))
        return;
    m_lastReport = now;
    QMetaObject::invokeMethod(
        this,
        [this, message, progress] {
            m_status = message;
            m_progress = progress;
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
bool Operations::transfer(GFile *source, GFile *destination, bool move, GError **error,
                          const TransferProgress *progress, QString *actualUri, bool *replaced,
                          bool allowConflict) {
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
        if (!allowConflict) {
            g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_EXISTS, "原位置已被占用，未覆盖现有文件");
            return false;
        }
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
    if (actualUri) {
        char *uri = g_file_get_uri(target);
        *actualUri = QString::fromUtf8(uri);
        g_free(uri);
    }
    if (replaced)
        *replaced = overwrite;
    auto flags =
        static_cast<GFileCopyFlags>(G_FILE_COPY_NOFOLLOW_SYMLINKS | (overwrite ? G_FILE_COPY_OVERWRITE : 0));
    // Nested files have no known weight within their selected directory. Do
    // not turn their per-file byte counts into an invented directory total.
    struct CallbackContext {
        Operations *self;
        const TransferProgress *progress;
        bool reportedBytes = false;
    } context{this, directory ? nullptr : progress};
    auto callback = +[](goffset current, goffset total, gpointer data) {
        auto &context = *static_cast<CallbackContext *>(data);
        double value = -1;
        if (context.progress && total > 0)
            value = activeProgress(context.progress->completed, context.progress->total,
                                   std::clamp(double(current) / double(total), 0.0, 1.0));
        context.self->report(
            QStringLiteral("正在复制 · %1 / %2").arg(nook::formatSize(current), nook::formatSize(total)),
            value, value >= 0 && !context.reportedBytes);
        if (value >= 0)
            context.reportedBytes = true;
    };
    if (move) {
        GError *moveError = nullptr;
        if (g_file_move(source, target, static_cast<GFileCopyFlags>(flags | G_FILE_COPY_NO_FALLBACK_FOR_MOVE),
                        m_cancel, callback, &context, &moveError))
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
            if (!transfer(child, dest, false, error, nullptr, nullptr, nullptr, allowConflict)) {
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
    bool success =
        g_file_copy(source, temp, G_FILE_COPY_NOFOLLOW_SYMLINKS, m_cancel, callback, &context, error);
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
    start(kind, uris, destination, name);
}
void Operations::start(const QString &kind, const QStringList &uris, const QString &destination,
                       const QString &name, int undoIndex) {
    if (m_busy)
        return;
    const QStringList supported{"copy",  "move",   "rename",  "mkdir", "touch",
                                "trash", "delete", "restore", "undo"};
    if (!supported.contains(kind) || (kind == "undo" && (undoIndex < 0 || undoIndex >= m_undo.size())))
        return;
    if ((kind == "mkdir" || kind == "touch" || kind == "rename") && !nook::validName(name)) {
        m_errors = {QStringLiteral("名称无效：不能包含 /、空字符，或超过文件系统名称长度")};
        Q_EMIT errorsChanged();
        Q_EMIT changed();
        return;
    }
    if (m_thread.joinable())
        m_thread.join();
    m_busy = true;
    m_cancelled = false;
    g_cancellable_reset(m_cancel);
    m_errorNotifyTimer.stop();
    m_errors.clear();
    Q_EMIT errorsChanged();
    m_status = QStringLiteral("正在准备…");
    m_kind = kind;
    m_completedItems = 0;
    m_totalItems = kind == "undo"                       ? m_undo.at(undoIndex).items.size()
                   : kind == "mkdir" || kind == "touch" ? 1
                                                        : int(uris.size());
    m_progress = -1;
    m_lastReport = {};
    Q_EMIT changed();
    // History cannot change while busy; keep the selected slot through completion,
    // including failed or cancelled batches, without disturbing other records.
    const auto undoRecord = kind == "undo" ? m_undo.at(undoIndex) : nook::UndoRecord{};
    m_thread = std::thread([this, kind, uris, destination, name, total = m_totalItems, undoRecord,
                            undoIndex]() mutable {
        const auto sources = kind == "move" || kind == "trash" ? topLevelSelections(uris, m_cancel) : uris;
        if (sources.size() != uris.size()) {
            total = sources.size();
            QMetaObject::invokeMethod(
                this,
                [this, total] {
                    m_totalItems = total;
                    Q_EMIT changed();
                },
                Qt::QueuedConnection);
        }
        QStringList failures;
        int completed = 0, skipped = 0, untracked = 0;
        nook::UndoRecord journal;
        journal.kind = kind;
        auto remaining = undoRecord;
        auto remember = [&](nook::UndoItem item) {
            if (item.fingerprint.isEmpty()) {
                ++untracked;
                return;
            }
            journal.items.append(item);
        };
        auto record = [&](const QString &uri, GError *error) {
            if (error) {
                const auto label = QUrl(uri).isLocalFile() ? nook::localPath(uri) : uri;
                const auto failure = label + ": " + QString::fromUtf8(error->message);
                failures.append(failure);
                g_error_free(error);
                // Queue only the new error, never a growing implicitly-shared list.
                // The first error is immediate; later UI rebuilds are coalesced.
                QMetaObject::invokeMethod(
                    this,
                    [this, failure] {
                        const bool first = m_errors.isEmpty();
                        m_errors.append(failure);
                        if (first)
                            Q_EMIT errorsChanged();
                        else if (!m_errorNotifyTimer.isActive())
                            m_errorNotifyTimer.start();
                    },
                    Qt::QueuedConnection);
            }
        };
        if (kind == "undo") {
            // Preflight the entire batch before touching anything. No conflict
            // dialog on undo: replacing unrelated data is never an option.
            bool safe = true;
            for (const auto &item : remaining.items) {
                if (m_cancelled) {
                    safe = false;
                    break;
                }
                if (nook::undoParentIdentity(item.current) != item.currentParent ||
                    nook::undoFingerprint(item.current, m_cancel) != item.fingerprint ||
                    (!item.original.isEmpty() &&
                     (nook::undoParentIdentity(item.original) != item.originalParent ||
                      nook::undoPathExists(item.original)))) {
                    GError *error = nullptr;
                    g_set_error_literal(&error, G_IO_ERROR, G_IO_ERROR_FAILED,
                                        "无法安全撤销：文件已变化、已不存在，或原位置已被占用。未覆盖任何文件"
                                        "；可处理后重试，或清空撤销历史。");
                    record(item.current, error);
                    safe = false;
                    break;
                }
            }
            while (safe && !remaining.items.isEmpty() && !m_cancelled) {
                const auto item = remaining.items.last();
                FileRef source(g_file_new_for_path(QFile::encodeName(item.current).constData()));
                GError *error = nullptr;
                bool ok;
                report(QStringLiteral("正在撤销 · %1").arg(QFileInfo(item.current).fileName()));
                if (item.original.isEmpty())
                    ok = g_file_trash(source, m_cancel, &error);
                else {
                    // GIO's check + rename is not atomic no-replace on Linux.
                    // Never fall back to copy/delete during an undo.
                    ok = nook::undoMove(item, &error);
                }
                if (ok) {
                    if (!item.original.isEmpty())
                        followTags(nook::normalizeUri(item.current), nook::normalizeUri(item.original));
                    if (!item.trashInfo.isEmpty())
                        QFile::remove(item.trashInfo);
                    remaining.items.removeLast();
                    ++completed;
                    QMetaObject::invokeMethod(
                        this,
                        [this, completed, total] {
                            m_completedItems = completed;
                            m_progress = activeProgress(completed, total);
                            Q_EMIT changed();
                        },
                        Qt::QueuedConnection);
                }
                record(item.current, error);
                if (!ok)
                    break;
            }
        } else if (kind == "mkdir" || kind == "touch") {
            GError *error = nullptr;
            FileRef child(g_file_new_for_uri(nook::childUri(destination, name).toUtf8().constData()));
            bool ok = false;
            if (kind == "mkdir")
                ok = g_file_make_directory(child, m_cancel, &error);
            else if (auto *stream = g_file_create(child, G_FILE_CREATE_NONE, m_cancel, &error)) {
                ok = g_output_stream_close(G_OUTPUT_STREAM(stream), m_cancel, &error);
                g_object_unref(stream);
            }
            if (ok) {
                ++completed;
                remember(nook::undoItem(nook::undoLocalPath(nook::childUri(destination, name)), {}, nullptr));
            } else
                record(destination, error);
        } else
            for (const auto &uri : sources) {
                if (m_cancelled)
                    break;
                FileRef source(g_file_new_for_uri(uri.toUtf8().constData()));
                GError *error = nullptr;
                bool ok = false;
                const TransferProgress progress{completed, total};
                // Reset at every selected item, even if status reporting is throttled.
                QMetaObject::invokeMethod(
                    this,
                    [this] {
                        m_progress = -1;
                        Q_EMIT changed();
                    },
                    Qt::QueuedConnection);
                report(QStringLiteral("正在处理 %1").arg(fileLabel(source)));
                QString actualUri;
                bool replaced = false;
                const auto localSource = nook::undoLocalPath(uri);
                QByteArray beforeTrash;
                if (kind == "trash") {
                    beforeTrash = nook::undoFingerprint(localSource, m_cancel);
                    ok = g_file_trash(source, m_cancel, &error);
                } else if (kind == "delete")
                    ok = removeTree(source, &error);
                else if (kind == "restore") {
                    GFileInfo *info = g_file_query_info(
                        source, "trash::orig-path", G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, m_cancel, &error);
                    if (info) {
                        const char *path = g_file_info_get_attribute_byte_string(info, "trash::orig-path");
                        if (path) {
                            FileRef target(g_file_new_for_path(path));
                            ok = transfer(source, target, true, &error, &progress, &actualUri, &replaced);
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
                    ok = transfer(source, target, kind == "move" || kind == "rename", &error, &progress,
                                  &actualUri, &replaced);
                }
                if (ok) {
                    ++completed;
                    if ((kind == "move" || kind == "rename" || kind == "restore") && !actualUri.isEmpty())
                        followTags(uri, actualUri);
                    else if (kind == "delete")
                        followTags(uri, {});
                    if (kind == "trash")
                        remember(nook::trashedUndoItem(localSource, beforeTrash, nullptr));
                    else if (kind != "delete" && kind != "restore" && !replaced && !localSource.isEmpty() &&
                             !nook::undoLocalPath(actualUri).isEmpty())
                        remember(nook::undoItem(nook::undoLocalPath(actualUri),
                                                kind == "copy" ? QString{} : localSource, nullptr));
                    else
                        ++untracked;
                    QMetaObject::invokeMethod(
                        this,
                        [this, completed, total] {
                            m_completedItems = completed;
                            m_progress = activeProgress(completed, total);
                            Q_EMIT changed();
                        },
                        Qt::QueuedConnection);
                } else if (!error && !m_cancelled)
                    ++skipped;
                record(uri, error);
            }
        if (!journal.items.isEmpty()) {
            const QMap<QString, QString> labels{{"copy", "粘贴"},        {"move", "移动"},
                                                {"rename", "重命名"},    {"trash", "删除"},
                                                {"mkdir", "新建文件夹"}, {"touch", "新建文件"}};
            const auto &item = journal.items.first();
            const auto labelPath = item.original.isEmpty() ? item.current : item.original;
            const auto title = QFileInfo(labelPath).fileName();
            journal.title = labels.value(kind, kind) + QStringLiteral("“%1”").arg(title);
            if (journal.items.size() > 1)
                journal.title += QStringLiteral("等 %1 项").arg(journal.items.size());
            journal.detail = item.original.isEmpty() ? item.current
                                                     : item.original + QStringLiteral("\n→ ") + item.current;
        }
        bool cancelled = m_cancelled;
        QMetaObject::invokeMethod(
            this,
            [this, failures, completed, skipped, cancelled, total, journal, remaining, kind, untracked,
             undoIndex] {
                if (kind == "undo") {
                    if (remaining.items.isEmpty())
                        m_undo.removeAt(undoIndex);
                    else
                        m_undo[undoIndex] = remaining;
                } else if (!journal.items.isEmpty()) {
                    m_undo.prepend(journal);
                    if (m_undo.size() > 5)
                        m_undo.removeLast();
                }
                Q_EMIT undoChanged();
                m_busy = false;
                m_waiting = false;
                m_errorNotifyTimer.stop();
                m_errors = failures;
                Q_EMIT errorsChanged();
                m_completedItems = completed;
                m_progress = total > 0 ? double(completed) / total : 0;
                m_status = QStringLiteral("%1 · 完成 %2 项，跳过 %3 项，错误 %4 项")
                               .arg(cancelled ? "已取消" : "任务结束")
                               .arg(completed)
                               .arg(skipped)
                               .arg(failures.size());
                if (untracked > 0)
                    m_status += QStringLiteral(" · %1 项不支持安全撤销").arg(untracked);
                Q_EMIT changed();
                Q_EMIT conflictChanged();
                Q_EMIT finished();
            },
            Qt::QueuedConnection);
    });
}
