#include "defaultapps.h"
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMimeDatabase>
#include <QSaveFile>
#include <QStandardPaths>
#include <gio/gdesktopappinfo.h>
#include <gio/gio.h>

namespace {

struct Unref {
    void operator()(gpointer object) const {
        if (object)
            g_object_unref(object);
    }
};
using App = std::unique_ptr<GAppInfo, Unref>;

App desktopApp(const QString &id) {
    return App(id.isEmpty() ? nullptr : G_APP_INFO(g_desktop_app_info_new(id.toUtf8().constData())));
}

QString errorText(GError *error) {
    const QString text = error ? QString::fromUtf8(error->message) : QStringLiteral("未知错误");
    if (error)
        g_error_free(error);
    return text;
}

} // namespace

DefaultApps::DefaultApps(const QString &desktopId, QObject *parent) : QObject(parent), m_id(desktopId) { refresh(); }

QString DefaultApps::mimeappsPath() const {
    return QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation) + "/mimeapps.list";
}

QString DefaultApps::recordPath() const {
    return QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation) + "/nook/archive-defaults.json";
}

QString DefaultApps::defaultFor(const QString &type) const {
    App app(g_app_info_get_default_for_type(type.toUtf8().constData(), FALSE));
    return app ? QString::fromUtf8(g_app_info_get_id(app.get())) : QString();
}

void DefaultApps::refresh() {
    const App self = desktopApp(m_id);
    m_types.clear();
    if (self) {
        const QMimeDatabase mime;
        for (const char *const *type = g_app_info_get_supported_types(self.get()); type && *type; ++type) {
            const QMimeType known = mime.mimeTypeForName(QString::fromUtf8(*type));
            const QString name = known.isValid() ? known.name() : QString::fromUtf8(*type);
            if (!m_types.contains(name))
                m_types << name;
        }
    }
    m_owned = 0;
    QHash<QString, int> others;
    QHash<QString, QString> names;
    for (const auto &type : std::as_const(m_types)) {
        App app(g_app_info_get_default_for_type(type.toUtf8().constData(), FALSE));
        const QString id = app ? QString::fromUtf8(g_app_info_get_id(app.get())) : QString();
        if (id == m_id) {
            ++m_owned;
        } else if (app) {
            ++others[id];
            names.insert(id, QString::fromUtf8(g_app_info_get_name(app.get())));
        }
    }
    QString most;
    for (auto it = others.cbegin(); it != others.cend(); ++it)
        if (most.isEmpty() || it.value() > others.value(most))
            most = it.key();
    m_otherName = names.value(most);
    m_state = !self || m_types.isEmpty() ? "unavailable" : m_owned == 0 ? "none" : m_owned == m_types.size() ? "all" : "some";
    Q_EMIT changed();
}

bool DefaultApps::canRestore() const { return QFileInfo::exists(recordPath()); }

QString DefaultApps::lastBackup() const {
    QFile file(recordPath());
    if (!file.open(QIODevice::ReadOnly))
        return {};
    return QJsonDocument::fromJson(file.readAll()).object().value("backup").toString();
}

QString DefaultApps::setAsDefault() {
    refresh();
    const App self = desktopApp(m_id);
    if (!self)
        return "找不到 " + m_id + "：nook解压缩 安装后才能设为默认";
    // A copy of the user's associations as they are, next to them.
    QString backup;
    const QString list = mimeappsPath();
    if (QFileInfo::exists(list)) {
        backup = list + ".bak-nook-archive-" + QDateTime::currentDateTime().toString("yyyyMMdd-HHmmss");
        QFile::remove(backup);
        if (!QFile::copy(list, backup))
            return "无法备份 " + list + "，没有修改";
    }
    // What opened each type, unless an earlier record already holds what was there before us.
    QJsonObject record;
    QFile old(recordPath());
    if (old.open(QIODevice::ReadOnly))
        record = QJsonDocument::fromJson(old.readAll()).object();
    old.close();
    QJsonObject previous = record.value("previous").toObject();
    for (const auto &type : std::as_const(m_types)) {
        const QString id = defaultFor(type);
        if (id != m_id)
            previous.insert(type, id);
        else if (!previous.contains(type))
            previous.insert(type, QString()); // ours already, by no one's choice: restoring forgets it
    }
    record.insert("previous", previous);
    if (!backup.isEmpty())
        record.insert("backup", backup);
    record.insert("time", QDateTime::currentDateTime().toString(Qt::ISODate));
    QDir().mkpath(QFileInfo(recordPath()).absolutePath());
    QSaveFile save(recordPath());
    if (!save.open(QIODevice::WriteOnly) || save.write(QJsonDocument(record).toJson()) < 0 || !save.commit())
        return "无法写入 " + recordPath() + "，没有修改";

    QStringList failed;
    for (const auto &type : std::as_const(m_types)) {
        GError *error = nullptr;
        if (!g_app_info_set_as_default_for_type(self.get(), type.toUtf8().constData(), &error)) {
            failed << type + "：" + errorText(error);
        }
    }
    refresh();
    if (!failed.isEmpty())
        return "有 " + QString::number(failed.size()) + " 种类型没有设好：" + failed.first();
    if (m_state != "all")
        // A desktop-specific list (such as gnome-mimeapps.list) overrides mimeapps.list.
        return QString::number(typeCount() - ownedCount()) + " 种类型仍由其他程序打开，可能被桌面专用的关联文件（如 gnome-mimeapps.list）覆盖";
    return {};
}

QString DefaultApps::restore() {
    QFile file(recordPath());
    if (!file.open(QIODevice::ReadOnly))
        return "没有可以恢复的记录";
    const QJsonObject previous = QJsonDocument::fromJson(file.readAll()).object().value("previous").toObject();
    file.close();
    QStringList failed;
    for (auto it = previous.begin(); it != previous.end(); ++it) {
        const QByteArray type = it.key().toUtf8();
        // Only what is still ours goes back; a choice made since then stays.
        if (defaultFor(it.key()) != m_id)
            continue;
        const App app = desktopApp(it.value().toString());
        GError *error = nullptr;
        if (app) {
            if (!g_app_info_set_as_default_for_type(app.get(), type.constData(), &error))
                failed << it.key() + "：" + errorText(error);
        } else {
            // Nothing (or something since removed) opened it: forget the choice altogether.
            g_app_info_reset_type_associations(type.constData());
        }
    }
    if (failed.isEmpty())
        QFile::remove(recordPath());
    refresh();
    return failed.isEmpty() ? QString() : "有 " + QString::number(failed.size()) + " 种类型没有恢复：" + failed.first();
}
