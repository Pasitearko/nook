#include "updatechecker.h"

#include <QCoreApplication>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QProcess>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QSysInfo>
#include <QTimer>
#include <algorithm>
#include <memory>

namespace {
struct Version {
    QStringList core;
    QStringList prerelease;
};

bool parseVersion(const QString &text, Version *version) {
    static const QRegularExpression pattern(QStringLiteral(
        "\\Av?(0|[1-9][0-9]*)\\.(0|[1-9][0-9]*)\\.(0|[1-9][0-9]*)"
        "(?:-([0-9A-Za-z-]+(?:\\.[0-9A-Za-z-]+)*))?"
        "(?:\\+([0-9A-Za-z-]+(?:\\.[0-9A-Za-z-]+)*))?\\z"));
    const auto match = pattern.match(text);
    if (!match.hasMatch())
        return false;
    version->core = {match.captured(1), match.captured(2), match.captured(3)};
    version->prerelease.clear();
    if (!match.captured(4).isEmpty()) {
        version->prerelease = match.captured(4).split('.');
        for (const auto &identifier : version->prerelease) {
            const bool numeric = std::all_of(identifier.cbegin(), identifier.cend(),
                                            [](QChar c) { return c >= u'0' && c <= u'9'; });
            if (numeric && identifier.size() > 1 && identifier.startsWith('0'))
                return false;
        }
    }
    return true;
}

// Compare numeric identifiers without integer conversion/overflow.
int numericCompare(const QString &a, const QString &b) {
    if (a.size() != b.size())
        return a.size() < b.size() ? -1 : 1;
    return QString::compare(a, b, Qt::CaseSensitive);
}

bool numeric(const QString &value) {
    return std::all_of(value.cbegin(), value.cend(), [](QChar c) { return c >= u'0' && c <= u'9'; });
}
} // namespace

UpdateChecker::UpdateChecker(QObject *parent)
    : QObject(parent), m_findExecutable([](const QString &name) {
          // Never elevate a PATH-resolved package manager, or run PATH-injected installer tools.
          const QStringList systemTools{"pacman", "sudo", "curl", "bash", "mktemp", "rm"};
          if (systemTools.contains(name)) {
              const QString path = QStringLiteral("/usr/bin/") + name;
              return QFileInfo(path).isExecutable() ? path : QString();
          }
          return QStandardPaths::findExecutable(name);
      }),
      m_startDetached([](const QString &program, const QStringList &arguments) {
          return QProcess::startDetached(program, arguments);
      }), m_architecture(QSysInfo::currentCpuArchitecture()) {}

bool UpdateChecker::compareVersions(const QString &left, const QString &right, int *comparison) {
    Version a, b;
    if (!comparison || !parseVersion(left, &a) || !parseVersion(right, &b))
        return false;
    int result = 0;
    for (int i = 0; i < 3 && result == 0; ++i)
        result = numericCompare(a.core[i], b.core[i]);
    if (!result && (a.prerelease.isEmpty() != b.prerelease.isEmpty()))
        result = a.prerelease.isEmpty() ? 1 : -1;
    if (!result) {
        for (qsizetype i = 0; i < qMin(a.prerelease.size(), b.prerelease.size()) && !result; ++i) {
            const auto &x = a.prerelease[i], &y = b.prerelease[i];
            const bool xn = numeric(x), yn = numeric(y);
            result = xn && yn ? numericCompare(x, y)
                              : xn != yn ? (xn ? -1 : 1) : QString::compare(x, y, Qt::CaseSensitive);
        }
        if (!result && a.prerelease.size() != b.prerelease.size())
            result = a.prerelease.size() < b.prerelease.size() ? -1 : 1;
    }
    *comparison = result < 0 ? -1 : result > 0 ? 1 : 0;
    return true;
}

bool UpdateChecker::validAssetUrl(const QString &url) {
    // Match the raw URL, not a normalized/decoded URL: no credentials, alternate hosts/ports,
    // queries, fragments, escapes, slashes in components, or shell metacharacters are accepted.
    static const QRegularExpression pattern(QStringLiteral(
        "\\Ahttps://github\\.com/Pasitearko/nook/releases/download/"
        "([A-Za-z0-9][A-Za-z0-9._+-]*)/(nook-[A-Za-z0-9][A-Za-z0-9._+-]*-x86_64\\.pkg\\.tar\\.zst)\\z"));
    const auto match = pattern.match(url);
    Version version;
    if (!match.hasMatch() || !parseVersion(match.captured(1), &version) || !version.prerelease.isEmpty())
        return false;
    const QString packageVersion = match.captured(1).startsWith('v') ? match.captured(1).mid(1) : match.captured(1);
    const QRegularExpression packagePattern(QStringLiteral("\\Anook-%1-[1-9][0-9]*(?:\\.[0-9]+)?-x86_64\\.pkg\\.tar\\.zst\\z")
                                               .arg(QRegularExpression::escape(packageVersion)));
    return packagePattern.match(match.captured(2)).hasMatch();
}

UpdateChecker::Release UpdateChecker::parseRelease(const QByteArray &json, const QString &currentVersion) {
    Release release;
    if (json.size() > ResponseLimit) {
        release.error = tr("Update response is too large.");
        return release;
    }
    QJsonParseError error;
    const auto document = QJsonDocument::fromJson(json, &error);
    if (error.error != QJsonParseError::NoError || !document.isObject()) {
        release.error = tr("Invalid update response.");
        return release;
    }
    const auto object = document.object();
    const auto tag = object.value("tag_name").toString();
    Version version;
    if (!object.value("draft").isBool() || object.value("draft").toBool()
        || !object.value("prerelease").isBool() || object.value("prerelease").toBool()
        || !parseVersion(tag, &version) || !version.prerelease.isEmpty()
        || !object.value("assets").isArray()) {
        release.error = tr("The update response is not a stable semantic release.");
        return release;
    }
    int comparison = 0;
    if (!compareVersions(tag, currentVersion, &comparison)) {
        release.error = tr("The application's current version is not a semantic version.");
        return release;
    }
    release.version = tag.startsWith('v') ? tag.mid(1) : tag;
    release.newer = comparison > 0;
    for (const auto &value : object.value("assets").toArray()) {
        const auto asset = value.toObject();
        const auto url = asset.value("browser_download_url").toString();
        const auto name = asset.value("name").toString();
        if (validAssetUrl(url)
            && url == QStringLiteral("https://github.com/Pasitearko/nook/releases/download/") + tag + '/' + name) {
            if (!release.assetUrl.isEmpty()) {
                release.assetUrl.clear();
                release.error = tr("Multiple supported packages were found; select a package manually.");
                return release;
            }
            release.assetUrl = url;
        }
    }
    if (release.newer && release.assetUrl.isEmpty())
        release.error = tr("No supported x86_64 Arch package was found in this release.");
    return release;
}

QString UpdateChecker::shellQuote(const QString &value) {
    QString escaped = value;
    escaped.replace('\'', QStringLiteral("'\\''"));
    return '\'' + escaped + '\'';
}

QString UpdateChecker::installScript(const QString &url, const QString &curl,
                                     const QString &sudo, const QString &pacman) {
    if (!validAssetUrl(url) || curl.isEmpty() || sudo.isEmpty() || pacman.isEmpty()
        || curl.contains(QChar::Null) || sudo.contains(QChar::Null) || pacman.contains(QChar::Null))
        return {};
    // No release-supplied filename is used on disk. Only pacman is privileged; curl and
    // cleanup run as the user. HTTPS-only redirects are needed for GitHub's asset CDN.
    return QStringLiteral(
        "set -eu\n"
        "export PATH=/usr/bin:/bin\n"
        "if (( EUID == 0 )); then printf '%s\\n' 'Run the updater as a regular user, not root.' >&2; exit 1; fi\n"
        "umask 077\n"
        "work=$(mktemp -d -- /tmp/nook-update.XXXXXXXXXX) || exit 1\n"
        "trap 'result=$?; rm -rf -- \"$work\"; if (( result != 0 )); then "
        "printf \"Update failed (exit %s).\\n\" \"$result\" >&2; "
        "fi; exit \"$result\"' EXIT\n"
        "package=\"$work/package-x86_64.pkg.tar.zst\"\n"
        "if ! %1 --disable --fail --location --proto '=https' --proto-redir '=https' "
        "--connect-timeout 15 --max-time 600 --max-redirs 5 --output \"$package\" -- %2; then\n"
        "  printf '%s\\n' 'Package download failed; nothing was installed.' >&2; exit 1\n"
        "fi\n"
        "if [[ ! -s \"$package\" ]]; then printf '%s\\n' 'Downloaded package is empty.' >&2; exit 1; fi\n"
        "%3 -- %4 -U --noconfirm -- \"$package\"\n"
        "printf '%s\\n' 'Update completed. Restart Nook to use the new version.'\n")
        .arg(shellQuote(curl), shellQuote(url), shellQuote(sudo), shellQuote(pacman));
}

void UpdateChecker::report(const QString &message) {
    m_status = message;
    Q_EMIT changed();
    Q_EMIT notice(message);
}

void UpdateChecker::prepareCommand() {
    m_command.clear();
    m_script.clear();
    m_bash.clear();
    if (m_architecture != QStringLiteral("x86_64")) {
        m_status = tr("Automatic updates are only supported on x86_64 Arch/pacman systems.");
        return;
    }
    const auto pacman = m_findExecutable(QStringLiteral("pacman"));
    if (pacman.isEmpty()) {
        m_status = tr("Automatic updates require pacman (Arch Linux).");
        return;
    }
    const auto curl = m_findExecutable(QStringLiteral("curl"));
    const auto sudo = m_findExecutable(QStringLiteral("sudo"));
    m_bash = m_findExecutable(QStringLiteral("bash"));
    if (curl.isEmpty() || sudo.isEmpty() || m_bash.isEmpty()
        || m_findExecutable(QStringLiteral("mktemp")).isEmpty()
        || m_findExecutable(QStringLiteral("rm")).isEmpty()) {
        m_status = tr("Automatic updates require bash, curl, sudo, mktemp and rm.");
        return;
    }
    m_script = installScript(m_assetUrl, curl, sudo, pacman);
    if (m_script.isEmpty()) {
        m_status = tr("The update package URL is not safe to install.");
        return;
    }
    m_command = shellQuote(m_bash) + QStringLiteral(" -c ") + shellQuote(m_script);
    m_status = tr("Version %1 is available. Install opens a terminal for the sudo password.").arg(m_latestVersion);
}

void UpdateChecker::check() {
    if (m_busy)
        return;
    m_busy = true;
    m_available = false;
    m_latestVersion.clear();
    m_assetUrl.clear();
    m_command.clear();
    m_script.clear();
    m_status = tr("Checking for updates…");
    Q_EMIT changed();
    if (!m_network)
        m_network = new QNetworkAccessManager(this);
    QNetworkRequest request(QUrl(QStringLiteral("https://api.github.com/repos/Pasitearko/nook/releases/latest")));
    request.setRawHeader("Accept", "application/vnd.github+json");
    request.setRawHeader("User-Agent", "nook-update-checker");
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
    request.setTransferTimeout(m_timeoutMs);
    auto *reply = m_network->get(request);
    reply->setReadBufferSize(ResponseLimit + 1);
    struct Response {
        QByteArray body;
        QString failure;
    };
    const auto response = std::make_shared<Response>();
    const auto consume = [reply, response] {
        if (!response->failure.isEmpty())
            return;
        const auto length = reply->header(QNetworkRequest::ContentLengthHeader).toLongLong();
        response->body += reply->read(ResponseLimit + 1 - response->body.size());
        if (length > ResponseLimit || response->body.size() > ResponseLimit) {
            response->failure = tr("Update response is too large.");
            reply->abort();
        }
    };
    connect(reply, &QNetworkReply::readyRead, this, consume);
    connect(reply, &QNetworkReply::metaDataChanged, this, consume);
    // Unlike an inactivity timeout, this also bounds a server that trickles bytes forever.
    auto *deadline = new QTimer(reply);
    deadline->setSingleShot(true);
    connect(deadline, &QTimer::timeout, this, [reply, response] {
        response->failure = tr("Update check timed out.");
        reply->abort();
    });
    connect(reply, &QNetworkReply::finished, this, [this, reply, response, consume, deadline] {
        deadline->stop();
        consume();
        reply->deleteLater();
        m_busy = false;
        if (!response->failure.isEmpty()) {
            report(response->failure);
            return;
        }
        const int code = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (code != 200 || reply->error() != QNetworkReply::NoError) {
            report(tr("Update check failed (HTTP %1): %2").arg(code).arg(reply->errorString()));
            return;
        }
        const auto release = parseRelease(response->body, QCoreApplication::applicationVersion());
        m_latestVersion = release.version;
        m_available = release.newer;
        m_assetUrl = release.assetUrl;
        if (!release.error.isEmpty()) {
            report(release.error);
            return;
        }
        if (m_available) {
            prepareCommand();
            report(m_status);
        } else {
            report(tr("Nook is up to date."));
        }
    });
    deadline->start(m_timeoutMs);
}

void UpdateChecker::install() {
    if (m_busy)
        return;
    if (!m_available || m_assetUrl.isEmpty()) {
        report(tr("Check for a supported update before installing."));
        return;
    }
    prepareCommand(); // Revalidate prerequisites; a previous check may be stale.
    if (m_command.isEmpty()) {
        report(m_status);
        return;
    }
    struct Terminal {
        const char *name;
        const char *execute;
    };
    static const Terminal terminals[] = {
        {"kitty", "-e"}, {"konsole", "-e"}, {"alacritty", "-e"}, {"foot", "-e"},
        {"gnome-terminal", "--"}, {"xfce4-terminal", "-x"}, {"mate-terminal", "-x"},
        {"xterm", "-e"}, {"urxvt", "-e"}, {"x-terminal-emulator", "-e"}
    };
    bool found = false;
    for (const auto &terminal : terminals) {
        const auto program = m_findExecutable(QString::fromLatin1(terminal.name));
        if (program.isEmpty())
            continue;
        found = true;
        // Pass the entire script as ONE argument. Never type into an existing terminal or
        // ask a terminal's single-string command parser to interpret untrusted text.
        const QStringList arguments{QString::fromLatin1(terminal.execute), m_bash,
                                    QStringLiteral("-c"), m_script};
        if (m_startDetached(program, arguments)) {
            report(tr("Update installer launched. The terminal will show the result; restart Nook after success."));
            return;
        }
    }
    report(found ? tr("Could not launch an update terminal.")
                 : tr("No supported terminal found. Install kitty, konsole, alacritty, foot, GNOME Terminal, XFCE Terminal or xterm."));
}
