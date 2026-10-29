#include "archiveapp.h"
#include "application.h"
#include "archivedocument.h"
#include "archivenames.h"
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
#include <QTimer>
#include <QUrl>
#include <QWindow>
#include <array>
#include <utility>
#include <limits>
#include <unistd.h>

namespace {
constexpr int RecentLimit = 12;




const QVariantMap PrefDefaults{{"concurrency", 1},       {"conflict", "ask"},      {"subfolder", "smart"},
                                {"notify", true},         {"autoTryPasswords", false}, {"openAfter", false},
                                {"format", "zip"},        {"levelStep", 3},          {"testAfter", false},
                                {"encryptNames", true},   {"zipCrypto", false}};

// What each format calls its suffix, in the order the compression window offers them.
const QList<QPair<QString, QString>> Formats{{"zip", ".zip"},         {"7z", ".7z"},          {"tar.gz", ".tar.gz"},
                                             {"tar.xz", ".tar.xz"},   {"tar.zst", ".tar.zst"}, {"tar.bz2", ".tar.bz2"},
                                             {"tar", ".tar"},         {"gz", ".gz"},          {"xz", ".xz"},
                                             {"zst", ".zst"},         {"bz2", ".bz2"}};
} // namespace

ArchiveApp::ArchiveApp(AppSettings *settings, std::function<void(const QString &)> openWindow, QObject *parent)
    : QObject(parent), m_settings(settings), m_openWindow(std::move(openWindow)),
      m_store(QSettings::IniFormat, QSettings::UserScope, "nook", "archive"), m_queue(&m_passwords) {
    m_queue.setConcurrency(prefs().value("concurrency").toInt());
    m_queue.setAutoTryPasswords(prefs().value("autoTryPasswords").toBool());
    connect(&m_queue, &TaskQueue::taskFinished, this, &ArchiveApp::finished);
    connect(&m_queue, &TaskQueue::attention, this, &ArchiveApp::needsAnswer);
    connect(&m_passwords, &PasswordStore::failed, this, &ArchiveApp::notice);
    connect(this, &ArchiveApp::chosen, this, [this](QObject *requester, const QString &purpose, const QStringList &paths) {
        if (requester != this || purpose != "destination")
            return;
        --m_choosing;
        const QStringList packages = std::exchange(m_pendingExtract, {});
        if (!paths.isEmpty())
            extractInto(packages, paths.first());
        Q_EMIT choosingDone();
    });
    if (prefs().value("autoTryPasswords").toBool())
        m_passwords.load();
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

void ArchiveApp::choose(QObject *requester, const QString &purpose, const QString &startFolder) {
    using nook::portal::Filter;
    using nook::portal::FilterRule;
    QString folder = startFolder.isEmpty() ? m_store.value("lastFolder").toString() : startFolder;
    if (folder.isEmpty() || !QFileInfo(folder).isDir())
        folder = QDir::homePath();
    QVariantMap options{{"current_folder", QVariant::fromValue(QFile::encodeName(folder) + '\0')}};
    QString title;
    if (purpose == "archives") {
        Filter archives{"压缩包", {}};
        for (const auto &pattern : nook::archive::archivePatterns())
            archives.rules.append(FilterRule{0, pattern});
        options.insert("filters", QVariant::fromValue(QList<Filter>{archives, Filter{"所有文件", {FilterRule{0, "*"}}}}));
        options.insert("multiple", true);
        options.insert("accept_label", "打开");
        title = "打开压缩包";
    } else if (purpose == "files") {
        options.insert("multiple", true);
        options.insert("accept_label", "添加");
        title = "选择要压缩的文件";
    } else if (purpose == "folders") {
        options.insert("multiple", true);
        options.insert("directory", true);
        options.insert("accept_label", "添加");
        title = "选择要压缩的文件夹";
    } else {
        options.insert("directory", true);
        options.insert("accept_label", "选择");
        title = purpose == "save" ? "选择保存位置" : "选择解压位置";
    }
    // Nook's own chooser, whichever chooser the desktop portal is set up with (same package).
    QDBusMessage call = QDBusMessage::createMethodCall("org.freedesktop.impl.portal.desktop.nook",
                                                       "/org/freedesktop/portal/desktop",
                                                       "org.freedesktop.impl.portal.FileChooser", "OpenFile");
    const QString handle = QStringLiteral("/org/freedesktop/portal/desktop/request/nook_archive/r%1_%2")
                               .arg(::getpid())
                               .arg(++m_requests);
    call << QVariant::fromValue(QDBusObjectPath(handle)) << QStringLiteral("local.nook.archive") << QString() << title
         << options;
    auto *watcher = new QDBusPendingCallWatcher(
        QDBusConnection::sessionBus().asyncCall(call, std::numeric_limits<int>::max()), this);
    const QPointer<QObject> target(requester);
    connect(watcher, &QDBusPendingCallWatcher::finished, this, [this, watcher, target, purpose] {
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
            Q_EMIT chosen(target, purpose, paths);
    });
}

void ArchiveApp::reveal(const QString &path, bool isFolder) {
    QDBusMessage call = QDBusMessage::createMethodCall("org.freedesktop.FileManager1", "/org/freedesktop/FileManager1",
                                                       "org.freedesktop.FileManager1",
                                                       isFolder ? "ShowFolders" : "ShowItems");
    call << QStringList{QUrl::fromLocalFile(path).toString()} << QString();
    QDBusConnection::sessionBus().asyncCall(call);
}

QVariantMap ArchiveApp::prefs() const {
    QVariantMap values = PrefDefaults;
    for (auto it = PrefDefaults.begin(); it != PrefDefaults.end(); ++it) {
        const QVariant stored = m_store.value("prefs/" + it.key());
        if (!stored.isValid())
            continue;
        // QSettings hands INI values back as strings.
        if (it.value().typeId() == QMetaType::Bool)
            values.insert(it.key(), stored.toString() == "true" || stored.toString() == "1");
        else if (it.value().typeId() == QMetaType::Int)
            values.insert(it.key(), stored.toInt());
        else
            values.insert(it.key(), stored.toString());
    }
    return values;
}

void ArchiveApp::setPref(const QString &key, const QVariant &value) {
    if (!PrefDefaults.contains(key))
        return;
    m_store.setValue("prefs/" + key, value);
    if (key == "concurrency")
        m_queue.setConcurrency(value.toInt());
    else if (key == "autoTryPasswords") {
        m_queue.setAutoTryPasswords(value.toBool());
        if (value.toBool())
            m_passwords.load();
    }
    Q_EMIT prefsChanged();
}

void ArchiveApp::attach(ArchiveDocument *document) {
    document->setPasswordSources([this](const QString &path) { return m_queue.sessionPassword(path); },
                                 [this] { return prefs().value("autoTryPasswords").toBool() ? m_passwords.passwords() : QStringList(); });
    connect(document, &ArchiveDocument::opened, this, [this, document](const QString &path) {
        m_queue.rememberPassword(path, document->password());
    });
}

void ArchiveApp::openCompose(const QStringList &items) {
    if (m_factory)
        m_factory("ArchiveCompose", {{"items", items}});
}

void ArchiveApp::openTaskWindow(int id) {
    if (m_factory && !m_queue.task(id).isEmpty())
        m_factory("ArchiveTaskWindow", {{"taskIds", QVariantList{id}}});
}

QList<int> ArchiveApp::runInProgressWindow(const QVariantList &requests, const QVariantMap &extras) {
    QList<int> ids;
    if (requests.isEmpty())
        return ids;
    // The window comes first so that it owns the tasks and asks their questions.
    QObject *window = m_factory ? m_factory("ArchiveTaskWindow", {{"taskIds", QVariantList{}}}) : nullptr;
    QVariantMap options = extras;
    options.insert("closesOnSuccess", true);
    QVariantList list;
    for (const auto &request : requests) {
        ids << m_queue.start(request.toMap(), options, window);
        list << ids.last();
    }
    if (window)
        window->setProperty("taskIds", list);
    return ids;
}

QStringList ArchiveApp::firstParts(const QStringList &archives) const {
    // Choosing every part of a split package still extracts it once, from its first part.
    QStringList result;
    for (const auto &archive : archives) {
        const QString path = QFileInfo(localPath(archive)).absoluteFilePath();
        const auto volumes = nook::archive::volumesOf(path);
        const QString first = volumes.split && !volumes.first.isEmpty() ? volumes.first : path;
        if (!result.contains(first))
            result << first;
    }
    return result;
}

QList<int> ArchiveApp::extractInto(const QStringList &archives, const QString &destination) {
    const QStringList packages = firstParts(archives);
    const QVariantMap values = prefs();
    QVariantList requests;
    for (const auto &archive : packages)
        requests << QVariantMap{{"op", "extract"},
                                {"archive", archive},
                                {"destination", destination.isEmpty() ? folderOf(archive) : destination},
                                // Several packages at once: each in a folder of its own.
                                {"subfolder", packages.size() > 1 ? QStringLiteral("always") : values.value("subfolder").toString()},
                                {"conflict", values.value("conflict")}};
    return runInProgressWindow(requests, {});
}

void ArchiveApp::extractHere(const QStringList &archives) { extractInto(archives, {}); }

void ArchiveApp::extractTo(const QStringList &archives) {
    const QStringList packages = firstParts(archives);
    if (packages.isEmpty())
        return;
    ++m_choosing;
    m_pendingExtract = packages;
    choose(this, "destination", folderOf(packages.first()));
}

QList<int> ArchiveApp::compressNow(const QStringList &items) {
    if (items.isEmpty())
        return {};
    const QVariantMap defaults = compressDefaults(items);
    const QString folder = defaults.value("folder").toString();
    // One click, no questions: the name gets a number when it is taken.
    const QString name = freeName(folder, defaults.value("name").toString() + ".zip");
    QVariantMap choices{{"name", name}, {"folder", folder}, {"format", "zip"}, {"levelStep", 3}};
    QVariantList requests = compressRequests(items, choices);
    return runInProgressWindow(requests, {});
}

void ArchiveApp::needsAnswer(int id) {
    // A window that started the task asks itself; with that one gone, the task gets its own.
    auto *window = qobject_cast<QWindow *>(m_queue.owner(id));
    if (!window || !window->isVisible())
        openTaskWindow(id);
}

void ArchiveApp::finished(int id) {
    const QVariantMap task = m_queue.task(id);
    const QString state = task.value("status").toString();
    if (state == "done" && task.value("openAfter").toBool() && !task.value("target").toString().isEmpty())
        reveal(task.value("target").toString(), task.value("targetIsFolder").toBool());
    if (state == "cancelled" || !prefs().value("notify").toBool())
        return;
    // The window that started it shows the outcome itself while someone looks at it.
    auto *window = qobject_cast<QWindow *>(m_queue.owner(id));
    if (window && window->isVisible() && window->isActive() && !m_queue.closesOnSuccess(id))
        return;
    if (window && m_queue.closesOnSuccess(id) && state != "done")
        return; // a progress window stays open on failure and says so there
    const QString kind = task.value("kind").toString();
    const bool ok = state == "done";
    const QString title = kind == "extract" ? (ok ? "解压完成" : "解压失败")
                          : kind == "test"  ? (ok ? "测试通过" : "测试未通过")
                                            : (ok ? "压缩完成" : "压缩失败");
    const QString body = task.value("archiveName").toString() + "\n" + task.value("message").toString();
    if (ok && !task.value("target").toString().isEmpty()) {
        const QString target = task.value("target").toString();
        const bool folder = task.value("targetIsFolder").toBool();
        m_notifier.notify(title, body, false, kind == "test" ? QString() : "打开位置",
                          kind == "test" ? std::function<void()>() : [this, target, folder] { reveal(target, folder); });
    } else {
        m_notifier.notify(title, body, !ok, ok ? QString() : "查看详情",
                          ok ? std::function<void()>() : [this, id] { openTaskWindow(id); });
    }
}

bool ArchiveApp::exists(const QString &path) const { return QFileInfo::exists(localPath(path)); }

QString ArchiveApp::localPath(const QString &location) const {
    const QUrl url(location);
    return url.isLocalFile() ? url.toLocalFile() : location;
}

bool ArchiveApp::looksLikeArchive(const QString &location) const {
    const QFileInfo info(localPath(location));
    return info.isFile() && nook::archive::looksLikeArchive(info.fileName());
}

QString ArchiveApp::folderOf(const QString &path) const { return QFileInfo(localPath(path)).absolutePath(); }

QString ArchiveApp::freeName(const QString &folder, const QString &name) const {
    const QDir dir(folder);
    if (!dir.exists(name))
        return name;
    // "报告.tar.gz" becomes "报告 (2).tar.gz": the number goes before the whole suffix.
    QString stem = name, suffix;
    for (const auto &format : Formats)
        if (name.endsWith(format.second) && name.size() > format.second.size()) {
            stem = name.chopped(format.second.size());
            suffix = format.second;
            break;
        }
    for (int n = 2; n < 10000; ++n) {
        const QString candidate = QStringLiteral("%1 (%2)%3").arg(stem).arg(n).arg(suffix);
        if (!dir.exists(candidate))
            return candidate;
    }
    return name;
}

QVariantList ArchiveApp::formats() const {
    static const QHash<QString, QString> notes{
        {"zip", "兼容性最好，Windows 和 macOS 直接打开"}, {"7z", "压缩率高，可加密文件名、分卷"},
        {"tar.gz", "Linux 常用，速度快"},                 {"tar.xz", "Linux 常用，压缩率高"},
        {"tar.zst", "速度快，压缩率也不错"},              {"tar.bz2", "较旧的 Linux 格式"},
        {"tar", "只打包，不压缩"},                        {"gz", "单个文件"},
        {"xz", "单个文件"},                               {"zst", "单个文件"},
        {"bz2", "单个文件"}};
    QVariantList list;
    for (const auto &[format, suffix] : Formats)
        list << QVariantMap{{"value", format},
                            {"suffix", suffix},
                            {"note", notes.value(format)},
                            {"single", format == "gz" || format == "xz" || format == "zst" || format == "bz2"},
                            {"encrypts", format == "zip" || format == "7z"},
                            {"levels", format != "tar"},
                            {"volumes", format == "7z"}};
    return list;
}

QString ArchiveApp::suffixFor(const QString &format) const {
    for (const auto &[value, suffix] : Formats)
        if (value == format)
            return suffix;
    return {};
}

int ArchiveApp::levelFor(const QString &format, int step) const {
    step = qBound(0, step, 5);
    static const QHash<QString, std::array<int, 6>> levels{
        {"zip", {0, 1, 3, 5, 7, 9}},    {"7z", {0, 1, 3, 5, 7, 9}},     {"gz", {0, 1, 3, 6, 8, 9}},
        {"xz", {0, 1, 3, 6, 8, 9}},     {"bz2", {1, 1, 3, 6, 8, 9}},    {"zst", {1, 1, 2, 3, 9, 19}}};
    const QString filter = format.startsWith(u"tar.") ? format.mid(4) : format;
    const auto it = levels.find(filter);
    return it == levels.end() ? -1 : it.value()[size_t(step)];
}

QVariantMap ArchiveApp::compressDefaults(const QStringList &items) const {
    if (items.isEmpty())
        return {};
    const QFileInfo first(localPath(items.first()));
    const QString folder = first.absolutePath();
    QStringList paths;
    for (const auto &item : items)
        paths << localPath(item);
    const QString name = nook::archive::suggestedName(paths);
    return {{"name", name},
            {"folder", folder},
            {"single", items.size() == 1 && first.isFile()},
            {"fileName", first.fileName()},
            {"writable", QFileInfo(folder).isWritable()}};
}

QVariantList ArchiveApp::compressRequests(const QStringList &items, const QVariantMap &choices) const {
    const QString format = choices.value("format").toString();
    const QString suffix = suffixFor(format);
    const bool single = format == "gz" || format == "xz" || format == "zst" || format == "bz2";
    const QString folder = choices.value("folder").toString();
    QVariantMap options;
    const int level = levelFor(format, choices.value("levelStep").toInt());
    if (level >= 0)
        options.insert("level", level);
    if (format == "7z") {
        options.insert("encryptNames", choices.value("encryptNames").toBool());
        if (choices.value("volumeSize").toDouble() > 0)
            options.insert("volumeSize", choices.value("volumeSize").toDouble());
    }
    if (format == "zip")
        options.insert("zipEncryption", choices.value("zipCrypto").toBool() ? "ZipCrypto" : "AES256");
    const auto request = [&](const QStringList &group, QString name) {
        if (single && name.isEmpty())
            name = QFileInfo(group.first()).fileName();
        if (!name.endsWith(suffix, Qt::CaseInsensitive))
            name += suffix;
        QVariantMap map{{"op", "create"},
                        {"format", format},
                        {"output", folder + u'/' + name},
                        {"items", group},
                        {"conflict", "ask"},
                        {"testAfter", choices.value("testAfter").toBool()},
                        {"options", options}};
        const QString password = choices.value("password").toString();
        if (!password.isEmpty() && (format == "zip" || format == "7z"))
            map.insert("password", password);
        return QVariant(map);
    };
    QStringList paths;
    for (const auto &item : items)
        paths << QFileInfo(localPath(item)).absoluteFilePath();
    QVariantList requests;
    if (choices.value("separate").toBool() && paths.size() > 1) {
        for (const auto &path : std::as_const(paths)) {
            const QVariantMap defaults = compressDefaults({path});
            requests << request({path}, single ? QString() : defaults.value("name").toString());
        }
    } else {
        requests << request(paths, choices.value("name").toString().trimmed());
    }
    return requests;
}

