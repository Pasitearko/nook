#pragma once
#include "directorymodel.h"
#include "fileproperties.h"
#include "diskmounts.h"
#include <QFutureWatcher>
#include "operations.h"
#include "preview.h"
#include "searchindex.h"
#include "appearance.h"
#include <QJsonObject>
#include <QSettings>
#include <QVariantList>
#include <memory>

class AppSettings : public QObject {
    Q_OBJECT
    Q_PROPERTY(QVariantMap values READ values NOTIFY changed)
    // The desktop's own dark/light preference, for the "system" theme.
    Q_PROPERTY(bool systemDark READ systemDark NOTIFY systemDarkChanged)
  public:
    explicit AppSettings(QObject *parent = nullptr);
    QVariantMap values() const;
    bool systemDark() const {
        return m_appearance.dark();
    }
    Q_INVOKABLE void set(const QString &key, const QVariant &value);
    QSettings &store() {
        return *m_store;
    }
  Q_SIGNALS:
    void changed();
    void columnWidthsReset();
    void systemDarkChanged();

  private:
    std::unique_ptr<QSettings> m_store;
    SystemAppearance m_appearance;
};
namespace nook {
// Keeps ~/.config/autostart in step with the "background index" preference.
void syncAutostart(bool enabled);
// The program this process was started from, recorded at startup.
QString executablePath();
// True once a package upgrade has replaced the running program on disk.
bool executableReplaced();
} // namespace nook
class Application : public QObject {
    Q_OBJECT
    Q_PROPERTY(QVariantList tabs READ tabs NOTIFY tabsChanged)
    Q_PROPERTY(int activeTab READ activeTab WRITE setActiveTab NOTIFY tabsChanged)
    Q_PROPERTY(DirectoryModel *left READ left NOTIFY tabsChanged)
    Q_PROPERTY(DirectoryModel *right READ right NOTIFY tabsChanged)
    Q_PROPERTY(DirectoryModel *activePane READ activePane NOTIFY activePaneChanged)
    Q_PROPERTY(bool dual READ dual WRITE setDual NOTIFY tabsChanged)
    Q_PROPERTY(bool grid READ grid WRITE setGrid NOTIFY tabsChanged)
    Q_PROPERTY(int activeSide READ activeSide WRITE setActiveSide NOTIFY activePaneChanged)
    Q_PROPERTY(QVariantList places READ places NOTIFY placesChanged)
    Q_PROPERTY(Operations *operations READ operations CONSTANT)
    Q_PROPERTY(SearchIndex *search READ search CONSTANT)
    Q_PROPERTY(Preview *preview READ preview CONSTANT)
    Q_PROPERTY(FileProperties *properties READ properties CONSTANT)
    Q_PROPERTY(DiskMounts *diskMounts READ diskMounts CONSTANT)
    Q_PROPERTY(AppSettings *settings READ settings CONSTANT)
    Q_PROPERTY(bool ntfsBusy READ ntfsBusy NOTIFY ntfsBusyChanged)
    Q_PROPERTY(bool ntfsInstalled READ ntfsInstalled CONSTANT)
    // After an upgrade the program on disk is newer than the running one.
    Q_PROPERTY(bool upgraded READ upgraded NOTIFY installedVersionChanged)
    Q_PROPERTY(QString installedVersion READ installedVersion NOTIFY installedVersionChanged)
  public:
    // state, from a restart: the tabs to open instead of the saved session.
    Application(SearchIndex *search, Operations *ops, AppSettings *settings, bool restore,
                QObject *parent = nullptr, const QJsonObject &state = {});
    ~Application() override;
    QVariantList tabs() const;
    int activeTab() const {
        return m_active;
    }
    DirectoryModel *left() const;
    DirectoryModel *right() const;
    DirectoryModel *activePane() const;
    bool dual() const;
    bool grid() const;
    int activeSide() const {
        return m_side;
    }
    QVariantList places() const {
        return m_places;
    }
    Operations *operations() const {
        return m_ops;
    }
    SearchIndex *search() const {
        return m_search;
    }
    Preview *preview() {
        return &m_preview;
    }
    FileProperties *properties() { return &m_properties; }
    DiskMounts *diskMounts() { return &m_diskMounts; }
    AppSettings *settings() const {
        return m_settings;
    }
    void setActiveTab(int index);
    void setActiveSide(int side);
    void setDual(bool dual);
    void setGrid(bool grid);
    Q_INVOKABLE void addTab(const QString &uri = {});
    Q_INVOKABLE void closeTab(int index);
    // Drag to reorder: the tab at from takes the place of the one at to; the active tab stays active.
    Q_INVOKABLE void moveTab(int from, int to);
    Q_INVOKABLE void navigate(const QString &uri);
    Q_INVOKABLE void openItem(const QVariantMap &entry);
    Q_INVOKABLE void openExternal(const QString &uri);
    Q_INVOKABLE void reveal(const QString &uri);
    Q_INVOKABLE void terminal(const QString &uri = {});
    Q_INVOKABLE bool moveModifier() const;
    Q_INVOKABLE int modifiers() const;
    Q_INVOKABLE void copy(bool cut = false, DirectoryModel *source = nullptr);
    Q_INVOKABLE void paste();
    Q_INVOKABLE void drop(const QStringList &uris, const QString &destination, bool move);
    // Drag and drop: Ctrl copies, Shift moves, otherwise move within one filesystem and copy
    // across filesystems. Items already in the destination (or the destination itself) are no-ops.
    Q_INVOKABLE bool canDrop(const QStringList &uris, const QString &destination) const;
    Q_INVOKABLE QString dropAction(const QStringList &uris, const QString &destination) const;
    Q_INVOKABLE void dragDrop(const QStringList &uris, const QString &destination);
    Q_INVOKABLE void copyPaths(const QStringList &uris);
    Q_INVOKABLE bool openLocation(const QString &text);
    Q_INVOKABLE QString localPath(const QString &uri) const { return uri.startsWith("trash:") ? QStringLiteral("回收站") : nook::localPath(uri); }
    Q_INVOKABLE QString parentUri(const QString &uri) const { return nook::parentUri(uri); }
    Q_INVOKABLE bool nameAvailable(const QString &directoryUri, const QString &name) const;
    Q_INVOKABLE void resetQuickAccess();
    Q_INVOKABLE void refreshPlaces();
    Q_INVOKABLE void activatePlace(const QVariantMap &place);
    Q_INVOKABLE void addQuickAccess(const QString &uri);
    Q_INVOKABLE void removeQuickAccess(const QString &uri);
    Q_INVOKABLE void reorderQuickAccess(const QString &sourceUri, const QString &targetUri);
    Q_INVOKABLE QVariantList openWithApplications(const QString &uri) const;
    Q_INVOKABLE void openWith(const QString &uri, const QString &applicationId);
    // nook解压缩 from the right-click menu. archiveActions says what the selection allows:
    // extract (every item is a local package), compress (every item is local), zipName.
    Q_INVOKABLE QVariantMap archiveActions(const QStringList &uris) const;
    // action: open, compress, compress-zip, extract-here, extract-to.
    Q_INVOKABLE void archive(const QString &action, const QStringList &uris);
    // The program and arguments archive() starts (nook-archive next to nook, else from PATH).
    QStringList archiveCommand(const QString &action, const QStringList &uris) const;
    Q_INVOKABLE void saveSession();
    // This window's tabs, as a restart hands them to the next process.
    QJsonObject state() const;
    // Replaces this process with the program now installed; windows and tabs come back as they were.
    Q_INVOKABLE void restart() {
        Q_EMIT restartRequested();
    }
    bool upgraded() const {
        return m_upgraded;
    }
    QString installedVersion() const {
        return m_installedVersion;
    }
    // Asks the program on disk for its version (it runs once more, without a window).
    Q_INVOKABLE void checkInstalledVersion();
    // A file chooser browses with the same controller but must never replace the saved tabs.
    void setRemembersSession(bool remember) {
        m_remembersSession = remember;
    }
    Q_INVOKABLE void requestWindow();
    Q_INVOKABLE QString home() const;
    // Turns NTFS file-table indexing on or off. Both ask for the administrator password once
    // (pkexec): enabling installs a polkit rule so later reads need none, disabling removes it.
    Q_INVOKABLE void setNtfsJournal(bool enabled);
    bool ntfsBusy() const {
        return m_ntfsSetup != nullptr;
    }
    bool ntfsInstalled() const;
  Q_SIGNALS:
    void tabsChanged();
    void activePaneChanged();
    void placesChanged();
    void windowRequested();
    void notice(const QString &message);
    void ntfsBusyChanged();
    void restartRequested();
    void installedVersionChanged();

  private:
    struct Tab {
        DirectoryModel *left, *right;
        bool dual = false, grid = false;
    };
    void connectPane(DirectoryModel *pane);
    QStringList dropSources(const QStringList &uris, const QString &destination) const;
    QList<Tab> m_tabs;
    int m_active = 0, m_side = 0;
    QVariantList m_places;
    SearchIndex *m_search;
    Operations *m_ops;
    AppSettings *m_settings;
    Preview m_preview;
    FileProperties m_properties;
    DiskMounts m_diskMounts;
    QList<QFutureWatcher<QString> *> m_openWatchers;
    QTimer m_saveTimer;
    QProcess *m_diskProbe = nullptr;
    QProcess *m_ntfsSetup = nullptr;
    bool m_remembersSession = true;
    bool m_mountBusy = false;
    bool m_upgraded = false;
    QString m_installedVersion;
    QProcess *m_versionProbe = nullptr;
};
