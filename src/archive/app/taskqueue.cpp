#include "taskqueue.h"
#include "archivejob.h"
#include "files.h"
#include "passwordstore.h"
#include <QDir>
#include <QFileInfo>
#include <gio/gio.h>

struct TaskQueue::Task {
    int id = 0;
    QString kind;
    QVariantMap request;
    QVariantMap extras;
    QPointer<QObject> owner;
    QString state = "queued";
    double fraction = 0;
    QString phase;
    QString current;
    int files = 0;
    QElapsedTimer clock;
    std::unique_ptr<ArchiveJob> job;
    QStringList candidates;   // saved passwords still to try without asking
    QString sentPassword;     // the one the worker is checking now
    bool typedPassword = false;
    QString saveName;
    bool passwordWrong = false;
    QVariantMap conflict;
    QString message;
    bool problem = false;
    QStringList warnings;
    QStringList created;
    QString destination;
    QString target;
    bool targetIsFolder = false;
    bool createdDeleted = false;

    bool finished() const { return state == "done" || state == "failed" || state == "cancelled"; }
    bool running() const { return !finished() && state != "queued"; }
    QString archive() const { return request.value(kind == "create" ? "output" : "archive").toString(); }
};

namespace {
QString quoted(const QString &name) { return QStringLiteral("“%1”").arg(name); }

QString duration(qint64 seconds) {
    if (seconds < 60)
        return QStringLiteral("%1 秒").arg(qMax<qint64>(1, seconds));
    if (seconds < 3600)
        return QStringLiteral("%1 分钟").arg((seconds + 30) / 60);
    return QStringLiteral("%1 小时 %2 分").arg(seconds / 3600).arg((seconds % 3600) / 60);
}
} // namespace

TaskQueue::TaskQueue(PasswordStore *passwords, QObject *parent) : QAbstractListModel(parent), m_passwords(passwords) {}

TaskQueue::~TaskQueue() = default;

int TaskQueue::rowCount(const QModelIndex &parent) const { return parent.isValid() ? 0 : count(); }

QHash<int, QByteArray> TaskQueue::roleNames() const {
    return {{IdRole, "taskId"},
            {KindRole, "kind"},
            {TitleRole, "title"},
            {DetailRole, "detail"},
            {StateRole, "status"},
            {StateTextRole, "statusText"},
            {FractionRole, "fraction"},
            {ProgressTextRole, "progressText"},
            {CurrentRole, "current"},
            {MessageRole, "message"},
            {ProblemRole, "problem"},
            {WarningsRole, "warnings"},
            {FinishedRole, "finished"},
            {CanMoveUpRole, "canMoveUp"},
            {CanMoveDownRole, "canMoveDown"},
            {CanDeleteCreatedRole, "canDeleteCreated"},
            {TargetRole, "target"},
            {TargetIsFolderRole, "targetIsFolder"},
            {ArchiveRole, "archive"}};
}

QVariant TaskQueue::data(const QModelIndex &index, int role) const {
    if (!index.isValid() || index.row() >= count())
        return {};
    const Task &task = *m_tasks[size_t(index.row())];
    const QString name = QFileInfo(task.archive()).fileName();
    switch (role) {
    case IdRole:
        return task.id;
    case KindRole:
        return task.kind;
    case TitleRole:
        return (task.kind == "extract" ? QStringLiteral("解压")
                : task.kind == "test"  ? QStringLiteral("测试")
                                       : QStringLiteral("压缩为")) +
               quoted(name);
    case DetailRole:
        if (task.kind == "extract")
            return "到 " + QDir::toNativeSeparators(task.request.value("destination").toString());
        if (task.kind == "create")
            return "保存到 " + QDir::toNativeSeparators(QFileInfo(task.archive()).absolutePath());
        return QDir::toNativeSeparators(QFileInfo(task.archive()).absolutePath());
    case StateRole:
        return task.state;
    case StateTextRole:
        return stateText(task);
    case FractionRole:
        return task.state == "done" ? 1.0 : task.fraction;
    case ProgressTextRole:
        return progressText(task);
    case CurrentRole:
        return task.current;
    case MessageRole:
        return task.message;
    case ProblemRole:
        return task.problem;
    case WarningsRole:
        return task.warnings;
    case FinishedRole:
        return task.finished();
    case CanMoveUpRole:
    case CanMoveDownRole: {
        if (task.state != "queued")
            return false;
        const int step = role == CanMoveUpRole ? -1 : 1;
        for (int row = index.row() + step; row >= 0 && row < count(); row += step)
            if (m_tasks[size_t(row)]->state == "queued")
                return true;
        return false;
    }
    case CanDeleteCreatedRole:
        return task.kind == "extract" && task.finished() && !task.created.isEmpty() && !task.createdDeleted;
    case TargetRole:
        return task.target;
    case TargetIsFolderRole:
        return task.targetIsFolder;
    case ArchiveRole:
        return task.archive();
    }
    return {};
}

QString TaskQueue::stateText(const Task &task) const {
    if (task.state == "queued")
        return QStringLiteral("排队中");
    if (task.state == "password")
        return task.passwordWrong ? QStringLiteral("密码不正确，等待重新输入") : QStringLiteral("等待输入密码");
    if (task.state == "conflict")
        return QStringLiteral("等待处理同名项目");
    if (task.state == "cancelling")
        return QStringLiteral("正在取消…");
    if (task.state == "done")
        return task.problem ? QStringLiteral("已完成，有提醒") : QStringLiteral("已完成");
    if (task.state == "failed")
        return QStringLiteral("失败");
    if (task.state == "cancelled")
        return QStringLiteral("已取消");
    if (task.phase == "commit")
        return QStringLiteral("正在移入目标位置…");
    if (task.phase == "test")
        return task.kind == "create" ? QStringLiteral("正在校验…") : QStringLiteral("正在测试…");
    if (task.kind == "extract")
        return QStringLiteral("正在解压…");
    if (task.kind == "create")
        return QStringLiteral("正在压缩…");
    return QStringLiteral("正在测试…");
}

QString TaskQueue::progressText(const Task &task) const {
    if (!task.running() || task.state != "running")
        return {};
    QString text = QString::number(int(task.fraction * 100)) + "%";
    const qint64 elapsed = task.clock.isValid() ? task.clock.elapsed() / 1000 : 0;
    if (task.fraction > 0.02 && elapsed >= 2)
        text += " · 剩余约 " + duration(qint64(elapsed * (1 - task.fraction) / task.fraction));
    return text;
}

int TaskQueue::activeCount() const {
    return int(std::count_if(m_tasks.begin(), m_tasks.end(), [](const auto &task) { return !task->finished(); }));
}

int TaskQueue::attentionCount() const {
    return int(std::count_if(m_tasks.begin(), m_tasks.end(),
                             [](const auto &task) { return task->state == "password" || task->state == "conflict"; }));
}

double TaskQueue::fraction() const {
    double sum = 0;
    int active = 0;
    for (const auto &task : m_tasks)
        if (!task->finished()) {
            sum += task->fraction;
            ++active;
        }
    return active ? sum / active : 1.0;
}

void TaskQueue::setConcurrency(int value) {
    value = qBound(1, value, 4);
    if (value == m_concurrency)
        return;
    m_concurrency = value;
    Q_EMIT changed();
    pump();
}

TaskQueue::Task *TaskQueue::find(int id) const {
    for (const auto &task : m_tasks)
        if (task->id == id)
            return task.get();
    return nullptr;
}

int TaskQueue::rowOf(int id) const {
    for (size_t row = 0; row < m_tasks.size(); ++row)
        if (m_tasks[row]->id == id)
            return int(row);
    return -1;
}

void TaskQueue::touch(Task *task) {
    const int row = rowOf(task->id);
    if (row >= 0)
        Q_EMIT dataChanged(index(row), index(row));
    Q_EMIT changed();
}

int TaskQueue::start(const QVariantMap &request, const QVariantMap &extras, QObject *owner) {
    auto task = std::make_unique<Task>();
    task->id = m_nextId++;
    task->kind = request.value("op").toString();
    task->request = request;
    task->extras = extras;
    task->owner = owner;
    task->saveName = extras.value("saveName").toString();
    if (owner)
        connect(owner, &QObject::destroyed, this, [this, id = task->id] {
            // The window that would have answered is gone; whoever is left answers instead.
            QMetaObject::invokeMethod(this, [this, id] {
                if (Task *t = find(id); t && (t->state == "password" || t->state == "conflict"))
                    Q_EMIT attention(id);
            }, Qt::QueuedConnection);
        });
    if (task->kind == "extract")
        task->destination = request.value("destination").toString();
    const int row = count();
    beginInsertRows({}, row, row);
    m_tasks.push_back(std::move(task));
    endInsertRows();
    // Rows above may now be able to move down.
    if (row > 0)
        Q_EMIT dataChanged(index(0), index(row - 1));
    Q_EMIT changed();
    pump();
    return m_tasks[size_t(row)]->id;
}

void TaskQueue::pump() {
    int running = 0;
    for (const auto &task : m_tasks)
        running += task->running() ? 1 : 0;
    for (const auto &task : m_tasks) {
        if (running >= m_concurrency)
            break;
        if (task->state == "queued") {
            launch(task.get());
            ++running;
        }
    }
}

void TaskQueue::launch(Task *task) {
    QVariantMap request = task->request;
    const QString archive = request.value("archive").toString();
    if (task->kind != "create") {
        if (m_autoTry && m_passwords)
            task->candidates = m_passwords->passwords();
        if (!request.contains("password") && m_session.contains(archive))
            request.insert("password", m_session.value(archive));
        else if (!request.contains("password") && !task->candidates.isEmpty())
            request.insert("password", task->candidates.takeFirst());
    }
    task->sentPassword = request.value("password").toString();
    task->typedPassword = task->request.contains("password");
    task->state = "running";
    task->clock.start();
    task->job = std::make_unique<ArchiveJob>();
    const int id = task->id;
    connect(task->job.get(), &ArchiveJob::workerEvent, this, [this, id](const QVariantMap &event) {
        if (Task *t = find(id))
            handle(t, event);
    });
    connect(task->job.get(), &ArchiveJob::finished, this, [this, id](const QVariantMap &result) {
        if (Task *t = find(id))
            finish(t, result);
    });
    touch(task);
    if (!task->job->start(request))
        finish(task, {{"ok", false}, {"error", QVariantMap{{"code", "missing-worker"}, {"message", "找不到 nook解压缩 的后台程序"}}}});
}

void TaskQueue::handle(Task *task, const QVariantMap &event) {
    const QString type = event.value("event").toString();
    if (type == "progress") {
        if (event.contains("fraction"))
            task->fraction = qBound(0.0, event.value("fraction").toDouble(), 1.0);
        task->phase = event.value("phase").toString();
        if (event.contains("current"))
            task->current = event.value("current").toString();
        task->files = event.value("files").toInt();
        touch(task);
    } else if (type == "need-password") {
        if (!task->candidates.isEmpty()) {
            task->sentPassword = task->candidates.takeFirst();
            task->job->sendPassword(task->sentPassword);
            return;
        }
        task->passwordWrong = event.value("wrong").toBool() && task->typedPassword;
        task->state = "password";
        ask(task);
    } else if (type == "conflict") {
        task->conflict = event;
        task->state = "conflict";
        ask(task);
    } else if (type == "warning") {
        if (task->warnings.size() < 200)
            task->warnings << event.value("message").toString();
        task->problem = true;
    } else if (type == "error") {
        if (task->warnings.size() < 200)
            task->warnings << event.value("path").toString() + "：" + event.value("message").toString();
        task->problem = true;
    }
}

void TaskQueue::ask(Task *task) {
    touch(task);
    Q_EMIT attention(task->id);
}

void TaskQueue::submitPassword(int id, const QString &password, const QString &saveName) {
    Task *task = find(id);
    if (!task || task->state != "password" || !task->job)
        return;
    task->sentPassword = password;
    task->typedPassword = true;
    if (!saveName.trimmed().isEmpty())
        task->saveName = saveName.trimmed();
    task->state = "running";
    task->job->sendPassword(password);
    touch(task);
}

void TaskQueue::resolveConflict(int id, const QString &choice, bool all) {
    Task *task = find(id);
    if (!task || task->state != "conflict" || !task->job)
        return;
    task->job->resolveConflict(task->conflict.value("id").toInt(), choice, all);
    task->conflict.clear();
    task->state = "running";
    touch(task);
}

void TaskQueue::finish(Task *task, const QVariantMap &result) {
    if (task->finished())
        return;
    // finish() runs inside the job's own signal; it goes once that has returned.
    if (task->job)
        task->job.release()->deleteLater();
    const bool ok = result.value("ok").toBool();
    const bool cancelled = result.value("cancelled").toBool();
    task->state = ok ? "done" : cancelled ? "cancelled" : "failed";
    task->fraction = ok ? 1.0 : task->fraction;
    task->current.clear();
    task->conflict.clear();
    const QString archive = task->request.value("archive").toString();
    if (ok && !task->sentPassword.isEmpty()) {
        if (task->kind != "create")
            m_session.insert(archive, task->sentPassword);
        if (!task->saveName.isEmpty() && m_passwords)
            m_passwords->save(task->saveName, task->sentPassword);
    }
    const QVariantMap error = result.value("error").toMap();
    const int skipped = result.value("skipped").toInt();
    for (const auto &path : result.value("created").toList())
        task->created << path.toString();
    if (task->kind == "extract") {
        task->destination = result.value("destination", task->destination).toString();
        if (task->created.size() == 1) {
            task->target = task->destination + u'/' + task->created.first();
            task->targetIsFolder = false;
        } else {
            task->target = task->destination;
            task->targetIsFolder = true;
        }
    }
    QStringList notes;
    if (skipped > 0)
        notes << QStringLiteral("跳过 %1 个同名项目").arg(skipped);
    if (!task->warnings.isEmpty())
        notes << QStringLiteral("%1 条提醒").arg(task->warnings.size());
    const QString tail = notes.isEmpty() ? QString() : "（" + notes.join("，") + "）";
    if (ok && task->kind == "extract") {
        task->message = task->created.isEmpty() ? "没有解压任何项目" + tail
                                                : QStringLiteral("已解压 %1 个项目到%2").arg(task->created.size()).arg(quoted(QFileInfo(task->destination).fileName().isEmpty() ? task->destination : QFileInfo(task->destination).fileName())) + tail;
    } else if (ok && task->kind == "test") {
        const int tested = result.value("tested").toInt();
        task->message = tested > 0 ? QStringLiteral("测试通过：%1 个文件都没有错误").arg(tested) : QStringLiteral("测试通过，没有发现错误");
    } else if (ok && task->kind == "create") {
        const QString output = result.value("output", task->archive()).toString();
        task->target = output;
        task->targetIsFolder = false;
        task->message = QStringLiteral("已创建%1（%2）").arg(quoted(QFileInfo(output).fileName()), nook::formatSize(QFileInfo(output).size())) + tail;
        if (task->extras.value("deleteSources").toBool()) {
            int failed = 0;
            for (const auto &item : task->request.value("items").toStringList()) {
                GFile *file = g_file_new_for_path(QFile::encodeName(item).constData());
                if (!g_file_trash(file, nullptr, nullptr))
                    ++failed;
                g_object_unref(file);
            }
            task->message += failed ? QStringLiteral("；%1 个源文件没能移到回收站").arg(failed) : QStringLiteral("，源文件已移到回收站");
            task->problem = task->problem || failed > 0;
        }
    } else if (cancelled) {
        task->message = task->kind == "extract" && !task->created.isEmpty()
                            ? QStringLiteral("已取消；已完整解压的 %1 个项目保留在目标位置").arg(task->created.size())
                            : QStringLiteral("已取消");
    } else {
        task->problem = true;
        task->message = error.value("message").toString();
        if (task->message.isEmpty())
            task->message = QStringLiteral("出错了");
        const QVariantList errors = result.value("errors").toList();
        for (const auto &entry : errors)
            if (task->warnings.size() < 200) {
                const QVariantMap map = entry.toMap();
                task->warnings << (map.isEmpty() ? entry.toString() : map.value("path").toString() + "：" + map.value("message").toString());
            }
        if (task->kind == "test" && !errors.isEmpty())
            task->message = QStringLiteral("发现 %1 处错误").arg(errors.size());
    }
    touch(task);
    Q_EMIT taskFinished(task->id);
    pump();
    if (activeCount() == 0)
        Q_EMIT idle();
}

void TaskQueue::cancel(int id) {
    Task *task = find(id);
    if (!task || task->finished())
        return;
    if (task->state == "queued") {
        task->state = "cancelled";
        task->message = QStringLiteral("已取消");
        touch(task);
        Q_EMIT taskFinished(id);
        if (activeCount() == 0)
            Q_EMIT idle();
        return;
    }
    task->state = "cancelling";
    task->candidates.clear();
    touch(task);
    if (task->job)
        task->job->cancel();
}

void TaskQueue::remove(int id) {
    const int row = rowOf(id);
    if (row < 0 || !m_tasks[size_t(row)]->finished())
        return;
    beginRemoveRows({}, row, row);
    m_tasks.erase(m_tasks.begin() + row);
    endRemoveRows();
    Q_EMIT changed();
}

void TaskQueue::clearFinished() {
    for (int row = count() - 1; row >= 0; --row)
        if (m_tasks[size_t(row)]->finished()) {
            beginRemoveRows({}, row, row);
            m_tasks.erase(m_tasks.begin() + row);
            endRemoveRows();
        }
    Q_EMIT changed();
}

void TaskQueue::move(int id, int delta) {
    const int row = rowOf(id);
    if (row < 0 || m_tasks[size_t(row)]->state != "queued" || delta == 0)
        return;
    const int step = delta < 0 ? -1 : 1;
    int other = row + step;
    while (other >= 0 && other < count() && m_tasks[size_t(other)]->state != "queued")
        other += step;
    if (other < 0 || other >= count())
        return;
    Q_EMIT layoutAboutToBeChanged();
    std::swap(m_tasks[size_t(row)], m_tasks[size_t(other)]);
    Q_EMIT layoutChanged();
    Q_EMIT dataChanged(index(0), index(count() - 1));
    Q_EMIT changed();
}

void TaskQueue::deleteCreated(int id) {
    Task *task = find(id);
    if (!task || task->kind != "extract" || !task->finished() || task->createdDeleted)
        return;
    int failed = 0;
    for (const auto &relative : std::as_const(task->created)) {
        GFile *file = g_file_new_for_path(QFile::encodeName(task->destination + u'/' + relative).constData());
        if (!g_file_trash(file, nullptr, nullptr))
            ++failed;
        g_object_unref(file);
    }
    task->createdDeleted = true;
    task->message = failed ? QStringLiteral("有 %1 个项目没能移到回收站").arg(failed)
                           : QStringLiteral("本次解压的 %1 个项目已移到回收站").arg(task->created.size());
    task->problem = failed > 0;
    touch(task);
}

QVariantMap TaskQueue::task(int id) const {
    const int row = rowOf(id);
    if (row < 0)
        return {};
    QVariantMap map;
    const auto names = roleNames();
    for (auto it = names.begin(); it != names.end(); ++it)
        map.insert(QString::fromUtf8(it.value()), data(index(row), it.key()));
    const Task &task = *m_tasks[size_t(row)];
    map.insert("conflict", task.conflict);
    map.insert("passwordWrong", task.passwordWrong);
    map.insert("archiveName", QFileInfo(task.archive()).fileName());
    map.insert("archiveStem", QFileInfo(task.archive()).completeBaseName());
    map.insert("openAfter", task.extras.value("openAfter").toBool());
    return map;
}

QObject *TaskQueue::owner(int id) const {
    Task *task = find(id);
    return task ? task->owner.data() : nullptr;
}

bool TaskQueue::ownedBy(int id, QObject *window) const {
    Task *task = find(id);
    return task && task->owner == window;
}

void TaskQueue::adopt(int id, QObject *window) {
    if (Task *task = find(id))
        task->owner = window;
}

bool TaskQueue::closesOnSuccess(int id) const {
    Task *task = find(id);
    return task && task->extras.value("closesOnSuccess").toBool();
}

QList<int> TaskQueue::waitingFor(QObject *window) const {
    QList<int> ids;
    for (const auto &task : m_tasks)
        if ((task->state == "password" || task->state == "conflict") && (task->owner == window || !task->owner))
            ids << task->id;
    return ids;
}

void TaskQueue::rememberPassword(const QString &archive, const QString &password) {
    if (!archive.isEmpty() && !password.isEmpty())
        m_session.insert(archive, password);
}
