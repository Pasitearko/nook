#include "archivejob.h"
#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <csignal>

ArchiveJob::ArchiveJob(QObject *parent) : QObject(parent) {
    m_killTimer.setSingleShot(true);
    m_killTimer.setInterval(5000);
    connect(&m_killTimer, &QTimer::timeout, this, [this] {
        if (m_process.state() != QProcess::NotRunning)
            ::kill(-pid_t(m_process.processId()), SIGKILL);
    });
    connect(&m_process, &QProcess::readyReadStandardOutput, this, &ArchiveJob::read);
    connect(&m_process, &QProcess::readyReadStandardError, this, [this] {
        m_stderr += m_process.readAllStandardError();
        if (m_stderr.size() > 8192)
            m_stderr = m_stderr.right(8192);
    });
    connect(&m_process, &QProcess::stateChanged, this, &ArchiveJob::runningChanged);
    connect(&m_process, &QProcess::finished, this, &ArchiveJob::ended);
    connect(&m_process, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart)
            ended();
    });
}

ArchiveJob::~ArchiveJob() {
    if (m_process.state() != QProcess::NotRunning) {
        disconnect(&m_process, nullptr, this, nullptr);
        ::kill(-pid_t(m_process.processId()), SIGKILL);
        m_process.waitForFinished(2000);
        if (!m_staging.isEmpty())
            QDir(m_staging).removeRecursively();
    }
}

QString ArchiveJob::workerPath() {
    const QString configured = qEnvironmentVariable("NOOK_ARCHIVE_WORKER");
    if (!configured.isEmpty())
        return configured;
    const QString beside = QCoreApplication::applicationDirPath() + "/nook-archive-worker";
    if (QFileInfo(beside).isExecutable())
        return beside;
    return QStringLiteral(NOOK_ARCHIVE_WORKER);
}

bool ArchiveJob::start(const QVariantMap &request) {
    if (running())
        return false;
    m_done = false;
    m_result = {};
    m_buffer.clear();
    m_stderr.clear();
    m_staging.clear();
    m_process.setProgram(workerPath());
    m_process.start(QIODevice::ReadWrite);
    send(request);
    return true;
}

void ArchiveJob::send(const QVariantMap &command) {
    if (m_process.state() == QProcess::NotRunning)
        return;
    m_process.write(QJsonDocument(QJsonObject::fromVariantMap(command)).toJson(QJsonDocument::Compact) + '\n');
}

void ArchiveJob::sendPassword(const QString &password) { send({{"cmd", "password"}, {"value", password}}); }

void ArchiveJob::resolveConflict(int id, const QString &choice, bool all) {
    send({{"cmd", "resolve"}, {"id", id}, {"choice", choice}, {"all", all}});
}

void ArchiveJob::cancel() {
    if (!running())
        return;
    send({{"cmd", "cancel"}});
    m_killTimer.start();
}

void ArchiveJob::read() {
    m_buffer += m_process.readAllStandardOutput();
    int newline;
    while ((newline = int(m_buffer.indexOf('\n'))) >= 0) {
        const QByteArray line = m_buffer.left(newline);
        m_buffer.remove(0, newline + 1);
        const QVariantMap event = QJsonDocument::fromJson(line).object().toVariantMap();
        if (event.isEmpty())
            continue;
        const QString type = event.value("event").toString();
        if (type == "staging")
            m_staging = event.value("path").toString();
        if (type == "done") {
            m_done = true;
            m_result = event;
        }
        Q_EMIT workerEvent(event);
    }
}

void ArchiveJob::ended() {
    m_killTimer.stop();
    read();
    if (!m_done) {
        // Killed or crashed: whatever it was staging is incomplete.
        const QString name = QFileInfo(m_staging).fileName();
        if (name.startsWith(".nook-extract-") || name.startsWith(".nook-create-"))
            QDir(m_staging).removeRecursively();
        const bool missing = m_process.error() == QProcess::FailedToStart;
        m_result = {{"event", "done"},
                    {"ok", false},
                    {"error", QVariantMap{{"code", missing ? "missing-worker" : "crashed"},
                                          {"message", missing ? "找不到解压缩后台程序" : "解压缩后台程序意外退出"}}},
                    {"stderr", QString::fromUtf8(m_stderr)}};
        m_done = true;
        Q_EMIT workerEvent(m_result);
    }
    Q_EMIT finished(m_result);
}
