#include "archiveapp.h"
#include "application.h"
#include "filechooser.h"
#include "files.h"
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusMetaType>
#include <QDBusObjectPath>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QDir>
#include <QFileInfo>
#include <QPointer>
#include <QUrl>
#include <limits>
#include <unistd.h>

namespace {
constexpr int RecentLimit = 12;

// The patterns the chooser offers under "压缩包"; the worker still decides by content.
const QStringList ArchivePatterns{"*.zip", "*.7z", "*.rar", "*.tar", "*.tar.gz", "*.tgz", "*.tar.bz2", "*.tbz2",
                                  "*.tbz", "*.tar.xz", "*.txz", "*.tar.zst", "*.tzst", "*.tar.lz4", "*.tar.lzma",
                                  "*.gz", "*.bz2", "*.xz", "*.zst", "*.lzma", "*.lz4", "*.iso", "*.cab", "*.cpio",
                                  "*.deb", "*.rpm", "*.xar", "*.lzh", "*.lha", "*.wim", "*.jar", "*.apk",
                                  "*.001", "*.r00", "*.z01", "*.zipx", "*.epub"};
}

ArchiveApp::ArchiveApp(AppSettings *settings, std::function<void(const QString &)> openWindow, QObject *parent)
    : QObject(parent), m_settings(settings), m_openWindow(std::move(openWindow)),
      m_store(QSettings::IniFormat, QSettings::UserScope, "nook", "archive") {
    // nook writes its preferences from another process; pick its changes up as they land.
    const QString preferences = m_settings->store().fileName();
    const auto watch = [this, preferences] {
        if (!m_watcher.files().contains(preferences) && QFileInfo::exists(preferences))
            m_watcher.addPath(preferences);
        if (!m_watcher.directories().contains(QFileInfo(preferences).absolutePath()))
            m_watcher.addPath(QFileInfo(preferences).absolutePath());
    };
    watch();
    const auto reload = [this, watch] {
        m_settings->store().sync();
        Q_EMIT m_settings->changed();
        watch(); // an atomic save replaces the file, which drops it from the watcher
    };
    connect(&m_watcher, &QFileSystemWatcher::fileChanged, this, reload);
    connect(&m_watcher, &QFileSystemWatcher::directoryChanged, this, reload);
}

QVariantList ArchiveApp::recent() const {
    QVariantList items;
    for (const auto &path : m_store.value("recent").toStringList()) {
        const QFileInfo info(path);
        items << QVariantMap{{"path", path},
                             {"name", info.fileName()},
                             {"folder", info.absolutePath()},
                             {"exists", info.isFile()},
                             {"sizeText", info.isFile() ? nook::formatSize(info.size()) : QString()}};
    }
    return items;
}

void ArchiveApp::addRecent(const QString &path) {
    QStringList paths = m_store.value("recent").toStringList();
    paths.removeAll(path);
    paths.prepend(path);
    while (paths.size() > RecentLimit)
        paths.removeLast();
    m_store.setValue("recent", paths);
    m_store.setValue("lastFolder", QFileInfo(path).absolutePath());
    Q_EMIT recentChanged();
}

void ArchiveApp::forgetRecent(const QString &path) {
    QStringList paths = m_store.value("recent").toStringList();
    paths.removeAll(path);
    m_store.setValue("recent", paths);
    Q_EMIT recentChanged();
}

void ArchiveApp::clearRecent() {
    m_store.remove("recent");
    Q_EMIT recentChanged();
}

QVariantList ArchiveApp::encodings() const {
    return {QVariantMap{{"value", "auto"}, {"label", "自动识别"}},
            QVariantMap{{"value", "UTF-8"}, {"label", "UTF-8"}},
            QVariantMap{{"value", "GB18030"}, {"label", "简体中文（GBK）"}},
            QVariantMap{{"value", "Big5"}, {"label", "繁体中文（Big5）"}},
            QVariantMap{{"value", "Shift_JIS"}, {"label", "日文（Shift_JIS）"}},
            QVariantMap{{"value", "EUC-JP"}, {"label", "日文（EUC-JP）"}},
            QVariantMap{{"value", "EUC-KR"}, {"label", "韩文（EUC-KR）"}},
            QVariantMap{{"value", "IBM437"}, {"label", "西文 DOS（CP437）"}},
            QVariantMap{{"value", "IBM866"}, {"label", "俄文 DOS（CP866）"}},
            QVariantMap{{"value", "windows-1251"}, {"label", "西里尔文（Windows-1251）"}},
            QVariantMap{{"value", "windows-1252"}, {"label", "西欧（Windows-1252）"}},
            QVariantMap{{"value", "KOI8-R"}, {"label", "俄文（KOI8-R）"}},
            QVariantMap{{"value", "ISO-8859-1"}, {"label", "西欧（ISO-8859-1）"}}};
}

QString ArchiveApp::formatSize(double bytes) const { return nook::formatSize(qint64(bytes)); }

bool ArchiveApp::isFile(const QString &location) const {
    const QUrl url(location);
    return QFileInfo(url.isLocalFile() ? url.toLocalFile() : location).isFile();
}

void ArchiveApp::chooseArchives(QObject *requester) {
    using nook::portal::Filter;
    using nook::portal::FilterRule;
    Filter archives{"压缩包", {}};
    for (const auto &pattern : ArchivePatterns)
        archives.rules.append(FilterRule{0, pattern});
    const QList<Filter> filters{archives, Filter{"所有文件", {FilterRule{0, "*"}}}};
    QString folder = m_store.value("lastFolder").toString();
    if (folder.isEmpty() || !QFileInfo(folder).isDir())
        folder = QDir::homePath();
    QVariantMap options{{"multiple", true},
                        {"filters", QVariant::fromValue(filters)},
                        {"current_folder", QVariant::fromValue(QFile::encodeName(folder) + '\0')},
                        {"accept_label", "打开"}};
    // Nook's own chooser, whichever chooser the desktop portal is set up with (same package).
    QDBusMessage call = QDBusMessage::createMethodCall("org.freedesktop.impl.portal.desktop.nook",
                                                       "/org/freedesktop/portal/desktop",
                                                       "org.freedesktop.impl.portal.FileChooser", "OpenFile");
    const QString handle = QStringLiteral("/org/freedesktop/portal/desktop/request/nook_archive/r%1_%2")
                               .arg(::getpid())
                               .arg(++m_requests);
    call << QVariant::fromValue(QDBusObjectPath(handle)) << QStringLiteral("local.nook.archive") << QString()
         << QStringLiteral("打开压缩包") << options;
    auto *watcher = new QDBusPendingCallWatcher(
        QDBusConnection::sessionBus().asyncCall(call, std::numeric_limits<int>::max()), this);
    const QPointer<QObject> target(requester);
    connect(watcher, &QDBusPendingCallWatcher::finished, this, [this, watcher, target] {
        watcher->deleteLater();
        const QDBusMessage reply = watcher->reply();
        QStringList paths;
        if (reply.type() == QDBusMessage::ErrorMessage) {
            Q_EMIT notice("无法打开文件选择器：" + reply.errorMessage());
        } else if (reply.arguments().size() >= 2 && reply.arguments().at(0).toUInt() == 0) {
            const auto results = qdbus_cast<QVariantMap>(reply.arguments().at(1));
            for (const auto &uri : results.value("uris").toStringList()) {
                const QUrl url(uri);
                if (url.isLocalFile())
                    paths << url.toLocalFile();
            }
        }
        if (target)
            Q_EMIT archivesChosen(target, paths);
    });
}

void ArchiveApp::showInFolder(const QString &path) {
    QDBusMessage call = QDBusMessage::createMethodCall("org.freedesktop.FileManager1", "/org/freedesktop/FileManager1",
                                                       "org.freedesktop.FileManager1", "ShowItems");
    call << QStringList{QUrl::fromLocalFile(path).toString()} << QString();
    QDBusConnection::sessionBus().asyncCall(call);
}
