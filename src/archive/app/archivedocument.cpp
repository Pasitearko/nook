#include "archivedocument.h"
#include "archivejob.h"
#include "archivenames.h"
#include <QFileInfo>
#include <QUrl>

ArchiveDocument::ArchiveDocument(QObject *parent) : QObject(parent) {}

ArchiveDocument::~ArchiveDocument() = default;

QString ArchiveDocument::name() const { return QFileInfo(m_path).fileName(); }

QString ArchiveDocument::folderPath() const { return m_path.isEmpty() ? QString() : QFileInfo(m_path).absolutePath(); }

double ArchiveDocument::archiveSize() const {
    double total = 0;
    for (const auto &part : nook::archive::volumesOf(m_path).parts)
        total += double(QFileInfo(part).size());
    return total;
}

QString ArchiveDocument::formatLabel() const {
    if (m_info.isEmpty())
        return {};
    const QString format = m_info.value("format").toString();
    const QStringList filters = m_info.value("filters").toStringList();
    static const QHash<QString, QString> short_{{"gzip", "gz"}, {"bzip2", "bz2"}, {"xz", "xz"}, {"zstd", "zst"},
                                                {"lzma", "lzma"}, {"lz4", "lz4"}, {"compress (.Z)", "Z"}, {"lzip", "lz"}};
    const QString filter = filters.isEmpty() ? QString() : short_.value(filters.first(), filters.first());
    QString label;
    if (m_info.value("stream").toBool())
        label = filter.isEmpty() ? QStringLiteral("压缩流") : filter + QStringLiteral(" 单文件");
    else if (format.contains(u"ZIP", Qt::CaseInsensitive))
        label = "ZIP";
    else if (format.contains(u"tar", Qt::CaseInsensitive))
        label = filter.isEmpty() ? QStringLiteral("tar") : "tar." + filter;
    else if (format.startsWith(u"Rar", Qt::CaseInsensitive))
        label = format.toUpper();
    else if (format.contains(u"ISO9660", Qt::CaseInsensitive))
        label = "ISO";
    else if (format.contains(u"cpio", Qt::CaseInsensitive))
        label = "cpio";
    else if (format.startsWith(u"ar", Qt::CaseInsensitive))
        label = "ar";
    else
        label = format.section(u' ', 0, 0);
    if (m_info.value("split").toBool())
        label += QStringLiteral(" · 分卷");
    if (m_info.value("solid").toBool())
        label += QStringLiteral(" · 固实");
    if (m_info.value("encrypted").toBool())
        label += QStringLiteral(" · 已加密");
    return label;
}

bool ArchiveDocument::readOnly() const {
    // What phase 5 can rewrite; everything else is browsed and extracted only.
    const QString label = formatLabel().section(u' ', 0, 0);
    return !(label == "ZIP" || label == "7z" || label.startsWith(u"tar")) || m_info.value("split").toBool();
}

void ArchiveDocument::setState(const QString &state) {
    if (m_state == state)
        return;
    m_state = state;
    Q_EMIT stateChanged();
}

void ArchiveDocument::open(const QString &location) {
    const QUrl url(location);
    const QString path = url.isLocalFile() ? url.toLocalFile() : location;
    m_path = QFileInfo(path).absoluteFilePath();
    m_password.clear();
    m_hasPassword = false;
    m_encoding = "auto";
    m_model.clear();
    Q_EMIT pathChanged();
    start();
}

void ArchiveDocument::reload() {
    if (!m_path.isEmpty())
        start();
}

void ArchiveDocument::close() {
    m_job.reset();
    m_path.clear();
    m_info.clear();
    m_entries.clear();
    m_password.clear();
    m_hasPassword = false;
    m_model.clear();
    m_passwordRequested = false;
    Q_EMIT passwordChanged();
    Q_EMIT pathChanged();
    Q_EMIT infoChanged();
    Q_EMIT loadedChanged();
    setState("empty");
}

void ArchiveDocument::setEncoding(const QString &encoding) {
    if (encoding == m_encoding)
        return;
    m_encoding = encoding;
    reload();
}

void ArchiveDocument::start() {
    m_job = std::make_unique<ArchiveJob>();
    connect(m_job.get(), &ArchiveJob::workerEvent, this, &ArchiveDocument::handle);
    m_entries.clear();
    m_error.clear();
    m_errorCode.clear();
    Q_EMIT loadedChanged();
    setState("loading");
    QVariantMap request{{"op", "list"}, {"archive", m_path}, {"encoding", m_encoding}};
    if (m_hasPassword)
        request.insert("password", m_password);
    m_job->start(request);
}

void ArchiveDocument::handle(const QVariantMap &event) {
    const QString type = event.value("event").toString();
    if (type == "entries") {
        m_entries += event.value("items").toList();
        Q_EMIT loadedChanged();
    } else if (type == "info") {
        m_info = event;
        Q_EMIT infoChanged();
    } else if (type == "need-password") {
        m_passwordWrong = event.value("wrong").toBool();
        m_passwordRequested = true;
        Q_EMIT passwordChanged();
    } else if (type == "done") {
        m_passwordRequested = false;
        Q_EMIT passwordChanged();
        if (event.value("ok").toBool()) {
            if (!m_pendingPassword.isEmpty()) {
                m_password = m_pendingPassword;
                m_hasPassword = true;
            }
            m_model.reset(m_entries, name());
            m_entries.clear();
            setState("ready");
            Q_EMIT opened(m_path);
        } else if (event.value("cancelled").toBool()) {
            m_error = QStringLiteral("没有输入密码，无法查看这个压缩包的内容");
            m_errorCode = "cancelled";
            setState("error");
        } else {
            const QVariantMap error = event.value("error").toMap();
            m_error = error.value("message").toString();
            m_errorCode = error.value("code").toString();
            setState("error");
        }
        m_pendingPassword.clear();
    }
}

void ArchiveDocument::submitPassword(const QString &password) {
    if (!m_job || !m_passwordRequested)
        return;
    m_pendingPassword = password;
    m_passwordRequested = false;
    Q_EMIT passwordChanged();
    m_job->sendPassword(password);
}

void ArchiveDocument::cancelPassword() {
    if (!m_job || !m_passwordRequested)
        return;
    m_passwordRequested = false;
    Q_EMIT passwordChanged();
    m_job->cancel();
}
