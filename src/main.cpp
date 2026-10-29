#include <algorithm>
#include "application.h"
#include "filechooser.h"
#include "filemanager1.h"
#include "storage.h"
#include <QDir>
#include <QCommandLineParser>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QGuiApplication>
#include <QImageReader>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocalServer>
#include <QLocalSocket>
#include <QLockFile>
#include <QProcess>
#include <QQmlApplicationEngine>
#include <QQuickWindow>
#include <QSaveFile>
#include <QStandardPaths>
#include <QTextStream>
#include <QThread>
#include <QTimer>
#include <sys/resource.h>
#include <unistd.h>

int main(int argc, char **argv) {
    auto launchClock = std::make_shared<QElapsedTimer>();
    launchClock->start();
    const bool worker = argc > 1 && (QByteArray(argv[1]) == "--preview-worker" ||
                                     QByteArray(argv[1]) == "--thumbnail-worker");
    if (worker)
        ::setpgid(0, 0);
    if (worker)
        qputenv("QT_QPA_PLATFORM", "offscreen");
    // xdg-desktop-portal starts the file chooser and waits for it: a platform theme asking that
    // same portal for its settings while Qt starts would hang both until the portal gives up.
    const bool chooser = std::any_of(argv + 1, argv + argc, [](const char *arg) { return qstrcmp(arg, "--file-chooser") == 0; });
    if (chooser && qgetenv("QT_QPA_PLATFORMTHEME").contains("portal"))
        qputenv("QT_QPA_PLATFORMTHEME", "generic");
    QGuiApplication app(argc, argv);
    app.setApplicationName("nook");
    app.setOrganizationName("nook");
    app.setApplicationVersion("0.3.1");
    app.setDesktopFileName("local.nook");
    nook::executablePath(); // before an upgrade can replace it
    QImageReader::setAllocationLimit(96);
    if (worker) {
        struct rlimit cpu{12, 12};
        setrlimit(RLIMIT_CPU, &cpu);
        struct rlimit fileSize{32 * 1024 * 1024, 32 * 1024 * 1024};
        setrlimit(RLIMIT_FSIZE, &fileSize);
        bool thumb = QByteArray(argv[1]) == "--thumbnail-worker";
        auto result = makePreview(argc > 2 ? QString::fromLocal8Bit(argv[2]) : QString(),
                                  argc > 3 ? QByteArray(argv[3]).toInt() : 1, thumb);
        QTextStream(stdout) << QJsonDocument::fromVariant(result).toJson(QJsonDocument::Compact) << Qt::endl;
        return 0;
    }
    QCommandLineParser parser;
    parser.setApplicationDescription("适用于 Wayland 的轻量文件管理器");
    parser.addHelpOption();
    parser.addVersionOption();
    parser.addPositionalArgument("paths", "目录、文件或 file:// URI", "[paths…]");
    parser.addOption({"new-window", "打开独立窗口"});
    parser.addOption({"select", "打开所在目录并选中文件", "path"});
    parser.addOption({"no-index", "本次不启动后台索引"});
    parser.addOption({"background", "后台常驻维护文件名索引，不打开窗口（开机自启使用）"});
    parser.addOption({"file-chooser", "作为桌面门户的文件选择器后端运行（D-Bus 激活使用）"});
    parser.addOption({"service", "作为 org.freedesktop.FileManager1 服务启动，不打开窗口（D-Bus 激活使用）"});
    parser.addOption({"isolated", "独立实例，不转发给已运行窗口"});
    parser.addOption({"restart-from", "等旧进程退出后接替它，恢复它的窗口（设置里的“重启”使用）", "pid"});
    parser.addOption({"profile-dir", "仅隔离 nook 的配置、索引和缓存，保留系统输入法主题（开发验证）", "path"});
    parser.addOption({"capture", "保存本窗口截图（开发验证）", "path"});
    parser.addOption({"metrics", "记录首次内容渲染时间（开发验证）", "path"});
    parser.addOption({"quit-after", "指定毫秒后退出（开发验证）", "milliseconds"});
    parser.addOption({"restart-after", "指定毫秒后重启，同设置里的“重启”（开发验证）", "milliseconds"});
    parser.addOption({"benchmark-directory", "仅测量隔离目录读取时间，不显示窗口", "path"});
    parser.addOption({"benchmark-query", "仅测量已有索引查询，不扫描文件", "query"});
    parser.process(app);
    const QString profileDir =
        parser.isSet("profile-dir") ? parser.value("profile-dir") : qEnvironmentVariable("NOOK_PROFILE_DIR");
    nook::initializeProfile(profileDir);
    const bool profile = nook::customProfile();
    const bool isolated = profile || parser.isSet("isolated");
    if (parser.isSet("file-chooser")) {
        // Other applications' Open/Save dialogs (xdg-desktop-portal). A process of its own, so a
        // dialog never waits on the file manager; it ends after a while without dialogs.
        app.setDesktopFileName("local.nook.chooser");
        app.setQuitOnLastWindowClosed(false);
        AppSettings settings;
        SearchIndex index; // queries the shared index; scanning stays with the file manager
        Operations operations;
        QQmlApplicationEngine engine;
        engine.addImageProvider("thumb", new ThumbnailProvider);
        QTimer idle;
        idle.setSingleShot(true);
        idle.setInterval(10 * 60 * 1000);
        QObject::connect(&idle, &QTimer::timeout, &app, &QCoreApplication::quit);
        int open = 0;
        // After an upgrade, step aside while no dialog is open: the next one starts the new program.
        QTimer upgrade;
        upgrade.setInterval(30 * 1000);
        QObject::connect(&upgrade, &QTimer::timeout, &app, [&] {
            if (open == 0 && nook::executableReplaced())
                app.quit();
        });
        upgrade.start();
        nook::portal::FileChooserPortal portal(
            [&](nook::portal::ChooserRequest *request) {
                ++open;
                idle.stop();
                auto *controller = new Application(&index, &operations, &settings, false, request);
                controller->setRemembersSession(false);
                engine.setInitialProperties({{"controller", QVariant::fromValue(controller)},
                                             {"request", QVariant::fromValue(request)}});
                const auto before = engine.rootObjects().size();
                engine.loadFromModule("Nook", "FileChooser");
                auto *window = engine.rootObjects().size() > before
                                   ? qobject_cast<QQuickWindow *>(engine.rootObjects().last())
                                   : nullptr;
                auto release = [&, request] {
                    request->deleteLater();
                    if (--open == 0)
                        idle.start();
                };
                if (!window) {
                    request->abort();
                    release();
                    return;
                }
                // Closing the window is cancelling; the window goes once there is an answer.
                QObject::connect(window, &QWindow::visibleChanged, request, [request](bool visible) {
                    if (!visible)
                        request->cancel();
                });
                QObject::connect(request, &nook::portal::ChooserRequest::finished, window,
                                 [window] { window->deleteLater(); });
                QObject::connect(window, &QObject::destroyed, request, release);
            },
            [&] { return settings.store().value("chooserFolder").toString(); },
            [&](const QString &uri) { settings.store().setValue("chooserFolder", uri); });
        if (!portal.publish()) {
            QTextStream(stderr) << "无法注册文件选择器门户（已有实例在运行？）" << Qt::endl;
            return 1;
        }
        idle.start();
        return app.exec();
    }
    if (parser.isSet("benchmark-query")) {
        SearchIndex index;
        QElapsedTimer elapsed;
        elapsed.start();
        QObject::connect(index.model(), &DirectoryModel::statusChanged, &app, [&] {
            if (index.model()->loading())
                return;
            QTextStream(stdout) << QJsonDocument(QJsonObject{{"returned", index.model()->count()},
                                                             {"more", index.hasMore()},
                                                             {"milliseconds", elapsed.elapsed()},
                                                             {"error", index.model()->error()}})
                                       .toJson(QJsonDocument::Compact)
                                << Qt::endl;
            app.quit();
        });
        index.query(parser.value("benchmark-query"));
        int rc = app.exec();
        QThreadPool::globalInstance()->waitForDone();
        return rc;
    }
    if (parser.isSet("benchmark-directory")) {
        DirectoryModel model;
        QElapsedTimer timer;
        timer.start();
        qint64 first = -1;
        QObject::connect(&model, &DirectoryModel::firstBatch, &app, [&](qint64 ms) { first = ms; });
        QObject::connect(&model, &DirectoryModel::statusChanged, &app, [&] {
            if (model.loading())
                return;
            QTextStream(stdout) << QJsonDocument(QJsonObject{{"entries", model.count()},
                                                             {"firstBatchMs", first},
                                                             {"completeMs", timer.elapsed()},
                                                             {"error", model.error()}})
                                       .toJson(QJsonDocument::Compact)
                                << Qt::endl;
            app.quit();
        });
        model.setShowHidden(true);
        model.navigate(parser.value("benchmark-directory"));
        int rc = app.exec();
        QThreadPool::globalInstance()->waitForDone();
        return rc;
    }
    QStringList paths;
    for (const auto &path : parser.positionalArguments())
        paths.append(nook::normalizeUri(path));
    QJsonObject request{
        {"paths", QJsonArray::fromStringList(paths)},
        {"select", parser.isSet("select") ? nook::normalizeUri(parser.value("select")) : QString()},
        {"newWindow", parser.isSet("new-window")}};
    // A restart: the previous process still holds the instance lock and the D-Bus name until it
    // has saved everything and exited.
    if (parser.isSet("restart-from")) {
        const QString stat = "/proc/" + QString::number(parser.value("restart-from").toInt()) + "/stat";
        auto running = [&stat] {
            QFile file(stat);
            if (!file.open(QIODevice::ReadOnly))
                return false;
            const QByteArray line = file.readAll();
            const auto at = line.lastIndexOf(") ") + 2;
            const char state = at >= 2 && at < line.size() ? line.at(at) : 'R';
            return state != 'Z' && state != 'X';
        };
        QElapsedTimer waited;
        waited.start();
        while (running() && waited.elapsed() < 10000)
            QThread::msleep(20);
    }
    QString socketPath = QStandardPaths::writableLocation(QStandardPaths::RuntimeLocation) + "/nook-" +
                         QString::number(getuid()) + ".sock";
    QLocalServer server;
    QLockFile instanceLock(socketPath + ".lock");
    instanceLock.setStaleLockTime(0);
    // D-Bus activation starts nook like the login autostart does: no window until asked.
    const bool service = parser.isSet("service");
    const bool background = parser.isSet("background") || service;
    if (!isolated) {
        if (!instanceLock.tryLock(0)) {
            // The resident instance already maintains the index; autostart has nothing to add.
            if (background)
                return 0;
            QLocalSocket socket;
            bool connected = false;
            for (int i = 0; i < 5 && !connected; ++i) {
                socket.abort();
                socket.connectToServer(socketPath);
                connected = socket.waitForConnected(200);
            }
            if (!connected) {
                QTextStream(stderr) << "已有实例正在启动或无响应，请稍后重试。" << Qt::endl;
                return 1;
            }
            socket.write(QJsonDocument(request).toJson(QJsonDocument::Compact) + '\n');
            socket.flush();
            socket.waitForBytesWritten(500);
            return 0;
        }
        QLocalServer::removeServer(socketPath);
        server.setSocketOptions(QLocalServer::UserAccessOption);
        if (!server.listen(socketPath)) {
            QTextStream(stderr) << "无法建立单实例连接：" << server.errorString() << Qt::endl;
            return 1;
        }
    }
    AppSettings settings;
    const auto residentEnabled = [&settings, profile] {
        return !profile && settings.values()["backgroundIndex"].toBool();
    };
    if (parser.isSet("background") && !residentEnabled())
        return 0;
    // Resident mode: closing the last window keeps the process (and its index watchers) alive,
    // so the next window opens instantly without re-scanning the disk.
    app.setQuitOnLastWindowClosed(!residentEnabled());
    if (!isolated)
        nook::syncAutostart(residentEnabled());
    QObject::connect(&settings, &AppSettings::changed, &app,
                     [&, previous = residentEnabled()]() mutable {
                         const bool current = residentEnabled();
                         if (current == previous)
                             return;
                         previous = current;
                         app.setQuitOnLastWindowClosed(!current);
                         if (!isolated)
                             nook::syncAutostart(current);
                     });
    SearchIndex index;
    Operations operations;
    index.setRoots(settings.values()["roots"].toStringList());
    index.setExclusions(settings.values()["exclusions"].toStringList());
    index.setIncluded(settings.values()["indexSystem"].toBool(), settings.values()["indexNoise"].toBool());
    index.setNtfsJournal(settings.values()["ntfsJournal"].toBool());
    QObject::connect(&settings, &AppSettings::changed, &index, [&] {
        index.setRoots(settings.values()["roots"].toStringList());
        index.setExclusions(settings.values()["exclusions"].toStringList());
        index.setIncluded(settings.values()["indexSystem"].toBool(), settings.values()["indexNoise"].toBool());
        const bool ntfs = settings.values()["ntfsJournal"].toBool();
        if (ntfs == index.ntfsJournal())
            return;
        // Switching sources is a full pass: the running one is stopped first.
        index.setNtfsJournal(ntfs);
        index.stop();
        auto *restart = new QTimer(&index);
        restart->setInterval(100);
        QObject::connect(restart, &QTimer::timeout, &index, [&index, restart] {
            if (index.scanning())
                return;
            restart->deleteLater();
            index.start();
        });
        restart->start();
    });
    std::vector<std::unique_ptr<Application>> controllers;
    QQmlApplicationEngine engine;
    engine.addImageProvider("thumb", new ThumbnailProvider);
    std::function<void(const QJsonObject &, bool, const QJsonObject &)> createWindow;
    // Restart: hand the open windows to a new process started from the program now installed.
    bool restarting = false;
    QJsonArray restartWindows;
    auto restart = [&] {
        if (restarting || operations.busy())
            return;
        restarting = true;
        for (auto *root : engine.rootObjects()) {
            auto *window = qobject_cast<QQuickWindow *>(root);
            auto *controller = root->property("controller").value<Application *>();
            if (!window || !window->isVisible() || !controller)
                continue;
            auto state = controller->state();
            state["width"] = window->width();
            state["height"] = window->height();
            state["maximized"] = window->visibility() == QWindow::Maximized;
            restartWindows.append(state);
        }
        app.quit();
    };
    auto handlePaths = [](Application *controller, const QJsonObject &req) {
        for (const auto &value : req["paths"].toArray()) {
            QString uri = nook::normalizeUri(value.toString());
            QFileInfo info(nook::localPath(uri));
            if (info.isFile() || info.isSymLink())
                controller->reveal(uri);
            else
                controller->navigate(uri);
        }
        if (!req["select"].toString().isEmpty())
            controller->reveal(nook::normalizeUri(req["select"].toString()));
    };
    createWindow = [&](const QJsonObject &req, bool restore, const QJsonObject &state) {
        auto controller = std::make_unique<Application>(&index, &operations, &settings, restore, nullptr, state);
        auto *ptr = controller.get();
        controllers.push_back(std::move(controller));
        QObject::connect(ptr, &Application::windowRequested, &app, [&] { createWindow({}, false, {}); });
        QObject::connect(ptr, &Application::restartRequested, &app, restart);
        QVariantMap properties{{"controller", QVariant::fromValue(ptr)}};
        if (state["width"].toInt() > 0 && state["height"].toInt() > 0) {
            properties["width"] = state["width"].toInt();
            properties["height"] = state["height"].toInt();
        }
        engine.setInitialProperties(properties);
        engine.loadFromModule("Nook", "Main");
        if (!engine.rootObjects().isEmpty()) {
            auto *root = engine.rootObjects().last();
            if (auto *window = qobject_cast<QQuickWindow *>(root)) {
                if (state["maximized"].toBool())
                    window->showMaximized();
                QObject::connect(window, &QWindow::visibleChanged, window, [window](bool visible) {
                    if (!visible)
                        window->deleteLater();
                });
                QObject::connect(window, &QObject::destroyed, &app, [&, ptr] {
                    std::erase_if(controllers, [ptr](const auto &item) { return item.get() == ptr; });
                });
                if (parser.isSet("metrics")) {
                    auto ready = std::make_shared<std::atomic_bool>(false),
                         written = std::make_shared<std::atomic_bool>(false);
                    QObject::connect(ptr->left(), &DirectoryModel::firstBatch, window,
                                     [ready](qint64) { *ready = true; });
                    const QString report = parser.value("metrics");
                    QObject::connect(
                        window, &QQuickWindow::afterRendering, window,
                        [ready, written, launchClock, report] {
                            if (!ready->load() || written->exchange(true))
                                return;
                            const qint64 ms = launchClock->elapsed();
                            QMetaObject::invokeMethod(
                                qApp,
                                [ms, report] {
                                    QSaveFile file(report);
                                    if (file.open(QIODevice::WriteOnly)) {
                                        file.write(
                                            QJsonDocument(QJsonObject{{"firstContentFrameMs", ms}}).toJson());
                                        file.commit();
                                    }
                                },
                                Qt::QueuedConnection);
                        },
                        Qt::DirectConnection);
                }
            }
        }
        handlePaths(ptr, req);
    };
    // The windows a restart handed over; none means it was a resident process without windows.
    QJsonArray handedOver;
    if (parser.isSet("restart-from")) {
        handedOver = QJsonDocument::fromJson(settings.store().value("restartWindows").toByteArray()).array();
        settings.store().remove("restartWindows");
    }
    if (!handedOver.isEmpty()) {
        for (const auto &window : handedOver)
            createWindow({}, false, window.toObject());
        if (engine.rootObjects().isEmpty())
            return 1;
    } else if (!background) {
        createWindow(request, !isolated && parser.positionalArguments().isEmpty() &&
                                  !parser.isSet("select"), {});
        if (engine.rootObjects().isEmpty())
            return 1;
    }
    // A request from a second launch, or from another application over D-Bus.
    auto dispatch = [&](const QJsonObject &req) {
        const auto roots = engine.rootObjects();
        QObject *root = nullptr;
        for (auto it = roots.crbegin(); it != roots.crend(); ++it)
            if (auto *candidate = qobject_cast<QQuickWindow *>(*it); candidate && candidate->isVisible()) {
                root = *it;
                break;
            }
        if (restarting)
            return;
        if (req["newWindow"].toBool())
            createWindow(req, false, {});
        else if (!root) {
            // First window after all were closed (or of a background start): restore the session.
            createWindow(req, req["paths"].toArray().isEmpty() && req["select"].toString().isEmpty() &&
                                  controllers.empty(), {});
        } else {
            auto *controller = root->property("controller").value<Application *>();
            if (controller)
                handlePaths(controller, req);
            if (auto *window = qobject_cast<QQuickWindow *>(root)) {
                window->show();
                window->requestActivate();
                // A second launch is an explicit user activation. niri's
                // IPC fallback focuses only this process's own window;
                // no session-wide focus policy is changed.
                if (qEnvironmentVariable("XDG_CURRENT_DESKTOP").contains("niri")) {
                    auto *query = new QProcess(&app);
                    const QString title = window->title();
                    QObject::connect(
                        query, &QProcess::finished, &app,
                        [query, title](int code, QProcess::ExitStatus) {
                            if (code == 0) {
                                auto windows =
                                    QJsonDocument::fromJson(query->readAllStandardOutput()).array();
                                for (const auto &value : windows) {
                                    auto item = value.toObject();
                                    if (item["pid"].toInteger() !=
                                            QCoreApplication::applicationPid() ||
                                        item["title"].toString() != title)
                                        continue;
                                    QProcess::startDetached(
                                        "niri", {"msg", "action", "focus-window", "--id",
                                                 QString::number(item["id"].toInteger())});
                                    break;
                                }
                            }
                            query->deleteLater();
                        });
                    query->start("niri", {"msg", "--json", "windows"});
                    QTimer::singleShot(1000, query, [query] {
                        if (query->state() != QProcess::NotRunning)
                            query->kill();
                    });
                }
            }
        }
    };
    if (!isolated && !FileManager1::publish(&app, dispatch))
        QTextStream(stderr) << "org.freedesktop.FileManager1 已被其他文件管理器占用" << Qt::endl;
    QObject::connect(&server, &QLocalServer::newConnection, &app, [&] {
        while (auto *socket = server.nextPendingConnection()) {
            auto buffer = std::make_shared<QByteArray>();
            auto *timeout = new QTimer(socket);
            timeout->setSingleShot(true);
            timeout->start(2000);
            QObject::connect(timeout, &QTimer::timeout, socket, &QLocalSocket::disconnectFromServer);
            auto consume = [&, socket, buffer] {
                *buffer += socket->readAll();
                if (buffer->size() > 65536) {
                    socket->disconnectFromServer();
                    return;
                }
                if (!buffer->contains('\n'))
                    return;
                dispatch(QJsonDocument::fromJson(buffer->split('\n').first()).object());
                socket->disconnectFromServer();
            };
            QObject::connect(socket, &QLocalSocket::readyRead, &app, consume);
            if (socket->bytesAvailable())
                consume();
            QObject::connect(socket, &QLocalSocket::disconnected, socket, &QObject::deleteLater);
        }
    });
    // At login, give the session a moment to settle before the low-priority scan.
    if (!parser.isSet("no-index"))
        QTimer::singleShot(background ? 8000 : 1500, &index, &SearchIndex::start);
    if (parser.isSet("capture"))
        QTimer::singleShot(2000, &app, [&] {
            if (auto *window = engine.rootObjects().isEmpty() ? nullptr : qobject_cast<QQuickWindow *>(engine.rootObjects().first()))
                window->grabWindow().save(parser.value("capture"));
        });
    // A resident process with no window open picks up an upgrade by itself.
    QTimer upgrade;
    upgrade.setInterval(60 * 1000);
    QObject::connect(&upgrade, &QTimer::timeout, &app, [&] {
        if (controllers.empty() && !operations.busy() && nook::executableReplaced())
            restart();
    });
    if (!isolated)
        upgrade.start();
    if (parser.isSet("quit-after"))
        QTimer::singleShot(parser.value("quit-after").toInt(), &app, &QCoreApplication::quit);
    if (parser.isSet("restart-after"))
        QTimer::singleShot(parser.value("restart-after").toInt(), &app, restart);
    int code = app.exec();
    index.stop();
    for (auto &controller : controllers) {
        controller->preview()->close();
        controller->saveSession();
    }
    operations.cancel();
    if (restarting) {
        // Written last, after every window has saved its session.
        settings.store().setValue("restartWindows", QJsonDocument(restartWindows).toJson(QJsonDocument::Compact));
        settings.store().sync();
        QStringList arguments{"--restart-from", QString::number(QCoreApplication::applicationPid())};
        if (!profileDir.isEmpty())
            arguments << "--profile-dir" << profileDir;
        else if (isolated)
            arguments << "--isolated";
        if (parser.isSet("no-index"))
            arguments << "--no-index";
        if (restartWindows.isEmpty())
            arguments << "--background";
        QProcess::startDetached(nook::executablePath(), arguments);
    }
    return code;
}
