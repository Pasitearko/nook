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

// What the confirmation lists. shared-mime-info tags its translations "zh-Hans-CN", which
// neither Qt nor GIO match for zh_CN, so the types nook解压缩 claims are named here.
const QHash<QString, QString> &labels() {
    static const QHash<QString, QString> names{
        {"application/zip", "ZIP 压缩包"},
        {"application/x-7z-compressed", "7z 压缩包"},
        {"application/vnd.rar", "RAR 压缩包"},
        {"application/x-tar", "tar 包"},
        {"application/x-compressed-tar", "tar.gz 压缩包"},
        {"application/x-bzip2-compressed-tar", "tar.bz2 压缩包"},
        {"application/x-xz-compressed-tar", "tar.xz 压缩包"},
        {"application/x-zstd-compressed-tar", "tar.zst 压缩包"},
        {"application/x-lz4-compressed-tar", "tar.lz4 压缩包"},
        {"application/x-lzma-compressed-tar", "tar.lzma 压缩包"},
        {"application/x-lzip-compressed-tar", "tar.lz 压缩包"},
        {"application/x-tarz", "tar.Z 压缩包"},
        {"application/gzip", "gzip 压缩文件"},
        {"application/x-bzip2", "bzip2 压缩文件"},
        {"application/x-xz", "xz 压缩文件"},
        {"application/zstd", "zstd 压缩文件"},
        {"application/x-lzma", "LZMA 压缩文件"},
        {"application/x-lz4", "LZ4 压缩文件"},
        {"application/x-lzip", "lzip 压缩文件"},
        {"application/x-compress", "compress 压缩文件（.Z）"},
        {"application/x-gzpostscript", "gzip 压缩的 PostScript 文档"},
        {"application/x-cpio", "cpio 包"},
        {"application/x-sv4cpio", "SVR4 cpio 包"},
        {"application/x-sv4crc", "SVR4 cpio 包（带校验）"},
        {"application/vnd.efi.iso", "ISO 光盘映像"},
        {"application/vnd.ms-cab-compressed", "CAB 压缩包"},
        {"application/vnd.debian.binary-package", "Debian 软件包（deb）"},
        {"application/x-archive", "ar 包（含 .a 静态库）"},
        {"application/x-rpm", "RPM 软件包"},
        {"application/x-source-rpm", "RPM 源码包"},
        {"application/x-xar", "XAR 包"},
        {"application/x-lha", "LHA 压缩包"},
        {"application/x-lhz", "LHZ 压缩包"},
        {"application/x-arj", "ARJ 压缩包"},
        {"application/java-archive", "Java 包（jar）"},
        {"application/vnd.android.package-archive", "Android 应用包（apk）"},
        {"application/x-ms-wim", "Windows 映像（WIM）"},
        {"application/x-apple-diskimage", "macOS 磁盘映像（dmg）"},
        {"application/vnd.squashfs", "SquashFS 映像"},
    };
    return names;
}

// GKeyFile preserves unrelated groups/comments. In particular, never use GIO's
// reset_type_associations(): it also deletes other applications' open-with choices.
const char *associationGroups[] = {"Default Applications", "Added Associations", "Removed Associations"};

QStringList ids(GKeyFile *file, const char *group, const QByteArray &type) {
    gsize count = 0;
    gchar **values = g_key_file_get_string_list(file, group, type.constData(), &count, nullptr);
    QStringList result;
    for (gsize i = 0; values && i < count; ++i)
        result << QString::fromUtf8(values[i]);
    g_strfreev(values);
    return result;
}

QJsonObject membership(const QString &path, const QString &type, const QString &id) {
    GKeyFile *file = g_key_file_new();
    g_key_file_load_from_file(file, QFile::encodeName(path).constData(), G_KEY_FILE_KEEP_COMMENTS, nullptr);
    QJsonObject result;
    for (const char *group : associationGroups)
        result.insert(QString::fromLatin1(group), ids(file, group, type.toUtf8()).contains(id));
    g_key_file_unref(file);
    return result;
}

QString removeOurAssociation(const QString &path, const QString &type, const QString &id,
                             const QJsonObject &baseline) {
    GKeyFile *file = g_key_file_new();
    GError *error = nullptr;
    if (!g_key_file_load_from_file(file, QFile::encodeName(path).constData(), G_KEY_FILE_KEEP_COMMENTS,
                                   &error)) {
        g_key_file_unref(file);
        return errorText(error);
    }
    const QByteArray key = type.toUtf8();
    for (const char *group : associationGroups) {
        QStringList values = ids(file, group, key);
        const bool hadOurs = baseline.value(QString::fromLatin1(group)).toBool();
        if (hadOurs) {
            if (!values.contains(id))
                values << id;
        } else {
            values.removeAll(id);
        }
        if (values.isEmpty()) {
            g_key_file_remove_key(file, group, key.constData(), nullptr);
        } else {
            QList<QByteArray> encoded;
            for (const auto &value : values)
                encoded << value.toUtf8();
            QVector<const char *> pointers;
            for (const auto &value : encoded)
                pointers << value.constData();
            g_key_file_set_string_list(file, group, key.constData(), pointers.constData(), pointers.size());
        }
    }
    gsize length = 0;
    gchar *data = g_key_file_to_data(file, &length, nullptr);
    QSaveFile save(path);
    const bool ok = save.open(QIODevice::WriteOnly) && save.write(data, qint64(length)) == qint64(length) &&
                    save.commit();
    g_free(data);
    g_key_file_unref(file);
    return ok ? QString() : QStringLiteral("无法保存 ") + path + "：" + save.errorString();
}

} // namespace

DefaultApps::DefaultApps(const QString &desktopId, QObject *parent) : QObject(parent), m_id(desktopId) {
    refresh();
}

QString DefaultApps::mimeappsPath() const {
    return QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation) + "/mimeapps.list";
}

QString DefaultApps::recordPath() const {
    return QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation) +
           "/nook/archive-defaults.json";
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
    m_associations.clear();
    QHash<QString, int> others;
    QHash<QString, QString> names;
    const QMimeDatabase mime;
    for (const auto &type : std::as_const(m_types)) {
        App app(g_app_info_get_default_for_type(type.toUtf8().constData(), FALSE));
        const QString id = app ? QString::fromUtf8(g_app_info_get_id(app.get())) : QString();
        // What people recognise: the type's description and its file names (".zip").
        const QMimeType known = mime.mimeTypeForName(type);
        QStringList suffixes;
        for (const auto &suffix : known.suffixes())
            suffixes << "." + suffix;
        m_associations << QVariantMap{
            {"type", type},
            {"label", labels().value(type, known.isValid() && !known.comment().isEmpty() ? known.comment() : type)},
            {"suffixes", suffixes.mid(0, 3).join(u' ')},
            {"name", app ? QString::fromUtf8(g_app_info_get_name(app.get())) : QStringLiteral("未指定")},
            {"desktopId", id},
            {"ours", id == m_id}};
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
    m_state = !self || m_types.isEmpty()  ? "unavailable"
              : m_owned == 0              ? "none"
              : m_owned == m_types.size() ? "all"
                                          : "some";
    Q_EMIT changed();
}

bool DefaultApps::canRestore() const {
    return QFileInfo::exists(recordPath());
}

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
    QJsonObject record;
    QFile old(recordPath());
    if (old.exists()) {
        if (!old.open(QIODevice::ReadOnly))
            return "无法读取之前的恢复记录，没有修改";
        QJsonParseError parse;
        record = QJsonDocument::fromJson(old.readAll(), &parse).object();
        if (parse.error != QJsonParseError::NoError || !record.value("previous").isObject())
            return "之前的恢复记录已损坏，没有修改";
    }
    old.close();
    // Keep the first backup for this takeover, including repeated clicks in the same second.
    QString backup = record.value("backup").toString();
    const QString list = mimeappsPath();
    if (backup.isEmpty() && QFileInfo::exists(list)) {
        const QString stamp = list + ".bak-nook-archive-" + QDateTime::currentDateTime().toString("yyyyMMdd-HHmmss");
        backup = stamp;
        for (int n = 2; QFileInfo::exists(backup) || QFileInfo(backup).isSymLink(); ++n)
            backup = stamp + "-" + QString::number(n);
        if (!QFile::copy(list, backup))
            return "无法备份 " + list + "，没有修改";
    }
    QJsonObject previous = record.value("previous").toObject();
    QJsonObject baselines = record.value("membership").toObject();
    for (const auto &type : std::as_const(m_types)) {
        const QString id = defaultFor(type);
        // An existing nook default may have been the user's deliberate choice: leave it alone.
        if (id == m_id)
            continue;
        previous.insert(type, id);
        baselines.insert(type, membership(list, type, m_id));
    }
    record.insert("previous", previous);
    record.insert("membership", baselines);
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
        return QString::number(typeCount() - ownedCount()) +
               " 种类型仍由其他程序打开，可能被桌面专用的关联文件（如 gnome-mimeapps.list）覆盖";
    return {};
}

QString DefaultApps::restore() {
    QFile file(recordPath());
    if (!file.open(QIODevice::ReadOnly))
        return "没有可以恢复的记录";
    QJsonParseError parse;
    const QJsonObject record = QJsonDocument::fromJson(file.readAll(), &parse).object();
    if (parse.error != QJsonParseError::NoError || !record.value("previous").isObject())
        return "恢复记录已损坏，未修改关联";
    const QJsonObject previous = record.value("previous").toObject();
    const QJsonObject baselines = record.value("membership").toObject();
    file.close();
    QStringList failed;
    for (auto it = previous.begin(); it != previous.end(); ++it) {
        const QByteArray type = it.key().toUtf8();
        // Only what is still ours goes back; a choice made since then stays.
        if (defaultFor(it.key()) != m_id)
            continue;
        const QString oldId = it.value().toString();
        const App app = desktopApp(oldId);
        GError *error = nullptr;
        if (app) {
            if (!g_app_info_set_as_default_for_type(app.get(), type.constData(), &error))
                failed << it.key() + "：" + errorText(error);
            else if (defaultFor(it.key()) != oldId)
                failed << it.key() + "：默认程序被其他配置覆盖";
        } else if (!oldId.isEmpty()) {
            failed << it.key() + "：之前的程序 " + oldId + " 已不可用，请重新安装后重试";
        } else {
            const QString failure =
                removeOurAssociation(mimeappsPath(), it.key(), m_id, baselines.value(it.key()).toObject());
            if (!failure.isEmpty())
                failed << it.key() + "：" + failure;
        }
    }
    if (failed.isEmpty() && !QFile::remove(recordPath()))
        failed << "关联已恢复，但无法删除恢复记录";
    refresh();
    return failed.isEmpty() ? QString()
                            : "有 " + QString::number(failed.size()) + " 种类型没有恢复：" + failed.first();
}
