#include "updatechecker.h"

#include <QCoreApplication>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPointer>
#include <QProcess>
#include <QProcessEnvironment>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTimer>
#include <QtTest>
#include <cstring>

namespace {
const QString assetName = QStringLiteral("nook-1.2.3-1-x86_64.pkg.tar.zst");
const QString assetUrl = QStringLiteral("https://github.com/Pasitearko/nook/releases/download/v1.2.3/") + assetName;

QByteArray releaseJson(const QString &tag = QStringLiteral("v1.2.3"),
                       const QString &url = assetUrl, const QString &name = assetName) {
    return QJsonDocument(QJsonObject{
        {"tag_name", tag}, {"draft", false}, {"prerelease", false},
        {"assets", QJsonArray{QJsonObject{{"name", name}, {"browser_download_url", url}}}}
    }).toJson(QJsonDocument::Compact);
}

// Fully in-memory replies. No socket, child process or package manager is ever started.
class FakeReply : public QNetworkReply {
  public:
    QByteArray body;
    qsizetype offset = 0;
    bool done = false;
    explicit FakeReply(const QNetworkRequest &request, QObject *parent) : QNetworkReply(parent) {
        setRequest(request);
        setUrl(request.url());
        open(QIODevice::ReadOnly);
    }
    void abort() override {
        if (done)
            return;
        setError(OperationCanceledError, QStringLiteral("Canceled"));
        complete();
    }
    void deliver(const QByteArray &data, int code = 200, NetworkError error = NoError,
                 qint64 contentLength = -1, bool finish = true) {
        body = data;
        setAttribute(QNetworkRequest::HttpStatusCodeAttribute, code);
        if (error != NoError)
            setError(error, QStringLiteral("Injected network error"));
        if (contentLength >= 0)
            setHeader(QNetworkRequest::ContentLengthHeader, contentLength);
        Q_EMIT metaDataChanged();
        if (done)
            return;
        Q_EMIT readyRead();
        if (finish && !done)
            complete();
    }
    void append(const QByteArray &data) {
        body += data;
        Q_EMIT readyRead();
    }
    qint64 bytesAvailable() const override { return body.size() - offset + QNetworkReply::bytesAvailable(); }
    bool isSequential() const override { return true; }

  protected:
    qint64 readData(char *data, qint64 maximum) override {
        const auto count = qMin<qint64>(maximum, body.size() - offset);
        if (count <= 0)
            return 0;
        std::memcpy(data, body.constData() + offset, size_t(count));
        offset += count;
        return count;
    }

  private:
    void complete() {
        done = true;
        setFinished(true);
        Q_EMIT finished();
    }
};

class FakeNetwork : public QNetworkAccessManager {
  public:
    int requests = 0;
    FakeReply *reply = nullptr;
    explicit FakeNetwork(QObject *parent) : QNetworkAccessManager(parent) {}

  protected:
    QNetworkReply *createRequest(Operation operation, const QNetworkRequest &request, QIODevice *outgoing) override {
        Q_UNUSED(operation);
        Q_UNUSED(outgoing);
        ++requests;
        reply = new FakeReply(request, this);
        return reply;
    }
};
} // namespace

class UpdateCheckerTest : public QObject {
    Q_OBJECT
  private:
    static void tools(UpdateChecker &checker, const QStringList &missing = {}) {
        checker.m_architecture = QStringLiteral("x86_64");
        checker.m_findExecutable = [missing](const QString &name) {
            return missing.contains(name) ? QString() : QStringLiteral("/fake tools/") + name;
        };
        // Safe by default even if a test accidentally calls install().
        checker.m_startDetached = [](const QString &, const QStringList &) { return false; };
    }
    static FakeNetwork *network(UpdateChecker &checker) {
        auto *manager = new FakeNetwork(&checker);
        checker.m_network = manager;
        tools(checker);
        return manager;
    }
    static void available(UpdateChecker &checker) {
        checker.m_available = true;
        checker.m_assetUrl = assetUrl;
        checker.m_latestVersion = QStringLiteral("1.2.3");
        tools(checker);
    }

  private Q_SLOTS:
    void init() { QCoreApplication::setApplicationVersion(QStringLiteral("1.0.0")); }

    void versions_data() {
        QTest::addColumn<QString>("left");
        QTest::addColumn<QString>("right");
        QTest::addColumn<int>("expected");
        QTest::newRow("numeric-not-lexical") << "v1.10.0" << "1.9.9" << 1;
        QTest::newRow("same") << "v1.2.3" << "1.2.3" << 0;
        QTest::newRow("metadata") << "1.2.3+build.4" << "1.2.3+other" << 0;
        QTest::newRow("older") << "1.2.2" << "1.2.3" << -1;
        QTest::newRow("stable") << "1.2.3" << "1.2.3-rc.1" << 1;
        QTest::newRow("prerelease-numeric") << "1.2.3-rc.10" << "1.2.3-rc.2" << 1;
        QTest::newRow("prerelease-alpha") << "1.2.3-alpha" << "1.2.3-beta" << -1;
        QTest::newRow("numeric-before-alpha") << "1.2.3-1" << "1.2.3-alpha" << -1;
        QTest::newRow("longer-prerelease") << "1.2.3-alpha.1" << "1.2.3-alpha" << 1;
        QTest::newRow("no-overflow") << "999999999999999999999.0.0" << "999999999999999999998.9.9" << 1;
    }
    void versions() {
        QFETCH(QString, left);
        QFETCH(QString, right);
        QFETCH(int, expected);
        int comparison = 99;
        QVERIFY(UpdateChecker::compareVersions(left, right, &comparison));
        QCOMPARE(comparison, expected);
        QVERIFY(UpdateChecker::compareVersions(right, left, &comparison));
        QCOMPARE(comparison, -expected);
    }
    void invalidVersions() {
        const QStringList invalid{"", "1", "1.2", "01.2.3", "1.02.3", "1.2.03", "1.2.3.4",
                                  "1.2.3-01", "1.2.3-alpha.01", "1.2.3-", "1.2.3+", "1.2.3+foo..bar",
                                  " 1.2.3", "1.2.3\n", "V1.2.3", "1.2.3;id", "1.2.3/evil"};
        for (const auto &version : invalid) {
            int comparison = 99;
            QVERIFY2(!UpdateChecker::compareVersions(version, "1.2.3", &comparison), qPrintable(version));
            QVERIFY(!UpdateChecker::compareVersions("1.2.3", version, &comparison));
        }
    }
    void releaseParsing() {
        auto release = UpdateChecker::parseRelease(releaseJson(), "1.0.0");
        QVERIFY(release.error.isEmpty());
        QVERIFY(release.newer);
        QCOMPARE(release.version, "1.2.3");
        QCOMPARE(release.assetUrl, assetUrl);
        release = UpdateChecker::parseRelease(releaseJson(), "1.2.3");
        QVERIFY(!release.newer);
        QVERIFY(release.error.isEmpty());
        release = UpdateChecker::parseRelease(releaseJson(), "2.0.0");
        QVERIFY(!release.newer); // Never downgrade.
        QVERIFY(!UpdateChecker::parseRelease(releaseJson(), "").error.isEmpty());
        QVERIFY(!UpdateChecker::parseRelease("[]", "1.0.0").error.isEmpty());
        QVERIFY(!UpdateChecker::parseRelease("{bad", "1.0.0").error.isEmpty());
        QVERIFY(!UpdateChecker::parseRelease(QByteArray(UpdateChecker::ResponseLimit + 1, ' '), "1.0.0").error.isEmpty());
        for (const auto &key : {QStringLiteral("draft"), QStringLiteral("prerelease")}) {
            auto object = QJsonDocument::fromJson(releaseJson()).object();
            object[key] = true;
            QVERIFY(!UpdateChecker::parseRelease(QJsonDocument(object).toJson(), "1.0.0").error.isEmpty());
            object[key] = QStringLiteral("false");
            QVERIFY(!UpdateChecker::parseRelease(QJsonDocument(object).toJson(), "1.0.0").error.isEmpty());
            object.remove(key);
            QVERIFY(!UpdateChecker::parseRelease(QJsonDocument(object).toJson(), "1.0.0").error.isEmpty());
        }
        QVERIFY(!UpdateChecker::parseRelease(releaseJson("v2.0.0-rc.1"), "1.0.0").error.isEmpty());
        auto object = QJsonDocument::fromJson(releaseJson()).object();
        object["assets"] = "not an array";
        QVERIFY(!UpdateChecker::parseRelease(QJsonDocument(object).toJson(), "1.0.0").error.isEmpty());
    }
    void unsafeAssets() {
        QVERIFY(UpdateChecker::validAssetUrl(assetUrl));
        const QStringList urls{
            QString(assetUrl).replace("https:", "http:"),
            QString(assetUrl).replace("github.com/", "github.com.evil/"),
            QString(assetUrl).replace("github.com/", "evil@github.com/"),
            QString(assetUrl).replace("github.com/", "github.com:443/"),
            QString(assetUrl).replace("Pasitearko/nook/", "attacker/nook/"),
            QString(assetUrl).replace("releases/download/", "releases/latest/"),
            QString(assetUrl).replace("v1.2.3/", "../"),
            QString(assetUrl).replace("v1.2.3/", "v1.2.3%2f..%2f/"),
            QString(assetUrl).replace("x86_64", "aarch64"),
            QString(assetUrl).replace("nook-1.2.3", "nook-debug-1.2.3"),
            QString(assetUrl).replace("nook-1.2.3", "nook-9.9.9"),
            QString(assetUrl).replace(".pkg.tar.zst", ".pkg.tar.zst.sig"),
            assetUrl + "?download=1", assetUrl + "#fragment", assetUrl + "\n",
            QString(assetUrl).replace("nook-1", "nook-$(touch pwned)-1"),
            QString(assetUrl).replace("nook-1", "nook-'`;id`-1"),
            QString(assetUrl).replace("nook-1", "nook-%27-1")
        };
        for (const auto &url : urls) {
            QVERIFY2(!UpdateChecker::validAssetUrl(url), qPrintable(url));
            const auto release = UpdateChecker::parseRelease(releaseJson("v1.2.3", url), "1.0.0");
            QVERIFY(release.assetUrl.isEmpty());
            QVERIFY(!release.error.isEmpty());
            QVERIFY(UpdateChecker::installScript(url, "/curl", "/sudo", "/pacman").isEmpty());
        }
        // URL and asset name must agree and the tag must match the release, not just the repository.
        QVERIFY(UpdateChecker::parseRelease(releaseJson("v1.2.4"), "1.0.0").assetUrl.isEmpty());
        QVERIFY(UpdateChecker::parseRelease(releaseJson("v1.2.3", assetUrl, "different"), "1.0.0").assetUrl.isEmpty());
        auto object = QJsonDocument::fromJson(releaseJson()).object();
        auto assets = object["assets"].toArray();
        assets.prepend(QJsonObject{{"name", "evil"}, {"browser_download_url", "https://evil/package"}});
        object["assets"] = assets;
        QCOMPARE(UpdateChecker::parseRelease(QJsonDocument(object).toJson(), "1.0.0").assetUrl, assetUrl);
    }
    void choosesMainPackageAndRejectsAmbiguity() {
        auto object = QJsonDocument::fromJson(releaseJson()).object();
        const auto main = object["assets"].toArray().first();
        const auto debugName = QString(assetName).replace("nook-", "nook-debug-");
        const auto debugUrl = QString(assetUrl).replace(assetName, debugName);
        const QJsonObject debug{{"name", debugName}, {"browser_download_url", debugUrl}};
        for (const auto &assets : {QJsonArray{debug, main}, QJsonArray{main, debug}}) {
            object["assets"] = assets;
            const auto release = UpdateChecker::parseRelease(QJsonDocument(object).toJson(), "1.0.0");
            QCOMPARE(release.assetUrl, assetUrl);
            QVERIFY(release.error.isEmpty());
        }
        object["assets"] = QJsonArray{main, main};
        const auto release = UpdateChecker::parseRelease(QJsonDocument(object).toJson(), "1.0.0");
        QVERIFY(release.assetUrl.isEmpty());
        QVERIFY(!release.error.isEmpty());
    }
    void trustedSystemTools() {
        QTemporaryDir fixture;
        QVERIFY(fixture.isValid());
        for (const auto &tool : {"pacman", "sudo", "curl", "bash", "mktemp", "rm"}) {
            QFile file(fixture.filePath(tool));
            QVERIFY(file.open(QIODevice::WriteOnly));
            file.write("#!/bin/sh\nexit 42\n");
            QVERIFY(file.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
        }
        const auto oldPath = qgetenv("PATH");
        qputenv("PATH", QFile::encodeName(fixture.path()) + ':' + oldPath);
        UpdateChecker checker;
        QStringList resolved;
        for (const auto &tool : {"pacman", "sudo", "curl", "bash", "mktemp", "rm"})
            resolved << checker.m_findExecutable(QString::fromLatin1(tool));
        qputenv("PATH", oldPath);
        for (const auto &path : resolved)
            QVERIFY2(path.isEmpty() || path.startsWith("/usr/bin/"), qPrintable(path));
    }
    void scriptSecurity() {
        QCOMPARE(UpdateChecker::shellQuote(""), "''");
        QCOMPARE(UpdateChecker::shellQuote("a'b;$(id)"), "'a'\\''b;$(id)'");
        const auto script = UpdateChecker::installScript(assetUrl, "/tools/c'url;$(id)", "/tools/sudo", "/tools/pac man");
        QVERIFY(script.contains("'/tools/c'\\''url;$(id)' --disable --fail --location"));
        QVERIFY(script.contains("--proto '=https' --proto-redir '=https'"));
        QVERIFY(script.contains("--output \"$package\" -- '" + assetUrl + "'"));
        QVERIFY(script.contains("'/tools/sudo' -- '/tools/pac man' -U --noconfirm -- \"$package\""));
        QVERIFY(script.contains("work=$(mktemp -d -- /tmp/nook-update.XXXXXXXXXX)"));
        QVERIFY(script.contains("umask 077"));
        QVERIFY(script.contains("EUID == 0"));
        QVERIFY(script.contains("rm -rf -- \"$work\""));
        QVERIFY(script.indexOf("if ! ") < script.indexOf("'/tools/sudo'"));
        QVERIFY(script.contains("Package download failed; nothing was installed."));
        QVERIFY(!script.contains("| sudo"));
        QVERIFY(!script.contains("sudo bash"));
        QVERIFY(!script.contains("read -r"));
        QVERIFY(UpdateChecker::installScript(assetUrl, "", "/sudo", "/pacman").isEmpty());
        QVERIFY(UpdateChecker::installScript(assetUrl, QString("/curl") + QChar::Null, "/sudo", "/pacman").isEmpty());
        // Syntax-only: -n parses but never executes the script (including its substitutions).
        QProcess parser;
        parser.start("/bin/bash", {"--noprofile", "--norc", "-n", "-c", script});
        QVERIFY(parser.waitForFinished());
        QCOMPARE(parser.exitCode(), 0);
    }
    void scriptWithHarmlessTools() {
        QTemporaryDir fixture;
        QVERIFY(fixture.isValid());
        const auto put = [](const QString &path, const QByteArray &body) {
            QFile file(path);
            return file.open(QIODevice::WriteOnly) && file.write(body) == body.size()
                && file.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
        };
        const auto curl = fixture.filePath("c'url; literal");
        const auto sudo = fixture.filePath("sudo stub");
        const auto pacman = fixture.filePath("pacman stub");
        QVERIFY(put(curl, "#!/bin/bash\n"
                          "while (( $# )); do if [[ $1 == --output ]]; then output=$2; shift; fi; shift; done\n"
                          "printf '%s' \"$output\" > \"$STUB_LOG/download\"\n"
                          "if [[ $STUB_MODE == curl-failure ]]; then exit 22; fi\n"
                          "if [[ $STUB_MODE == empty ]]; then : > \"$output\"; else printf package > \"$output\"; fi\n"));
        QVERIFY(put(sudo, "#!/bin/bash\n"
                          "printf reached > \"$STUB_LOG/sudo\"\n"
                          "[[ $1 == -- ]] || exit 90\nshift\nexec \"$@\"\n"));
        QVERIFY(put(pacman, "#!/bin/bash\n"
                            "printf '%s\\n' \"$@\" > \"$STUB_LOG/pacman\"\n"
                            "[[ $1 == -U && $2 == --noconfirm && $3 == -- && $# == 4 && -s $4 ]] || exit 91\n"
                            "if [[ $STUB_MODE == pacman-failure ]]; then exit 42; fi\n"));
        const auto script = UpdateChecker::installScript(assetUrl, curl, sudo, pacman);
        const auto read = [](const QString &path) {
            QFile file(path);
            return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
        };
        for (const auto &mode : {"success", "curl-failure", "empty", "pacman-failure"}) {
            QFile::remove(fixture.filePath("download"));
            QFile::remove(fixture.filePath("sudo"));
            QFile::remove(fixture.filePath("pacman"));
            QProcess process;
            auto environment = QProcessEnvironment::systemEnvironment();
            environment.insert("STUB_LOG", fixture.path());
            environment.insert("STUB_MODE", QString::fromLatin1(mode));
            process.setProcessEnvironment(environment);
            process.start("/bin/bash", {"--noprofile", "--norc", "-c", script});
            // Leave stdin open: a failure must not wait for a keypress.
            if (!process.waitForFinished(5000)) {
                process.kill();
                process.waitForFinished();
                QFAIL("The installer script waited for input or failed to terminate.");
            }
            const auto error = process.readAllStandardError();
            if (error.contains("Run the updater as a regular user")) {
                QCOMPARE(process.exitCode(), 1);
                QVERIFY(!QFile::exists(fixture.filePath("download")));
                QVERIFY(!QFile::exists(fixture.filePath("sudo")));
                return; // Root may only test refusal; it must never download or elevate.
            }
            const auto package = QString::fromUtf8(read(fixture.filePath("download")));
            QVERIFY(package.startsWith("/tmp/nook-update."));
            QVERIFY(package.endsWith("/package-x86_64.pkg.tar.zst"));
            QVERIFY(!QFile::exists(package));
            QVERIFY(!QFile::exists(QFileInfo(package).absolutePath())); // EXIT cleanup on every path.
            const bool downloadFailed = QByteArray(mode) == "curl-failure" || QByteArray(mode) == "empty";
            QCOMPARE(QFile::exists(fixture.filePath("sudo")), !downloadFailed);
            QCOMPARE(QFile::exists(fixture.filePath("pacman")), !downloadFailed);
            if (!downloadFailed)
                QCOMPARE(read(fixture.filePath("pacman")), ("-U\n--noconfirm\n--\n" + package + '\n').toUtf8());
            const int expected = QByteArray(mode) == "success" ? 0 : QByteArray(mode) == "pacman-failure" ? 42 : 1;
            QCOMPARE(process.exitCode(), expected);
        }
    }
    void asynchronousCheckAndReset() {
        UpdateChecker checker;
        auto *manager = network(checker);
        QSignalSpy changed(&checker, &UpdateChecker::changed);
        QSignalSpy notices(&checker, &UpdateChecker::notice);
        QVERIFY(!checker.busy());
        QVERIFY(!checker.available());
        QVERIFY(checker.command().isEmpty());
        QCOMPARE(manager->requests, 0);
        checker.check();
        QVERIFY(checker.busy());
        checker.check();
        checker.install();
        QCOMPARE(manager->requests, 1);
        const auto request = manager->reply->request();
        QCOMPARE(request.url().toString(), "https://api.github.com/repos/Pasitearko/nook/releases/latest");
        QCOMPARE(request.attribute(QNetworkRequest::RedirectPolicyAttribute).toInt(),
                 int(QNetworkRequest::ManualRedirectPolicy));
        QVERIFY(request.transferTimeout() > 0);
        manager->reply->deliver(releaseJson());
        QVERIFY(!checker.busy());
        QVERIFY(checker.available());
        QCOMPARE(checker.latestVersion(), "1.2.3");
        QVERIFY(!checker.command().isEmpty());
        QCOMPARE(notices.size(), 1);
        QVERIFY(changed.size() >= 2);
        checker.check();
        QVERIFY(checker.busy());
        QVERIFY(!checker.available());
        QVERIFY(checker.latestVersion().isEmpty());
        QVERIFY(checker.command().isEmpty());
        manager->reply->deliver("{\"message\":\"rate limited\"}", 403);
        QVERIFY(!checker.busy());
        QVERIFY(checker.status().contains("403"));
        QVERIFY(!checker.available());
        QVERIFY(checker.command().isEmpty());
    }
    void responseFailures() {
        for (int scenario = 0; scenario < 6; ++scenario) {
            UpdateChecker checker;
            auto *manager = network(checker);
            QSignalSpy notices(&checker, &UpdateChecker::notice);
            checker.check();
            switch (scenario) {
            case 0: manager->reply->deliver(releaseJson(), 302); break;
            case 1: manager->reply->deliver({}, 0, QNetworkReply::SslHandshakeFailedError); break;
            case 2: manager->reply->deliver("bad json"); break;
            case 3: manager->reply->deliver({}, 200, QNetworkReply::NoError, UpdateChecker::ResponseLimit + 1); break;
            case 4: manager->reply->deliver(QByteArray(UpdateChecker::ResponseLimit + 1, 'x')); break;
            case 5: manager->reply->deliver(releaseJson("v1.2.3", "https://evil/package")); break;
            }
            QVERIFY(!checker.busy());
            QVERIFY(checker.command().isEmpty());
            QVERIFY(!checker.status().isEmpty());
            QCOMPARE(notices.size(), 1);
            if (scenario == 3 || scenario == 4)
                QVERIFY(checker.status().contains("too large"));
        }
    }
    void timeout() {
        UpdateChecker checker;
        auto *manager = network(checker);
        checker.m_timeoutMs = 20;
        checker.check();
        // A server that keeps delivering bytes must not extend the absolute deadline.
        manager->reply->deliver("{", 200, QNetworkReply::NoError, -1, false);
        QTimer trickle;
        int chunks = 0;
        connect(&trickle, &QTimer::timeout, &checker, [&] {
            if (checker.busy()) {
                ++chunks;
                manager->reply->append(" ");
            }
        });
        trickle.start(2);
        QTRY_VERIFY_WITH_TIMEOUT(!checker.busy(), 1000);
        QVERIFY(chunks > 0);
        QVERIFY(checker.status().contains("timed out"));
        QVERIFY(checker.command().isEmpty());
    }
    void streamingCapAndDestruction() {
        UpdateChecker checker;
        auto *manager = network(checker);
        checker.check();
        manager->reply->deliver(QByteArray(UpdateChecker::ResponseLimit / 2, 'x'),
                                200, QNetworkReply::NoError, -1, false);
        QVERIFY(checker.busy());
        manager->reply->append(QByteArray(UpdateChecker::ResponseLimit / 2, 'x'));
        QVERIFY(checker.busy()); // Exactly the cap is allowed; the next byte aborts.
        manager->reply->append("x");
        QVERIFY(!checker.busy());
        QVERIFY(checker.status().contains("too large"));
        auto *pending = new UpdateChecker;
        auto *pendingNetwork = network(*pending);
        pending->check();
        QPointer<QNetworkReply> reply = pendingNetwork->reply;
        delete pending;
        QVERIFY(reply.isNull());
        QCoreApplication::processEvents(); // No pending deadline/readyRead callback may access it.
    }
    void prerequisitesAndLaunch() {
        UpdateChecker checker;
        QSignalSpy notices(&checker, &UpdateChecker::notice);
        int launches = 0;
        checker.install();
        QVERIFY(checker.status().contains("Check"));
        available(checker);
        tools(checker, {"pacman"});
        checker.install();
        QVERIFY(checker.status().contains("pacman"));
        QVERIFY(checker.command().isEmpty());
        tools(checker);
        checker.m_architecture = "aarch64";
        checker.install();
        QVERIFY(checker.status().contains("x86_64"));
        QVERIFY(checker.command().isEmpty());
        for (const auto &tool : {"curl", "sudo", "bash", "mktemp", "rm"}) {
            tools(checker, {QString::fromLatin1(tool)});
            checker.install();
            QVERIFY(checker.command().isEmpty());
            QVERIFY(checker.status().contains("require"));
        }
        tools(checker);
        QStringList programs;
        bool argumentsCorrect = true;
        checker.m_startDetached = [&](const QString &program, const QStringList &arguments) {
            ++launches;
            programs << program;
            argumentsCorrect &= arguments == QStringList{"-e", "/fake tools/bash", "-c", checker.m_script};
            return program.endsWith("konsole"); // Failed kitty launch falls back.
        };
        checker.install();
        QVERIFY(argumentsCorrect);
        QCOMPARE(launches, 2);
        QCOMPARE(programs, (QStringList{"/fake tools/kitty", "/fake tools/konsole"}));
        QVERIFY(checker.status().contains("launched"));
        QVERIFY(!checker.status().contains("completed"));
        QVERIFY(!checker.busy());
        QVERIFY(notices.size() > 0);
        tools(checker);
        checker.install();
        QVERIFY(checker.status().contains("Could not launch"));
        checker.m_findExecutable = [](const QString &name) {
            const QStringList required{"pacman", "curl", "sudo", "bash", "mktemp", "rm"};
            return required.contains(name) ? "/fake/" + name : QString();
        };
        checker.install();
        QVERIFY(checker.status().contains("No supported terminal"));
    }
    void gnomeArguments() {
        UpdateChecker checker;
        available(checker);
        tools(checker, {"kitty", "konsole", "alacritty", "foot"});
        bool launched = false;
        bool argumentsCorrect = false;
        checker.m_startDetached = [&](const QString &program, const QStringList &arguments) {
            launched = true;
            argumentsCorrect = program == "/fake tools/gnome-terminal"
                && arguments == QStringList{"--", "/fake tools/bash", "-c", checker.m_script};
            return true;
        };
        checker.install();
        QVERIFY(launched);
        QVERIFY(argumentsCorrect);
    }
};

QTEST_GUILESS_MAIN(UpdateCheckerTest)
#include "updatechecker_test.moc"
