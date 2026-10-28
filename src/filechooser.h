#pragma once
#include <QDBusAbstractAdaptor>
#include <QDBusArgument>
#include <QDBusMessage>
#include <QDBusObjectPath>
#include <QObject>
#include <QVariantMap>
#include <functional>

// The file chooser behind xdg-desktop-portal (org.freedesktop.impl.portal.FileChooser): the
// "Open…" and "Save…" dialogs of other applications, shown as nook windows.
namespace nook::portal {

struct FilterRule {
    uint type = 0; // 0 glob, 1 MIME type
    QString pattern;
};
struct Filter {
    QString name;
    QList<FilterRule> rules;
};
struct ChoiceOption {
    QString id, label;
};
struct Choice {
    QString id, label;
    QList<ChoiceOption> options; // none: a check box ("true"/"false")
    QString initial;
};
struct ChoiceValue {
    QString id, value;
};
void registerTypes();

// One dialog: what the application asked for, and its answer.
class ChooserRequest : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString mode MEMBER m_mode CONSTANT) // open, save, saveFiles
    Q_PROPERTY(QString appId MEMBER m_appId CONSTANT)
    Q_PROPERTY(QString title MEMBER m_title CONSTANT)
    Q_PROPERTY(QString acceptLabel MEMBER m_acceptLabel CONSTANT)
    Q_PROPERTY(bool multiple MEMBER m_multiple CONSTANT)
    Q_PROPERTY(bool directory MEMBER m_directory CONSTANT)
    Q_PROPERTY(QVariantList filters READ filters CONSTANT)
    Q_PROPERTY(int currentFilter MEMBER m_currentFilter CONSTANT)
    Q_PROPERTY(QVariantList choices READ choices CONSTANT)
    Q_PROPERTY(QString currentName MEMBER m_currentName CONSTANT)
    Q_PROPERTY(QString currentFolder MEMBER m_currentFolder CONSTANT)
    Q_PROPERTY(QStringList files MEMBER m_files CONSTANT)
  public:
    ChooserRequest(const QString &mode, const QString &appId, const QString &title, const QVariantMap &options,
                   const QString &fallbackFolder, QObject *parent = nullptr);
    QVariantList filters() const;
    QVariantList choices() const;
    bool done() const {
        return m_done;
    }
    // filter: index into filters (-1 none); choices: id -> chosen value.
    Q_INVOKABLE void accept(const QStringList &uris, int filter, const QVariantMap &choices);
    Q_INVOKABLE void cancel();
    // The portal withdrew the request (the application went away or gave up).
    void abort();
    // A typed name or path, relative to the folder shown.
    Q_INVOKABLE QString resolve(const QString &folderUri, const QString &text) const;
    Q_INVOKABLE bool exists(const QString &uri) const;
    Q_INVOKABLE bool isFolder(const QString &uri) const;
    // SaveFiles: where each file goes in this folder, renamed rather than overwriting.
    Q_INVOKABLE QStringList targetsIn(const QString &folderUri) const;

  Q_SIGNALS:
    void finished(uint response, const QVariantMap &results);

  private:
    void finish(uint response, const QVariantMap &results);
    QString m_mode, m_appId, m_title, m_acceptLabel, m_currentName, m_currentFolder;
    QStringList m_files;
    bool m_multiple = false, m_directory = false, m_done = false;
    QList<Filter> m_filters;
    QList<Choice> m_choices;
    int m_currentFilter = -1;
};

class FileChooserPortal : public QObject {
    Q_OBJECT
  public:
    using Show = std::function<void(ChooserRequest *)>;
    // show: open a window for the request (it is deleted after it finishes);
    // lastFolder/remember: where the previous dialog ended, for dialogs that name none.
    FileChooserPortal(Show show, std::function<QString()> lastFolder, std::function<void(const QString &)> remember,
                      QObject *parent = nullptr);
    bool publish();
    // Starts a dialog and answers the D-Bus call once it is done.
    void begin(const QString &mode, const QDBusObjectPath &handle, const QString &appId, const QString &title,
               const QVariantMap &options, const QDBusMessage &message);

  private:
    Show m_show;
    std::function<QString()> m_lastFolder;
    std::function<void(const QString &)> m_remember;
};

class FileChooserAdaptor : public QDBusAbstractAdaptor {
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.freedesktop.impl.portal.FileChooser")
    Q_PROPERTY(uint version READ version)
  public:
    explicit FileChooserAdaptor(FileChooserPortal *portal);
    uint version() const {
        return 4;
    }
  public Q_SLOTS:
    uint OpenFile(const QDBusObjectPath &handle, const QString &app_id, const QString &parent_window,
                  const QString &title, const QVariantMap &options, const QDBusMessage &message, QVariantMap &results);
    uint SaveFile(const QDBusObjectPath &handle, const QString &app_id, const QString &parent_window,
                  const QString &title, const QVariantMap &options, const QDBusMessage &message, QVariantMap &results);
    uint SaveFiles(const QDBusObjectPath &handle, const QString &app_id, const QString &parent_window,
                   const QString &title, const QVariantMap &options, const QDBusMessage &message, QVariantMap &results);

  private:
    FileChooserPortal *m_portal;
};

// org.freedesktop.impl.portal.Request at the call's handle: lets the portal close the dialog.
class RequestAdaptor : public QDBusAbstractAdaptor {
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.freedesktop.impl.portal.Request")
  public:
    RequestAdaptor(QObject *host, ChooserRequest *request);
  public Q_SLOTS:
    void Close();

  private:
    ChooserRequest *m_request;
};

} // namespace nook::portal

Q_DECLARE_METATYPE(nook::portal::FilterRule)
Q_DECLARE_METATYPE(nook::portal::Filter)
Q_DECLARE_METATYPE(nook::portal::ChoiceOption)
Q_DECLARE_METATYPE(nook::portal::Choice)
Q_DECLARE_METATYPE(nook::portal::ChoiceValue)
