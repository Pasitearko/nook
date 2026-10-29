#include "archivemodel.h"
#include "files.h"
#include <QDateTime>
#include <QFileInfo>
#include <algorithm>

ArchiveModel::ArchiveModel(QObject *parent) : QAbstractListModel(parent) {
    m_collator.setNumericMode(true);
    m_collator.setCaseSensitivity(Qt::CaseInsensitive);
    clear();
}

int ArchiveModel::rowCount(const QModelIndex &parent) const { return parent.isValid() ? 0 : int(m_rows.size()); }

QHash<int, QByteArray> ArchiveModel::roleNames() const {
    return {{NameRole, "name"},          {PathRole, "path"},           {LocationRole, "location"},
            {FolderRole, "folder"},      {IconRole, "iconName"},       {KindRole, "kind"},
            {SizeTextRole, "sizeText"},  {PackedTextRole, "packedText"}, {ModifiedTextRole, "modifiedText"},
            {BytesRole, "bytes"},        {EncryptedRole, "encrypted"}, {LinkRole, "link"},
            {UnsafeRole, "unsafe"},      {SelectedRole, "selected"},   {MethodRole, "method"}};
}

QString ArchiveModel::kindOf(const Node &node) const {
    if (node.folder)
        return QStringLiteral("文件夹");
    if (node.type == u"link")
        return QStringLiteral("符号链接");
    const QString suffix = QFileInfo(node.name).suffix().toUpper();
    return suffix.isEmpty() ? QStringLiteral("文件") : suffix;
}

QVariant ArchiveModel::data(const QModelIndex &index, int role) const {
    if (!index.isValid() || index.row() < 0 || index.row() >= int(m_rows.size()))
        return {};
    const int id = m_rows[size_t(index.row())];
    const Node &node = m_nodes[size_t(id)];
    switch (role) {
    case NameRole:
        return node.name;
    case PathRole:
        return pathOf(id);
    case LocationRole: {
        const QString parent = pathOf(node.parent);
        return parent.isEmpty() ? m_nodes[0].name : parent;
    }
    case FolderRole:
        return node.folder;
    case IconRole:
        if (node.icon.isEmpty()) {
            static const QStringList packages{"zip", "7z", "rar", "tar", "gz", "tgz", "bz2", "xz", "zst", "lz4",
                                              "lzma", "cab", "iso", "jar", "apk", "deb", "rpm", "wim", "cpio", "001"};
            const QString suffix = QFileInfo(node.name).suffix().toLower();
            node.icon = node.unsafe                                  ? QStringLiteral("file")
                        : !node.folder && packages.contains(suffix) ? QStringLiteral("archive")
                                                                    : nook::iconFor(node.name, node.folder);
        }
        return node.icon;
    case KindRole:
        return kindOf(node);
    case SizeTextRole:
        return node.size < 0 ? QStringLiteral("—") : nook::formatSize(node.size);
    case PackedTextRole:
        return node.folder || node.packed < 0 ? QStringLiteral("—") : nook::formatSize(node.packed);
    case ModifiedTextRole:
        return node.mtime > 0 ? QDateTime::fromSecsSinceEpoch(node.mtime).toString("yyyy-MM-dd HH:mm") : QString();
    case BytesRole:
        return double(node.size);
    case EncryptedRole:
        return node.encrypted;
    case LinkRole:
        return node.link;
    case UnsafeRole:
        return node.unsafe;
    case SelectedRole:
        return m_selected.contains(id);
    case MethodRole:
        return node.method;
    }
    return {};
}

QString ArchiveModel::pathOf(int id) const {
    QStringList parts;
    for (int at = id; at > 0; at = m_nodes[size_t(at)].parent)
        parts.prepend(m_nodes[size_t(at)].name);
    return parts.join(u'/');
}

int ArchiveModel::nodeAt(const QString &path) const { return path.isEmpty() ? 0 : m_byPath.value(path, -1); }

int ArchiveModel::child(int parent, const QString &name) const {
    const QString parentPath = pathOf(parent);
    return m_byPath.value(parentPath.isEmpty() ? name : parentPath + u'/' + name, -1);
}

void ArchiveModel::clear() {
    beginResetModel();
    m_nodes.assign(1, Node{});
    m_nodes[0].folder = true;
    m_nodes[0].listed = true;
    m_byPath.clear();
    m_rows.clear();
    m_selected.clear();
    m_back.clear();
    m_forward.clear();
    m_folder = 0;
    m_currentRow = -1;
    m_anchor = -1;
    m_query.clear();
    m_files = m_folders = 0;
    m_packed = 0;
    m_packedKnown = false;
    endResetModel();
    Q_EMIT rowsChanged();
    Q_EMIT folderChanged();
    Q_EMIT selectionChanged();
    Q_EMIT contentsChanged();
    Q_EMIT currentRowChanged();
}

void ArchiveModel::reset(const QVariantList &entries, const QString &rootTitle) {
    const QString previous = folder();
    beginResetModel();
    m_nodes.assign(1, Node{});
    Node &root = m_nodes[0];
    root.name = rootTitle;
    root.folder = true;
    root.listed = true;
    m_byPath.clear();
    m_byPath.reserve(entries.size());
    const auto makeNode = [this](int parent, const QString &name, bool folder, const QString &path) {
        Node node;
        node.name = name;
        node.parent = parent;
        node.folder = folder;
        node.type = folder ? QStringLiteral("dir") : QStringLiteral("file");
        m_nodes.push_back(std::move(node));
        const int id = int(m_nodes.size()) - 1;
        m_nodes[size_t(parent)].children.push_back(id);
        m_byPath.insert(path, id);
        return id;
    };
    for (const auto &value : entries) {
        const QVariantMap entry = value.toMap();
        const QString path = entry.value("path").toString();
        const bool unsafe = entry.value("unsafe").toBool();
        const QString type = entry.value("type").toString();
        const bool folder = type == u"dir";
        const QStringList parts = unsafe ? QStringList{path} : path.split(u'/', Qt::SkipEmptyParts);
        if (parts.isEmpty())
            continue;
        int parent = 0;
        QString prefix;
        for (qsizetype i = 0; i + 1 < parts.size(); ++i) {
            prefix = prefix.isEmpty() ? parts[i] : prefix + u'/' + parts[i];
            int id = m_byPath.value(prefix, -1);
            if (id < 0)
                id = makeNode(parent, parts[i], true, prefix);
            else
                m_nodes[size_t(id)].folder = true;
            parent = id;
        }
        const QString full = prefix.isEmpty() ? parts.last() : prefix + u'/' + parts.last();
        int id = m_byPath.value(full, -1);
        if (id < 0)
            id = makeNode(parent, parts.last(), folder, full);
        Node &node = m_nodes[size_t(id)];
        // The same path twice: the later entry is the one extraction keeps.
        node.listed = true;
        node.folder = node.folder || folder;
        node.type = node.folder ? QStringLiteral("dir") : type;
        node.unsafe = unsafe;
        node.mtime = qint64(entry.value("mtime").toDouble());
        node.mode = entry.value("mode").toInt();
        node.encrypted = entry.value("encrypted").toBool();
        node.link = entry.value("link").toString();
        node.method = entry.value("method").toString();
        if (!node.folder) {
            node.size = qint64(entry.value("size", -1).toDouble());
            node.packed = entry.contains("packed") ? qint64(entry.value("packed").toDouble()) : -1;
        }
    }
    m_files = m_folders = 0;
    m_packed = 0;
    m_packedKnown = false;
    // Parents always come before their children, so one pass from the end adds folders up.
    for (size_t i = m_nodes.size() - 1; i > 0; --i) {
        const Node &node = m_nodes[i];
        if (node.folder) {
            ++m_folders;
        } else {
            ++m_files;
            if (node.packed >= 0) {
                m_packed += node.packed;
                m_packedKnown = true;
            }
        }
        m_nodes[size_t(node.parent)].size += std::max<qint64>(node.size, 0);
    }
    m_back.clear();
    m_forward.clear();
    m_selected.clear();
    m_anchor = -1;
    m_currentRow = -1;
    m_query.clear();
    const int kept = nodeAt(previous);
    m_folder = kept >= 0 && m_nodes[size_t(kept)].folder ? kept : 0;
    rebuild();
    endResetModel();
    Q_EMIT contentsChanged();
    Q_EMIT rowsChanged();
    Q_EMIT folderChanged();
    Q_EMIT selectionChanged();
    Q_EMIT currentRowChanged();
}

bool ArchiveModel::lessThan(int a, int b) const {
    const Node &x = m_nodes[size_t(a)];
    const Node &y = m_nodes[size_t(b)];
    if (x.folder != y.folder)
        return x.folder;
    int order = 0;
    if (m_sortKey == u"size")
        order = x.size < y.size ? -1 : x.size > y.size ? 1 : 0;
    else if (m_sortKey == u"packed")
        order = x.packed < y.packed ? -1 : x.packed > y.packed ? 1 : 0;
    else if (m_sortKey == u"date")
        order = x.mtime < y.mtime ? -1 : x.mtime > y.mtime ? 1 : 0;
    else if (m_sortKey == u"type")
        order = m_collator.compare(kindOf(x), kindOf(y));
    if (order == 0)
        order = m_collator.compare(x.name, y.name);
    return m_descending ? order > 0 : order < 0;
}

void ArchiveModel::rebuild(bool keepSelection) {
    m_rows.clear();
    if (m_query.isEmpty()) {
        m_rows = m_nodes[size_t(m_folder)].children;
    } else {
        for (size_t i = 1; i < m_nodes.size(); ++i) {
            if (m_nodes[i].name.contains(m_query, Qt::CaseInsensitive))
                m_rows.push_back(int(i));
        }
    }
    std::stable_sort(m_rows.begin(), m_rows.end(), [this](int a, int b) { return lessThan(a, b); });
    if (!keepSelection) {
        m_selected.clear();
        m_anchor = -1;
        m_currentRow = -1;
    }
}

void ArchiveModel::go(int target, bool remember) {
    if (target < 0 || !m_nodes[size_t(target)].folder)
        return;
    if (remember && (target != m_folder || !m_query.isEmpty())) {
        m_back.append(m_folder);
        m_forward.clear();
    }
    m_folder = target;
    m_query.clear();
    beginResetModel();
    rebuild();
    endResetModel();
    Q_EMIT rowsChanged();
    Q_EMIT folderChanged();
    Q_EMIT selectionChanged();
    Q_EMIT currentRowChanged();
}

void ArchiveModel::openFolder(const QString &path) { go(nodeAt(path), true); }

void ArchiveModel::activate(int row) {
    if (row >= 0 && row < int(m_rows.size()))
        go(m_rows[size_t(row)], true);
}

void ArchiveModel::up() {
    if (m_folder == 0)
        return;
    const int from = m_folder;
    go(m_nodes[size_t(m_folder)].parent, true);
    // Land on the folder just left, as file managers do.
    const auto it = std::find(m_rows.begin(), m_rows.end(), from);
    if (it != m_rows.end())
        click(int(it - m_rows.begin()), 0);
}

void ArchiveModel::back() {
    if (m_back.isEmpty())
        return;
    m_forward.append(m_folder);
    go(m_back.takeLast(), false);
}

void ArchiveModel::forward() {
    if (m_forward.isEmpty())
        return;
    m_back.append(m_folder);
    go(m_forward.takeLast(), false);
}

void ArchiveModel::setQuery(const QString &query) {
    const QString trimmed = query.trimmed();
    if (trimmed == m_query)
        return;
    m_query = trimmed;
    beginResetModel();
    rebuild();
    endResetModel();
    Q_EMIT rowsChanged();
    Q_EMIT selectionChanged();
    Q_EMIT currentRowChanged();
}

void ArchiveModel::sortBy(const QString &key, bool descending) {
    m_sortKey = key;
    m_descending = descending;
    const int current = m_currentRow >= 0 && m_currentRow < int(m_rows.size()) ? m_rows[size_t(m_currentRow)] : -1;
    beginResetModel();
    rebuild(true);
    endResetModel();
    const auto it = std::find(m_rows.begin(), m_rows.end(), current);
    m_currentRow = it == m_rows.end() ? -1 : int(it - m_rows.begin());
    Q_EMIT sortChanged();
    Q_EMIT rowsChanged();
    Q_EMIT currentRowChanged();
}

void ArchiveModel::setCurrentRow(int row) {
    row = row < 0 || row >= int(m_rows.size()) ? -1 : row;
    if (row == m_currentRow)
        return;
    m_currentRow = row;
    Q_EMIT currentRowChanged();
}

void ArchiveModel::click(int row, int modifiers) {
    if (row < 0 || row >= int(m_rows.size()))
        return;
    const int id = m_rows[size_t(row)];
    if (modifiers & Qt::ShiftModifier && m_anchor >= 0 && m_anchor < int(m_rows.size())) {
        if (!(modifiers & Qt::ControlModifier))
            m_selected.clear();
        for (int i = std::min(m_anchor, row); i <= std::max(m_anchor, row); ++i)
            m_selected.insert(m_rows[size_t(i)]);
    } else if (modifiers & Qt::ControlModifier) {
        if (!m_selected.remove(id))
            m_selected.insert(id);
        m_anchor = row;
    } else {
        m_selected = {id};
        m_anchor = row;
    }
    setCurrentRow(row);
    if (!m_rows.empty())
        Q_EMIT dataChanged(index(0), index(int(m_rows.size()) - 1), {SelectedRole});
    Q_EMIT selectionChanged();
}

void ArchiveModel::selectAll() {
    m_selected = QSet<int>(m_rows.begin(), m_rows.end());
    if (!m_rows.empty())
        Q_EMIT dataChanged(index(0), index(int(m_rows.size()) - 1), {SelectedRole});
    Q_EMIT selectionChanged();
}

void ArchiveModel::clearSelection() {
    if (m_selected.isEmpty())
        return;
    m_selected.clear();
    m_anchor = -1;
    if (!m_rows.empty())
        Q_EMIT dataChanged(index(0), index(int(m_rows.size()) - 1), {SelectedRole});
    Q_EMIT selectionChanged();
}

QString ArchiveModel::selectionText() const {
    if (m_selected.isEmpty())
        return {};
    qint64 bytes = 0;
    for (const int id : m_selected)
        bytes += std::max<qint64>(m_nodes[size_t(id)].size, 0);
    return QStringLiteral("已选 %1 项 · %2").arg(m_selected.size()).arg(nook::formatSize(bytes));
}

QVariantList ArchiveModel::breadcrumbs() const {
    QVariantList crumbs;
    for (int at = m_folder; at >= 0; at = m_nodes[size_t(at)].parent)
        crumbs.prepend(QVariantMap{{"title", m_nodes[size_t(at)].name}, {"path", pathOf(at)}});
    return crumbs;
}

QVariantMap ArchiveModel::entry(int row) const {
    if (row < 0 || row >= int(m_rows.size()))
        return {};
    const QModelIndex at = index(row);
    QVariantMap map;
    const auto names = roleNames();
    for (auto it = names.begin(); it != names.end(); ++it)
        map.insert(QString::fromLatin1(it.value()), data(at, it.key()));
    return map;
}

bool ArchiveModel::isFolder(int row) const {
    return row >= 0 && row < int(m_rows.size()) && m_nodes[size_t(m_rows[size_t(row)])].folder;
}

QStringList ArchiveModel::selectedPaths() const {
    QStringList paths;
    for (const int id : m_selected)
        paths << pathOf(id);
    paths.sort();
    return paths;
}
