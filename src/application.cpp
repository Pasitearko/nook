#include "application.h"
#include "archivenames.h"
#include "storage.h"
#include <QClipboard>
#include <QDesktopServices>
#include <QDir>
#include <QFileInfo>
#include <QPointer>
#include <QElapsedTimer>
#include <QDirIterator>
#include <QGuiApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMimeData>
#include <QMimeDatabase>
#include <QStandardPaths>
#include <QStorageInfo>
#include <QStyleHints>
#include <QUrl>
#include <QSaveFile>
#include <QStringDecoder>
#include <algorithm>
#include <utility>
#include <sys/stat.h>
#include <unistd.h>
#include <cmath>

extern char **environ;
#include <QtConcurrent>

AppSettings::AppSettings(QObject *parent)
    : QObject(parent),
      m_store(nook::customProfile()
                  ? std::make_unique<QSettings>(nook::storageDirectory(nook::Storage::Config) +
                                                    "/preferences.ini", QSettings::IniFormat)
                  : std::make_unique<QSettings>(QSettings::IniFormat, QSettings::UserScope,
                                                "nook", "preferences")) {
    connect(&m_appearance, &SystemAppearance::changed, this, &AppSettings::systemDarkChanged);
}
QVariantMap AppSettings::values() const {
    // An INI file stores everything as text: a saved `false` comes back as the string
    // "false", which QML treats as true. Restore the real types on the way out.
    const auto flag = [this](const char *key, bool fallback) { return m_store->value(key, fallback).toBool(); };
    const auto list = [this](const char *key) { return m_store->value(key, QStringList{}).toStringList(); };
    QVariantList widths;
    for (const auto &width : m_store->value("columnWidths", QVariantList{}).toList())
        widths.append(width.toInt());
    return {{"theme", m_store->value("theme", "system").toString()},
            {"effects", flag("effects", true)},
            {"animations", flag("animations", true)},
            {"dense", flag("dense", false)},
            {"hoverPreview", flag("hoverPreview", false)},
            {"columnWidths", widths},
            {"scrollSpeed", m_store->value("scrollSpeed", 3).toInt()},
            {"quickAccess", list("quickAccess")},
            {"quickAccessOrder", list("quickAccessOrder")},
            {"uiScale", m_store->value("uiScale", 1.0).toDouble()},
            // The file view's own size, one for the list and one for the grid. Until first set it
            // is the interface scale, which used to size the file view too, so nothing jumps.
            {"listZoom", m_store->value("listZoom", m_store->value("uiScale", 1.0)).toDouble()},
            {"gridZoom", m_store->value("gridZoom", m_store->value("uiScale", 1.0)).toDouble()},
            {"startFolder", m_store->value("startFolder", QDir::homePath()).toString()},
            {"hiddenPlaces", list("hiddenPlaces")},
            {"backgroundIndex", flag("backgroundIndex", true)},
            {"ntfsJournal", flag("ntfsJournal", false)},
            {"chooserGrid", flag("chooserGrid", false)},
            {"detailsPane", flag("detailsPane", true)},
            {"detailsWidth", m_store->value("detailsWidth", 304).toInt()},
            {"sidebarWidth", m_store->value("sidebarWidth", 224).toInt()},
            {"sidebarSections", m_store->value("sidebarSections", QStringList{"places", "tags", "disks"}).toStringList()},
            {"indexSystem", flag("indexSystem", false)},
            {"indexNoise", flag("indexNoise", false)},
            {"roots", list("roots")},
            {"exclusions", list("exclusions")}};
}
void AppSettings::set(const QString &key, const QVariant &value) {
    if (!values().contains(key))
        return;
    QVariant safeValue = value;
    if (key == "uiScale") {
        const auto scale = value.toDouble();
        if (!std::isfinite(scale)) return;
        safeValue = qBound(0.75, scale, 1.5);
    }
    if (key == "listZoom" || key == "gridZoom") {
        const auto zoom = value.toDouble();
        if (!std::isfinite(zoom)) return;
        safeValue = key == "listZoom" ? qBound(0.8, zoom, 1.6) : qBound(0.6, zoom, 2.0);
    }
    if (key == "scrollSpeed")
        safeValue = qBound(1, value.toInt(), 8);
    if (key == "detailsWidth")
        safeValue = qBound(260, value.toInt(), 560);
    if (key == "sidebarWidth")
        safeValue = qBound(150, value.toInt(), 380);
    if (key == "sidebarSections") {
        // The three sections, each once, in the order given.
        QStringList order;
        for (const auto &name : value.toStringList())
            if (QStringList{"places", "tags", "disks"}.contains(name) && !order.contains(name))
                order << name;
        if (order.size() != 3)
            return;
        safeValue = order;
    }
    if (key == "columnWidths") {
        const auto widths = value.toList();
        if (!widths.isEmpty() && widths.size() != 4)
            return;
        QVariantList bounded;
        const int minimums[] = {140, 100, 70, 65};
        for (int i = 0; i < widths.size(); ++i)
            bounded.append(qBound(minimums[i], widths[i].toInt(), 1600));
        safeValue = bounded;
    }
    m_store->setValue(key, safeValue);
    m_store->sync();
    Q_EMIT changed();
    if (key == "columnWidths" && safeValue.toList().isEmpty())
        Q_EMIT columnWidthsReset();
}
namespace {
QStringList quickAccessState(AppSettings *settings) {
    const auto values = settings->values();
    return values["quickAccess"].toStringList() + QStringList{"|"} + values["quickAccessOrder"].toStringList() +
           QStringList{"|"} + values["hiddenPlaces"].toStringList();
}
} // namespace
Application::Application(SearchIndex *search, Operations *ops, AppSettings *settings, bool restore,
                         QObject *parent, const QJsonObject &state)
    : QObject(parent), m_search(search), m_ops(ops), m_settings(settings), m_preview(this), m_glance(this) {
    m_search = search->createSession(this);
    // Migrate away from visit tracking; executable undo history stays in memory.
    settings->store().remove("recentLocations");
    settings->store().remove("rememberRecentLocations");
    m_saveTimer.setSingleShot(true);
    m_saveTimer.setInterval(800);
    connect(&m_saveTimer, &QTimer::timeout, this, &Application::saveSession);
    connect(m_ops, &Operations::finished, this, [this] {
        for (const auto &tab : m_tabs) {
            tab.left->refresh();
            if (tab.dual)
                tab.right->refresh();
        }
    });
    m_installedVersion = QCoreApplication::applicationVersion();
    const QJsonArray saved = !state.isEmpty() ? state["tabs"].toArray()
                             : restore ? QJsonDocument::fromJson(settings->store().value("session").toByteArray()).array()
                                       : QJsonArray();
    for (const auto &value : saved) {
        auto object = value.toObject();
        addTab(object["left"].toString());
        m_tabs.last().right->navigate(object["right"].toString(home()));
        m_tabs.last().dual = object["dual"].toBool();
        m_tabs.last().grid = object["grid"].toBool();
    }
    if (m_tabs.isEmpty())
        addTab(settings->values()["startFolder"].toString());
    m_active = qBound(0, state["active"].toInt(), int(m_tabs.size()) - 1);
    connect(&m_diskMounts, &DiskMounts::mountedChanged, this, &Application::refreshPlaces);
    connect(&m_properties, &FileProperties::error, this, &Application::notice);
    connect(&m_properties, &FileProperties::hiddenApplied, this, [this](bool hidden) {
        Q_EMIT notice(hidden ? QStringLiteral("已设为隐藏") : QStringLiteral("已取消隐藏"));
        for (const auto &tab : m_tabs) { tab.left->refresh(); if (tab.dual) tab.right->refresh(); }
    });
    connect(&m_properties, &FileProperties::permissionsApplied, this, [this] {
        Q_EMIT notice(QStringLiteral("权限已更新"));
        for (const auto &tab : m_tabs) { tab.left->refresh(); if (tab.dual) tab.right->refresh(); }
    });
    auto applyScrollSpeed = [this] {
        QGuiApplication::styleHints()->setWheelScrollLines(
            3 * qBound(1, m_settings->values()["scrollSpeed"].toInt(), 8));
    };
    applyScrollSpeed();
    connect(m_settings, &AppSettings::changed, this, applyScrollSpeed);
    connect(m_settings, &AppSettings::changed, this,
            [this, previous = quickAccessState(m_settings)]() mutable {
                const auto current = quickAccessState(m_settings);
                if (current != previous) {
                    previous = current;
                    refreshPlaces();
                }
            });

    refreshPlaces();
}
Application::~Application() {
    if (m_describing)
        *m_describing = true;
    for (auto *watcher : m_openWatchers) {
        watcher->disconnect(this);
        watcher->waitForFinished();
    }
    // Stop discovery/mount helpers before members and their captured pointers are torn down.
    for (auto *process : findChildren<QProcess *>(QString(), Qt::FindDirectChildrenOnly)) {
        process->disconnect(this);
        if (process->state() != QProcess::NotRunning) {
            process->kill();
            process->waitForFinished(1000);
        }
    }
    saveSession();
}
namespace {
const QString kVolumeHelper = QStringLiteral("/usr/lib/nook/nook-volume-helper");
const QString kVolumeSetup = QStringLiteral("/usr/lib/nook/nook-volume-setup");
} // namespace
bool Application::ntfsInstalled() const {
    return QFileInfo(kVolumeHelper).isExecutable() && QFileInfo(kVolumeSetup).isExecutable();
}
void Application::setNtfsJournal(bool enabled) {
    if (m_ntfsSetup)
        return;
    if (!enabled)
        m_settings->set("ntfsJournal", false); // stop reading partitions now; the rule goes next
    if (!ntfsInstalled()) {
        if (enabled)
            Q_EMIT notice(QStringLiteral("NTFS 极速索引需要软件包里的读取助手（%1），请先安装 nook 软件包")
                              .arg(QFileInfo(kVolumeHelper).absolutePath()));
        return;
    }
    m_ntfsSetup = new QProcess(this);
    auto *process = m_ntfsSetup;
    process->setProcessChannelMode(QProcess::MergedChannels);
    auto done = [this, process, enabled](bool ok) {
        const QString output = QString::fromLocal8Bit(process->readAll()).trimmed().left(500);
        const int code = process->exitCode();
        m_ntfsSetup = nullptr;
        process->deleteLater();
        Q_EMIT ntfsBusyChanged();
        if (enabled && ok) {
            m_settings->set("ntfsJournal", true);
            Q_EMIT notice(QStringLiteral("已开启 NTFS 极速索引：之后读取 Windows 分区无需再输入密码"));
        } else if (enabled)
            Q_EMIT notice(code == 126 || code == 127 ? QStringLiteral("已取消授权，NTFS 极速索引未开启")
                                                     : QStringLiteral("NTFS 极速索引开启失败：%1").arg(output));
        else if (ok)
            Q_EMIT notice(QStringLiteral("已关闭 NTFS 极速索引，并移除了免密授权规则"));
        else
            Q_EMIT notice(QStringLiteral("NTFS 极速索引已关闭，但免密规则未移除；可稍后运行 pkexec %1 disable")
                              .arg(kVolumeSetup));
    };
    connect(process, &QProcess::finished, this,
            [process, done](int code, QProcess::ExitStatus status) { done(status == QProcess::NormalExit && code == 0); });
    connect(process, &QProcess::errorOccurred, this, [done](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart)
            done(false);
    });
    // The session's polkit agent shows the password prompt.
    process->start(QStringLiteral("/usr/bin/pkexec"),
                   {QStringLiteral("--disable-internal-agent"), kVolumeSetup,
                    enabled ? QStringLiteral("enable") : QStringLiteral("disable")});
    Q_EMIT ntfsBusyChanged();
}
QString Application::home() const {
    return QDir::homePath();
}
QVariantList Application::tabs() const {
    QVariantList result;
    for (const auto &tab : m_tabs)
        result.append(QVariantMap{{"title", tab.left->title()}, {"uri", tab.left->uri()}});
    return result;
}
DirectoryModel *Application::left() const {
    return m_tabs.isEmpty() ? nullptr : m_tabs[m_active].left;
}
DirectoryModel *Application::right() const {
    return m_tabs.isEmpty() ? nullptr : m_tabs[m_active].right;
}
DirectoryModel *Application::activePane() const {
    return m_side == 1 && dual() ? right() : left();
}
bool Application::dual() const {
    return !m_tabs.isEmpty() && m_tabs[m_active].dual;
}
bool Application::grid() const {
    return !m_tabs.isEmpty() && m_tabs[m_active].grid;
}
void Application::connectPane(DirectoryModel *pane) {
    connect(pane, &DirectoryModel::locationChanged, this, [this] {
        Q_EMIT tabsChanged();
        Q_EMIT activePaneChanged();
        m_saveTimer.start();
    });
}
void Application::addTab(const QString &uri) {
    Tab tab{new DirectoryModel(this), new DirectoryModel(this)};
    m_tabs.append(tab);
    m_active = m_tabs.size() - 1;
    connectPane(tab.left);
    connectPane(tab.right);
    tab.left->navigate(uri.isEmpty() ? m_settings->values()["startFolder"].toString() : uri);
    m_side = 0;
    Q_EMIT tabsChanged();
    Q_EMIT activePaneChanged();
    m_saveTimer.start();
}
void Application::closeTab(int index) {
    if (index < 0 || index >= m_tabs.size())
        return;
    if (m_tabs.size() == 1) {
        m_tabs[0].left->navigate(home());
        return;
    }
    auto tab = m_tabs.takeAt(index);
    tab.left->deleteLater();
    tab.right->deleteLater();
    if (index < m_active)
        --m_active;
    m_active = qBound(0, m_active, int(m_tabs.size() - 1));
    m_side = 0;
    Q_EMIT tabsChanged();
    Q_EMIT activePaneChanged();
    m_saveTimer.start();
}
void Application::moveTab(int from, int to) {
    if (from < 0 || from >= m_tabs.size() || to < 0 || to >= m_tabs.size() || from == to)
        return;
    m_tabs.move(from, to);
    if (m_active == from)
        m_active = to;
    else if (from < m_active && to >= m_active)
        --m_active;
    else if (from > m_active && to <= m_active)
        ++m_active;
    Q_EMIT tabsChanged();
    m_saveTimer.start();
}
void Application::setActiveTab(int index) {
    if (index < 0 || index >= m_tabs.size())
        return;
    m_active = index;
    m_side = 0;
    Q_EMIT tabsChanged();
    Q_EMIT activePaneChanged();
}
void Application::setActiveSide(int side) {
    m_side = side == 1 && dual() ? 1 : 0;
    Q_EMIT activePaneChanged();
}
void Application::setDual(bool enabled) {
    if (m_tabs.isEmpty())
        return;
    m_tabs[m_active].dual = enabled;
    if (enabled && right()->uri().isEmpty())
        right()->navigate(left()->uri());
    if (!enabled)
        m_side = 0;
    Q_EMIT tabsChanged();
    Q_EMIT activePaneChanged();
    m_saveTimer.start();
}
void Application::setGrid(bool enabled) {
    if (m_tabs.isEmpty())
        return;
    m_tabs[m_active].grid = enabled;
    Q_EMIT tabsChanged();
    m_saveTimer.start();
}
void Application::navigate(const QString &uri) {
    if (activePane())
        activePane()->navigate(uri);
}
void Application::openItem(const QVariantMap &entry) {
    QString uri = entry["fileUri"].toString();
    if (uri.isEmpty())
        return;
    if (entry["folder"].toBool())
        navigate(uri);
    else
        openExternal(uri);
}
void Application::openExternal(const QString &uri) {
    const QUrl url(uri);
    if (!url.isValid() || url.scheme().isEmpty()) {
        Q_EMIT notice(QStringLiteral("无法打开：无效地址"));
        return;
    }
    auto *watcher = new QFutureWatcher<QString>(this);
    m_openWatchers.append(watcher);
    connect(watcher, &QFutureWatcher<QString>::finished, this, [this, watcher] {
        const auto error = watcher->result();
        m_openWatchers.removeAll(watcher);
        watcher->deleteLater();
        if (!error.isEmpty()) Q_EMIT notice(error);
    });
    watcher->setFuture(QtConcurrent::run([uri, local = url.isLocalFile(), scheme = url.scheme()] {
        auto *file = g_file_new_for_uri(uri.toUtf8().constData());
        GError *error = nullptr;
        auto *handler = local ? g_file_query_default_handler(file, nullptr, &error)
                              : g_app_info_get_default_for_uri_scheme(scheme.toUtf8().constData());
        QString result;
        if (!handler) {
            result = error && !g_error_matches(error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED)
                         ? QStringLiteral("查询默认应用失败：") + QString::fromUtf8(error->message)
                         : QStringLiteral("未设置此类型的默认应用，请使用「打开方式」选择应用。");
        } else {
            auto *context = g_app_launch_context_new();
            GList files{file, nullptr, nullptr};
            auto encoded = uri.toUtf8();
            GList uris{encoded.data(), nullptr, nullptr};
            const bool launched = local && g_app_info_supports_files(handler)
                                      ? g_app_info_launch(handler, &files, context, &error)
                                      : g_app_info_launch_uris(handler, &uris, context, &error);
            if (!launched) result = QStringLiteral("打开失败：") + QString::fromUtf8(error ? error->message : "未知错误");
            g_object_unref(context);
            g_object_unref(handler);
        }
        if (error) g_error_free(error);
        g_object_unref(file);
        return result;
    }));
}
void Application::reveal(const QString &uri) {
    navigate(nook::parentUri(uri));
    activePane()->selectUri(uri);
}
bool Application::moveModifier() const {
    return QGuiApplication::keyboardModifiers().testFlag(Qt::ShiftModifier);
}
int Application::modifiers() const {
    return int(QGuiApplication::queryKeyboardModifiers());
}
void Application::terminal(const QString &uri) {
    auto path = nook::localPath(uri.isEmpty() ? activePane()->uri() : uri);
    if (path.isEmpty()) {
        Q_EMIT notice(QStringLiteral("此位置无法打开终端"));
        return;
    }
    // kitty is a Python program and quits before its window opens when any variable it hands to
    // the shell is not valid UTF-8 (a shell can leave "_" holding a mangled path), so it gets
    // only the variables that are, and never "_", which names nook and not kitty.
    QProcessEnvironment environment;
    for (char **entry = environ; *entry; ++entry) {
        QStringDecoder utf8(QStringDecoder::Utf8);
        const QString text = utf8(QByteArrayView(*entry));
        const qsizetype equals = text.indexOf('=');
        if (utf8.hasError() || equals <= 0 || text.startsWith(QLatin1String("_=")))
            continue;
        environment.insert(text.left(equals), text.mid(equals + 1));
    }
    QProcess kitty;
    kitty.setProgram(QStringLiteral("kitty"));
    kitty.setArguments({QStringLiteral("--directory"), path});
    kitty.setWorkingDirectory(path);
    kitty.setProcessEnvironment(environment);
    kitty.setStandardInputFile(QProcess::nullDevice());
    if (!kitty.startDetached())
        Q_EMIT notice(QStringLiteral("无法启动 kitty，请检查安装"));
}
void Application::copy(bool cut, DirectoryModel *source) {
    if (!source)
        source = activePane();
    if (!source || source->selectedUris().isEmpty())
        return;
    auto *mime = new QMimeData;
    QList<QUrl> urls;
    QByteArray gnome = cut ? "cut\n" : "copy\n";
    for (const auto &uri : source->selectedUris()) {
        urls.append(QUrl(uri));
        gnome += uri.toUtf8() + '\n';
    }
    mime->setUrls(urls);
    mime->setData("x-special/gnome-copied-files", gnome);
    mime->setData("application/x-kde-cutselection", cut ? "1" : "0");
    QGuiApplication::clipboard()->setMimeData(mime);
    Q_EMIT notice(cut ? QStringLiteral("已剪切选中项目") : QStringLiteral("已复制选中项目"));
}
void Application::paste() {
    if (!activePane() || nook::tagUri(activePane()->uri()))
        return;
    const auto *mime = QGuiApplication::clipboard()->mimeData();
    QStringList uris;
    for (const auto &url : mime->urls())
        if (url.isLocalFile())
            uris.append(url.toString(QUrl::FullyEncoded));
    if (uris.isEmpty()) {
        Q_EMIT notice(QStringLiteral("剪贴板没有本地文件"));
        return;
    }
    bool move = mime->data("x-special/gnome-copied-files").startsWith("cut\n") ||
                mime->data("application/x-kde-cutselection") == "1";
    drop(uris, activePane()->uri(), move);
}
void Application::drop(const QStringList &uris, const QString &destination, bool move) {
    if (destination.startsWith("trash:"))
        m_ops->run("trash", uris);
    else
        m_ops->run(move ? "move" : "copy", uris, destination);
}
QStringList Application::dropSources(const QStringList &uris, const QString &destination) const {
    QStringList result;
    const auto target = nook::normalizeUri(QUrl(destination).toString(QUrl::FullyEncoded));
    for (const auto &value : uris) {
        const auto uri = nook::normalizeUri(QUrl(value).toString(QUrl::FullyEncoded));
        // Dropping onto itself or back into the folder it came from changes nothing.
        if (uri.isEmpty() || uri == target || nook::parentUri(uri) == target || result.contains(uri))
            continue;
        result.append(uri);
    }
    return result;
}
bool Application::canDrop(const QStringList &uris, const QString &destination) const {
    return !destination.isEmpty() && !nook::tagUri(destination) && !dropSources(uris, destination).isEmpty();
}
QString Application::dropAction(const QStringList &uris, const QString &destination) const {
    const auto sources = dropSources(uris, destination);
    if (sources.isEmpty())
        return {};
    if (destination.startsWith("trash:"))
        return QStringLiteral("trash");
    const auto modifiers = QGuiApplication::keyboardModifiers();
    if (modifiers.testFlag(Qt::ControlModifier))
        return QStringLiteral("copy");
    if (modifiers.testFlag(Qt::ShiftModifier))
        return QStringLiteral("move");
    struct stat target{};
    const auto targetPath = QFile::encodeName(nook::localPath(destination));
    if (targetPath.isEmpty() || ::stat(targetPath.constData(), &target) != 0)
        return QStringLiteral("copy");
    for (const auto &uri : sources) {
        if (uri.startsWith("trash:"))
            continue;
        struct stat source{};
        const auto path = QFile::encodeName(nook::localPath(uri));
        if (path.isEmpty() || ::lstat(path.constData(), &source) != 0 || source.st_dev != target.st_dev)
            return QStringLiteral("copy");
    }
    return QStringLiteral("move");
}
void Application::dragDrop(const QStringList &uris, const QString &destination) {
    const auto action = dropAction(uris, destination);
    const auto sources = dropSources(uris, destination);
    if (action.isEmpty() || sources.isEmpty())
        return;
    if (action == "trash")
        m_ops->run("trash", sources);
    else
        m_ops->run(action, sources, destination);
}
void Application::copyPaths(const QStringList &uris) {
    QStringList paths;
    for (const auto &uri : uris) {
        const auto path = nook::localPath(uri);
        paths.append(path.isEmpty() ? uri : path);
    }
    if (paths.isEmpty())
        return;
    QGuiApplication::clipboard()->setText(paths.join('\n'));
    Q_EMIT notice(paths.size() == 1 ? QStringLiteral("已复制路径：") + paths.first()
                                    : QStringLiteral("已复制 %1 个路径").arg(paths.size()));
}
bool Application::openLocation(const QString &text) {
    QString input = text.trimmed();
    if (input.size() >= 2 && ((input.startsWith('"') && input.endsWith('"')) ||
                              (input.startsWith('\'') && input.endsWith('\''))))
        input = input.mid(1, input.size() - 2);
    if (input.isEmpty())
        return false;
    if (input == QStringLiteral("回收站") || input.startsWith("trash:")) {
        navigate("trash:///");
        return true;
    }
    if (!input.startsWith('/') && !input.startsWith('~') && !input.contains("://")) {
        const auto base = nook::localPath(activePane() ? activePane()->uri() : QString());
        input = (base.isEmpty() ? QDir::homePath() : base) + '/' + input;
    }
    const auto uri = nook::normalizeUri(input);
    const QFileInfo info(nook::localPath(uri));
    if (!QUrl(uri).isLocalFile() || !info.exists()) {
        Q_EMIT notice(QStringLiteral("路径不存在：") + text.trimmed());
        return false;
    }
    if (info.isDir())
        navigate(uri);
    else
        reveal(uri);
    return true;
}
bool Application::nameAvailable(const QString &directoryUri, const QString &name) const {
    if (!nook::validName(name))
        return false;
    const auto path = nook::localPath(nook::childUri(directoryUri, name));
    struct stat info{};
    return path.isEmpty() || ::lstat(QFile::encodeName(path).constData(), &info) != 0;
}
void Application::refreshPlaces() {
    m_places.clear();
    const auto hiddenPlaces = m_settings->values()["hiddenPlaces"].toStringList();
    auto add = [&](QString title, QString path, QString icon) {
        if (QFileInfo::exists(path) && !hiddenPlaces.contains(nook::normalizeUri(path)))
            m_places.append(QVariantMap{
                {"title", title}, {"uri", nook::normalizeUri(path)}, {"icon", icon}, {"disk", false}});
    };
    add("主目录", home(), "home");
    add("桌面", QStandardPaths::writableLocation(QStandardPaths::DesktopLocation), "folder");
    add("下载", QStandardPaths::writableLocation(QStandardPaths::DownloadLocation), "download");
    add("文档", QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation), "document");
    add("图片", QStandardPaths::writableLocation(QStandardPaths::PicturesLocation), "image");
    add("音乐", QStandardPaths::writableLocation(QStandardPaths::MusicLocation), "music");
    if (!hiddenPlaces.contains("trash:///"))
        m_places.append(
            QVariantMap{{"title", "回收站"}, {"uri", "trash:///"}, {"icon", "trash"}, {"disk", false}});
    for (const auto &uri : m_settings->values()["quickAccess"].toStringList()) {
        const auto duplicate = std::any_of(m_places.cbegin(), m_places.cend(), [&](const QVariant &p) {
            return p.toMap()["uri"].toString() == uri;
        });
        if (!duplicate)
            m_places.append(QVariantMap{{"title", nook::displayName(uri)},
                                        {"uri", uri},
                                        {"icon", "folder"},
                                        {"disk", false},
                                        {"quick", true}});
    }
    const auto order = m_settings->values()["quickAccessOrder"].toStringList();
    std::stable_sort(m_places.begin(), m_places.end(), [&order](const QVariant &a, const QVariant &b) {
        auto rank = [&order](const QVariant &p) { int i = order.indexOf(p.toMap()["uri"].toString()); return i < 0 ? order.size() : i; };
        return rank(a) < rank(b);
    });
    QStorageInfo root("/");
    m_places.append(QVariantMap{
        {"title", "Linux · 文件系统"},
        {"uri", "file:///"},
        {"icon", "drive"},
        {"disk", true},
        {"capacity",
         nook::formatSize(root.bytesAvailable()) + " 可用 / " + nook::formatSize(root.bytesTotal())},
        {"used", root.bytesTotal() > 0 ? 1.0 - double(root.bytesAvailable()) / root.bytesTotal() : 0.0}});
    Q_EMIT placesChanged();
    if (m_diskProbe)
        return;
    // Sidebar discovery is intentionally independent of the index scan roots.
    m_diskProbe = new QProcess(this);
    auto *probe = m_diskProbe;
    auto *timeout = new QTimer(probe);
    timeout->setSingleShot(true);
    connect(timeout, &QTimer::timeout, probe, &QProcess::kill);
    connect(probe, &QProcess::errorOccurred, this, [this, probe](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart) {
            m_diskProbe = nullptr;
            probe->deleteLater();
        }
    });
    connect(
        probe, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this,
        [this, probe](int code, QProcess::ExitStatus status) {
            if (code == 0 && status == QProcess::NormalExit) {
                const auto devices = QJsonDocument::fromJson(probe->readAllStandardOutput())
                                         .object()["blockdevices"]
                                         .toArray();
                std::function<void(const QJsonArray &)> append = [&](const QJsonArray &nodes) {
                    for (const auto &node : nodes) {
                        const auto disk = node.toObject();
                        const auto fs = disk["fstype"].toString();
                        const auto label = disk["label"].toString();
                        if (nook::sidebarDataPartition(fs, disk["parttype"].toString(), label)) {
                            QString mount;
                            for (const auto &path : disk["mountpoints"].toArray())
                                if (!path.toString().isEmpty()) {
                                    mount = path.toString();
                                    break;
                                }
                            QStorageInfo volume(mount);
                            const auto device = disk["name"].toString();
                            const auto size = qint64(disk["size"].toDouble());
                            m_places.append(QVariantMap{
                                {"title", label.isEmpty()
                                              ? QStringLiteral("Windows · ") + nook::formatSize(size)
                                              : label},
                                {"uri", mount.isEmpty() ? QString() : nook::normalizeUri(mount)},
                                {"device", device},
                                {"icon", "drive"},
                                {"disk", true},
                                {"capacity", mount.isEmpty()
                                                 ? nook::formatSize(size) + " · 点击挂载"
                                                 : nook::formatSize(volume.bytesAvailable()) + " 可用 / " +
                                                       nook::formatSize(volume.bytesTotal())},
                                {"used", !mount.isEmpty() && volume.bytesTotal() > 0
                                             ? 1.0 - double(volume.bytesAvailable()) / volume.bytesTotal()
                                             : 0.0}});
                        }
                        append(disk["children"].toArray());
                    }
                };
                append(devices);
            }
            m_diskProbe = nullptr;
            probe->deleteLater();
            Q_EMIT placesChanged();
        });
    probe->start("lsblk",
                 {"--json", "--bytes", "--paths", "--output", "NAME,FSTYPE,LABEL,PARTTYPE,SIZE,MOUNTPOINTS"});
    timeout->start(5000);
}
void Application::activatePlace(const QVariantMap &place) {
    const auto uri = place["uri"].toString();
    if (!uri.isEmpty()) {
        navigate(uri);
        return;
    }
    const auto device = place["device"].toString();
    if (m_mountBusy || !device.startsWith("/dev/"))
        return;
    m_mountBusy = true;
    auto *process = new QProcess(this);
    auto *timer = new QTimer(process);
    timer->setSingleShot(true);
    connect(timer, &QTimer::timeout, process, &QProcess::kill);
    connect(process, &QProcess::errorOccurred, this, [this, process](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart) {
            m_mountBusy = false;
            Q_EMIT notice(QStringLiteral("无法挂载：请安装 udisks2"));
            process->deleteLater();
        }
    });
    connect(process, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this,
            [this, process, device](int code, QProcess::ExitStatus status) {
                m_mountBusy = false;
                if (code == 0 && status == QProcess::NormalExit) {
                    for (auto volume : QStorageInfo::mountedVolumes()) {
                        volume.refresh();
                        if (volume.device() == QFile::encodeName(device)) {
                            navigate(nook::normalizeUri(volume.rootPath()));
                            break;
                        }
                    }
                    refreshPlaces();
                } else {
                    Q_EMIT notice(QStringLiteral("挂载失败（未尝试强制修复）：") +
                                  QString::fromLocal8Bit(process->readAllStandardError()));
                }
                process->deleteLater();
            });
    process->start("udisksctl", {"mount", "--block-device", device});
    timer->start(60000);
}
void Application::addQuickAccess(const QString &uri) {
    if (!QUrl(uri).isLocalFile() || !QFileInfo(nook::localPath(uri)).isDir()) {
        Q_EMIT notice(QStringLiteral("只能将本地文件夹添加到快速访问"));
        return;
    }
    const auto normalized = nook::normalizeUri(uri);
    auto hidden = m_settings->values()["hiddenPlaces"].toStringList();
    if (hidden.removeAll(normalized))
        m_settings->set("hiddenPlaces", hidden);
    auto entries = m_settings->values()["quickAccess"].toStringList();
    if (!entries.contains(normalized)) {
        entries.append(normalized);
        m_settings->set("quickAccess", entries);
    }
    Q_EMIT notice(QStringLiteral("已添加到快速访问"));
}
void Application::removeQuickAccess(const QString &uri) {
    // User-added folders leave the list; built-in places are only hidden, never deleted.
    auto entries = m_settings->values()["quickAccess"].toStringList();
    if (entries.removeAll(uri)) {
        m_settings->set("quickAccess", entries);
        return;
    }
    auto hidden = m_settings->values()["hiddenPlaces"].toStringList();
    if (!hidden.contains(uri)) {
        hidden.append(uri);
        m_settings->set("hiddenPlaces", hidden);
    }
}
void Application::resetQuickAccess() {
    m_settings->set("hiddenPlaces", QStringList{});
    Q_EMIT notice(QStringLiteral("已恢复默认快速访问项"));
}
void Application::reorderQuickAccess(const QString &sourceUri, const QString &targetUri) {
    QStringList order;
    for (const auto &place : m_places) {
        const auto map = place.toMap();
        if (!map["disk"].toBool()) order.append(map["uri"].toString());
    }
    const auto from = order.indexOf(sourceUri), to = order.indexOf(targetUri);
    if (from < 0 || to < 0 || from == to) return;
    order.move(from, to);
    m_settings->set("quickAccessOrder", order);
}
QVariantList Application::openWithApplications(const QString &uri) const {
    QVariantList result;
    if (!QUrl(uri).isLocalFile())
        return result;
    const auto mime = QMimeDatabase().mimeTypeForUrl(QUrl(uri)).name().toUtf8();
    char *type = g_content_type_from_mime_type(mime.constData());
    if (!type)
        return result;
    GList *apps = g_app_info_get_all_for_type(type);
    auto *defaultApp = g_app_info_get_default_for_type(type, false);
    for (GList *i = apps; i; i = i->next) {
        auto *app = G_APP_INFO(i->data);
        const auto *id = g_app_info_get_id(app);
        if (!id || !g_app_info_should_show(app) ||
            (!g_app_info_supports_files(app) && !g_app_info_supports_uris(app)))
            continue;
        QVariantMap entry{{"id", QString::fromUtf8(id)},
                          {"name", QString::fromUtf8(g_app_info_get_display_name(app))},
                          {"isDefault", defaultApp && g_app_info_equal(app, defaultApp)}};
        if (!result.contains(entry)) {
            if (entry["isDefault"].toBool())
                result.prepend(entry);
            else
                result.append(entry);
        }
    }
    if (defaultApp)
        g_object_unref(defaultApp);
    g_list_free_full(apps, g_object_unref);
    g_free(type);
    return result;
}
QVariantMap Application::archiveActions(const QStringList &uris) const {
    bool local = !uris.isEmpty(), packages = !uris.isEmpty();
    QStringList paths;
    for (const auto &uri : uris) {
        const QString path = uri.startsWith("trash:") ? QString() : nook::localPath(uri);
        if (path.isEmpty() || !QFileInfo::exists(path)) {
            local = packages = false;
            break;
        }
        paths << path;
        packages = packages && QFileInfo(path).isFile() && nook::archive::looksLikeArchive(QFileInfo(path).fileName());
    }
    return {{"extract", packages},
            {"compress", local},
            {"single", paths.size() == 1},
            {"zipName", local ? nook::archive::suggestedName(paths) + ".zip" : QString()},
            {"folderName", packages && paths.size() == 1 ? nook::archive::stemOf(QFileInfo(paths.first()).fileName()) : QString()}};
}

QStringList Application::archiveCommand(const QString &action, const QStringList &uris) const {
    const QString beside = QCoreApplication::applicationDirPath() + "/nook-archive";
    QStringList command{QFileInfo(beside).isExecutable() ? beside : QStringLiteral("nook-archive")};
    if (action != "open")
        command << "--" + action;
    command << "--";
    for (const auto &uri : uris)
        command << nook::localPath(uri);
    return command;
}

void Application::archive(const QString &action, const QStringList &uris) {
    if (uris.isEmpty())
        return;
    QStringList command = archiveCommand(action, uris);
    const QString program = command.takeFirst();
    if (!QProcess::startDetached(program, command))
        Q_EMIT notice(QStringLiteral("无法启动 nook解压缩，请检查安装"));
}

void Application::openWith(const QString &uri, const QString &applicationId) {
    if (!QUrl(uri).isLocalFile())
        return;
    const auto mime = QMimeDatabase().mimeTypeForUrl(QUrl(uri)).name().toUtf8();
    char *type = g_content_type_from_mime_type(mime.constData());
    if (!type)
        return;
    GList *apps = g_app_info_get_all_for_type(type);
    bool found = false;
    for (GList *i = apps; i; i = i->next) {
        auto *app = G_APP_INFO(i->data);
        if (applicationId != QString::fromUtf8(g_app_info_get_id(app)) || !g_app_info_should_show(app) ||
            (!g_app_info_supports_files(app) && !g_app_info_supports_uris(app)))
            continue;
        found = true;
        auto *file = g_file_new_for_uri(uri.toUtf8().constData());
        GList files{file, nullptr, nullptr};
        GError *error = nullptr;
        auto *context = g_app_launch_context_new();
        auto encodedUri = uri.toUtf8();
        GList uris{encodedUri.data(), nullptr, nullptr};
        const bool launched = g_app_info_supports_files(app)
                                  ? g_app_info_launch(app, &files, context, &error)
                                  : g_app_info_launch_uris(app, &uris, context, &error);
        if (!launched) {
            Q_EMIT notice(QStringLiteral("打开失败：") +
                          QString::fromUtf8(error ? error->message : "未知错误"));
            if (error)
                g_error_free(error);
        }
        g_object_unref(context);
        g_object_unref(file);
        break;
    }
    g_list_free_full(apps, g_object_unref);
    g_free(type);
    if (!found)
        Q_EMIT notice(QStringLiteral("此应用已不可用，请重新选择打开方式"));
}
QJsonObject Application::state() const {
    QJsonArray array;
    for (const auto &tab : m_tabs)
        array.append(QJsonObject{
            {"left", tab.left->uri()}, {"right", tab.right->uri()}, {"dual", tab.dual}, {"grid", tab.grid}});
    return {{"tabs", array}, {"active", m_active}};
}
void Application::saveSession() {
    if (!m_remembersSession)
        return;
    auto data = QJsonDocument(state()["tabs"].toArray()).toJson(QJsonDocument::Compact);
    if (m_settings->store().value("session").toByteArray() != data)
        m_settings->store().setValue("session", data);
}
namespace {
QString timeText(const QDateTime &time) {
    return time.isValid() ? time.toLocalTime().toString(QStringLiteral("yyyy-MM-dd HH:mm")) : QStringLiteral("—");
}
// A folder's contents added up, giving up after a while on very large trees (then "partial").
struct Tally {
    qint64 bytes = 0;
    int files = 0, folders = 0;
    bool partial = false;
};
void tally(const QString &path, Tally *total, const std::shared_ptr<std::atomic_bool> &cancel, QElapsedTimer &clock) {
    QDirIterator it(path, QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System,
                    QDirIterator::Subdirectories);
    while (it.hasNext()) {
        if (cancel->load() || clock.elapsed() > 4000) {
            total->partial = true;
            return;
        }
        const QFileInfo info = it.nextFileInfo();
        if (info.isDir() && !info.isSymLink())
            ++total->folders;
        else {
            ++total->files;
            total->bytes += info.isSymLink() ? 0 : info.size();
        }
    }
}
} // namespace
void Application::describe(const QStringList &uris) {
    if (uris == m_described && !m_facts.isEmpty())
        return;
    m_described = uris;
    if (m_describing)
        *m_describing = true;
    auto cancel = std::make_shared<std::atomic_bool>(false);
    m_describing = cancel;
    m_facts = {{"uris", uris}, {"busy", true}};
    Q_EMIT factsChanged();
    QPointer<Application> guard(this);
    (void)QtConcurrent::run([guard, uris, cancel] {
        QVariantMap facts{{"uris", uris}};
        QElapsedTimer clock;
        clock.start();
        Tally total;
        int folders = 0, files = 0;
        for (const auto &uri : uris) {
            const QString path = nook::localPath(uri);
            const QFileInfo info(path);
            if (path.isEmpty() || !(info.exists() || info.isSymLink()))
                continue;
            if (info.isDir() && !info.isSymLink()) {
                ++folders;
                tally(path, &total, cancel, clock);
            } else {
                ++files;
                total.bytes += info.isSymLink() ? 0 : info.size();
            }
            if (uris.size() == 1) {
                facts["name"] = info.fileName().isEmpty() ? path : info.fileName();
                facts["path"] = path;
                facts["location"] = info.absolutePath();
                facts["folder"] = info.isDir();
                facts["created"] = timeText(info.birthTime());
                facts["modified"] = timeText(info.lastModified());
                facts["accessed"] = timeText(info.lastRead());
                if (info.isDir()) {
                    // Direct contents, as the folder shows them.
                    const QDir dir(path);
                    facts["children"] = int(dir.entryList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System).size());
                }
            }
        }
        if (cancel->load())
            return;
        facts["folders"] = folders;
        facts["files"] = files;
        facts["bytes"] = total.bytes;
        facts["sizeText"] = (total.partial ? QStringLiteral("至少 ") : QString()) + nook::formatSize(total.bytes) +
                            (total.bytes >= 1024 ? QStringLiteral("（%L1 字节）").arg(total.bytes) : QString());
        facts["contents"] = total.files + total.folders > 0
                                ? QStringLiteral("%1 个文件，%2 个文件夹").arg(total.files).arg(total.folders)
                                : QString();
        facts["partial"] = total.partial;
        facts["busy"] = false;
        QMetaObject::invokeMethod(
            qApp,
            [guard, facts, cancel] {
                if (!guard || cancel->load())
                    return;
                guard->m_facts = facts;
                Q_EMIT guard->factsChanged();
            },
            Qt::QueuedConnection);
    });
}
void Application::openInNewTab(const QString &uri) {
    const QString path = nook::localPath(uri);
    if (path.isEmpty())
        return;
    if (QFileInfo(path).isDir()) {
        addTab(uri);
        return;
    }
    addTab(nook::parentUri(uri));
    left()->selectUri(uri);
}
void Application::checkInstalledVersion() {
    const bool upgraded = nook::executableReplaced();
    if (!upgraded) {
        if (m_upgraded || m_installedVersion != QCoreApplication::applicationVersion()) {
            m_upgraded = false;
            m_installedVersion = QCoreApplication::applicationVersion();
            Q_EMIT installedVersionChanged();
        }
        return;
    }
    if (m_versionProbe)
        return;
    m_versionProbe = new QProcess(this);
    auto environment = QProcessEnvironment::systemEnvironment();
    environment.insert(QStringLiteral("QT_QPA_PLATFORM"), QStringLiteral("offscreen"));
    m_versionProbe->setProcessEnvironment(environment);
    connect(m_versionProbe, &QProcess::finished, this, [this] {
        // `nook --version` prints "nook 0.4".
        const auto words = QString::fromUtf8(m_versionProbe->readAllStandardOutput()).simplified().split(' ');
        m_versionProbe->deleteLater();
        m_versionProbe = nullptr;
        m_upgraded = true;
        m_installedVersion = words.size() == 2 && words.first() == QLatin1String("nook") ? words.last() : QString();
        Q_EMIT installedVersionChanged();
    });
    m_versionProbe->start(nook::executablePath(), {QStringLiteral("--version")});
}
void Application::requestWindow() {
    Q_EMIT windowRequested();
}
namespace nook {
QString executablePath() {
    static const QString path = QCoreApplication::applicationFilePath();
    return path;
}
bool executableReplaced() {
    // pacman installs a new file over the old name; the kernel then marks our link "(deleted)".
    char target[4096];
    const auto length = ::readlink("/proc/self/exe", target, sizeof target - 1);
    return length > 0 && QByteArray(target, length).endsWith(" (deleted)");
}
void syncAutostart(bool enabled) {
    if (customProfile())
        return;
    const auto directory = QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation) + "/autostart";
    const auto path = directory + "/local.nook-background.desktop";
    if (!enabled) {
        QFile::remove(path);
        return;
    }
    const auto executable = QCoreApplication::applicationFilePath();
    const auto command = executable == "/usr/bin/nook" ? QStringLiteral("nook")
                                                               : '"' + executable + '"';
    const QByteArray content = QStringLiteral(
                                   "[Desktop Entry]\n"
                                   "Type=Application\n"
                                   "Name=nook Background Index\n"
                                   "Name[zh_CN]=星择文件索引（后台）\n"
                                   "Comment[zh_CN]=开机后在后台维护文件名索引，打开文件管理器时无需重新扫描\n"
                                   "Exec=%1 --background\n"
                                   "Icon=nook\n"
                                   "Terminal=false\n"
                                   "NoDisplay=true\n"
                                   "X-GNOME-Autostart-enabled=true\n")
                                   .arg(command)
                                   .toUtf8();
    QFile current(path);
    if (current.open(QIODevice::ReadOnly) && current.readAll() == content)
        return;
    current.close();
    QDir().mkpath(directory);
    QSaveFile file(path);
    if (file.open(QIODevice::WriteOnly) && file.write(content) == content.size())
        file.commit();
}
} // namespace nook
