#include "diskmounts.h"
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QProcessEnvironment>
#include <QRegularExpression>
#include <QSet>
#include <QTimer>
#include <functional>

namespace {
constexpr qsizetype MaxOutput = 2 * 1024 * 1024;
constexpr qsizetype MaxFstab = 1024 * 1024;
const QString BasicDataGuid = QStringLiteral("ebd0a0a2-b9e5-4433-87c0-68b6b72699c7");
const QRegularExpression
    UuidPattern(QStringLiteral("\\A(?:[0-9A-Fa-f]{16}|[0-9A-Fa-f]{4}-[0-9A-Fa-f]{4})\\z"));

// A desktop user may not be able to kill pkexec after it has switched to root.
// Never destroy a still-running QProcess (its destructor waits). Let it reap and
// delete itself when it exits, draining output meanwhile. No rollback is implied.
void retireProcess(QProcess *process) {
    for (auto *timer : process->findChildren<QTimer *>())
        timer->stop();
    process->disconnect();
    process->setParent(nullptr);
    if (process->state() == QProcess::NotRunning) {
        process->deleteLater();
        return;
    }
    process->closeWriteChannel();
    QObject::connect(process, &QProcess::readyReadStandardOutput, process,
                     [process] { process->readAllStandardOutput(); });
    QObject::connect(process, &QProcess::readyReadStandardError, process,
                     [process] { process->readAllStandardError(); });
    QObject::connect(process, &QProcess::finished, process, &QObject::deleteLater);
    QObject::connect(process, &QProcess::errorOccurred, process, [process](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart)
            process->deleteLater();
    });
}

bool systemMount(const QString &path) {
    if (path == "/" || path == "/run" || (path.startsWith("/run/") && !path.startsWith("/run/media/")))
        return true;
    for (const auto &prefix : {"/boot", "/efi", "/usr", "/etc", "/var", "/home", "/opt", "/srv", "/bin",
                               "/sbin", "/lib", "/lib64", "/tmp", "/dev", "/proc", "/sys"}) {
        if (path == QLatin1String(prefix) || path.startsWith(QString::fromLatin1(prefix) + '/'))
            return true;
    }
    return false;
}

bool fixedFlag(const QJsonValue &value) {
    return (value.isBool() && !value.toBool()) || (value.isDouble() && value.toDouble() == 0) ||
           (value.isString() && value.toString() == "0");
}

QString displaySize(const QJsonValue &value) {
    double size = value.isString() ? value.toString().toDouble() : value.toDouble();
    const QStringList units = {"B", "KB", "MB", "GB", "TB", "PB"};
    int unit = 0;
    while (size >= 1024.0 && unit < units.size() - 1) {
        size /= 1024.0;
        ++unit;
    }
    return QString::number(size, 'f', unit == 0 ? 0 : 1) + ' ' + units[unit];
}

// Recognize precisely the helper's ownership marker and safe v1 line format.
// Never infer ownership from a UUID, target directory or an untagged fstab row.
bool readOwned(QHash<QString, bool> &owned, QString &error) {
    QFile fstab(QStringLiteral("/etc/fstab"));
    if (!fstab.open(QIODevice::ReadOnly)) {
        error = QObject::tr("Cannot read /etc/fstab: %1").arg(fstab.errorString());
        return false;
    }
    const auto bytes = fstab.read(MaxFstab + 1);
    if (bytes.size() > MaxFstab || fstab.error() != QFileDevice::NoError) {
        error = QObject::tr("Cannot read boot mount settings: fstab is too large or unreadable.");
        return false;
    }
    const QRegularExpression whitespace(QStringLiteral("\\s+"));
    const QRegularExpression opts(
        QStringLiteral("^(ro|rw),uid=([1-9][0-9]*),gid=(0|[1-9][0-9]*),umask=0077,nosuid,nodev,nofail,"
                       "x-systemd\\.device-timeout=5s,x-systemd\\.mount-timeout=15s$"));
    for (const auto &line : QString::fromUtf8(bytes).split('\n')) {
        if (!line.contains("nook:"))
            continue;
        const auto fields = line.split(whitespace, Qt::SkipEmptyParts);
        bool valid = fields.size() == 9;
        QString uuid;
        QRegularExpressionMatch match;
        if (valid) {
            uuid = fields[0].mid(5);
            match = opts.match(fields[3]);
            bool uidOk = false, gidOk = false;
            const auto uid = match.captured(2).toULongLong(&uidOk);
            const auto gid = match.captured(3).toULongLong(&gidOk);
            valid = fields[0] == "UUID=" + uuid && UuidPattern.match(uuid).hasMatch() &&
                    uuid == uuid.toUpper() && fields[1] == "/mnt/nook-" + uuid &&
                    (fields[2] == "ntfs3" || fields[2] == "exfat") && fields[4] == "0" && fields[5] == "0" &&
                    fields[6] == "#" && fields[7] == "nook:v1" && fields[8] == "UUID=" + uuid &&
                    match.hasMatch() && uidOk && gidOk && uid > 0 && uid < 4294967295ULL &&
                    gid < 4294967295ULL && !owned.contains(uuid);
        }
        if (!valid) {
            error =
                QObject::tr("Malformed or duplicate Nook-owned fstab entry; administrator review required.");
            return false;
        }
        owned.insert(uuid, match.captured(1) == "ro");
    }
    return true;
}
} // namespace

DiskMounts::DiskMounts(QObject *parent) : QObject(parent) {
    QTimer::singleShot(0, this, &DiskMounts::refresh);
}

DiskMounts::~DiskMounts() {
    if (m_process) {
        m_process->disconnect(this);
        m_process->kill();
        retireProcess(m_process);
    }
}

void DiskMounts::refresh() {
    if (m_busy)
        return;
    m_busy = true;
    m_status = tr("Reading fixed data partitions…");
    Q_EMIT changed();
    startRefresh(false);
}

void DiskMounts::startRefresh(bool afterApply) {
    m_refreshForApply = afterApply;
    startProcess(QStringLiteral("/usr/bin/lsblk"),
                 {"--json", "--tree", "--bytes", "--paths", "--output",
                  "PATH,UUID,FSTYPE,LABEL,SIZE,PARTTYPE,MOUNTPOINTS,RM,TYPE"},
                 {}, false);
}

void DiskMounts::apply(const QVariantList &choices) {
    if (m_busy)
        return;
    QJsonArray entries;
    QSet<QString> seen;
    QString error;
    if (choices.size() > 32)
        error = tr("Select at most 32 partitions per operation.");
    for (const auto &choice : choices) {
        if (!error.isEmpty())
            break;
        const auto map = choice.toMap();
        const auto uuid = map.value("uuid").toString().toUpper();
        if (map.size() != 3 || !map.contains("uuid") || !map.contains("enabled") ||
            !map.contains("readOnly") || map.value("uuid").metaType().id() != QMetaType::QString ||
            !UuidPattern.match(uuid).hasMatch() || seen.contains(uuid) ||
            map.value("enabled").metaType().id() != QMetaType::Bool ||
            map.value("readOnly").metaType().id() != QMetaType::Bool) {
            error = tr("Each choice requires a unique UUID and boolean enabled/readOnly values only.");
            break;
        }
        seen.insert(uuid);
        entries.append(QJsonObject{{"uuid", uuid},
                                   {"enabled", map.value("enabled").toBool()},
                                   {"readOnly", map.value("readOnly").toBool()}});
    }
    if (!error.isEmpty() || entries.isEmpty()) {
        m_status = error.isEmpty() ? tr("No changes requested.") : error;
        Q_EMIT changed();
        return;
    }
    const auto request =
        QJsonDocument(QJsonObject{{"version", 1}, {"entries", entries}}).toJson(QJsonDocument::Compact);
    m_busy = true;
    m_status = tr("Waiting for administrator authorization and applying boot mount settings…");
    Q_EMIT changed();
    startProcess(QStringLiteral("/usr/bin/pkexec"),
                 {QStringLiteral("/usr/lib/nook/nook-mount-helper")}, request, true);
}

void DiskMounts::startProcess(const QString &program, const QStringList &arguments, const QByteArray &input,
                              bool applying) {
    m_stdout.clear();
    m_stderr.clear();
    m_failure.clear();
    m_applying = applying;
    auto *process = new QProcess(this);
    m_process = process;
    auto env = QProcessEnvironment::systemEnvironment();
    env.insert("LC_ALL", "C.UTF-8");
    process->setProcessEnvironment(env);
    auto *timer = new QTimer(process);
    timer->setSingleShot(true);
    connect(timer, &QTimer::timeout, this, [this, process, applying] {
        if (m_process != process)
            return;
        m_failure = applying ? tr("Authorization/helper timed out. An authorized operation may still finish; "
                                  "refresh before retrying.")
                             : tr("Disk discovery timed out.");
        process->kill();
        if (m_process == process)
            processFinished(-1, false);
    });
    connect(process, &QProcess::started, this, [this, process, input] {
        if (!input.isEmpty() && process->write(input) != input.size()) {
            m_failure = tr("Could not send the complete mount request to the helper.");
            process->kill();
        }
        process->closeWriteChannel();
    });
    connect(process, &QProcess::readyReadStandardOutput, this, &DiskMounts::collectOutput);
    connect(process, &QProcess::readyReadStandardError, this, &DiskMounts::collectOutput);
    connect(process, &QProcess::errorOccurred, this, [this, process](QProcess::ProcessError error) {
        if (m_process != process)
            return;
        if (error == QProcess::FailedToStart) {
            m_failure = tr("Cannot start %1: %2").arg(process->program(), process->errorString());
            processFinished(-1, false);
        }
    });
    connect(process, &QProcess::finished, this, [this, process](int code, QProcess::ExitStatus status) {
        if (m_process == process)
            processFinished(code, status == QProcess::NormalExit);
    });
    timer->start(applying ? 240000 : 15000);
    process->start(program, arguments, QIODevice::ReadWrite);
}

void DiskMounts::collectOutput() {
    if (!m_process)
        return;
    const auto out = m_process->readAllStandardOutput();
    const auto err = m_process->readAllStandardError();
    if (m_stdout.size() + out.size() > MaxOutput || m_stderr.size() + err.size() > MaxOutput) {
        m_failure = tr("Disk command output exceeded its safety limit.");
        auto *process = m_process;
        process->kill();
        QTimer::singleShot(0, this, [this, process] {
            if (m_process == process)
                processFinished(-1, false);
        });
        return;
    }
    m_stdout += out;
    m_stderr += err;
}

void DiskMounts::processFinished(int exitCode, bool normalExit) {
    collectOutput();
    auto *process = m_process;
    m_process = nullptr;
    retireProcess(process);
    QString error = m_failure;
    if (m_applying) {
        QJsonParseError parseError;
        const auto reply = QJsonDocument::fromJson(m_stdout, &parseError).object();
        if (error.isEmpty() && parseError.error == QJsonParseError::NoError &&
            reply.value("version").toInt() == 1 && reply.value("ok").isBool() &&
            reply.value("configSaved").isBool() && reply.value("message").isString()) {
            m_status = reply.value("message").toString();
            if (reply.value("backup").isString())
                m_status += '\n' + tr("Backup: %1").arg(reply.value("backup").toString());
            for (const auto &key : {"errors", "warnings"}) {
                for (const auto &item : reply.value(key).toArray())
                    m_status += '\n' + item.toString();
            }
            if ((!normalExit || exitCode != 0) && reply.value("ok").toBool())
                m_status += '\n' + tr("Helper did not exit successfully; refresh and review system state.");
        } else {
            if (error.isEmpty()) {
                error = tr("Authorization was cancelled/denied, or the helper did not return a valid result. "
                           "If already authorized, check system state before retrying.");
                const auto detail = QString::fromUtf8(m_stderr).trimmed().left(4096);
                if (!detail.isEmpty())
                    error += '\n' + detail;
            }
            m_status = error;
        }
        Q_EMIT changed();
        startRefresh(true);
        return;
    }
    if (error.isEmpty() && (!normalExit || exitCode != 0))
        error = tr("Disk discovery failed: %1").arg(QString::fromUtf8(m_stderr).trimmed().left(4096));
    QVariantList disks;
    if (error.isEmpty())
        disks = parseDisks(m_stdout, error);
    if (!error.isEmpty()) {
        if (m_refreshForApply)
            m_status += '\n' + tr("Refresh failed: %1").arg(error);
        else
            m_status = error;
    } else if (!m_refreshForApply) {
        m_status = tr("Found %1 supported fixed data partition(s).").arg(disks.size());
    }
    if (m_disks != disks) {
        m_disks = disks;
        Q_EMIT disksChanged();
    }
    m_busy = false;
    Q_EMIT changed();
    if (m_refreshForApply)
        Q_EMIT mountedChanged();
}

QVariantList DiskMounts::parseDisks(const QByteArray &data, QString &error) const {
    QJsonParseError parseError;
    const auto document = QJsonDocument::fromJson(data, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject() ||
        !document.object().value("blockdevices").isArray()) {
        error = tr("Invalid lsblk JSON response.");
        return {};
    }
    QHash<QString, bool> owned;
    if (!readOwned(owned, error))
        return {};
    QHash<QString, int> counts;
    QList<QJsonObject> candidates;
    std::function<void(const QJsonArray &, bool, bool)> walk;
    walk = [&](const QJsonArray &nodes, bool parentFixed, bool parentRemovable) {
        for (const auto &value : nodes) {
            if (!value.isObject()) {
                error = tr("Invalid lsblk device tree.");
                continue;
            }
            const auto node = value.toObject();
            const auto uuid = node.value("uuid").toString().toUpper();
            if (!uuid.isEmpty())
                ++counts[uuid];
            const bool removable = parentRemovable || !fixedFlag(node.value("rm"));
            const auto type = node.value("type").toString();
            const auto fs = node.value("fstype").toString().toLower();
            bool safeMounts = true;
            const auto points = node.value("mountpoints");
            if (!points.isNull() && !points.isArray())
                safeMounts = false;
            for (const auto &point : points.toArray()) {
                if ((!point.isNull() && !point.isString()) || systemMount(point.toString()))
                    safeMounts = false;
            }
            if (parentFixed && !removable && type == "part" && safeMounts &&
                ((fs == "ntfs" && uuid.size() == 16) || (fs == "exfat" && uuid.size() == 9)) &&
                UuidPattern.match(uuid).hasMatch() &&
                node.value("parttype").toString().toLower() == BasicDataGuid &&
                QRegularExpression(QStringLiteral("^/dev/[A-Za-z0-9_./-]+$"))
                    .match(node.value("path").toString())
                    .hasMatch())
                candidates.append(node);
            if (node.contains("children") && !node.value("children").isArray())
                error = tr("Invalid lsblk children.");
            walk(node.value("children").toArray(), type == "disk" && !removable, removable);
        }
    };
    walk(document.object().value("blockdevices").toArray(), false, false);
    if (!error.isEmpty())
        return {};
    QVariantList result;
    for (const auto &node : candidates) {
        const auto uuid = node.value("uuid").toString().toUpper();
        if (counts.value(uuid) != 1)
            continue;
        QStringList points;
        for (const auto &point : node.value("mountpoints").toArray()) {
            if (!point.toString().isEmpty())
                points.append(point.toString());
        }
        const auto deviceName = QFileInfo(node.value("path").toString()).fileName();
        const auto label = node.value("label").toString();
        result.append(QVariantMap{{"name", label.isEmpty() ? deviceName : label + " (" + deviceName + ")"},
                                  {"size", displaySize(node.value("size"))},
                                  {"uuid", uuid},
                                  {"filesystem", node.value("fstype").toString().toLower()},
                                  {"mountpoint", points.join(", ")},
                                  {"autoMount", owned.contains(uuid)},
                                  {"readOnly", owned.value(uuid, false)}});
    }
    return result;
}
