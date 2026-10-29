#pragma once
#include <QObject>
#include <QProcess>
#include <QTimer>
#include <QVariantMap>

// One request to nook-archive-worker: list, test, extract or create. Events arrive as they happen;
// questions (need-password, conflict) wait until answered or the job is cancelled.
class ArchiveJob : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool running READ running NOTIFY runningChanged)
  public:
    explicit ArchiveJob(QObject *parent = nullptr);
    ~ArchiveJob() override;
    static QString workerPath();
    bool start(const QVariantMap &request);
    Q_INVOKABLE void sendPassword(const QString &password);
    Q_INVOKABLE void resolveConflict(int id, const QString &choice, bool all);
    // Asks the worker to stop; after five seconds it is killed and its staging folder removed.
    Q_INVOKABLE void cancel();
    bool running() const { return m_process.state() != QProcess::NotRunning; }
    QVariantMap result() const { return m_result; }

  Q_SIGNALS:
    void workerEvent(const QVariantMap &event);
    void finished(const QVariantMap &result);
    void runningChanged();

  private:
    void send(const QVariantMap &command);
    void read();
    void ended();
    QProcess m_process;
    QTimer m_killTimer;
    QByteArray m_buffer;
    QByteArray m_stderr;
    QString m_staging;
    QVariantMap m_result;
    bool m_done = false;
};
