#pragma once
#include <QByteArray>
#include <QString>
#include <QVector>
#include <gio/gio.h>

namespace nook {
// Session-only journal. Never serialize stale filesystem identities across restarts.
struct UndoItem {
    QString current, original, trashInfo;
    QByteArray fingerprint, currentParent, originalParent;
};
struct UndoRecord {
    QString kind, title, detail;
    QVector<UndoItem> items;
};
QByteArray undoFingerprint(const QString &path, GCancellable *cancel);
QByteArray undoParentIdentity(const QString &path);
UndoItem undoItem(const QString &current, const QString &original, GCancellable *cancel);
// g_file_trash does not return its destination. Match the Trash spec metadata AND
// the original inode; never restore a same-named, older trash entry by guessing.
UndoItem trashedUndoItem(const QString &original, const QByteArray &fingerprint, GCancellable *cancel);
bool undoPathExists(const QString &path);
// Reject lossy QString paths (Linux filenames are arbitrary bytes).
QString undoLocalPath(const QString &uri);
// Linux atomic no-replace, pinned parent directories; no copy/delete fallback.
bool undoMove(const UndoItem &item, GError **error);
} // namespace nook
