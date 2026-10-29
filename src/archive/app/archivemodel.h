#pragma once
#include <QAbstractListModel>
#include <QCollator>
#include <QSet>
#include <QStringList>
#include <QVariantList>
#include <vector>

// The contents of one package as a tree, shown one folder at a time (or as search results
// from the whole package). Folders the package never lists on their own still appear.
class ArchiveModel : public QAbstractListModel {
    Q_OBJECT
    Q_PROPERTY(int count READ count NOTIFY rowsChanged)
    Q_PROPERTY(QString folder READ folder NOTIFY folderChanged)
    Q_PROPERTY(QVariantList breadcrumbs READ breadcrumbs NOTIFY folderChanged)
    Q_PROPERTY(bool canBack READ canBack NOTIFY folderChanged)
    Q_PROPERTY(bool canForward READ canForward NOTIFY folderChanged)
    Q_PROPERTY(bool canUp READ canUp NOTIFY folderChanged)
    Q_PROPERTY(QString query READ query NOTIFY rowsChanged)
    Q_PROPERTY(QString sortKey READ sortKey NOTIFY sortChanged)
    Q_PROPERTY(bool descending READ descending NOTIFY sortChanged)
    Q_PROPERTY(int currentRow READ currentRow WRITE setCurrentRow NOTIFY currentRowChanged)
    Q_PROPERTY(int selectedCount READ selectedCount NOTIFY selectionChanged)
    Q_PROPERTY(QString selectionText READ selectionText NOTIFY selectionChanged)
    Q_PROPERTY(int fileCount READ fileCount NOTIFY contentsChanged)
    Q_PROPERTY(int folderCount READ folderCount NOTIFY contentsChanged)
    Q_PROPERTY(double totalSize READ totalSize NOTIFY contentsChanged)
    Q_PROPERTY(double packedSize READ packedSize NOTIFY contentsChanged)
    Q_PROPERTY(bool packedKnown READ packedKnown NOTIFY contentsChanged)
  public:
    enum Role {
        NameRole = Qt::UserRole + 1,
        PathRole,
        LocationRole,
        FolderRole,
        IconRole,
        KindRole,
        SizeTextRole,
        PackedTextRole,
        ModifiedTextRole,
        BytesRole,
        EncryptedRole,
        LinkRole,
        UnsafeRole,
        SelectedRole,
        MethodRole,
    };
    explicit ArchiveModel(QObject *parent = nullptr);
    int rowCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    // Replaces the contents with the worker's entries; keeps the folder when it still exists.
    void reset(const QVariantList &entries, const QString &rootTitle);
    void clear();

    int count() const { return int(m_rows.size()); }
    QString folder() const { return pathOf(m_folder); }
    QVariantList breadcrumbs() const;
    bool canBack() const { return !m_back.isEmpty(); }
    bool canForward() const { return !m_forward.isEmpty(); }
    bool canUp() const { return m_folder != 0; }
    QString query() const { return m_query; }
    QString sortKey() const { return m_sortKey; }
    bool descending() const { return m_descending; }
    int currentRow() const { return m_currentRow; }
    void setCurrentRow(int row);
    int selectedCount() const { return int(m_selected.size()); }
    QString selectionText() const;
    int fileCount() const { return m_files; }
    int folderCount() const { return m_folders; }
    double totalSize() const { return double(m_nodes.empty() ? 0 : m_nodes[0].size); }
    double packedSize() const { return double(m_packed); }
    bool packedKnown() const { return m_packedKnown; }

    Q_INVOKABLE void openFolder(const QString &path);
    Q_INVOKABLE void activate(int row); // a folder opens; files are left to the caller
    Q_INVOKABLE void up();
    Q_INVOKABLE void back();
    Q_INVOKABLE void forward();
    Q_INVOKABLE void setQuery(const QString &query);
    Q_INVOKABLE void sortBy(const QString &key, bool descending);
    // Ctrl toggles, Shift extends from the anchor, a plain click selects one row.
    Q_INVOKABLE void click(int row, int modifiers);
    Q_INVOKABLE void selectAll();
    Q_INVOKABLE void clearSelection();
    // A selection band: exactly these rows, plus those of base (what was selected when a Ctrl+drag began).
    Q_INVOKABLE void selectRows(const QList<int> &rows, const QList<int> &base);
    Q_INVOKABLE QList<int> selectedRows() const;
    Q_INVOKABLE QVariantMap entry(int row) const;
    Q_INVOKABLE QStringList selectedPaths() const;
    Q_INVOKABLE bool isFolder(int row) const;
    // Whether the package has this path (also folders only implied by what is inside them).
    Q_INVOKABLE bool contains(const QString &path) const { return nodeAt(path) >= 0; }

  Q_SIGNALS:
    void rowsChanged();
    void folderChanged();
    void sortChanged();
    void currentRowChanged();
    void selectionChanged();
    void contentsChanged();

  private:
    struct Node {
        QString name;
        int parent = -1;
        bool folder = false;
        bool listed = false; // the package has an entry for it (not only for things inside)
        bool encrypted = false;
        bool unsafe = false;
        QString type;
        QString link;
        QString method;
        qint64 size = 0; // a folder: everything inside it
        qint64 packed = -1;
        qint64 mtime = 0;
        int mode = 0;
        std::vector<int> children;
        mutable QString icon;
    };
    QString pathOf(int node) const;
    int nodeAt(const QString &path) const;
    int child(int parent, const QString &name) const;
    void go(int folder, bool remember);
    void rebuild(bool keepSelection = false);
    bool lessThan(int a, int b) const;
    QString kindOf(const Node &node) const;

    std::vector<Node> m_nodes;
    QHash<QString, int> m_byPath;
    std::vector<int> m_rows;
    QSet<int> m_selected;
    int m_anchor = -1;
    int m_currentRow = -1;
    int m_folder = 0;
    QList<int> m_back, m_forward;
    QString m_query;
    QString m_sortKey = "name";
    bool m_descending = false;
    int m_files = 0, m_folders = 0;
    qint64 m_packed = 0;
    bool m_packedKnown = false;
    QCollator m_collator;
};
