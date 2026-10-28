#include "filemanager1.h"
#include "files.h"
#include <QDBusConnection>
#include <QJsonArray>

FileManager1::FileManager1(QObject *host, Handler handler) : QDBusAbstractAdaptor(host), m_handler(std::move(handler)) {}

bool FileManager1::publish(QObject *host, Handler handler) {
    auto bus = QDBusConnection::sessionBus();
    if (!bus.isConnected())
        return false;
    new FileManager1(host, std::move(handler));
    return bus.registerObject(QStringLiteral("/org/freedesktop/FileManager1"), host) &&
           bus.registerService(QStringLiteral("org.freedesktop.FileManager1"));
}

void FileManager1::request(const QString &key, const QStringList &uris, const QString &startupId) {
    // The activation token lets the compositor raise the new window for the caller.
    if (!startupId.isEmpty())
        qputenv("XDG_ACTIVATION_TOKEN", startupId.toUtf8());
    for (const auto &uri : uris) {
        if (uri.isEmpty())
            continue;
        const QString normalized = nook::normalizeUri(uri);
        m_handler(key == QLatin1String("select")
                      ? QJsonObject{{"select", normalized}, {"newWindow", true}}
                      : QJsonObject{{key, QJsonArray{normalized}}, {"newWindow", true}});
    }
}

void FileManager1::ShowFolders(const QStringList &uris, const QString &startupId) {
    request(QStringLiteral("paths"), uris, startupId);
}
void FileManager1::ShowItems(const QStringList &uris, const QString &startupId) {
    request(QStringLiteral("select"), uris, startupId);
}
void FileManager1::ShowItemProperties(const QStringList &uris, const QString &startupId) {
    request(QStringLiteral("select"), uris, startupId);
}
