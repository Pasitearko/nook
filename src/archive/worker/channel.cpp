#include "worker.h"
#include <QDateTime>
#include <QJsonDocument>
#include <cstdio>
#include <iostream>
#include <thread>

namespace nook::archive::worker {

Channel &Channel::instance() {
    static Channel channel;
    return channel;
}

QJsonObject Channel::readRequest() {
    std::string line;
    if (!std::getline(std::cin, line))
        return {};
    return QJsonDocument::fromJson(QByteArray::fromStdString(line)).object();
}

void Channel::listen() {
    std::thread([this] {
        std::string line;
        while (std::getline(std::cin, line)) {
            const QJsonObject command = QJsonDocument::fromJson(QByteArray::fromStdString(line)).object();
            if (command.value("cmd").toString() == "cancel") {
                m_cancelled = true;
                m_ready.notify_all();
                continue;
            }
            std::lock_guard lock(m_mutex);
            m_commands.push_back(command);
            m_ready.notify_all();
        }
        // The client went away: nobody can answer a question or see the result.
        std::lock_guard lock(m_mutex);
        m_closed = true;
        m_cancelled = true;
        m_ready.notify_all();
    }).detach();
}

void Channel::send(const QJsonObject &event) {
    const QByteArray line = QJsonDocument(event).toJson(QJsonDocument::Compact) + '\n';
    std::lock_guard lock(m_writeMutex);
    std::fwrite(line.constData(), 1, size_t(line.size()), stdout);
    std::fflush(stdout);
}

void Channel::progress(QJsonObject event, bool force) {
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (!force && now - m_lastProgress < 100)
        return;
    m_lastProgress = now;
    event.insert("event", "progress");
    send(event);
}

std::optional<QJsonObject> Channel::await(const QString &command) {
    std::unique_lock lock(m_mutex);
    for (;;) {
        for (auto it = m_commands.begin(); it != m_commands.end(); ++it) {
            if (it->value("cmd").toString() == command) {
                QJsonObject found = *it;
                m_commands.erase(it);
                return found;
            }
        }
        if (m_cancelled || m_closed)
            return std::nullopt;
        m_ready.wait(lock);
    }
}

void warn(const QString &code, const QString &path, const QString &message) {
    Channel::instance().send({{"event", "warning"}, {"code", code}, {"path", path}, {"message", message}});
}

QJsonObject failure(const QString &code, const QString &message) {
    return {{"code", code}, {"message", message}};
}

std::optional<QString> askPassword(bool wrong) {
    auto &channel = Channel::instance();
    channel.send({{"event", "need-password"}, {"wrong", wrong}});
    const auto answer = channel.await("password");
    if (!answer)
        return std::nullopt;
    return answer->value("value").toString();
}

Choice Conflicts::resolve(const QString &path, const struct stat &existing, const struct stat &incoming) {
    if (policy == "overwrite")
        return Choice::Overwrite;
    if (policy == "skip")
        return Choice::Skip;
    if (policy == "rename")
        return Choice::Rename;
    auto &channel = Channel::instance();
    if (channel.cancelled())
        return Choice::Skip;
    const auto describe = [](const struct stat &st) {
        return QJsonObject{{"size", double(st.st_size)},
                           {"mtime", double(st.st_mtim.tv_sec)},
                           {"directory", S_ISDIR(st.st_mode)}};
    };
    const int id = ++counter;
    channel.send({{"event", "conflict"}, {"id", id}, {"path", path},
                  {"existing", describe(existing)}, {"incoming", describe(incoming)}});
    const auto answer = channel.await("resolve");
    if (!answer)
        return Choice::Skip;
    const QString choice = answer->value("choice").toString();
    if (answer->value("all").toBool())
        policy = choice;
    return choice == "overwrite" ? Choice::Overwrite : choice == "rename" ? Choice::Rename : Choice::Skip;
}

}
