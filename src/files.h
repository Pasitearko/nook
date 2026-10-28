#pragma once
#include <QDateTime>
#include <QSet>
#include <QStringList>
#include <QVariantMap>
#include <atomic>
#include <functional>
#include <memory>
#include <vector>

namespace nook {
using Cancel = std::shared_ptr<std::atomic_bool>;
struct Entry {
    QString uri, name, kind, icon, originalUri;
    qint64 size = 0, modified = 0;
    bool directory = false, symlink = false, hidden = false;
};
QString normalizeUri(const QString &input);
QString localPath(const QString &uri);
QString parentUri(const QString &uri);
QString childUri(const QString &uri, const QString &name);
QString displayName(const QString &uri);
QString formatSize(qint64 bytes);
QString iconFor(const QString &name, bool directory);
QVariantMap entryMap(const Entry &entry);
QList<Entry> enumerate(const QString &uri, bool hidden, const Cancel &cancel, QString *error,
                       const std::function<void(QList<Entry>)> &batch = {});
QStringList localRoots();
bool sidebarDataPartition(const QString &filesystem, const QString &partitionType, const QString &label);
bool excludedPath(const QByteArray &path, const QStringList &extra);
// Windows system areas at the root of an NTFS/FAT volume ($Recycle.Bin, Windows, …): huge,
// churning and never what a file-name search is for. Case-insensitive, as on Windows.
bool windowsSystemEntry(const QByteArray &name);
QString folded(const QString &text);
QPair<QString, QString> pinyinKeys(const QString &text);
bool validName(const QString &name);
// Linux hidden state: a leading dot, or listed in the parent's .hidden file
// (the GIO/Nautilus/Dolphin convention, which hides without renaming).
QSet<QByteArray> hiddenNames(const QByteArray &directory);
bool hiddenFile(const QByteArray &path);
bool setHidden(const QString &uri, bool hide, QString *error, QString *resultUri);
} // namespace nook
