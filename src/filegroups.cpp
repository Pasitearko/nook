#include "filegroups.h"
#include "directorymodel.h"
#include "storage.h"
#include <QCryptographicHash>
#include <QDateTime>
#include <QMimeDatabase>
#include <QSettings>
#include <algorithm>
#include <limits>

namespace {
QString settingsPath() { return nook::storageDirectory(nook::Storage::Config) + "/folder-groups.ini"; }
QString settingsKey(const QString &uri) {
    return QString::fromLatin1(QCryptographicHash::hash(uri.toUtf8(), QCryptographicHash::Sha256).toHex());
}
bool validMode(const QString &mode) {
    return mode == "none" || mode == "date" || mode == "type" || mode == "size";
}
// Keys sort into a fixed, meaningful group order, independently of the file sort direction.
QPair<QString, QString> category(const QVariantMap &entry, const QString &mode, const QDate &today) {
    if (entry.value("folder").toBool()) return {"00-folders", "文件夹"};
    if (mode == "date") {
        const auto stamp = entry.value("modified").toLongLong();
        const auto date = QDateTime::fromSecsSinceEpoch(stamp).toLocalTime().date();
        if (stamp <= 0 || !date.isValid()) return {"99-unknown", "时间未知"};
        const qint64 age = date.daysTo(today);
        if (age < 0) return {"01-future", "未来日期"};
        if (age == 0) return {"02-today", "今天"};
        if (age == 1) return {"03-yesterday", "昨天"};
        if (age < 7) return {"04-week", "过去 7 天"};
        if (age < 30) return {"05-month", "过去 30 天"};
        return {"06-earlier", "更早"};
    }
    if (mode == "size") {
        const auto size = entry.value("bytes").toLongLong();
        if (size < 0) return {"99-unknown", "大小未知"};
        if (size < 1024 * 1024) return {"01-small", "小于 1 MB"};
        if (size < 100 * 1024 * 1024) return {"02-medium", "1–100 MB"};
        if (size < 1024LL * 1024 * 1024) return {"03-large", "100 MB–1 GB"};
        return {"04-huge", "1 GB 及以上"};
    }
    static const QMimeDatabase mimeDatabase;
    const QString mime = mimeDatabase.mimeTypeForFile(entry.value("fileName").toString(), QMimeDatabase::MatchExtension).name();
    if (mime.startsWith("image/")) return {"01-images", "图片"};
    if (mime.startsWith("video/")) return {"02-video", "视频"};
    if (mime.startsWith("audio/")) return {"03-audio", "音频"};
    const auto icon = entry.value("iconName").toString();
    if (mime.startsWith("text/") || icon == "document" || icon == "code" || mime == "application/pdf"
        || mime.contains("officedocument") || mime.contains("opendocument") || mime == "application/epub+zip")
        return {"04-documents", "文档"};
    // iconFor() intentionally uses a generic file icon for archives. Classify their
    // MIME types directly; office/EPUB ZIP containers have already stayed documents.
    static const QSet<QString> archives = {"application/zip", "application/x-7z-compressed",
        "application/vnd.rar", "application/x-rar", "application/x-tar", "application/gzip",
        "application/x-bzip", "application/x-bzip2", "application/x-xz", "application/zstd",
        "application/x-lzma", "application/x-compressed-tar", "application/x-archive",
        "application/vnd.debian.binary-package", "application/x-rpm"};
    if (icon == "archive" || archives.contains(mime)) return {"05-archives", "压缩包"};
    return {"06-other", "其他"};
}
}

FileGroups::FileGroups(DirectoryModel *source) : QAbstractListModel(source), m_source(source) {
    m_rebuildTimer.setSingleShot(true);
    m_rebuildTimer.setInterval(0);
    connect(&m_rebuildTimer, &QTimer::timeout, this, &FileGroups::rebuild);
    const auto schedule = [this] { if (active()) m_rebuildTimer.start(); };
    const auto invalidate = [this] {
        if (!active() || !m_ready) return;
        m_ready = false; Q_EMIT readyChanged();
    };
    connect(source, &QAbstractItemModel::modelAboutToBeReset, this, invalidate);
    connect(source, &QAbstractItemModel::rowsAboutToBeInserted, this, invalidate);
    connect(source, &QAbstractItemModel::rowsAboutToBeRemoved, this, invalidate);
    connect(source, &QAbstractItemModel::rowsAboutToBeMoved, this, invalidate);
    connect(source, &QAbstractItemModel::modelReset, this, schedule);
    connect(source, &QAbstractItemModel::rowsInserted, this, schedule);
    connect(source, &QAbstractItemModel::rowsRemoved, this, schedule);
    connect(source, &QAbstractItemModel::rowsMoved, this, schedule);
    connect(source, &QAbstractItemModel::dataChanged, this,
        [this](const QModelIndex &first, const QModelIndex &last, const QList<int> &roles) {
            if (!active() || roles == QList<int>{DirectoryModel::SelectedRole}) return;
            // Folder totals and human-readable dates do not change group membership. Updating
            // them must not recycle a tile beneath a pointer or restart every thumbnail.
            if (roles.isEmpty() || roles.contains(DirectoryModel::NameRole)
                || roles.contains(DirectoryModel::ModifiedEpochRole) || roles.contains(DirectoryModel::FolderRole)
                || roles.contains(DirectoryModel::IconRole)) m_rebuildTimer.start();
            else if (m_mode == "size" && roles.contains(DirectoryModel::BytesRole)) {
                bool files = false;
                for (int row = first.row(); row <= last.row(); ++row)
                    files |= !m_source->data(m_source->index(row), DirectoryModel::FolderRole).toBool();
                if (files) m_rebuildTimer.start();
                else { ++m_revision; Q_EMIT revisionChanged(); }
            }
            else { ++m_revision; Q_EMIT revisionChanged(); }
        });
    connect(source, &DirectoryModel::selectionChanged, this, [this] {
        if (!m_ready) return;
        for (int i = 0; i < m_rows.size(); ++i)
            if (m_rows[i].sources.isEmpty()) Q_EMIT dataChanged(index(i), index(i), {PickedRole});
    });
    connect(source, &DirectoryModel::locationChanged, this, &FileGroups::restore);
    // Calendar buckets advance even when a window is left open overnight.
    m_dayTimer.setInterval(60000);
    connect(&m_dayTimer, &QTimer::timeout, this, [this, day = QDate::currentDate()]() mutable {
        const auto today = QDate::currentDate();
        if (day != today) { day = today; if (active() && m_mode == "date") rebuild(); }
    });
    m_dayTimer.start();
}
bool FileGroups::available() const { return !nook::localPath(m_source->uri()).isEmpty() && !m_source->tagView(); }
int FileGroups::rowCount(const QModelIndex &parent) const { return parent.isValid() ? 0 : m_rows.size(); }
QHash<int, QByteArray> FileGroups::roleNames() const {
    return {{HeaderRole, "groupHeader"}, {KeyRole, "groupKey"}, {TitleRole, "groupTitle"},
            {CountRole, "itemCount"}, {PickedRole, "pickedCount"}, {CollapsedRole, "groupCollapsed"}, {SourcesRole, "sourceRows"}};
}
QVariant FileGroups::data(const QModelIndex &idx, int role) const {
    if (!idx.isValid() || idx.row() < 0 || idx.row() >= m_rows.size()) return {};
    const auto &row = m_rows[idx.row()];
    const auto &group = m_groups[row.group];
    switch (role) {
    case HeaderRole: return row.sources.isEmpty();
    case KeyRole: return group.key;
    case TitleRole: return group.title;
    case CountRole: return group.sources.size();
    case PickedRole: {
        if (!row.sources.isEmpty()) return 0;
        int count = 0;
        for (int source : group.sources) count += m_source->data(m_source->index(source), DirectoryModel::SelectedRole).toBool();
        return count;
    }
    case CollapsedRole: return m_collapsed.contains(group.key);
    case SourcesRole: { QVariantList list; for (int source : row.sources) list.append(source); return list; }
    default: return {};
    }
}
void FileGroups::setMode(const QString &mode) {
    if (!available() || !validMode(mode) || mode == m_mode) return;
    m_mode = mode; m_collapsed.clear(); m_anchorUri.clear();
    save(); rebuild(); Q_EMIT modeChanged();
}
void FileGroups::setColumns(int columns) {
    columns = qBound(1, columns, 128);
    if (columns == m_columns) return;
    m_columns = columns;
    if (m_ready) relayout();
    Q_EMIT columnsChanged();
}
void FileGroups::restore() {
    m_mode = "none"; m_collapsed.clear(); m_anchorUri.clear();
    if (available()) {
        QSettings store(settingsPath(), QSettings::IniFormat);
        const auto saved = store.value(settingsKey(m_source->uri())).toMap();
        const auto mode = saved.value("mode").toString();
        if (validMode(mode)) m_mode = mode;
        const auto collapsed = saved.value("collapsed").toStringList();
        m_collapsed = QSet<QString>(collapsed.begin(), collapsed.end());
    }
    // locationChanged precedes clearing/loading the previous directory's entries.
    beginResetModel(); m_groups.clear(); m_rows.clear(); m_visible.clear(); m_sourceLayout.clear(); m_uriSources.clear();
    m_ready = true; endResetModel(); Q_EMIT readyChanged();
    ++m_revision; Q_EMIT revisionChanged(); Q_EMIT modeChanged();
}
void FileGroups::save() const {
    if (!available()) return;
    QSettings store(settingsPath(), QSettings::IniFormat);
    store.setValue(settingsKey(m_source->uri()), QVariantMap{{"mode", m_mode}, {"collapsed", QStringList(m_collapsed.begin(), m_collapsed.end())}});
}
void FileGroups::rebuild() {
    m_rebuildTimer.stop();
    beginResetModel();
    m_groups.clear(); m_uriSources.clear();
    if (active()) {
        QMap<QString, Group> groups;
        const auto today = QDate::currentDate();
        for (int i = 0; i < m_source->count(); ++i) {
            const auto entry = m_source->entry(i);
            m_uriSources.insert(entry.value("fileUri").toString(), i);
            const auto [key, title] = category(entry, m_mode, today);
            auto &group = groups[key]; group.key = key; group.title = title; group.sources.append(i);
        }
        for (const auto &group : groups) m_groups.append(group);
    }
    const bool wasReady = m_ready;
    m_ready = true;
    relayout(false);
    if (!wasReady) Q_EMIT readyChanged();
}
void FileGroups::relayout(bool startReset) {
    if (startReset) beginResetModel();
    m_rows.clear(); m_visible.clear(); m_sourceLayout.clear();
    for (int i = 0; i < m_groups.size(); ++i) {
        const auto &group = m_groups[i];
        m_rows.append(Row{i, {}});
        if (m_collapsed.contains(group.key)) continue;
        for (int at = 0; at < group.sources.size(); at += m_columns) {
            const auto sources = group.sources.mid(at, m_columns);
            for (int source : sources) { m_sourceLayout.insert(source, m_rows.size()); m_visible.append(source); }
            m_rows.append({i, sources});
        }
    }
    endResetModel();
    ++m_revision; Q_EMIT revisionChanged();
}
void FileGroups::toggle(const QString &key) {
    if (!m_ready) return;
    if (std::none_of(m_groups.cbegin(), m_groups.cend(), [&](const Group &g) { return g.key == key; })) return;
    if (!m_collapsed.remove(key)) m_collapsed.insert(key);
    save(); relayout();
}
void FileGroups::expandAll() { if (!m_ready) return; m_collapsed.clear(); save(); relayout(); }
void FileGroups::collapseAll() {
    if (!m_ready) return;
    for (const auto &group : m_groups) m_collapsed.insert(group.key);
    save(); relayout();
}
void FileGroups::selectGroup(const QString &key) {
    if (!m_ready) return;
    for (const auto &group : m_groups)
        if (group.key == key) { m_source->selectRows(group.sources); m_anchorUri.clear(); return; }
}
void FileGroups::selectSource(int row, int modifiers) {
    if (!m_ready) return;
    const int target = m_visible.indexOf(row);
    if (target < 0) return;
    const int anchor = (modifiers & 2) ? m_visible.indexOf(m_uriSources.value(m_anchorUri, -1)) : -1;
    if ((modifiers & 2) && anchor >= 0) {
        m_source->selectRows(m_visible.mid(qMin(anchor, target), qAbs(anchor - target) + 1),
                             modifiers & 1 ? m_source->selectedUris() : QStringList());
    } else {
        m_source->select(row, modifiers & 1);
        m_anchorUri = m_source->data(m_source->index(row), DirectoryModel::UriRole).toString();
    }
}
int FileGroups::moveSource(int current, int step) const {
    if (!m_ready || m_visible.isEmpty()) return -1;
    const int at = m_visible.indexOf(current);
    if (step < -1000000) return m_visible.first();
    if (step > 1000000) return m_visible.last();
    if (at < 0) return step < 0 ? m_visible.last() : m_visible.first();
    const auto next = qBound<qint64>(0, qint64(at) + step, m_visible.size() - 1);
    return m_visible[next];
}
int FileGroups::moveVertical(int current, int direction) const {
    if (!m_ready || (direction != -1 && direction != 1)) return -1;
    const int row = layoutRow(current);
    if (row < 0) return moveSource(-1, direction);
    const int column = m_rows[row].sources.indexOf(current);
    for (int next = row + direction; next >= 0 && next < m_rows.size(); next += direction) {
        const auto &sources = m_rows[next].sources;
        if (!sources.isEmpty()) return sources[qMin(column, int(sources.size()) - 1)];
    }
    return current;
}
int FileGroups::sourceForUri(const QString &uri) const {
    if (!m_ready) return -1;
    const int source = m_uriSources.value(uri, -1);
    return m_sourceLayout.contains(source) ? source : -1;
}
int FileGroups::layoutRow(int source) const { return m_ready ? m_sourceLayout.value(source, -1) : -1; }
QVariantMap FileGroups::info(int group) const {
    if (group < 0 || group >= m_groups.size()) return {};
    const auto &g = m_groups[group];
    int picked = 0;
    for (int source : g.sources) picked += m_source->data(m_source->index(source), DirectoryModel::SelectedRole).toBool();
    return {{"key", g.key}, {"title", g.title}, {"count", g.sources.size()}, {"picked", picked}, {"collapsed", m_collapsed.contains(g.key)}};
}
QVariantMap FileGroups::groupInfo(int row) const { return row >= 0 && row < m_rows.size() ? info(m_rows[row].group) : QVariantMap(); }
int FileGroups::layoutForGroup(const QString &key) const {
    if (!m_ready) return -1;
    for (int i = 0; i < m_rows.size(); ++i)
        if (m_rows[i].sources.isEmpty() && m_groups[m_rows[i].group].key == key) return i;
    return -1;
}
QList<int> FileGroups::sourcesBetweenRows(int first, int last, double x1, double x2, double cellWidth, bool grid) const {
    QList<int> result;
    if (!m_ready || cellWidth <= 0 || first < 0 || last < 0) return result;
    const int begin = qMin(first, last), end = qMin(qMax(first, last), int(m_rows.size()) - 1);
    for (int i = begin; i <= end; ++i) {
        const auto &sources = m_rows[i].sources;
        for (int c = 0; c < sources.size(); ++c) {
            const double left = grid ? c * cellWidth + 5 : 9;
            const double right = grid ? (c + 1) * cellWidth - 5 : cellWidth - 9;
            if (x2 >= left && x1 <= right) result.append(sources[c]);
        }
    }
    return result;
}
