#pragma once
#include <QByteArray>
#include <QObject>
#include <QStringList>
#include <QVariantList>

class QProcess;

// System-level boot mounts, not a session automounter. All privileged validation
// lives in the installed helper; this object always runs as the desktop user.
class DiskMounts : public QObject {
    Q_OBJECT
    Q_PROPERTY(QVariantList disks READ disks NOTIFY disksChanged)
    Q_PROPERTY(bool busy READ busy NOTIFY changed)
    Q_PROPERTY(QString status READ status NOTIFY changed)
  public:
    explicit DiskMounts(QObject *parent = nullptr);
    ~DiskMounts() override;
    QVariantList disks() const {
        return m_disks;
    }
    bool busy() const {
        return m_busy;
    }
    QString status() const {
        return m_status;
    }

    Q_INVOKABLE void refresh();
    // A selected subset of {uuid: string, enabled: bool, readOnly: bool} maps.
    // Omitted UUIDs are untouched. Use disk.autoMount as the initial enabled value.
    // Calls while busy are ignored. Completion includes an asynchronous refresh.
    Q_INVOKABLE void apply(const QVariantList &choices);
  Q_SIGNALS:
    void disksChanged();
    void changed();
    // Emitted after every helper attempt AND its refresh, including partial failure.
    // Connect to Application::refreshPlaces (and any actual-mount index refresh).
    void mountedChanged();

  private:
    void startRefresh(bool afterApply);
    void startProcess(const QString &program, const QStringList &arguments, const QByteArray &input,
                      bool applying);
    void collectOutput();
    void processFinished(int exitCode, bool normalExit);
    QVariantList parseDisks(const QByteArray &data, QString &error) const;
    QVariantList m_disks;
    QString m_status, m_failure;
    QByteArray m_stdout, m_stderr;
    QProcess *m_process = nullptr;
    bool m_busy = false, m_applying = false, m_refreshForApply = false;
};
