#include "appearance.h"
#include <QDBusArgument>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QGuiApplication>
#include <QStyleHints>

namespace {
const QString service = QStringLiteral("org.freedesktop.portal.Desktop");
const QString path = QStringLiteral("/org/freedesktop/portal/desktop");
const QString settings = QStringLiteral("org.freedesktop.portal.Settings");
const QString group = QStringLiteral("org.freedesktop.appearance");
const QString key = QStringLiteral("color-scheme");

// Settings.Read wraps the value in one variant too many; ReadOne does not. Unwrap either.
QVariant unwrap(QVariant value) {
    for (int i = 0; i < 3; ++i) {
        if (value.canConvert<QDBusVariant>() && value.metaType() == QMetaType::fromType<QDBusVariant>())
            value = value.value<QDBusVariant>().variant();
        else if (value.metaType() == QMetaType::fromType<QDBusArgument>()) {
            QDBusVariant inner;
            value.value<QDBusArgument>() >> inner;
            value = inner.variant();
        } else
            break;
    }
    return value;
}
} // namespace

SystemAppearance::SystemAppearance(QObject *parent) : QObject(parent) {
    auto bus = QDBusConnection::sessionBus();
    if (bus.isConnected()) {
        bus.connect(service, path, settings, QStringLiteral("SettingChanged"), this,
                    SLOT(settingChanged(QString, QString, QDBusVariant)));
        read(true);
    }
    if (auto *hints = QGuiApplication::styleHints())
        connect(hints, &QStyleHints::colorSchemeChanged, this, [this] {
            if (m_portal < 0)
                Q_EMIT changed();
        });
}

bool SystemAppearance::dark() const {
    // "No preference" (0) reads as light, as GNOME's own applications do.
    if (m_portal >= 0)
        return m_portal == 1;
    auto *hints = QGuiApplication::styleHints();
    return hints && hints->colorScheme() == Qt::ColorScheme::Dark;
}

void SystemAppearance::read(bool readOne) {
    auto message = QDBusMessage::createMethodCall(service, path, settings,
                                                  readOne ? QStringLiteral("ReadOne") : QStringLiteral("Read"));
    message << group << key;
    auto *watcher = new QDBusPendingCallWatcher(QDBusConnection::sessionBus().asyncCall(message), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this, [this, watcher, readOne] {
        watcher->deleteLater();
        const QDBusPendingReply<QDBusVariant> reply = *watcher;
        if (!reply.isError())
            apply(reply.value().variant());
        else if (readOne) // Portals older than 1.15 only have Read.
            read(false);
    });
}

void SystemAppearance::settingChanged(const QString &changedGroup, const QString &changedKey,
                                      const QDBusVariant &value) {
    if (changedGroup == group && changedKey == key)
        apply(value.variant());
}

void SystemAppearance::apply(const QVariant &value) {
    bool ok = false;
    const int scheme = unwrap(value).toInt(&ok);
    const bool before = dark();
    m_portal = ok ? scheme : -1;
    if (dark() != before)
        Q_EMIT changed();
}
