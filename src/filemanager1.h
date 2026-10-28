#pragma once
#include <QDBusAbstractAdaptor>
#include <QJsonObject>
#include <functional>

// org.freedesktop.FileManager1: how other applications ask "show this folder" or "show this
// file in its folder" (browsers' "Show in folder", portals' OpenDirectory, …). Each call is
// turned into the same request a second `nook` launch sends to the running instance.
class FileManager1 : public QDBusAbstractAdaptor {
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.freedesktop.FileManager1")
  public:
    using Handler = std::function<void(const QJsonObject &request)>;
    FileManager1(QObject *host, Handler handler);
    // Exports the interface on the session bus; false when another file manager holds the name.
    static bool publish(QObject *host, Handler handler);

  public Q_SLOTS:
    void ShowFolders(const QStringList &uris, const QString &startupId);
    void ShowItems(const QStringList &uris, const QString &startupId);
    void ShowItemProperties(const QStringList &uris, const QString &startupId);

  private:
    void request(const QString &key, const QStringList &uris, const QString &startupId);
    Handler m_handler;
};
