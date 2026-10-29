#include "notifier.h"
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QVariantMap>

namespace {
constexpr auto Service = "org.freedesktop.Notifications";
constexpr auto Path = "/org/freedesktop/Notifications";
} // namespace

Notifier::Notifier(QObject *parent) : QObject(parent) {
    auto bus = QDBusConnection::sessionBus();
    bus.connect(Service, Path, Service, "ActionInvoked", this, SLOT(invoked(uint, QString)));
    bus.connect(Service, Path, Service, "NotificationClosed", this, SLOT(closed(uint, uint)));
}

void Notifier::notify(const QString &title, const QString &body, bool problem, const QString &actionLabel,
                      std::function<void()> onAction) {
    ++m_sent;
    QStringList actions;
    if (onAction) {
        actions << "default" << actionLabel;
        if (!actionLabel.isEmpty())
            actions << "open" << actionLabel;
    }
    const QVariantMap hints{{"desktop-entry", "local.nook.archive"}, {"urgency", QVariant::fromValue<uchar>(problem ? 2 : 1)}};
    QDBusMessage call = QDBusMessage::createMethodCall(Service, Path, Service, "Notify");
    call << QStringLiteral("nook解压缩") << uint(0) << QStringLiteral("nook-archive") << title << body << actions
         << hints << int(problem ? 0 : 8000);
    auto *watcher = new QDBusPendingCallWatcher(QDBusConnection::sessionBus().asyncCall(call), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this, [this, watcher, onAction] {
        watcher->deleteLater();
        const QDBusPendingReply<uint> reply = *watcher;
        if (!reply.isError() && onAction)
            m_actions.insert(reply.value(), onAction);
    });
}

void Notifier::invoked(uint id, const QString &) {
    const auto action = m_actions.take(id);
    if (action)
        action();
}

void Notifier::closed(uint id, uint) { m_actions.remove(id); }
