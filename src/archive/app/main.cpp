#include "application.h"
#include "archiveapp.h"
#include "archivedocument.h"
#include "filechooser.h"
#include <QCommandLineParser>
#include <QDir>
#include <QFileInfo>
#include <QGuiApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocalServer>
#include <QLocalSocket>
#include <QLockFile>
#include <QQmlApplicationEngine>
#include <QQuickWindow>
#include <QStandardPaths>
#include <QTextStream>
#include <QTimer>
#include <QUrl>
#include <unistd.h>

// nook解压缩: one process, one window per package. A second launch hands its packages to the
// running one, so tasks and the session's passwords stay together.
int main(int argc, char **argv) {
    QGuiApplication app(argc, argv);
    app.setApplicationName("nook-archive");
    app.setOrganizationName("nook");
    app.setApplicationDisplayName("nook解压缩");
    app.setApplicationVersion("0.1");
    app.setDesktopFileName("local.nook.archive");

    QCommandLineParser parser;
    parser.setApplicationDescription("nook解压缩：浏览、解压和创建压缩包");
    parser.addHelpOption();
    parser.addVersionOption();
    parser.addPositionalArgument("archives", "压缩包路径或 file:// URI", "[archives…]");
    parser.addOption({"compress", "打开压缩窗口，压缩给出的文件和文件夹"});
    parser.addOption({"isolated", "独立实例，不转发给已运行的窗口（开发验证）"});
    parser.addOption({"capture", "窗口显示后保存截图（开发验证）", "path"});
    parser.addOption({"quit-after", "指定毫秒后退出（开发验证）", "milliseconds"});
    parser.process(app);

    QStringList archives;
    for (const auto &argument : parser.positionalArguments()) {
        const QUrl url(argument);
        archives << QFileInfo(url.isLocalFile() ? url.toLocalFile() : argument).absoluteFilePath();
    }

    const QString socketPath = QStandardPaths::writableLocation(QStandardPaths::RuntimeLocation) +
                               "/nook-archive-" + QString::number(getuid()) + ".sock";
    QLockFile lock(socketPath + ".lock");
    lock.setStaleLockTime(0);
    QLocalServer server;
    if (!parser.isSet("isolated")) {
        if (!lock.tryLock(0)) {
            QLocalSocket socket;
            bool connected = false;
            for (int i = 0; i < 5 && !connected; ++i) {
                socket.abort();
                socket.connectToServer(socketPath);
                connected = socket.waitForConnected(200);
            }
            if (!connected) {
                QTextStream(stderr) << "nook解压缩已在启动或无响应，请稍后重试。" << Qt::endl;
                return 1;
            }
            const char *key = parser.isSet("compress") ? "compress" : "archives";
            socket.write(QJsonDocument(QJsonObject{{key, QJsonArray::fromStringList(archives)}})
                             .toJson(QJsonDocument::Compact) + '\n');
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

    nook::portal::registerTypes();
    // Tasks keep going with every window closed; the process ends once both are done.
    app.setQuitOnLastWindowClosed(false);
    AppSettings settings;
    std::function<void(const QString &)> openWindow;
    ArchiveApp archiveApp(&settings, [&](const QString &path) { openWindow(path); });
    QList<QQuickWindow *> windows;
    QList<QQuickWindow *> others; // compression and task windows
    bool backgroundNoticeShown = false;
    const auto maybeQuit = [&] {
        if (!windows.isEmpty() || !others.isEmpty())
            return;
        const int active = archiveApp.queue()->activeCount();
        if (active == 0) {
            QCoreApplication::quit();
        } else if (!backgroundNoticeShown) {
            backgroundNoticeShown = true;
            archiveApp.notifier()->notify("nook解压缩在后台继续", QStringLiteral("还有 %1 个任务，完成后会通知你。").arg(active), false);
        }
    };
    const auto track = [&](QQuickWindow *window, QList<QQuickWindow *> &list) {
        list << window;
        QObject::connect(window, &QWindow::visibleChanged, window, [window](bool visible) {
            if (!visible)
                window->deleteLater();
        });
        QObject::connect(window, &QObject::destroyed, &app, [window, &list, &app, &maybeQuit] {
            list.removeAll(window);
            QTimer::singleShot(0, &app, maybeQuit);
        });
    };
    QObject::connect(archiveApp.queue(), &TaskQueue::idle, &app, [&] { QTimer::singleShot(0, &app, maybeQuit); });
    // Declared last, destroyed first: its windows go while everything they use still exists.
    QQmlApplicationEngine engine;
    archiveApp.setWindowFactory([&](const QString &component, const QVariantMap &properties) -> QObject * {
        QVariantMap initial = properties;
        initial.insert("app", QVariant::fromValue(&archiveApp));
        engine.setInitialProperties(initial);
        const auto before = engine.rootObjects().size();
        engine.loadFromModule("NookArchive", component);
        auto *window = engine.rootObjects().size() > before ? qobject_cast<QQuickWindow *>(engine.rootObjects().last()) : nullptr;
        if (window)
            track(window, others);
        return window;
    });
    openWindow = [&](const QString &path) {
        // An empty window takes the package before a new one opens.
        for (auto *window : std::as_const(windows)) {
            auto *document = window->property("document").value<ArchiveDocument *>();
            if (!path.isEmpty() && document && document->state() == "empty") {
                document->open(path);
                window->raise();
                window->requestActivate();
                return;
            }
        }
        auto *document = new ArchiveDocument;
        QObject::connect(document, &ArchiveDocument::opened, &archiveApp, &ArchiveApp::addRecent);
        archiveApp.attach(document);
        engine.setInitialProperties({{"app", QVariant::fromValue(&archiveApp)},
                                     {"document", QVariant::fromValue(document)}});
        const auto before = engine.rootObjects().size();
        engine.loadFromModule("NookArchive", "ArchiveMain");
        auto *window = engine.rootObjects().size() > before
                           ? qobject_cast<QQuickWindow *>(engine.rootObjects().last())
                           : nullptr;
        if (!window) {
            delete document;
            return;
        }
        document->setParent(window);
        track(window, windows);
        if (!path.isEmpty())
            document->open(path);
    };
    QObject::connect(&server, &QLocalServer::newConnection, &app, [&] {
        while (auto *socket = server.nextPendingConnection()) {
            QObject::connect(socket, &QLocalSocket::readyRead, socket, [&, socket] {
                while (socket->canReadLine()) {
                    const auto request = QJsonDocument::fromJson(socket->readLine()).object();
                    if (request.contains("compress")) {
                        QStringList items;
                        for (const auto &value : request.value("compress").toArray())
                            items << value.toString();
                        archiveApp.openCompose(items);
                        continue;
                    }
                    const auto list = request.value("archives").toArray();
                    if (list.isEmpty())
                        openWindow({});
                    for (const auto &value : list)
                        openWindow(value.toString());
                }
            });
            QObject::connect(socket, &QLocalSocket::disconnected, socket, &QObject::deleteLater);
        }
    });

    if (parser.isSet("compress")) {
        if (archives.isEmpty()) {
            QTextStream(stderr) << "--compress 需要至少一个文件或文件夹。" << Qt::endl;
            return 1;
        }
        archiveApp.openCompose(archives);
    } else {
        if (archives.isEmpty())
            openWindow({});
        for (const auto &archive : std::as_const(archives))
            openWindow(archive);
    }
    if (windows.isEmpty() && others.isEmpty())
        return 1;

    if (parser.isSet("capture")) {
        const QString target = parser.value("capture");
        QTimer::singleShot(1500, &app, [&, target] {
            if (!windows.isEmpty() || !others.isEmpty())
                (windows.isEmpty() ? others : windows).first()->grabWindow().save(target);
        });
    }
    if (parser.isSet("quit-after"))
        QTimer::singleShot(parser.value("quit-after").toInt(), &app, &QCoreApplication::quit);
    return app.exec();
}
