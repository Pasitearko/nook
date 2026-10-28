#include "directorymodel.h"
#include <QCollator>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QMimeDatabase>
#include <QRegularExpression>
#include <QPointer>
#include <QtConcurrent>

DirectoryModel::DirectoryModel(QObject *parent) : QAbstractListModel(parent) {
    m_refreshTimer.setSingleShot(true);
    m_refreshTimer.setInterval(180);
    connect(&m_refreshTimer, &QTimer::timeout, this, &DirectoryModel::refresh);
    connect(&m_watcher, &QFileSystemWatcher::directoryChanged, this, [this] { m_refreshTimer.start(); });
}
DirectoryModel::~DirectoryModel() {
    if (m_cancel)
        *m_cancel = true;
}
int DirectoryModel::rowCount(const QModelIndex &parent) const {
    return parent.isValid() ? 0 : m_entries.size();
}
QHash<int, QByteArray> DirectoryModel::roleNames() const {
    return {{NameRole, "fileName"},     {UriRole, "fileUri"},   {FolderRole, "folder"},
            {LinkRole, "isLink"},       {SizeRole, "sizeText"}, {ModifiedRole, "modifiedText"},
            {KindRole, "kind"},         {IconRole, "iconName"}, {SelectedRole, "selected"},
            {FullPathRole, "fullPath"}, {BytesRole, "bytes"},   {OriginalRole, "originalUri"},
            {HiddenRole, "hiddenItem"}};
}
QVariant DirectoryModel::data(const QModelIndex &idx, int role) const {
    if (!idx.isValid() || idx.row() < 0 || idx.row() >= m_entries.size())
        return {};
    const auto &e = m_entries[idx.row()];
    switch (role) {
    case NameRole:
        return e.name;
    case UriRole:
        return e.uri;
    case FolderRole:
        return e.directory;
    case LinkRole:
        return e.symlink;
    case SizeRole:
        return e.directory ? QStringLiteral("—") : nook::formatSize(e.size);
    case ModifiedRole:
        return QDateTime::fromSecsSinceEpoch(e.modified).toLocalTime().toString("MM-dd HH:mm");
    case KindRole:
        return e.kind;
    case IconRole:
        return e.icon;
    case SelectedRole:
        return m_selected.contains(e.uri);
    case FullPathRole:
        return nook::localPath(e.uri);
    case BytesRole:
        return e.size;
    case OriginalRole:
        return e.originalUri;
    case HiddenRole:
        return e.hidden;
    default:
        return {};
    }
}
QStringList DirectoryModel::selectedUris() const {
    // Read by many bindings per selection change; rebuild once per change, not per read.
    if (m_selectedDirty) {
        m_selectedCache.clear();
        for (const auto &e : m_entries)
            if (m_selected.contains(e.uri))
                m_selectedCache.append(e.uri);
        m_selectedDirty = false;
    }
    return m_selectedCache;
}
QVariantMap DirectoryModel::firstSelected() const {
    if (!m_selected.isEmpty())
        for (const auto &e : m_entries)
            if (m_selected.contains(e.uri))
                return nook::entryMap(e);
    return {};
}
QVariantList DirectoryModel::selectedEntries() const {
    QVariantList result;
    if (!m_selected.isEmpty())
        for (const auto &e : m_entries)
            if (m_selected.contains(e.uri))
                result << nook::entryMap(e);
    return result;
}
QVariantList DirectoryModel::breadcrumbs() const {
    QVariantList result;
    QString current = m_uri;
    for (int i = 0; i < 64 && !current.isEmpty(); ++i) {
        result.prepend(QVariantMap{{"title", nook::displayName(current)}, {"uri", current}});
        auto parent = nook::parentUri(current);
        if (parent == current)
            break;
        current = parent;
    }
    if (result.size() > 5) {
        QVariantList compact{result.first(),
                             QVariantMap{{"title", "…"}, {"uri", result[result.size() - 4].toMap()["uri"]}}};
        for (int i = result.size() - 3; i < result.size(); ++i)
            compact.append(result[i]);
        return compact;
    }
    return result;
}
void DirectoryModel::setShowHidden(bool value) {
    if (value == m_hidden)
        return;
    m_hidden = value;
    Q_EMIT showHiddenChanged();
    refresh();
}
void DirectoryModel::navigate(const QString &input) {
    QString u = nook::normalizeUri(input);
    if (m_uri == u) {
        refresh();
        return;
    }
    while (m_history.size() > m_historyAt + 1)
        m_history.removeLast();
    m_history.append(u);
    m_historyAt = m_history.size() - 1;
    m_uri = u;
    m_selected.clear();
    m_anchor = -1;
    Q_EMIT locationChanged();
    notifySelection();
    load();
}
void DirectoryModel::refresh() {
    if (!m_uri.isEmpty())
        load(true);
}
void DirectoryModel::back() {
    if (canBack()) {
        m_uri = m_history[--m_historyAt];
        clearSelection();
        Q_EMIT locationChanged();
        load();
    }
}
void DirectoryModel::forward() {
    if (canForward()) {
        m_uri = m_history[++m_historyAt];
        clearSelection();
        Q_EMIT locationChanged();
        load();
    }
}
void DirectoryModel::up() {
    if (!m_uri.startsWith("trash:"))
        navigate(nook::parentUri(m_uri));
}
void DirectoryModel::setBusy(bool value, QString error) {
    m_loading = value;
    m_error = std::move(error);
    Q_EMIT statusChanged();
}
void DirectoryModel::load(bool keep) {
    if (m_cancel)
        *m_cancel = true;
    m_cancel = std::make_shared<std::atomic_bool>(false);
    auto cancel = m_cancel;
    const auto generation = ++m_generation;
    const QString uri = m_uri, sort = m_sort;
    const bool hidden = m_hidden, descending = m_descending;
    const auto filter = m_filter;
    if (!m_watcher.directories().isEmpty())
        m_watcher.removePaths(m_watcher.directories());
    const QString local = nook::localPath(uri);
    if (!local.isEmpty())
        m_watcher.addPath(local);
    if (!keep) {
        beginResetModel();
        m_entries.clear();
        m_anchor = -1;
        endResetModel();
        Q_EMIT countChanged();
        m_selectedDirty = true;
    }
    setBusy(true);
    QPointer<DirectoryModel> guard(this);
    // Implicitly shared: the worker diffs against this snapshot without copying rows.
    const QList<nook::Entry> previous = keep ? m_entries : QList<nook::Entry>();
    (void)QtConcurrent::run([guard, uri, sort, hidden, descending, cancel, generation, keep, previous, filter] {
        QElapsedTimer timer;
        timer.start();
        QList<nook::Entry> all, pending;
        QString error;
        nook::enumerate(uri, hidden, cancel, &error, [&](QList<nook::Entry> batch) {
            if (filter)
                batch.removeIf([&filter](const nook::Entry &entry) { return !filter->accepts(entry); });
            all.append(batch);
            // Fast folders arrive as one sorted set; only slow ones stream partial rows.
            // A refresh never streams: the current rows stay until the diff replaces them.
            auto elapsed = timer.elapsed();
            if (keep || elapsed < 150) {
                pending.append(batch);
                return;
            }
            batch = pending + batch;
            pending.clear();
            QMetaObject::invokeMethod(
                qApp,
                [guard, generation, batch = std::move(batch), elapsed] {
                    if (!guard || guard->m_generation != generation)
                        return;
                    bool first = guard->m_entries.isEmpty();
                    guard->appendEntries(batch);
                    if (first)
                        Q_EMIT guard->firstBatch(elapsed);
                },
                Qt::QueuedConnection);
        });
        if (cancel->load())
            return;
        QCollator collator;
        collator.setNumericMode(true);
        collator.setCaseSensitivity(Qt::CaseInsensitive);
        std::stable_sort(all.begin(), all.end(), [&](const auto &a, const auto &b) {
            if (a.directory != b.directory)
                return a.directory;
            int cmp = sort == "size"   ? (a.size > b.size   ? 1
                                          : a.size < b.size ? -1
                                                            : 0)
                      : sort == "date" ? (a.modified > b.modified   ? 1
                                          : a.modified < b.modified ? -1
                                                                    : 0)
                      : sort == "type" ? QString::compare(a.kind, b.kind)
                                       : 0;
            if (cmp == 0)
                cmp = collator.compare(a.name, b.name);
            return descending ? cmp > 0 : cmp < 0;
        });
        QList<Change> changes;
        const bool incremental = keep && diff(previous, all, &changes);
        const auto elapsed = timer.elapsed();
        QMetaObject::invokeMethod(
            qApp,
            [guard, generation, all = std::move(all), changes = std::move(changes), incremental, error, elapsed] {
                if (!guard || guard->m_generation != generation)
                    return;
                const bool first = guard->m_entries.isEmpty() && !all.isEmpty();
                if (incremental)
                    guard->apply(changes);
                else
                    guard->replaceEntries(all);
                if (first)
                    Q_EMIT guard->firstBatch(elapsed);
                guard->setBusy(false, error);
                if (!guard->m_pendingSelection.isEmpty()) {
                    guard->selectUri(guard->m_pendingSelection);
                    guard->m_pendingSelection.clear();
                }
            },
            Qt::QueuedConnection);
    });
}
bool nook::EntryFilter::accepts(const Entry &entry) const {
    if (entry.directory)
        return true;
    if (foldersOnly)
        return false;
    if (globs.isEmpty() && mimeTypes.isEmpty())
        return true;
    for (const auto &glob : globs)
        if (glob.match(entry.name).hasMatch())
            return true;
    if (mimeTypes.isEmpty())
        return false;
    static const QMimeDatabase database;
    const auto mime = database.mimeTypeForFile(entry.name, QMimeDatabase::MatchExtension);
    for (const auto &type : mimeTypes) {
        if (type.endsWith(QLatin1String("/*")) ? mime.name().startsWith(type.chopped(1)) : mime.inherits(type))
            return true;
    }
    return false;
}
void DirectoryModel::setFilter(const QVariantList &rules, bool foldersOnly) {
    auto filter = std::make_shared<nook::EntryFilter>();
    filter->foldersOnly = foldersOnly;
    for (const auto &value : rules) {
        const auto rule = value.toMap();
        const QString pattern = rule.value("pattern").toString().trimmed();
        if (pattern.isEmpty())
            continue;
        if (rule.value("type").toInt() == 1)
            filter->mimeTypes.append(pattern);
        else
            filter->globs.append(QRegularExpression(
                QRegularExpression::wildcardToRegularExpression(pattern, QRegularExpression::NonPathWildcardConversion),
                QRegularExpression::CaseInsensitiveOption));
    }
    m_filter = std::move(filter);
    refresh();
}
QList<nook::Entry> DirectoryModel::filtered(QList<nook::Entry> entries) const {
    if (!m_filter)
        return entries;
    entries.removeIf([filter = m_filter.get()](const nook::Entry &entry) { return !filter->accepts(entry); });
    return entries;
}
void DirectoryModel::appendEntries(QList<nook::Entry> entries) {
    entries = filtered(std::move(entries));
    if (entries.isEmpty())
        return;
    int first = m_entries.size();
    beginInsertRows({}, first, first + entries.size() - 1);
    m_entries.append(entries);
    endInsertRows();
    m_selectedDirty = true;
    Q_EMIT countChanged();
}
void DirectoryModel::replaceEntries(QList<nook::Entry> entries) {
    beginResetModel();
    m_entries = filtered(std::move(entries));
    m_anchor = -1;
    endResetModel();
    QSet<QString> present;
    for (const auto &e : m_entries)
        present.insert(e.uri);
    m_selected.intersect(present);
    Q_EMIT countChanged();
    notifySelection();
}
bool DirectoryModel::diff(QList<nook::Entry> current, const QList<nook::Entry> &next, QList<Change> *changes) {
    // Small, local edits animate; wholesale changes (a re-sort of a big folder) reset instead.
    constexpr int maxMoves = 64, maxChanges = 512;
    QSet<QString> wanted;
    wanted.reserve(next.size());
    for (const auto &e : next)
        wanted.insert(e.uri);
    for (int i = current.size() - 1; i >= 0; --i) {
        if (wanted.contains(current[i].uri))
            continue;
        int start = i;
        while (start > 0 && !wanted.contains(current[start - 1].uri))
            --start;
        changes->append({Change::Remove, start, i - start + 1, 0, {}});
        current.remove(start, i - start + 1);
        i = start;
    }
    QSet<QString> present;
    present.reserve(current.size());
    for (const auto &e : current)
        present.insert(e.uri);
    auto same = [](const nook::Entry &a, const nook::Entry &b) {
        return a.name == b.name && a.kind == b.kind && a.icon == b.icon && a.size == b.size &&
               a.modified == b.modified && a.directory == b.directory && a.symlink == b.symlink &&
               a.hidden == b.hidden && a.originalUri == b.originalUri;
    };
    int moves = 0;
    for (int i = 0; i < next.size(); ++i) {
        if (changes->size() > maxChanges)
            return false;
        if (i < current.size() && current[i].uri == next[i].uri) {
            if (!same(current[i], next[i])) {
                changes->append({Change::Update, i, 1, 0, {next[i]}});
                current[i] = next[i];
            }
            continue;
        }
        if (!present.contains(next[i].uri)) {
            int end = i;
            while (end + 1 < next.size() && !present.contains(next[end + 1].uri))
                ++end;
            const auto rows = next.mid(i, end - i + 1);
            changes->append({Change::Insert, i, int(rows.size()), 0, rows});
            for (int k = 0; k < rows.size(); ++k)
                current.insert(i + k, rows[k]);
            i = end;
            continue;
        }
        if (++moves > maxMoves)
            return false;
        int from = i + 1;
        while (from < current.size() && current[from].uri != next[i].uri)
            ++from;
        if (from >= current.size())
            return false;
        changes->append({Change::Move, i, 1, from, {}});
        current.move(from, i);
        if (!same(current[i], next[i])) {
            changes->append({Change::Update, i, 1, 0, {next[i]}});
            current[i] = next[i];
        }
    }
    return current.size() == next.size();
}
void DirectoryModel::apply(const QList<Change> &changes) {
    if (changes.isEmpty())
        return;
    const int before = m_entries.size();
    for (const auto &change : changes) {
        switch (change.kind) {
        case Change::Remove:
            beginRemoveRows({}, change.at, change.at + change.count - 1);
            m_entries.remove(change.at, change.count);
            endRemoveRows();
            break;
        case Change::Insert:
            beginInsertRows({}, change.at, change.at + change.count - 1);
            for (int k = 0; k < change.entries.size(); ++k)
                m_entries.insert(change.at + k, change.entries[k]);
            endInsertRows();
            break;
        case Change::Move:
            beginMoveRows({}, change.from, change.from, {}, change.at);
            m_entries.move(change.from, change.at);
            endMoveRows();
            break;
        case Change::Update:
            m_entries[change.at] = change.entries.first();
            Q_EMIT dataChanged(index(change.at), index(change.at));
            break;
        }
    }
    m_anchor = -1; // rows may have shifted under the old anchor
    m_selectedDirty = true;
    const auto previous = m_selected;
    QSet<QString> present;
    for (const auto &e : m_entries)
        present.insert(e.uri);
    m_selected.intersect(present);
    if (m_entries.size() != before)
        Q_EMIT countChanged();
    notifySelection(previous);
}
QVariantMap DirectoryModel::entry(int row) const {
    return row >= 0 && row < m_entries.size() ? nook::entryMap(m_entries[row]) : QVariantMap();
}
void DirectoryModel::select(int row, int modifiers) {
    if (row < 0 || row >= m_entries.size())
        return;
    const auto previous = m_selected;
    if ((modifiers & 2) && m_anchor >= 0 && m_anchor < m_entries.size()) {
        if (!(modifiers & 1))
            m_selected.clear();
        for (int i = qMin(row, m_anchor); i <= qMax(row, m_anchor); ++i)
            m_selected.insert(m_entries[i].uri);
    } else {
        if (!(modifiers & 1))
            m_selected.clear();
        const QString uri = m_entries[row].uri;
        if ((modifiers & 1) && m_selected.contains(uri))
            m_selected.remove(uri);
        else
            m_selected.insert(uri);
        m_anchor = row;
    }
    notifySelection(previous);
}
void DirectoryModel::notifySelection() {
    m_selectedDirty = true;
    if (!m_entries.isEmpty())
        Q_EMIT dataChanged(index(0), index(m_entries.size() - 1), {SelectedRole});
    Q_EMIT selectionChanged();
}
void DirectoryModel::notifySelection(const QSet<QString> &previous) {
    // Signal only rows whose state flipped, in contiguous runs, so a click stays O(changed).
    m_selectedDirty = true;
    const auto flipped = (previous - m_selected) + (m_selected - previous);
    if (!flipped.isEmpty()) {
        int start = -1;
        for (int i = 0; i <= m_entries.size(); ++i) {
            const bool hit = i < m_entries.size() && flipped.contains(m_entries[i].uri);
            if (hit && start < 0)
                start = i;
            else if (!hit && start >= 0) {
                Q_EMIT dataChanged(index(start), index(i - 1), {SelectedRole});
                start = -1;
            }
        }
    }
    Q_EMIT selectionChanged();
}
void DirectoryModel::clearSelection() {
    const auto previous = m_selected;
    m_selected.clear();
    m_anchor = -1;
    notifySelection(previous);
}
void DirectoryModel::selectAll() {
    const auto previous = m_selected;
    for (const auto &e : m_entries)
        m_selected.insert(e.uri);
    notifySelection(previous);
}
void DirectoryModel::selectUri(const QString &uri) {
    if (m_loading) {
        m_pendingSelection = uri;
        return;
    }
    const auto previous = m_selected;
    m_selected.clear();
    for (const auto &e : m_entries)
        if (e.uri == uri)
            m_selected.insert(uri);
    notifySelection(previous);
}
void DirectoryModel::selectRows(const QList<int> &rows, const QStringList &base, bool toggle) {
    const auto previous = m_selected;
    const QSet<QString> start(base.cbegin(), base.cend());
    m_selected = start;
    for (const int row : rows)
        if (row >= 0 && row < m_entries.size()) {
            const auto &uri = m_entries[row].uri;
            if (toggle && start.contains(uri))
                m_selected.remove(uri);
            else
                m_selected.insert(uri);
        }
    if (m_selected != previous)
        notifySelection(previous);
}
void DirectoryModel::sortBy(const QString &key, bool descending) {
    m_sort = key;
    m_descending = descending;
    refresh();
}
