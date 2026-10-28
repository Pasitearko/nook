#include "filechooser.h"
#include <QRegularExpression>
#include "files.h"
#include <QDBusConnection>
#include <QDBusMetaType>
#include <QDir>
#include <QFileInfo>

namespace nook::portal {

QDBusArgument &operator<<(QDBusArgument &out, const FilterRule &rule) {
    out.beginStructure();
    out << rule.type << rule.pattern;
    out.endStructure();
    return out;
}
const QDBusArgument &operator>>(const QDBusArgument &in, FilterRule &rule) {
    in.beginStructure();
    in >> rule.type >> rule.pattern;
    in.endStructure();
    return in;
}
QDBusArgument &operator<<(QDBusArgument &out, const Filter &filter) {
    out.beginStructure();
    out << filter.name << filter.rules;
    out.endStructure();
    return out;
}
const QDBusArgument &operator>>(const QDBusArgument &in, Filter &filter) {
    in.beginStructure();
    in >> filter.name >> filter.rules;
    in.endStructure();
    return in;
}
QDBusArgument &operator<<(QDBusArgument &out, const ChoiceOption &option) {
    out.beginStructure();
    out << option.id << option.label;
    out.endStructure();
    return out;
}
const QDBusArgument &operator>>(const QDBusArgument &in, ChoiceOption &option) {
    in.beginStructure();
    in >> option.id >> option.label;
    in.endStructure();
    return in;
}
QDBusArgument &operator<<(QDBusArgument &out, const Choice &choice) {
    out.beginStructure();
    out << choice.id << choice.label << choice.options << choice.initial;
    out.endStructure();
    return out;
}
const QDBusArgument &operator>>(const QDBusArgument &in, Choice &choice) {
    in.beginStructure();
    in >> choice.id >> choice.label >> choice.options >> choice.initial;
    in.endStructure();
    return in;
}
QDBusArgument &operator<<(QDBusArgument &out, const ChoiceValue &value) {
    out.beginStructure();
    out << value.id << value.value;
    out.endStructure();
    return out;
}
const QDBusArgument &operator>>(const QDBusArgument &in, ChoiceValue &value) {
    in.beginStructure();
    in >> value.id >> value.value;
    in.endStructure();
    return in;
}

void registerTypes() {
    qDBusRegisterMetaType<FilterRule>();
    qDBusRegisterMetaType<QList<FilterRule>>();
    qDBusRegisterMetaType<Filter>();
    qDBusRegisterMetaType<QList<Filter>>();
    qDBusRegisterMetaType<ChoiceOption>();
    qDBusRegisterMetaType<QList<ChoiceOption>>();
    qDBusRegisterMetaType<Choice>();
    qDBusRegisterMetaType<QList<Choice>>();
    qDBusRegisterMetaType<ChoiceValue>();
    qDBusRegisterMetaType<QList<ChoiceValue>>();
    qDBusRegisterMetaType<QList<QByteArray>>();
}

namespace {
// Options arrive as variants holding either the plain value or its D-Bus encoding.
template <typename T> T optionOf(const QVariantMap &options, const char *key) {
    const QVariant value = options.value(QLatin1String(key));
    if (value.metaType() == QMetaType::fromType<QDBusArgument>())
        return qdbus_cast<T>(value.value<QDBusArgument>());
    return value.value<T>();
}
// "ay" paths are NUL-terminated.
QString pathOf(QByteArray bytes) {
    while (bytes.endsWith('\0'))
        bytes.chop(1);
    return QFile::decodeName(bytes);
}
// GTK-style mnemonics ("_Open") have no meaning here.
QString withoutMnemonic(QString label) {
    label.replace(QLatin1String("__"), QLatin1String("\x01"));
    label.remove('_');
    label.replace(QChar(1), QChar('_'));
    return label;
}
bool sameFilter(const Filter &a, const Filter &b) {
    if (a.name != b.name || a.rules.size() != b.rules.size())
        return false;
    for (int i = 0; i < a.rules.size(); ++i)
        if (a.rules[i].type != b.rules[i].type || a.rules[i].pattern != b.rules[i].pattern)
            return false;
    return true;
}
} // namespace

ChooserRequest::ChooserRequest(const QString &mode, const QString &appId, const QString &title,
                               const QVariantMap &options, const QString &fallbackFolder, QObject *parent)
    : QObject(parent), m_mode(mode), m_appId(appId), m_title(title) {
    m_acceptLabel = withoutMnemonic(options.value("accept_label").toString());
    m_multiple = mode == "open" && options.value("multiple").toBool();
    m_directory = mode == "open" && options.value("directory").toBool();
    m_filters = optionOf<QList<Filter>>(options, "filters");
    if (options.contains("current_filter")) {
        const auto current = optionOf<Filter>(options, "current_filter");
        for (int i = 0; i < m_filters.size() && m_currentFilter < 0; ++i)
            if (sameFilter(m_filters[i], current))
                m_currentFilter = i;
        // A current filter with no list applies unconditionally.
        if (m_filters.isEmpty() && !current.rules.isEmpty()) {
            m_filters.append(current);
            m_currentFilter = 0;
        }
    }
    if (m_currentFilter < 0 && !m_filters.isEmpty())
        m_currentFilter = 0;
    m_choices = optionOf<QList<Choice>>(options, "choices");
    m_currentName = options.value("current_name").toString();
    QString folder = pathOf(optionOf<QByteArray>(options, "current_folder"));
    // SaveFile may name an existing file instead: its folder, and its name to keep.
    const QString file = pathOf(optionOf<QByteArray>(options, "current_file"));
    if (!file.isEmpty()) {
        const QFileInfo info(file);
        if (folder.isEmpty())
            folder = info.absolutePath();
        if (m_currentName.isEmpty())
            m_currentName = info.fileName();
    }
    for (const auto &name : optionOf<QList<QByteArray>>(options, "files"))
        m_files.append(QFileInfo(pathOf(name)).fileName());
    if (folder.isEmpty() || !QFileInfo(folder).isDir())
        m_currentFolder = fallbackFolder.isEmpty() ? nook::normalizeUri(QDir::homePath()) : fallbackFolder;
    else
        m_currentFolder = nook::normalizeUri(folder);
}

QVariantList ChooserRequest::filters() const {
    static const QRegularExpression plainExtension(QStringLiteral("^\\*\\.([^*?\\[\\]]+)$"));
    QVariantList result;
    for (const auto &filter : m_filters) {
        QVariantList rules;
        QStringList patterns, globs;
        QString extension;
        for (const auto &rule : filter.rules) {
            rules.append(QVariantMap{{"type", rule.type}, {"pattern", rule.pattern}});
            patterns.append(rule.pattern);
            if (rule.type != 0)
                continue;
            globs.append(rule.pattern);
            const auto match = plainExtension.match(rule.pattern);
            if (extension.isEmpty() && match.hasMatch())
                extension = match.captured(1);
        }
        const QString name = filter.name.isEmpty() ? patterns.join(", ") : filter.name;
        // As Windows shows them: "文本文件 (*.txt;*.md)", unless the name already lists them.
        QString label = name;
        if (!globs.isEmpty() && globs != QStringList{QStringLiteral("*")} && !name.contains(QLatin1String("*.")))
            label += QStringLiteral(" (%1)").arg(globs.join(';'));
        result.append(QVariantMap{{"name", name}, {"label", label}, {"extension", extension}, {"rules", rules}});
    }
    return result;
}

QVariantList ChooserRequest::choices() const {
    QVariantList result;
    for (const auto &choice : m_choices) {
        QVariantList options;
        for (const auto &option : choice.options)
            options.append(QVariantMap{{"id", option.id}, {"label", withoutMnemonic(option.label)}});
        result.append(QVariantMap{{"id", choice.id},
                                  {"label", withoutMnemonic(choice.label)},
                                  {"options", options},
                                  {"initial", choice.initial}});
    }
    return result;
}

void ChooserRequest::accept(const QStringList &uris, int filter, const QVariantMap &choices) {
    if (uris.isEmpty())
        return;
    QVariantMap results{{"uris", uris}};
    if (filter >= 0 && filter < m_filters.size())
        results.insert("current_filter", QVariant::fromValue(m_filters[filter]));
    if (!m_choices.isEmpty()) {
        QList<ChoiceValue> values;
        for (const auto &choice : m_choices)
            values.append({choice.id, choices.value(choice.id, choice.initial).toString()});
        results.insert("choices", QVariant::fromValue(values));
    }
    finish(0, results);
}

void ChooserRequest::cancel() {
    finish(1, {});
}

void ChooserRequest::abort() {
    finish(2, {});
}

void ChooserRequest::finish(uint response, const QVariantMap &results) {
    if (m_done)
        return;
    m_done = true;
    Q_EMIT finished(response, results);
}

QString ChooserRequest::resolve(const QString &folderUri, const QString &text) const {
    QString name = text.trimmed();
    if (name.startsWith(QLatin1String("~/")))
        name = QDir::homePath() + name.mid(1);
    if (name.startsWith('/'))
        return nook::normalizeUri(QDir::cleanPath(name));
    const QString folder = nook::localPath(folderUri);
    return nook::normalizeUri(QDir::cleanPath(folder + '/' + name));
}

bool ChooserRequest::exists(const QString &uri) const {
    const QString path = nook::localPath(uri);
    return !path.isEmpty() && QFileInfo(path).exists();
}

bool ChooserRequest::isFolder(const QString &uri) const {
    const QString path = nook::localPath(uri);
    return !path.isEmpty() && QFileInfo(path).isDir();
}

QStringList ChooserRequest::targetsIn(const QString &folderUri) const {
    const QDir folder(nook::localPath(folderUri));
    QStringList result, taken;
    for (const auto &name : m_files) {
        QString candidate = name;
        const QFileInfo info(name);
        const QString stem = info.completeBaseName(), suffix = info.suffix().isEmpty() ? QString() : '.' + info.suffix();
        for (int n = 1; folder.exists(candidate) || taken.contains(candidate); ++n)
            candidate = QStringLiteral("%1 (%2)%3").arg(stem).arg(n).arg(suffix);
        taken.append(candidate);
        result.append(nook::normalizeUri(folder.filePath(candidate)));
    }
    return result;
}

FileChooserPortal::FileChooserPortal(Show show, std::function<QString()> lastFolder,
                                     std::function<void(const QString &)> remember, QObject *parent)
    : QObject(parent), m_show(std::move(show)), m_lastFolder(std::move(lastFolder)), m_remember(std::move(remember)) {
    registerTypes();
    new FileChooserAdaptor(this);
}

bool FileChooserPortal::publish() {
    auto bus = QDBusConnection::sessionBus();
    return bus.isConnected() && bus.registerObject(QStringLiteral("/org/freedesktop/portal/desktop"), this) &&
           bus.registerService(QStringLiteral("org.freedesktop.impl.portal.desktop.nook"));
}

void FileChooserPortal::begin(const QString &mode, const QDBusObjectPath &handle, const QString &appId,
                              const QString &title, const QVariantMap &options, const QDBusMessage &message) {
    message.setDelayedReply(true);
    auto *request = new ChooserRequest(mode, appId, title, options, m_lastFolder ? m_lastFolder() : QString(), this);
    auto *handleObject = new QObject(request);
    new RequestAdaptor(handleObject, request);
    auto bus = QDBusConnection::sessionBus();
    const QString path = handle.path();
    const bool exported = bus.registerObject(path, handleObject);
    connect(request, &ChooserRequest::finished, this,
            [this, request, message, path, exported](uint response, const QVariantMap &results) {
                auto bus = QDBusConnection::sessionBus();
                if (exported)
                    bus.unregisterObject(path);
                if (response == 0 && m_remember) {
                    const QString first = results.value("uris").toStringList().value(0);
                    const bool folderResult = request->property("directory").toBool();
                    m_remember(folderResult ? first : nook::parentUri(first));
                }
                bus.send(message.createReply(QVariantList{QVariant::fromValue(response), QVariant::fromValue(results)}));
            });
    m_show(request);
}

FileChooserAdaptor::FileChooserAdaptor(FileChooserPortal *portal) : QDBusAbstractAdaptor(portal), m_portal(portal) {}

uint FileChooserAdaptor::OpenFile(const QDBusObjectPath &handle, const QString &app_id, const QString &,
                                  const QString &title, const QVariantMap &options, const QDBusMessage &message,
                                  QVariantMap &) {
    m_portal->begin(QStringLiteral("open"), handle, app_id, title, options, message);
    return 0;
}

uint FileChooserAdaptor::SaveFile(const QDBusObjectPath &handle, const QString &app_id, const QString &,
                                  const QString &title, const QVariantMap &options, const QDBusMessage &message,
                                  QVariantMap &) {
    m_portal->begin(QStringLiteral("save"), handle, app_id, title, options, message);
    return 0;
}

uint FileChooserAdaptor::SaveFiles(const QDBusObjectPath &handle, const QString &app_id, const QString &,
                                   const QString &title, const QVariantMap &options, const QDBusMessage &message,
                                   QVariantMap &) {
    m_portal->begin(QStringLiteral("saveFiles"), handle, app_id, title, options, message);
    return 0;
}

RequestAdaptor::RequestAdaptor(QObject *host, ChooserRequest *request)
    : QDBusAbstractAdaptor(host), m_request(request) {}

void RequestAdaptor::Close() {
    m_request->abort();
}

} // namespace nook::portal
