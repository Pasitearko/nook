#pragma once
#include "directorymodel.h"
#include <QFileSystemWatcher>
#include <QObject>
#include <QTimer>
#include <atomic>
#include <functional>
#include <map>
#include <thread>

struct sqlite3;

class SearchIndex : public QObject {
    Q_OBJECT
    Q_PROPERTY(DirectoryModel *model READ model CONSTANT)
    Q_PROPERTY(bool scanning READ scanning NOTIFY changed)
    Q_PROPERTY(QString status READ status NOTIFY changed)
    Q_PROPERTY(qint64 indexed READ indexed NOTIFY changed)
    Q_PROPERTY(QStringList warnings READ warnings NOTIFY changed)
    Q_PROPERTY(bool hasMore READ hasMore NOTIFY resultsChanged)
    Q_PROPERTY(QStringList roots READ roots WRITE setRoots NOTIFY rootsChanged)
    Q_PROPERTY(QStringList exclusions READ exclusions WRITE setExclusions NOTIFY rootsChanged)
  public:
    explicit SearchIndex(const QString &database = {}, QObject *parent = nullptr);
    ~SearchIndex() override;
    DirectoryModel *model() {
        return &m_results;
    }
    bool scanning() const {
        return m_scanning;
    }
    QString status() const {
        return m_status;
    }
    qint64 indexed() const {
        return m_indexed;
    }
    QStringList warnings() const {
        return m_warnings;
    }
    bool hasMore() const {
        return m_more;
    }
    QStringList roots() const {
        return m_roots;
    }
    QStringList exclusions() const {
        return m_exclusions;
    }
    void setRoots(QStringList roots);
    void setExclusions(QStringList exclusions);
    // Whole NTFS partitions are indexed from their file table ($MFT) and kept current from the
    // USN change journal, like Everything, instead of being walked folder by folder. The
    // partition is opened read-only through nook-volume-helper (pkexec) once per session.
    bool ntfsJournal() const {
        return m_ntfsJournal;
    }
    void setNtfsJournal(bool enabled) {
        m_ntfsJournal = enabled;
    }
    // Tests: treats every root as an NTFS partition and opens it with this (key = root path).
    using VolumeOpener = std::function<int(const QString &key, QString *error)>;
    void setVolumeOpener(VolumeOpener opener) {
        m_volumeOpener = std::move(opener);
    }
    Q_INVOKABLE void start();
    Q_INVOKABLE void stop();
    Q_INVOKABLE void query(const QString &text, const QString &scope = {}, const QString &type = "all",
                           qint64 minBytes = 0, qint64 maxBytes = 0, qint64 after = 0, qint64 before = 0);
    Q_INVOKABLE void nextPage();
    QString databasePath() const {
        return m_database;
    }
    SearchIndex *createSession(QObject *parent);
  Q_SIGNALS:
    void changed();
    void rootsChanged();
    void resultsChanged();
    void scanFinished();

  private:
    void scan(QStringList scopes);
    void runQuery(bool append);
    // Scan thread: brings one NTFS partition up to date (journal replay, or a file-table pass
    // when the journal cannot vouch for the index). False: the walk has to cover it this time.
    bool syncNtfs(sqlite3 *db, const VolumeOpener &opener, const QString &key, const QString &root,
                  const QStringList &excludes, bool rebuild, qint64 generation, qint64 &count, QStringList &watches,
                  QString *error);
    QString m_database, m_status = QStringLiteral("尚未建立索引");
    QStringList m_roots, m_exclusions, m_warnings;
    DirectoryModel m_results;
    bool m_scanning = false, m_more = false;
    qint64 m_indexed = 0;
    std::atomic_bool m_stop{false};
    nook::Cancel m_queryCancel;
    quint64 m_queryGeneration = 0;
    std::thread m_thread;
    QFileSystemWatcher m_watcher;
    QTimer m_sweep, m_debounce;
    QSet<QString> m_dirty;
    QString m_text, m_scope, m_type;
    qint64 m_min = 0, m_max = 0, m_after = 0, m_before = 0;
    int m_offset = 0;
    SearchIndex *m_scanSource = nullptr;
    bool m_ntfsJournal = false;
    VolumeOpener m_volumeOpener;
    std::map<QString, int> m_volumes; // read-only partition descriptors, used by the scan thread only
};
