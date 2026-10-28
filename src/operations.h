#pragma once
#include <QObject>
#include <QStringList>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <gio/gio.h>
#include <mutex>
#include <thread>

class Operations : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool busy READ busy NOTIFY changed)
    Q_PROPERTY(QString status READ status NOTIFY changed)
    Q_PROPERTY(QStringList errors READ errors NOTIFY changed)
    Q_PROPERTY(bool waitingConflict READ waitingConflict NOTIFY conflictChanged)
    Q_PROPERTY(QString conflictName READ conflictName NOTIFY conflictChanged)
    Q_PROPERTY(bool conflictFolder READ conflictFolder NOTIFY conflictChanged)
  public:
    explicit Operations(QObject *parent = nullptr);
    ~Operations() override;
    bool busy() const {
        return m_busy;
    }
    QString status() const {
        return m_status;
    }
    QStringList errors() const {
        return m_errors;
    }
    bool waitingConflict() const {
        return m_waiting;
    }
    QString conflictName() const {
        return m_conflictName;
    }
    bool conflictFolder() const {
        return m_conflictFolder;
    }
    Q_INVOKABLE void run(const QString &kind, const QStringList &uris, const QString &destination = {},
                         const QString &name = {});
    Q_INVOKABLE void cancel();
    Q_INVOKABLE void resolveConflict(int choice);
  Q_SIGNALS:
    void changed();
    void conflictChanged();
    void finished();

  private:
    int conflict(const QString &name, bool folder);
    bool transfer(GFile *source, GFile *destination, bool move, GError **error);
    bool removeTree(GFile *file, GError **error);
    void report(QString message);
    bool m_busy = false, m_waiting = false, m_conflictFolder = false;
    QString m_status, m_conflictName;
    QStringList m_errors;
    std::atomic_bool m_cancelled{false};
    GCancellable *m_cancel = nullptr;
    std::thread m_thread;
    std::mutex m_mutex;
    std::condition_variable m_condition;
    int m_choice = -1;
    std::chrono::steady_clock::time_point m_lastReport;
};
