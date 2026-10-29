#pragma once
#include "application.h"
#include "defaultapps.h"
#include "filesessions.h"
#include "notifier.h"
#include "passwordstore.h"
#include "taskqueue.h"
#include <QFileSystemWatcher>
#include <QObject>
#include <QSettings>
#include <QVariantList>
#include <functional>

// What every nook解压缩 window shares: nook's appearance settings (followed live, never written
// here), this application's own preferences, recent packages, the task queue, saved passwords,
// notifications and the Nook file chooser.
class ArchiveDocument;

class ArchiveApp : public QObject {
    Q_OBJECT
    Q_PROPERTY(AppSettings *settings READ settings CONSTANT)
    Q_PROPERTY(QVariantList recent READ recent NOTIFY recentChanged)
    Q_PROPERTY(QVariantList encodings READ encodings CONSTANT)
    Q_PROPERTY(QVariantList formats READ formats CONSTANT)
    Q_PROPERTY(TaskQueue *queue READ queue CONSTANT)
    Q_PROPERTY(PasswordStore *passwords READ passwords CONSTANT)
    Q_PROPERTY(FileSessions *files READ files CONSTANT)
    Q_PROPERTY(DefaultApps *defaults READ defaults CONSTANT)
    // concurrency, conflict, subfolder, notify, autoTryPasswords, openAfter, format, level …
    Q_PROPERTY(QVariantMap prefs READ prefs NOTIFY prefsChanged)
  public:
    // component is a QML type of the NookArchive module; properties are its initial properties
    // (app is added). An empty component with path opens a package window ("" for an empty one).
    using WindowFactory = std::function<QObject *(const QString &component, const QVariantMap &properties)>;
    ArchiveApp(AppSettings *settings, std::function<void(const QString &)> openWindow, QObject *parent = nullptr);
    void setWindowFactory(WindowFactory factory) { m_factory = std::move(factory); }
    // Lets a package window use and share the session's passwords.
    void attach(ArchiveDocument *document);
    AppSettings *settings() const { return m_settings; }
    QVariantList recent() const;
    QVariantList encodings() const;
    QVariantList formats() const;
    TaskQueue *queue() { return &m_queue; }
    PasswordStore *passwords() { return &m_passwords; }
    FileSessions *files() { return &m_files; }
    DefaultApps *defaults() { return &m_defaults; }
    Notifier *notifier() { return &m_notifier; }
    QVariantMap prefs() const;

    Q_INVOKABLE void setPref(const QString &key, const QVariant &value);
    Q_INVOKABLE void addRecent(const QString &path);
    Q_INVOKABLE void forgetRecent(const QString &path);
    Q_INVOKABLE void clearRecent();
    Q_INVOKABLE void openWindow(const QString &path) { m_openWindow(path); }
    // The compact compression window for these files and folders.
    Q_INVOKABLE void openCompose(const QStringList &items);
    // The same window, converting a package to another format (names read with encoding).
    Q_INVOKABLE void openConvert(const QString &archive, const QString &encoding);
    Q_INVOKABLE QVariantMap convertRequest(const QString &archive, const QVariantMap &choices, const QString &encoding) const;
    // A small window that follows one task and asks its questions.
    Q_INVOKABLE void openTaskWindow(int id);
    // nook's menu: each runs in a small progress window that closes on success.
    // Extracts each package into its own folder (several: always a folder each).
    Q_INVOKABLE void extractHere(const QStringList &archives);
    // Asks where with the Nook chooser first.
    Q_INVOKABLE void extractTo(const QStringList &archives);
    // "压缩为 <名称>.zip": ZIP at the standard level, next to the first item.
    Q_INVOKABLE QList<int> compressNow(const QStringList &items);
    QList<int> extractInto(const QStringList &archives, const QString &destination);
    QList<int> runInProgressWindow(const QVariantList &requests, const QVariantMap &extras);
    QStringList firstParts(const QStringList &archives) const;
    // A destination chooser is open with nothing else to show for it yet.
    bool choosing() const { return m_choosing > 0; }
    Q_INVOKABLE QString formatSize(double bytes) const;
    Q_INVOKABLE bool isFile(const QString &location) const;
    Q_INVOKABLE bool exists(const QString &path) const;
    Q_INVOKABLE QString localPath(const QString &location) const;
    // Whether this looks like a package by its name (dropping files decides open or compress).
    Q_INVOKABLE bool looksLikeArchive(const QString &location) const;
    // Shows Nook's chooser; chosen answers requester with purpose (empty paths: cancelled).
    // purpose: "archives" (packages to open), "files", "folders" (to compress), "destination".
    Q_INVOKABLE void choose(QObject *requester, const QString &purpose, const QString &folder = {});
    Q_INVOKABLE void chooseArchives(QObject *requester) { choose(requester, "archives"); }
    // Shows path in the file manager: the folder itself, or its parent with it selected.
    Q_INVOKABLE void reveal(const QString &path, bool isFolder);
    Q_INVOKABLE void showInFolder(const QString &path) { reveal(path, false); }
    // For the compression window: suggested name, folder and whether one file is chosen.
    Q_INVOKABLE QVariantMap compressDefaults(const QStringList &items) const;
    // The worker request(s) for the compression window's choices; one per item when separate.
    Q_INVOKABLE QVariantList compressRequests(const QStringList &items, const QVariantMap &choices) const;
    // The level number a format takes for one of the six named levels (0 store … 5 best).
    Q_INVOKABLE int levelFor(const QString &format, int step) const;
    Q_INVOKABLE QString suffixFor(const QString &format) const;
    // Where "解压到当前文件夹" puts a package: the folder it is in.
    Q_INVOKABLE QString folderOf(const QString &path) const;
    // "photos.tar.gz" → "photos".
    Q_INVOKABLE QString stemOf(const QString &path) const;
    // A free name for a new folder or file in folder ("名称", then "名称 (2)" …).
    Q_INVOKABLE QString freeName(const QString &folder, const QString &name) const;

  Q_SIGNALS:
    void recentChanged();
    void prefsChanged();
    void notice(const QString &message);
    void chosen(QObject *requester, const QString &purpose, const QStringList &paths);
    void choosingDone();
    // A package was changed (added to, deleted from, renamed in): windows showing it reload.
    void archiveChanged(const QString &path);

  private:
    void finished(int id);
    void needsAnswer(int id);
    AppSettings *m_settings;
    std::function<void(const QString &)> m_openWindow;
    WindowFactory m_factory;
    QSettings m_store;
    QFileSystemWatcher m_watcher;
    PasswordStore m_passwords;
    TaskQueue m_queue;
    FileSessions m_files{&m_queue};
    DefaultApps m_defaults;
    Notifier m_notifier;
    int m_requests = 0;
    int m_choosing = 0;
    QStringList m_pendingExtract;
};
