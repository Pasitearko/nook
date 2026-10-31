#include "directorymodel.h"
#include "storage.h"
#include "tags.h"
#include <QCollator>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QElapsedTimer>
#include <QFile>
#include <QLocale>
#include <QMimeDatabase>
#include <QPointer>
#include <QRegularExpression>
#include <QSettings>
#include <QtConcurrent>
#include <cerrno>
#include <dirent.h>
#include <fcntl.h>
#include <limits>
#include <set>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace {
QString sortSettingsPath() {
    return nook::storageDirectory(nook::Storage::Config) + "/folder-sorts.ini";
}
QString sortSettingsKey(const QString &uri) {
    return QString::fromLatin1(QCryptographicHash::hash(uri.toUtf8(), QCryptographicHash::Sha256).toHex());
}
bool validSort(const QString &key) {
    return key == "name" || key == "date" || key == "type" || key == "size" || key == "created";
}
void sortRows(QList<nook::Entry> &rows, const QString &sort, bool descending,
              const QHash<QString, qint64> &sizes) {
    QCollator collator;
    collator.setNumericMode(true);
    collator.setCaseSensitivity(Qt::CaseInsensitive);
    std::stable_sort(rows.begin(), rows.end(), [&](const auto &a, const auto &b) {
        if (a.directory != b.directory)
            return a.directory;
        const qint64 aSize = a.directory ? qMax<qint64>(-1, sizes.value(a.uri, -1)) : a.size;
        const qint64 bSize = b.directory ? qMax<qint64>(-1, sizes.value(b.uri, -1)) : b.size;
        // Unknown totals follow known totals in either direction, with names as a stable tie-break.
        if (sort == "size" && (aSize < 0) != (bSize < 0))
            return aSize >= 0;
        if (sort == "created" && a.createdKnown != b.createdKnown)
            return a.createdKnown;
        int cmp = sort == "size"   ? (aSize > bSize   ? 1
                                      : aSize < bSize ? -1
                                                      : 0)
                  : sort == "date" ? (a.modified > b.modified   ? 1
                                      : a.modified < b.modified ? -1
                                                                : 0)
                  : sort == "created" && a.createdKnown ? (a.created > b.created ? 1 : a.created < b.created ? -1 : 0)
                  : sort == "type" ? QString::compare(a.kind, b.kind)
                                   : 0;
        if (cmp == 0)
            cmp = collator.compare(a.name, b.name);
        return descending ? cmp > 0 : cmp < 0;
    });
}
// Use directory-relative descriptors with O_NOFOLLOW: even a symlink swapped in while scanning
// cannot escape into its target. Track inode identities too (bind mounts can form cycles).
qint64 folderBytes(const QString &path, const nook::Cancel &cancel) {
    struct CloseDirectory {
        void operator()(DIR *directory) const {
            closedir(directory);
        }
    };
    using Directory = std::unique_ptr<DIR, CloseDirectory>;
    std::vector<Directory> stack;
    std::set<std::pair<dev_t, ino_t>> visited;
    auto enter = [&](int fd) {
        if (fd < 0)
            return false;
        if (stack.size() >= 128) { close(fd); return false; }
        struct stat st;
        if (fstat(fd, &st) != 0) {
            close(fd);
            return false;
        }
        if (!visited.emplace(st.st_dev, st.st_ino).second) {
            close(fd);
            return true;
        }
        DIR *dir = fdopendir(fd);
        if (!dir) {
            close(fd);
            return false;
        }
        stack.emplace_back(dir);
        return true;
    };
    if (!enter(open(QFile::encodeName(path).constData(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW)))
        return -1;
    qint64 total = 0, entries = 0;
    QElapsedTimer clock;
    clock.start();
    while (!stack.empty()) {
        if (cancel->load() || ++entries > 250000 || clock.elapsed() > 15000)
            return -1;
        const int fd = dirfd(stack.back().get());
        errno = 0;
        const auto *item = readdir(stack.back().get());
        if (!item) {
            if (errno)
                return -1;
            stack.pop_back();
            continue;
        }
        const QByteArray name(item->d_name);
        if (name == "." || name == "..")
            continue;
        struct stat st;
        if (fstatat(fd, name.constData(), &st, AT_SYMLINK_NOFOLLOW) != 0)
            return -1; // Incomplete scans are unknown, never a misleading partial/zero size.
        if (S_ISDIR(st.st_mode)) {
            if (!enter(openat(fd, name.constData(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW)))
                return -1;
        } else if (S_ISREG(st.st_mode)) {
            if (st.st_size < 0 || st.st_size > std::numeric_limits<qint64>::max() - total)
                return -1;
            total += st.st_size;
        }
    }
    return total;
}
} // namespace

DirectoryModel::DirectoryModel(QObject *parent) : QAbstractListModel(parent) {
    m_dateTimer.setInterval(60000);
    connect(&m_dateTimer, &QTimer::timeout, this, [this] {
        if (m_smartDates && !m_entries.isEmpty())
            Q_EMIT dataChanged(index(0), index(m_entries.size() - 1), {ModifiedRole, CreatedRole});
    });
    m_dateTimer.start();
    m_sortTimer.setSingleShot(true);
    m_sortTimer.setInterval(80);
    connect(&m_sortTimer, &QTimer::timeout, this, &DirectoryModel::sortEntries);
    m_refreshTimer.setSingleShot(true);
    m_refreshTimer.setInterval(180);
    connect(&m_refreshTimer, &QTimer::timeout, this, &DirectoryModel::refresh);
    connect(&m_watcher, &QFileSystemWatcher::directoryChanged, this, [this] { m_refreshTimer.start(); });
    connect(TagStore::instance(), &TagStore::changed, this, [this] {
        if (!m_entries.isEmpty())
            Q_EMIT dataChanged(index(0), index(m_entries.size() - 1), {TagsRole});
        // A tag view lists what carries the tag; a filtered folder shows what does.
        if (tagView() || !m_tagFilter.isEmpty()) {
            Q_EMIT locationChanged(); // a renamed tag renames its view
            m_refreshTimer.start();
        }
    });
}
QString DirectoryModel::title() const {
    return tagView() ? TagStore::instance()->nameOf(m_uri.mid(4)) : nook::displayName(m_uri);
}
void DirectoryModel::setTagFilter(const QString &color) {
    if (color == m_tagFilter)
        return;
    m_tagFilter = color;
    Q_EMIT tagFilterChanged();
    Q_EMIT rearranging();
    refresh();
}
DirectoryModel::~DirectoryModel() {
    cancelFolderSizes();
    if (m_cancel)
        *m_cancel = true;
}
int DirectoryModel::rowCount(const QModelIndex &parent) const {
    return parent.isValid() ? 0 : m_entries.size();
}
QHash<int, QByteArray> DirectoryModel::roleNames() const {
    return {{NameRole, "fileName"},      {UriRole, "fileUri"},   {FolderRole, "folder"},
            {LinkRole, "isLink"},        {SizeRole, "sizeText"}, {ModifiedRole, "modifiedText"},
            {KindRole, "kind"},          {IconRole, "iconName"}, {SelectedRole, "selected"},
            {FullPathRole, "fullPath"},  {BytesRole, "bytes"},   {OriginalRole, "originalUri"},
            {HiddenRole, "hiddenItem"},  {TagsRole, "tags"},     {ModifiedEpochRole, "modified"},
            {SizeKnownRole, "sizeKnown"}, {CreatedRole, "createdText"},
            {CreatedEpochRole, "created"}, {CreatedKnownRole, "createdKnown"}};
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
    case SizeRole: {
        const qint64 size = e.directory ? m_folderSizes.value(e.uri, -1) : e.size;
        return size == -2 ? QStringLiteral("计算中…")
               : size < 0 ? QStringLiteral("—")
                          : nook::formatSize(size);
    }
    case SizeKnownRole:
        return !e.directory || m_folderSizes.value(e.uri, -1) >= 0;
    case ModifiedRole:
        return modifiedText(e.modified);
    case ModifiedEpochRole:
        return e.modified;
    case CreatedRole:
        return e.createdKnown ? modifiedText(e.created) : QStringLiteral("—");
    case CreatedEpochRole:
        return e.created;
    case CreatedKnownRole:
        return e.createdKnown;
    case KindRole:
        return e.kind;
    case IconRole:
        return e.icon;
    case SelectedRole:
        return m_selected.contains(e.uri);
    case FullPathRole:
        return nook::localPath(e.uri);
    case BytesRole:
        return e.directory ? qMax<qint64>(-1, m_folderSizes.value(e.uri, -1)) : e.size;
    case OriginalRole:
        return e.originalUri;
    case HiddenRole:
        return e.hidden;
    case TagsRole:
        return TagStore::instance()->tagsOf(e.uri);
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
    if (tagView())
        return {QVariantMap{{"title", title()}, {"uri", m_uri}}};
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
    Q_EMIT rearranging();
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
    restoreSort();
    m_selected.clear();
    m_anchor = -1;
    clearTagFilter();
    Q_EMIT locationChanged();
    notifySelection();
    load();
}
void DirectoryModel::refresh() {
    if (!m_uri.isEmpty())
        load(true);
}
void DirectoryModel::clearTagFilter() {
    if (m_tagFilter.isEmpty())
        return;
    m_tagFilter.clear();
    Q_EMIT tagFilterChanged();
}
void DirectoryModel::back() {
    if (canBack()) {
        clearTagFilter();
        m_uri = m_history[--m_historyAt];
        restoreSort();
        clearSelection();
        Q_EMIT locationChanged();
        load();
    }
}
void DirectoryModel::forward() {
    if (canForward()) {
        clearTagFilter();
        m_uri = m_history[++m_historyAt];
        restoreSort();
        clearSelection();
        Q_EMIT locationChanged();
        load();
    }
}
void DirectoryModel::up() {
    if (!m_uri.startsWith("trash:") && !tagView())
        navigate(nook::parentUri(m_uri));
}
void DirectoryModel::setBusy(bool value, QString error) {
    m_loading = value;
    m_error = std::move(error);
    Q_EMIT statusChanged();
}
void DirectoryModel::load(bool keep) {
    ++m_orderGeneration;
    m_sortTimer.stop();
    cancelFolderSizes();
    if (m_cancel)
        *m_cancel = true;
    m_cancel = std::make_shared<std::atomic_bool>(false);
    auto cancel = m_cancel;
    const auto generation = ++m_generation;
    const QString uri = m_uri, sort = m_sort;
    const bool hidden = m_hidden, descending = m_descending;
    auto filter = m_filter;
    if (!m_tagFilter.isEmpty()) {
        auto tagged = std::make_shared<nook::EntryFilter>(m_filter ? *m_filter : nook::EntryFilter{});
        const auto uris = TagStore::instance()->urisWith(m_tagFilter);
        tagged->tagged = std::make_shared<const QSet<QString>>(uris.begin(), uris.end());
        filter = std::move(tagged);
    }
    const QStringList tagged = tagView() ? TagStore::instance()->urisWith(uri.mid(4)) : QStringList();
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
    (void)QtConcurrent::run([guard, uri, sort, hidden, descending, cancel, generation, keep, previous, filter,
                             tagged] {
        QElapsedTimer timer;
        timer.start();
        QList<nook::Entry> all, pending;
        QString error;
        auto list = [&](const std::function<void(QList<nook::Entry>)> &batch) {
            if (!nook::tagUri(uri)) {
                nook::enumerate(uri, hidden, cancel, &error, batch);
                return;
            }
            // Items gone from their place (or on a disk not mounted now) are left out.
            QList<nook::Entry> found;
            for (const auto &item : tagged) {
                if (cancel->load())
                    return;
                nook::Entry entry;
                if (nook::entryAt(item, &entry))
                    found << entry;
            }
            batch(std::move(found));
        };
        list([&](QList<nook::Entry> batch) {
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
        sortRows(all, sort, descending, {});
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
                if (!guard->m_entries.isEmpty())
                    Q_EMIT guard->dataChanged(guard->index(0), guard->index(guard->m_entries.size() - 1),
                                             {SizeRole, BytesRole, SizeKnownRole});
                if (!guard->m_pendingSelection.isEmpty()) {
                    guard->selectUri(guard->m_pendingSelection);
                    guard->m_pendingSelection.clear();
                }
            },
            Qt::QueuedConnection);
    });
}
bool nook::EntryFilter::accepts(const Entry &entry) const {
    if (tagged && !tagged->contains(entry.uri))
        return false;
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
    Q_EMIT rearranging();
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
    ++m_orderGeneration;
    int first = m_entries.size();
    beginInsertRows({}, first, first + entries.size() - 1);
    m_entries.append(entries);
    endInsertRows();
    m_selectedDirty = true;
    Q_EMIT countChanged();
}
void DirectoryModel::replaceEntries(QList<nook::Entry> entries) {
    ++m_orderGeneration;
    cancelFolderSizes();
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
               a.modified == b.modified && a.created == b.created && a.createdKnown == b.createdKnown &&
               a.directory == b.directory && a.symlink == b.symlink &&
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
    ++m_orderGeneration;
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
    if (row < 0 || row >= m_entries.size())
        return {};
    auto result = nook::entryMap(m_entries[row]);
    result.insert("sizeText", data(index(row), SizeRole));
    result.insert("bytes", data(index(row), BytesRole));
    result.insert("sizeKnown", data(index(row), SizeKnownRole));
    result.insert("modifiedText", data(index(row), ModifiedRole));
    result.insert("createdText", data(index(row), CreatedRole));
    return result;
}
QString DirectoryModel::columnText(int row, int column) const {
    static const int roles[] = {NameRole, ModifiedRole, KindRole, SizeRole, CreatedRole, FullPathRole};
    return column >= 0 && column < 6 ? data(index(row), roles[column]).toString() : QString();
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
    if (!validSort(key))
        return;
    m_sort = key;
    m_descending = descending;
    if (!m_uri.isEmpty()) {
        QSettings store(sortSettingsPath(), QSettings::IniFormat);
        store.setValue(sortSettingsKey(m_uri), QVariantMap{{"key", key}, {"descending", descending}});
    }
    Q_EMIT sortChanged();
    if (m_loading)
        refresh();
    else
        sortEntries();
}
void DirectoryModel::sortEntries() {
    if (m_loading || m_entries.isEmpty())
        return;
    const auto generation = ++m_orderGeneration;
    const auto previous = m_entries;
    const auto sizes = m_folderSizes;
    const auto sort = m_sort;
    const bool descending = m_descending;
    QPointer<DirectoryModel> guard(this);
    (void)QtConcurrent::run([guard, generation, previous, sizes, sort, descending] {
        auto sorted = previous;
        sortRows(sorted, sort, descending, sizes);
        QList<Change> changes;
        const bool incremental = diff(previous, sorted, &changes);
        QMetaObject::invokeMethod(
            qApp,
            [guard, generation, sorted = std::move(sorted), changes = std::move(changes), incremental] {
                if (!guard || guard->m_orderGeneration != generation)
                    return;
                if (incremental && changes.isEmpty())
                    return;
                Q_EMIT guard->rearranging();
                if (incremental) {
                    guard->apply(changes);
                } else {
                    guard->beginResetModel();
                    guard->m_entries = sorted;
                    guard->m_anchor = -1;
                    guard->m_selectedDirty = true;
                    guard->endResetModel();
                    Q_EMIT guard->selectionChanged();
                }
            },
            Qt::QueuedConnection);
    });
}
void DirectoryModel::restoreSort() {
    QSettings store(sortSettingsPath(), QSettings::IniFormat);
    const auto saved = store.value(sortSettingsKey(m_uri)).toMap();
    const QString key = saved.value("key").toString();
    m_sort = validSort(key) ? key : QStringLiteral("name");
    m_descending = validSort(key) && saved.value("descending", false).toBool();
    Q_EMIT sortChanged();
}
QString DirectoryModel::modifiedText(qint64 seconds) const {
    const auto time = QDateTime::fromSecsSinceEpoch(seconds).toLocalTime();
    if (!m_smartDates)
        return time.toString("yyyy-MM-dd HH:mm");
    const auto today = QDate::currentDate();
    const auto days = time.date().daysTo(today);
    if (days == 0)
        return QStringLiteral("今天 %1").arg(time.toString("HH:mm"));
    if (days == 1)
        return QStringLiteral("昨天 %1").arg(time.toString("HH:mm"));
    if (days > 1 && days < 7)
        return QLocale().toString(time, "dddd HH:mm");
    return time.toString(time.date().year() == today.year() ? "MM-dd HH:mm" : "yyyy-MM-dd HH:mm");
}
void DirectoryModel::setSmartDates(bool value) {
    if (m_smartDates == value)
        return;
    m_smartDates = value;
    Q_EMIT smartDatesChanged();
    if (!m_entries.isEmpty())
        Q_EMIT dataChanged(index(0), index(m_entries.size() - 1), {ModifiedRole, CreatedRole});
}
void DirectoryModel::cancelFolderSizes() {
    if (m_sizeCancel)
        *m_sizeCancel = true;
    ++m_sizeGeneration;
    m_sizeRunning = false;
    m_sizeUri.clear();
    m_sizeUsers.clear();
    m_sizeQueue.clear();
    m_folderSizes.clear();
}
void DirectoryModel::requestFolderSize(const QString &uri) {
    if (m_loading)
        return;
    if (m_sizeUsers.contains(uri)) {
        ++m_sizeUsers[uri];
        return;
    }
    for (int row = 0; row < m_entries.size(); ++row) {
        const auto &e = m_entries[row];
        if (e.uri != uri)
            continue;
        if (!e.directory || e.symlink || nook::localPath(uri).isEmpty())
            return;
        m_sizeUsers.insert(uri, 1);
        if (m_folderSizes.contains(uri))
            return;
        m_folderSizes.insert(uri, -2);
        m_sizeQueue.append(uri);
        Q_EMIT dataChanged(index(row), index(row), {SizeRole});
        startNextFolderSize();
        return;
    }
}
void DirectoryModel::releaseFolderSize(const QString &uri) {
    auto user = m_sizeUsers.find(uri);
    if (user == m_sizeUsers.end() || --user.value() > 0)
        return;
    m_sizeUsers.erase(user);
    if (m_folderSizes.value(uri, -1) != -2)
        return;
    m_sizeQueue.removeAll(uri);
    m_folderSizes.remove(uri);
    if (m_sizeRunning && m_sizeUri == uri) {
        *m_sizeCancel = true;
        ++m_sizeGeneration;
        m_sizeRunning = false;
        m_sizeUri.clear();
        startNextFolderSize();
    }
    for (int row = 0; row < m_entries.size(); ++row)
        if (m_entries[row].uri == uri) {
            Q_EMIT dataChanged(index(row), index(row), {SizeRole});
            break;
        }
}
void DirectoryModel::startNextFolderSize() {
    if (m_sizeRunning || m_sizeQueue.isEmpty())
        return;
    m_sizeRunning = true;
    const auto uri = m_sizeQueue.takeFirst();
    m_sizeUri = uri;
    const auto path = nook::localPath(uri);
    const auto generation = m_sizeGeneration;
    m_sizeCancel = std::make_shared<std::atomic_bool>(false);
    const auto cancel = m_sizeCancel;
    QPointer<DirectoryModel> guard(this);
    (void)QtConcurrent::run([guard, uri, path, generation, cancel] {
        const qint64 bytes = folderBytes(path, cancel);
        if (cancel->load())
            return;
        QMetaObject::invokeMethod(
            qApp,
            [guard, uri, generation, bytes] {
                if (!guard || guard->m_sizeGeneration != generation)
                    return;
                guard->m_sizeRunning = false;
                guard->m_sizeUri.clear();
                guard->m_folderSizes.insert(uri, bytes);
                if (guard->m_sort == "size") {
                    ++guard->m_orderGeneration;
                    guard->m_sortTimer.start();
                }
                for (int row = 0; row < guard->m_entries.size(); ++row)
                    if (guard->m_entries[row].uri == uri) {
                        Q_EMIT guard->dataChanged(guard->index(row), guard->index(row),
                                                  {SizeRole, BytesRole, SizeKnownRole});
                        break;
                    }
                guard->startNextFolderSize();
            },
            Qt::QueuedConnection);
    });
}
