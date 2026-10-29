#include "filesessions.h"
#include "taskqueue.h"
#include <QDateTime>
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRandomGenerator>
#include <QStandardPaths>
#include <QUrl>

namespace {
constexpr auto UnsavedMarker = ".nook-unsaved";

QString parentOf(const QString &entry) { return entry.contains(u'/') ? entry.section(u'/', 0, -2) : QString(); }
} // namespace

FileSessions::FileSessions(TaskQueue *queue, QObject *parent) : QObject(parent), m_queue(queue) {
    m_opener = [](const QString &path) { QDesktopServices::openUrl(QUrl::fromLocalFile(path)); };
    connect(m_queue, &TaskQueue::taskFinished, this, &FileSessions::finished);
    const auto changedPath = [this](const QString &path) {
        for (auto &session : m_sessions)
            if (session.open && (session.file == path || session.folder == path)) {
                QTimer *&timer = m_timers[session.id];
                if (!timer) {
                    timer = new QTimer(this);
                    timer->setSingleShot(true);
                    timer->setInterval(450); // editors save in several steps
                    connect(timer, &QTimer::timeout, this, [this, id = session.id] { check(id); });
                }
                timer->start();
            }
    };
    connect(&m_watcher, &QFileSystemWatcher::fileChanged, this, changedPath);
    connect(&m_watcher, &QFileSystemWatcher::directoryChanged, this, changedPath);
}

FileSessions::~FileSessions() {
    for (const auto &session : std::as_const(m_sessions)) {
        if (session.dirty) {
            // Kept for the user: a note says what it was.
            QFile note(session.folder + u'/' + UnsavedMarker);
            if (note.open(QIODevice::WriteOnly))
                note.write((session.archive + u'\n' + session.entry + u'\n').toUtf8());
        } else {
            QDir(session.folder).removeRecursively();
        }
    }
    for (const auto &folder : std::as_const(m_dragFolders))
        QDir(folder).removeRecursively();
}

QString FileSessions::cacheRoot() {
    return QStandardPaths::writableLocation(QStandardPaths::GenericCacheLocation) + "/nook-archive";
}

void FileSessions::sweep() {
    const QDir root(cacheRoot());
    const QDateTime dayAgo = QDateTime::currentDateTime().addDays(-1);
    for (const auto &info : root.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot | QDir::Hidden))
        if (info.lastModified() < dayAgo && !QFileInfo::exists(info.filePath() + u'/' + UnsavedMarker))
            QDir(info.filePath()).removeRecursively();
}

QString FileSessions::newFolder(const QString &kind) const {
    QDir().mkpath(cacheRoot());
    for (int attempt = 0; attempt < 32; ++attempt) {
        const QString path = cacheRoot() + u'/' + kind + u'-' + QString::number(QRandomGenerator::global()->generate(), 16);
        if (QDir().mkdir(path)) {
            QFile::setPermissions(path, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
            return path;
        }
    }
    return {};
}

void FileSessions::open(QObject *owner, const QString &archive, const QString &entry, const QString &encoding, bool readOnly) {
    // The same file opened again: hand the copy we have to its application once more.
    for (auto &session : m_sessions)
        if (session.archive == archive && session.entry == entry && session.open && QFileInfo::exists(session.file)) {
            session.owner = owner;
            m_opener(session.file);
            return;
        }
    const QString folder = newFolder("open");
    if (folder.isEmpty()) {
        Q_EMIT failed(owner, "无法创建临时文件夹：" + cacheRoot());
        return;
    }
    Session session;
    session.id = m_next++;
    session.archive = archive;
    session.entry = entry;
    session.folder = folder;
    session.file = folder + u'/' + entry.section(u'/', -1);
    session.owner = owner;
    session.readOnly = readOnly;
    QVariantMap request{{"op", "extract"}, {"archive", archive}, {"destination", folder}, {"entries", QStringList{entry}},
                        {"subfolder", "never"}, {"conflict", "overwrite"}};
    if (!parentOf(entry).isEmpty())
        request.insert("prefix", parentOf(entry));
    if (!encoding.isEmpty() && encoding != "auto")
        request.insert("encoding", encoding);
    m_sessions.insert(session.id, session);
    m_extracting.insert(m_queue->start(request, {{"quiet", true}}, owner), session.id);
}

int FileSessions::exportForDrag(QObject *owner, const QString &archive, const QStringList &entries, const QString &prefix,
                                const QString &encoding) {
    const QString folder = newFolder("drag");
    if (folder.isEmpty() || entries.isEmpty())
        return 0;
    QVariantMap request{{"op", "extract"}, {"archive", archive}, {"destination", folder}, {"entries", entries},
                        {"subfolder", "never"}, {"conflict", "overwrite"}};
    if (!prefix.isEmpty())
        request.insert("prefix", prefix);
    if (!encoding.isEmpty() && encoding != "auto")
        request.insert("encoding", encoding);
    const int token = m_next++;
    m_dragFolders.insert(token, folder);
    m_dragging.insert(m_queue->start(request, {{"quiet", true}}, owner), token);
    return token;
}

void FileSessions::finished(int task) {
    const QVariantMap info = m_queue->task(task);
    const bool ok = info.value("status").toString() == "done";
    if (m_extracting.contains(task)) {
        const int id = m_extracting.take(task);
        // Not a task the user started; it leaves the list once everyone has seen it end.
        QMetaObject::invokeMethod(m_queue, [queue = m_queue, task] { queue->remove(task); }, Qt::QueuedConnection);
        auto it = m_sessions.find(id);
        if (it == m_sessions.end())
            return;
        if (!ok || !QFileInfo(it->file).isFile()) {
            if (info.value("status").toString() != "cancelled")
                Q_EMIT failed(it->owner, "无法打开“" + it->entry.section(u'/', -1) + "”：" + info.value("message").toString());
            QDir(it->folder).removeRecursively();
            m_sessions.erase(it);
            return;
        }
        const QFileInfo file(it->file);
        it->savedTime = it->answeredTime = file.lastModified().toMSecsSinceEpoch();
        it->savedSize = it->answeredSize = file.size();
        it->open = true;
        watch(*it);
        m_opener(it->file);
        Q_EMIT changed();
    } else if (m_dragging.contains(task)) {
        const int token = m_dragging.take(task);
        QMetaObject::invokeMethod(m_queue, [queue = m_queue, task] { queue->remove(task); }, Qt::QueuedConnection);
        QStringList urls;
        const QDir folder(m_dragFolders.value(token));
        if (ok)
            for (const auto &name : folder.entryList(QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot, QDir::Name))
                urls << QUrl::fromLocalFile(folder.filePath(name)).toString();
        Q_EMIT dragReady(token, urls);
    } else {
        for (auto &session : m_sessions)
            if (session.writing == task) {
                session.writing = 0;
                if (ok) {
                    const QFileInfo file(session.file);
                    session.savedTime = session.answeredTime = file.lastModified().toMSecsSinceEpoch();
                    session.savedSize = session.answeredSize = file.size();
                    session.dirty = false;
                }
                Q_EMIT written(session.id, ok, info.value("message").toString());
                Q_EMIT changed();
                // Saved again while it was being written: ask about the newer change.
                check(session.id);
            }
    }
}

void FileSessions::watch(Session &session) {
    if (!m_watcher.files().contains(session.file))
        m_watcher.addPath(session.file);
    if (!m_watcher.directories().contains(session.folder))
        m_watcher.addPath(session.folder);
}

void FileSessions::check(int id) {
    auto it = m_sessions.find(id);
    if (it == m_sessions.end() || !it->open || it->writing)
        return;
    watch(*it); // an editor that saves by replacing the file drops it from the watcher
    const QFileInfo file(it->file);
    if (!file.isFile())
        return;
    const qint64 time = file.lastModified().toMSecsSinceEpoch(), size = file.size();
    if (time == it->savedTime && size == it->savedSize)
        return;
    it->dirty = true;
    Q_EMIT changed();
    if (time != it->answeredTime || size != it->answeredSize) {
        it->answeredTime = time;
        it->answeredSize = size;
        Q_EMIT modified(id);
    }
}

void FileSessions::writeBack(int id) {
    auto it = m_sessions.find(id);
    if (it == m_sessions.end() || it->readOnly || it->writing)
        return;
    QVariantMap request{{"op", "add"}, {"archive", it->archive}, {"items", QStringList{it->file}}, {"conflict", "overwrite"}};
    if (!parentOf(it->entry).isEmpty())
        request.insert("prefix", parentOf(it->entry));
    it->writing = m_queue->start(request, {}, it->owner);
}

void FileSessions::decline(int id) { Q_UNUSED(id); }

QString FileSessions::saveCopy(int id, const QString &folder) {
    const auto it = m_sessions.constFind(id);
    if (it == m_sessions.constEnd())
        return {};
    const QFileInfo source(it->file);
    QString name = source.fileName();
    const QDir dir(folder);
    for (int number = 2; dir.exists(name) && number < 10000; ++number)
        name = QStringLiteral("%1 (%2)%3").arg(source.completeBaseName()).arg(number).arg(source.suffix().isEmpty() ? QString() : "." + source.suffix());
    const QString target = dir.filePath(name);
    if (!QFile::copy(it->file, target))
        return {};
    auto &session = m_sessions[id];
    session.dirty = false;
    session.savedTime = source.lastModified().toMSecsSinceEpoch();
    session.savedSize = source.size();
    Q_EMIT changed();
    return target;
}

QVariantMap FileSessions::session(int id) const {
    const auto it = m_sessions.constFind(id);
    if (it == m_sessions.constEnd())
        return {};
    return {{"id", it->id},
            {"archive", it->archive},
            {"archiveName", QFileInfo(it->archive).fileName()},
            {"entry", it->entry},
            {"name", it->entry.section(u'/', -1)},
            {"file", it->file},
            {"readOnly", it->readOnly},
            {"dirty", it->dirty},
            {"writing", it->writing != 0}};
}

bool FileSessions::ownedBy(int id, QObject *window) const {
    const auto it = m_sessions.constFind(id);
    return it != m_sessions.constEnd() && it->owner == window;
}

QVariantList FileSessions::unsaved(const QString &archive) const {
    QVariantList list;
    for (const auto &session : m_sessions)
        if (session.dirty && (archive.isEmpty() || session.archive == archive))
            list << QVariantMap{{"id", session.id}, {"name", session.entry.section(u'/', -1)}, {"readOnly", session.readOnly}};
    return list;
}

void FileSessions::discard(const QString &archive) {
    for (auto it = m_sessions.begin(); it != m_sessions.end();) {
        if (it->archive == archive && !it->writing) {
            m_watcher.removePath(it->file);
            m_watcher.removePath(it->folder);
            QDir(it->folder).removeRecursively();
            it = m_sessions.erase(it);
        } else {
            ++it;
        }
    }
    Q_EMIT changed();
}

int FileSessions::unsavedCount() const {
    return int(std::count_if(m_sessions.begin(), m_sessions.end(), [](const Session &s) { return s.dirty; }));
}
