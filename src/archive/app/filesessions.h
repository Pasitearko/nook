#pragma once
#include <QFileSystemWatcher>
#include <QHash>
#include <QObject>
#include <QPointer>
#include <QTimer>
#include <QVariantList>

class TaskQueue;

// Files taken out of a package for a while: opened with their default application (and watched,
// so a saved change can be written back into the package, after asking each time), or extracted
// to be dragged out. They live in ~/.cache/nook-archive and go when the application ends, except
// edits that were never written back.
class FileSessions : public QObject {
    Q_OBJECT
    Q_PROPERTY(int unsavedCount READ unsavedCount NOTIFY changed)
  public:
    explicit FileSessions(TaskQueue *queue, QObject *parent = nullptr);
    ~FileSessions() override;
    static QString cacheRoot();
    // Removes what earlier runs left behind (a day old and not holding unsaved edits).
    static void sweep();
    // How a file inside a package is opened: something else (the tests) may stand in.
    void setOpener(std::function<void(const QString &path)> opener) { m_opener = std::move(opener); }

    // Extracts entry (with the package's password from this session, asking if needed) and
    // opens it. readOnly: changes can only be saved as a copy elsewhere.
    Q_INVOKABLE void open(QObject *owner, const QString &archive, const QString &entry, const QString &encoding, bool readOnly);
    // Extracts entries for a drag; dragReady(token, urls) once they exist.
    Q_INVOKABLE int exportForDrag(QObject *owner, const QString &archive, const QStringList &entries, const QString &prefix,
                                  const QString &encoding);
    Q_INVOKABLE void writeBack(int id);
    // The user chose not to write this change back (yet); asks again after the next change.
    Q_INVOKABLE void decline(int id);
    // Copies the edited file into folder; the path of the copy, or empty.
    Q_INVOKABLE QString saveCopy(int id, const QString &folder);
    Q_INVOKABLE QVariantMap session(int id) const;
    Q_INVOKABLE bool ownedBy(int id, QObject *window) const;
    // Edits of archive not written back: [{id, name}].
    Q_INVOKABLE QVariantList unsaved(const QString &archive) const;
    // Gives up the edits of archive (its window closes without writing them back).
    Q_INVOKABLE void discard(const QString &archive);
    int unsavedCount() const;

  Q_SIGNALS:
    void changed();
    // The opened file was saved with changes; its window asks what to do.
    void modified(int id);
    void dragReady(int token, const QStringList &urls);
    void failed(QObject *owner, const QString &message);
    void written(int id, bool ok, const QString &message);

  private:
    struct Session {
        int id = 0;
        QString archive;
        QString entry;
        QString folder; // our directory in the cache
        QString file;
        QPointer<QObject> owner;
        bool readOnly = false;
        bool open = false; // extracted and handed to its application
        bool dirty = false;
        qint64 savedTime = 0, savedSize = -1;     // as last written back (or extracted)
        qint64 answeredTime = 0, answeredSize = -1; // as when the user last answered
        int writing = 0; // task writing it back
    };
    QString newFolder(const QString &kind) const;
    void finished(int task);
    void check(int id);
    void watch(Session &session);
    TaskQueue *m_queue;
    QHash<int, Session> m_sessions;
    QHash<int, int> m_extracting; // task → session
    QHash<int, int> m_dragging;   // task → drag token
    QHash<int, QString> m_dragFolders;
    QFileSystemWatcher m_watcher;
    QHash<int, QTimer *> m_timers;
    std::function<void(const QString &)> m_opener;
    int m_next = 1;
};
