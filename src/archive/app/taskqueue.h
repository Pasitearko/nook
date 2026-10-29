#pragma once
#include <QAbstractListModel>
#include <QElapsedTimer>
#include <QHash>
#include <QPointer>
#include <QVariantMap>
#include <memory>
#include <vector>

class ArchiveJob;
class PasswordStore;

// Every extract, test and create job of the application, in order. A few run at once (the
// concurrency setting); the rest wait their turn and can be reordered. Questions a job asks
// (a password, what to do with a name that exists) go to the window that started it, or to a
// window of their own when that one has closed. Jobs keep running with no window open.
class TaskQueue : public QAbstractListModel {
    Q_OBJECT
    Q_PROPERTY(int count READ count NOTIFY changed)
    Q_PROPERTY(int activeCount READ activeCount NOTIFY changed)
    Q_PROPERTY(int finishedCount READ finishedCount NOTIFY changed)
    Q_PROPERTY(int attentionCount READ attentionCount NOTIFY changed)
    // Of everything still running or waiting, 0–1.
    Q_PROPERTY(double fraction READ fraction NOTIFY changed)
    Q_PROPERTY(int concurrency READ concurrency WRITE setConcurrency NOTIFY changed)
  public:
    enum Role {
        IdRole = Qt::UserRole + 1,
        KindRole,          // extract, test, create, add, delete, rename, convert
        TitleRole,         // 解压“a.zip”
        DetailRole,        // where to
        StateRole,         // queued, running, password, conflict, cancelling, done, failed, cancelled
        StateTextRole,
        FractionRole,
        ProgressTextRole,  // 42% · 剩余约 1 分钟
        CurrentRole,       // the file being worked on
        MessageRole,       // the outcome
        ProblemRole,       // the outcome is a failure or has warnings
        WarningsRole,      // [string]
        FinishedRole,
        CanMoveUpRole,
        CanMoveDownRole,
        CanDeleteCreatedRole,
        TargetRole,        // what "打开位置" shows
        TargetIsFolderRole,
        ArchiveRole,
        QuietRole,         // work for a window (opening a file inside), not shown as a task
    };
    explicit TaskQueue(PasswordStore *passwords, QObject *parent = nullptr);
    ~TaskQueue() override;
    int rowCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    int count() const { return int(m_tasks.size()); }
    int activeCount() const;
    int finishedCount() const { return count() - activeCount(); }
    int attentionCount() const;
    double fraction() const;
    int concurrency() const { return m_concurrency; }
    void setConcurrency(int value);
    void setAutoTryPasswords(bool value) { m_autoTry = value; }

    // request is a worker request (op extract, test or create). extras: saveName (keep the
    // password in the keyring under this name once it worked), deleteSources (create: move the
    // items to the trash afterwards), closesOnSuccess (the owner is a progress window).
    Q_INVOKABLE int start(const QVariantMap &request, const QVariantMap &extras, QObject *owner);
    Q_INVOKABLE void cancel(int id);
    Q_INVOKABLE void remove(int id);
    Q_INVOKABLE void clearFinished();
    Q_INVOKABLE void move(int id, int delta);
    Q_INVOKABLE void submitPassword(int id, const QString &password, const QString &saveName);
    Q_INVOKABLE void resolveConflict(int id, const QString &choice, bool all);
    // Moves what a cancelled or finished extraction created to the trash.
    Q_INVOKABLE void deleteCreated(int id);
    Q_INVOKABLE QVariantMap task(int id) const;
    Q_INVOKABLE bool ownedBy(int id, QObject *window) const;
    Q_INVOKABLE void adopt(int id, QObject *window);
    // Ids of tasks waiting for an answer that window should give (owned by it, or by nobody).
    Q_INVOKABLE QList<int> waitingFor(QObject *window) const;
    // A password that opened this package earlier in this session.
    QString sessionPassword(const QString &archive) const { return m_session.value(archive); }
    void rememberPassword(const QString &archive, const QString &password);
    QObject *owner(int id) const;
    bool closesOnSuccess(int id) const;

  Q_SIGNALS:
    void changed();
    // The task waits for a password or a conflict answer.
    void attention(int id);
    // The task reached done, failed or cancelled.
    void taskFinished(int id);
    void idle();

  private:
    struct Task;
    Task *find(int id) const;
    int rowOf(int id) const;
    void touch(Task *task);
    void pump();
    void launch(Task *task);
    void handle(Task *task, const QVariantMap &event);
    void finish(Task *task, const QVariantMap &result);
    void ask(Task *task);
    QString progressText(const Task &task) const;
    QString stateText(const Task &task) const;
    std::vector<std::unique_ptr<Task>> m_tasks;
    QHash<QString, QString> m_session;
    PasswordStore *m_passwords;
    int m_nextId = 1;
    int m_concurrency = 1;
    bool m_autoTry = false;
};
