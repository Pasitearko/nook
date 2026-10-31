#pragma once

#include <QByteArray>
#include <QObject>
#include <QStringList>
#include <functional>

class QNetworkAccessManager;

// No work is performed until check() or install() is called. The detached installer reports
// its final result in the terminal, not through this object's busy/status properties.
class UpdateChecker : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool busy READ busy NOTIFY changed)
    Q_PROPERTY(bool available READ available NOTIFY changed)
    Q_PROPERTY(QString latestVersion READ latestVersion NOTIFY changed)
    Q_PROPERTY(QString status READ status NOTIFY changed)
    Q_PROPERTY(QString command READ command NOTIFY changed)

  public:
    explicit UpdateChecker(QObject *parent = nullptr);
    bool busy() const { return m_busy; }
    bool available() const { return m_available; }
    QString latestVersion() const { return m_latestVersion; }
    QString status() const { return m_status; }
    // A shell-quoted bash invocation, suitable for manual use; empty when installation is unavailable.
    QString command() const { return m_command; }
    Q_INVOKABLE void check();
    Q_INVOKABLE void install();

  Q_SIGNALS:
    void changed();
    void notice(QString message);

  private:
    friend class UpdateCheckerTest;
    struct Release {
        QString version;
        QString assetUrl;
        QString error;
        bool newer = false;
    };
    static constexpr qsizetype ResponseLimit = 1024 * 1024;
    // Returns false for invalid SemVer (an optional leading 'v' is accepted).
    static bool compareVersions(const QString &left, const QString &right, int *comparison);
    static bool validAssetUrl(const QString &url);
    static Release parseRelease(const QByteArray &json, const QString &currentVersion);
    static QString shellQuote(const QString &value);
    static QString installScript(const QString &url, const QString &curl,
                                 const QString &sudo, const QString &pacman);
    void prepareCommand();
    void report(const QString &message);

    QNetworkAccessManager *m_network = nullptr;
    // Small injection seams: tests never need the host's tools or a real detached process.
    std::function<QString(const QString &)> m_findExecutable;
    std::function<bool(const QString &, const QStringList &)> m_startDetached;
    QString m_architecture;
    int m_timeoutMs = 15000;
    bool m_busy = false;
    bool m_available = false;
    QString m_latestVersion;
    QString m_assetUrl;
    QString m_status;
    QString m_command;
    QString m_script;
    QString m_bash;
};
