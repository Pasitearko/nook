#pragma once
#include "archivemodel.h"
#include <QObject>
#include <QVariantMap>
#include <functional>
#include <memory>

class ArchiveJob;

// One window's package: opening it with the worker, its properties, the name encoding and the
// password it was opened with (kept for this session only).
class ArchiveDocument : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString state READ state NOTIFY stateChanged) // empty, loading, ready, error
    Q_PROPERTY(QString path READ path NOTIFY pathChanged)
    Q_PROPERTY(QString name READ name NOTIFY pathChanged)
    Q_PROPERTY(QString folderPath READ folderPath NOTIFY pathChanged)
    Q_PROPERTY(QVariantMap info READ info NOTIFY infoChanged)
    Q_PROPERTY(QString formatLabel READ formatLabel NOTIFY infoChanged)
    Q_PROPERTY(bool readOnly READ readOnly NOTIFY infoChanged)
    // Why it cannot be changed (empty when it can).
    Q_PROPERTY(QString readOnlyReason READ readOnlyReason NOTIFY infoChanged)
    Q_PROPERTY(QString encoding READ encoding NOTIFY infoChanged)
    Q_PROPERTY(QString encodingChoice READ encodingChoice NOTIFY infoChanged)
    Q_PROPERTY(bool legacyNames READ legacyNames NOTIFY infoChanged)
    Q_PROPERTY(QString error READ error NOTIFY stateChanged)
    Q_PROPERTY(QString errorCode READ errorCode NOTIFY stateChanged)
    Q_PROPERTY(int loaded READ loaded NOTIFY loadedChanged)
    Q_PROPERTY(bool passwordRequested READ passwordRequested NOTIFY passwordChanged)
    Q_PROPERTY(bool passwordWrong READ passwordWrong NOTIFY passwordChanged)
    Q_PROPERTY(double archiveSize READ archiveSize NOTIFY pathChanged)
    // The password that opened the file list, if it needed one.
    Q_PROPERTY(QString password READ password NOTIFY stateChanged)
    Q_PROPERTY(ArchiveModel *model READ model CONSTANT)
  public:
    explicit ArchiveDocument(QObject *parent = nullptr);
    ~ArchiveDocument() override;
    QString state() const { return m_state; }
    QString path() const { return m_path; }
    QString name() const;
    QString folderPath() const;
    QVariantMap info() const { return m_info; }
    QString formatLabel() const;
    bool readOnly() const;
    QString readOnlyReason() const;
    QString encoding() const { return m_info.value("encoding").toString(); }
    QString encodingChoice() const { return m_encoding; }
    bool legacyNames() const { return m_info.value("legacyNames").toBool(); }
    QString error() const { return m_error; }
    QString errorCode() const { return m_errorCode; }
    int loaded() const { return int(m_entries.size()); }
    bool passwordRequested() const { return m_passwordRequested; }
    bool passwordWrong() const { return m_passwordWrong; }
    double archiveSize() const;
    ArchiveModel *model() { return &m_model; }

    // A path or file:// URI.
    Q_INVOKABLE void open(const QString &location);
    Q_INVOKABLE void reload();
    Q_INVOKABLE void close();
    Q_INVOKABLE void setEncoding(const QString &encoding);
    Q_INVOKABLE void submitPassword(const QString &password);
    Q_INVOKABLE void cancelPassword();
    QString password() const { return m_password; }
    // Where a password can come from before the user is asked: one that opened this package
    // earlier in the session, and the saved ones (tried silently when that setting is on).
    void setPasswordSources(std::function<QString(const QString &)> session, std::function<QStringList()> saved) {
        m_sessionPassword = std::move(session);
        m_savedPasswords = std::move(saved);
    }

  Q_SIGNALS:
    void stateChanged();
    void pathChanged();
    void infoChanged();
    void loadedChanged();
    void passwordChanged();
    void opened(const QString &path);

  private:
    void start();
    void setState(const QString &state);
    void handle(const QVariantMap &event);
    std::unique_ptr<ArchiveJob> m_job;
    ArchiveModel m_model;
    QString m_state = "empty";
    QString m_path;
    QString m_encoding = "auto";
    QString m_password;
    QString m_pendingPassword;
    bool m_hasPassword = false;
    QVariantMap m_info;
    QVariantList m_entries;
    QString m_error;
    QString m_errorCode;
    bool m_passwordRequested = false;
    bool m_passwordWrong = false;
    bool m_typedPassword = false;
    QStringList m_candidates;
    std::function<QString(const QString &)> m_sessionPassword;
    std::function<QStringList()> m_savedPasswords;
};
