#pragma once
#include "files.h"
#include <QAbstractListModel>
#include <QFileSystemWatcher>
#include <QRegularExpression>
#include <QSet>
#include <QTimer>
#include <memory>

namespace nook {
// Which files a file chooser offers: glob patterns and MIME types (any may match); folders
// always pass so they can be entered, and foldersOnly hides every file.
struct EntryFilter {
    QList<QRegularExpression> globs;
    QStringList mimeTypes;
    bool foldersOnly = false;
    // A tag filter: only these items (folders included).
    std::shared_ptr<const QSet<QString>> tagged;
    bool accepts(const Entry &entry) const;
};
} // namespace nook

class DirectoryModel : public QAbstractListModel {
    Q_OBJECT
    Q_PROPERTY(QString uri READ uri NOTIFY locationChanged)
    Q_PROPERTY(QString displayPath READ displayPath NOTIFY locationChanged)
    Q_PROPERTY(QString title READ title NOTIFY locationChanged)
    Q_PROPERTY(QVariantList breadcrumbs READ breadcrumbs NOTIFY locationChanged)
    Q_PROPERTY(bool loading READ loading NOTIFY statusChanged)
    Q_PROPERTY(QString error READ error NOTIFY statusChanged)
    Q_PROPERTY(int count READ count NOTIFY countChanged)
    Q_PROPERTY(bool showHidden READ showHidden WRITE setShowHidden NOTIFY showHiddenChanged)
    Q_PROPERTY(int selectedCount READ selectedCount NOTIFY selectionChanged)
    Q_PROPERTY(QStringList selectedUris READ selectedUris NOTIFY selectionChanged)
    Q_PROPERTY(bool canBack READ canBack NOTIFY locationChanged)
    Q_PROPERTY(bool canForward READ canForward NOTIFY locationChanged)
    // Shows only the items carrying this colour tag ("" shows all); cleared on leaving the folder.
    Q_PROPERTY(QString tagFilter READ tagFilter WRITE setTagFilter NOTIFY tagFilterChanged)
    Q_PROPERTY(bool tagView READ tagView NOTIFY locationChanged)
  public:
    enum Role {
        NameRole = Qt::UserRole + 1,
        UriRole,
        FolderRole,
        LinkRole,
        SizeRole,
        ModifiedRole,
        KindRole,
        IconRole,
        SelectedRole,
        FullPathRole,
        BytesRole,
        OriginalRole,
        HiddenRole,
        TagsRole
    };
    explicit DirectoryModel(QObject *parent = nullptr);
    ~DirectoryModel() override;
    int rowCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;
    QString uri() const {
        return m_uri;
    }
    QString title() const;
    bool tagView() const {
        return nook::tagUri(m_uri);
    }
    QString tagFilter() const {
        return m_tagFilter;
    }
    void setTagFilter(const QString &color);
    QVariantList breadcrumbs() const;
    QString displayPath() const {
        return m_uri.startsWith("trash:") ? QStringLiteral("回收站") : tagView() ? QStringLiteral("标签：") + title() : nook::localPath(m_uri);
    }
    bool loading() const {
        return m_loading;
    }
    QString error() const {
        return m_error;
    }
    int count() const {
        return m_entries.size();
    }
    bool showHidden() const {
        return m_hidden;
    }
    int selectedCount() const {
        return m_selected.size();
    }
    QStringList selectedUris() const;
    bool canBack() const {
        return m_historyAt > 0;
    }
    bool canForward() const {
        return m_historyAt + 1 < m_history.size();
    }
    void setShowHidden(bool value);
    Q_INVOKABLE void navigate(const QString &input);
    Q_INVOKABLE void refresh();
    Q_INVOKABLE void back();
    Q_INVOKABLE void forward();
    Q_INVOKABLE void up();
    Q_INVOKABLE QVariantMap entry(int row) const;
    // First selected entry in view order; one map instead of building every row in QML.
    Q_INVOKABLE QVariantMap firstSelected() const;
    // Every selected entry, in list order.
    Q_INVOKABLE QVariantList selectedEntries() const;
    Q_INVOKABLE void select(int row, int modifiers = 0);
    Q_INVOKABLE void selectAll();
    Q_INVOKABLE void clearSelection();
    Q_INVOKABLE void selectUri(const QString &uri);
    // Rubber-band selection: exactly base plus these rows (signals only rows that flip).
    // Selection = base plus rows; with toggle, rows already in base are taken out instead
    // (a band in selection mode flips what it covers).
    Q_INVOKABLE void selectRows(const QList<int> &rows, const QStringList &base = {}, bool toggle = false);
    Q_INVOKABLE void sortBy(const QString &key, bool descending = false);
    // rules: [{type: 0 glob | 1 MIME type, pattern}]; none = every file.
    Q_INVOKABLE void setFilter(const QVariantList &rules, bool foldersOnly = false);
    void replaceEntries(QList<nook::Entry> entries);
    void appendEntries(QList<nook::Entry> entries);
    void setBusy(bool value, QString error = {});
    // One step of an incremental refresh, replayed on the GUI thread as row signals.
    struct Change {
        enum Kind { Remove, Insert, Move, Update } kind;
        int at = 0, count = 0, from = 0;
        QList<nook::Entry> entries;
    };
    // Turns old into next with removes/inserts/moves/updates; false when a reset is cheaper.
    static bool diff(QList<nook::Entry> old, const QList<nook::Entry> &next, QList<Change> *changes);

  Q_SIGNALS:
    void locationChanged();
    // The same listing is about to come back in another order or with other rows (sort, filter,
    // hidden files), so views can move their rows from where they were.
    void rearranging();
    void statusChanged();
    void countChanged();
    void showHiddenChanged();
    void tagFilterChanged();
    void selectionChanged();
    void firstBatch(qint64 milliseconds);

  private:
    // keep: refresh in place (diffed, scroll and selection kept) instead of clearing first.
    void load(bool keep = false);
    void clearTagFilter();
    void apply(const QList<Change> &changes);
    void notifySelection(const QSet<QString> &previous);
    void notifySelection();
    QString m_uri, m_error, m_sort = "name", m_pendingSelection, m_tagFilter;
    QStringList m_history;
    int m_historyAt = -1, m_anchor = -1;
    quint64 m_generation = 0;
    bool m_loading = false, m_hidden = false, m_descending = false;
    QList<nook::Entry> m_entries;
    QSet<QString> m_selected;
    mutable QStringList m_selectedCache;
    mutable bool m_selectedDirty = true;
    nook::Cancel m_cancel;
    std::shared_ptr<const nook::EntryFilter> m_filter;
    QList<nook::Entry> filtered(QList<nook::Entry> entries) const;
    QFileSystemWatcher m_watcher;
    QTimer m_refreshTimer;
};
