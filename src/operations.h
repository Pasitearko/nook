#pragma once
#include "undorecord.h"
#include <QObject>
#include <QStringList>
#include <QTimer>
#include <QVariantList>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <gio/gio.h>
#include <mutex>
#include <thread>

class Operations : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool busy READ busy NOTIFY changed)
    Q_PROPERTY(QVariantList undoHistory READ undoHistory NOTIFY undoChanged)
    Q_PROPERTY(QString undoText READ undoText NOTIFY undoChanged)
    Q_PROPERTY(bool canUndo READ canUndo NOTIFY changed)
    Q_PROPERTY(QString status READ status NOTIFY changed)
    Q_PROPERTY(QStringList errors READ errors NOTIFY errorsChanged)
    Q_PROPERTY(QString kind READ kind NOTIFY changed)
    Q_PROPERTY(int completedItems READ completedItems NOTIFY changed)
    Q_PROPERTY(int totalItems READ totalItems NOTIFY changed)
    Q_PROPERTY(double progress READ progress NOTIFY changed)
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
    QString kind() const {
        return m_kind;
    }
    int completedItems() const {
        return m_completedItems;
    }
    int totalItems() const {
        return m_totalItems;
    }
    // Selected-item-weighted progress; -1 means the current work has no known size.
    double progress() const {
        return m_progress;
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
    QVariantList undoHistory() const;
    QString undoText() const;
    bool canUndo() const {
        return !m_busy && !m_undo.isEmpty();
    }
    Q_INVOKABLE void undo();
    Q_INVOKABLE void clearUndoHistory();
    Q_INVOKABLE void cancel();
    Q_INVOKABLE void resolveConflict(int choice);
  Q_SIGNALS:
    void changed();
    void undoChanged();
    void errorsChanged();
    void conflictChanged();
    void finished();

  private:
    int conflict(const QString &name, bool folder);
    struct TransferProgress {
        int completed;
        int total;
    };
    bool transfer(GFile *source, GFile *destination, bool move, GError **error,
                  const TransferProgress *progress = nullptr, QString *actualUri = nullptr,
                  bool *replaced = nullptr, bool allowConflict = true);
    bool removeTree(GFile *file, GError **error);
    void report(QString message, double progress = -1, bool force = false);
    QVector<nook::UndoRecord> m_undo;
    bool m_busy = false, m_waiting = false, m_conflictFolder = false;
    QString m_status, m_conflictName, m_kind;
    int m_completedItems = 0, m_totalItems = 0;
    double m_progress = -1;
    QStringList m_errors;
    QTimer m_errorNotifyTimer;
    std::atomic_bool m_cancelled{false};
    GCancellable *m_cancel = nullptr;
    std::thread m_thread;
    std::mutex m_mutex;
    std::condition_variable m_condition;
    int m_choice = -1;
    std::chrono::steady_clock::time_point m_lastReport;
};
