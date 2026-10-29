#pragma once
#include "application.h"
#include <QFileSystemWatcher>
#include <QObject>
#include <QSettings>
#include <QVariantList>
#include <functional>

#include "application.h"

// What every nook解压缩 window shares: nook's appearance settings (followed live, never written
// here), this application's own preferences, recent packages, and the Nook file chooser.
class ArchiveApp : public QObject {
    Q_OBJECT
    Q_PROPERTY(AppSettings *settings READ settings CONSTANT)
    Q_PROPERTY(QVariantList recent READ recent NOTIFY recentChanged)
    Q_PROPERTY(QVariantList encodings READ encodings CONSTANT)
  public:
    // openWindow opens a package (or an empty window for "") in a window of its own.
    ArchiveApp(AppSettings *settings, std::function<void(const QString &)> openWindow, QObject *parent = nullptr);
    AppSettings *settings() const { return m_settings; }
    QVariantList recent() const;
    QVariantList encodings() const;

    Q_INVOKABLE void addRecent(const QString &path);
    Q_INVOKABLE void forgetRecent(const QString &path);
    Q_INVOKABLE void clearRecent();
    Q_INVOKABLE void openWindow(const QString &path) { m_openWindow(path); }
    Q_INVOKABLE QString formatSize(double bytes) const;
    Q_INVOKABLE bool isFile(const QString &location) const;
    // Shows Nook's chooser for packages; archivesChosen answers requester (empty: cancelled).
    Q_INVOKABLE void chooseArchives(QObject *requester);
    // Shows the folder that holds path in the file manager, with path selected.
    Q_INVOKABLE void showInFolder(const QString &path);

  Q_SIGNALS:
    void recentChanged();
    void notice(const QString &message);
    void archivesChosen(QObject *requester, const QStringList &paths);

  private:
    AppSettings *m_settings;
    std::function<void(const QString &)> m_openWindow;
    QSettings m_store;
    QFileSystemWatcher m_watcher;
    int m_requests = 0;
};
