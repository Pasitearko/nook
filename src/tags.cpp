#include "tags.h"
#include "files.h"
#include "storage.h"
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>

namespace {
const QHash<QString, QString> DefaultNames{{"red", "红色"},   {"orange", "橙色"}, {"yellow", "黄色"},
                                           {"green", "绿色"}, {"blue", "蓝色"},   {"purple", "紫色"},
                                           {"gray", "灰色"}};
bool within(const QString &uri, const QString &folder) {
    return uri == folder || uri.startsWith(folder.endsWith('/') ? folder : folder + '/');
}
bool exists(const QString &uri) {
    const QString path = nook::localPath(uri);
    return !path.isEmpty() && QFileInfo(path).exists();
}
} // namespace

TagStore *TagStore::instance() {
    static TagStore *store = new TagStore(QCoreApplication::instance());
    return store;
}
const QStringList &TagStore::colors() {
    static const QStringList palette{"red", "orange", "yellow", "green", "blue", "purple", "gray"};
    return palette;
}
TagStore::TagStore(QObject *parent) : QObject(parent) {
    m_saveTimer.setSingleShot(true);
    m_saveTimer.setInterval(300);
    connect(&m_saveTimer, &QTimer::timeout, this, &TagStore::flush);
    reload();
}
TagStore::~TagStore() {
    if (m_saveTimer.isActive())
        flush();
}
void TagStore::reload() {
    m_saveTimer.stop();
    m_file = nook::storageDirectory(nook::Storage::Data) + "/tags.json";
    m_items.clear();
    m_names.clear();
    m_order = colors();
    QFile file(m_file);
    if (file.open(QIODevice::ReadOnly)) {
        const auto root = QJsonDocument::fromJson(file.readAll()).object();
        const auto names = root["names"].toObject();
        for (auto it = names.begin(); it != names.end(); ++it)
            if (colors().contains(it.key()) && !it.value().toString().trimmed().isEmpty())
                m_names.insert(it.key(), it.value().toString().trimmed());
        // The saved order, then any colour it lacks.
        QStringList order;
        for (const auto &value : root["order"].toArray())
            if (colors().contains(value.toString()) && !order.contains(value.toString()))
                order << value.toString();
        for (const auto &color : colors())
            if (!order.contains(color))
                order << color;
        m_order = order;
        const auto items = root["items"].toObject();
        for (auto it = items.begin(); it != items.end(); ++it) {
            QStringList tags;
            for (const auto &value : it.value().toArray())
                if (colors().contains(value.toString()) && !tags.contains(value.toString()))
                    tags << value.toString();
            if (!tags.isEmpty())
                m_items.insert(it.key(), tags);
        }
    }
    m_dirty = true;
    Q_EMIT changed();
}
void TagStore::flush() {
    m_saveTimer.stop();
    QJsonObject names, items;
    for (auto it = m_names.begin(); it != m_names.end(); ++it)
        names.insert(it.key(), it.value());
    for (auto it = m_items.begin(); it != m_items.end(); ++it)
        items.insert(it.key(), QJsonArray::fromStringList(it.value()));
    QDir().mkpath(QFileInfo(m_file).absolutePath());
    QSaveFile file(m_file);
    if (file.open(QIODevice::WriteOnly)) {
        file.write(QJsonDocument(QJsonObject{{"names", names}, {"order", QJsonArray::fromStringList(m_order)}, {"items", items}})
                       .toJson(QJsonDocument::Compact));
        file.commit();
    }
}
void TagStore::touch() {
    m_dirty = true;
    m_saveTimer.start();
    Q_EMIT changed();
}
QVariantList TagStore::tags() const {
    if (m_dirty) {
        QHash<QString, int> counts;
        for (auto it = m_items.begin(); it != m_items.end(); ++it)
            if (exists(it.key()))
                for (const auto &color : it.value())
                    ++counts[color];
        m_cache.clear();
        for (const auto &color : m_order)
            m_cache << QVariantMap{{"id", color}, {"name", nameOf(color)}, {"count", counts.value(color)}};
        m_dirty = false;
    }
    return m_cache;
}
QStringList TagStore::tagsOf(const QString &uri) const {
    QStringList tags = m_items.value(uri);
    if (tags.size() > 1)
        std::sort(tags.begin(), tags.end(), [this](const QString &a, const QString &b) {
            return m_order.indexOf(a) < m_order.indexOf(b);
        });
    return tags;
}
QString TagStore::nameOf(const QString &color) const {
    return m_names.value(color, DefaultNames.value(color, color));
}
int TagStore::state(const QStringList &uris, const QString &color) const {
    int with = 0;
    for (const auto &uri : uris)
        if (m_items.value(uri).contains(color))
            ++with;
    return with == 0 ? 0 : with == uris.size() ? 2 : 1;
}
void TagStore::toggle(const QStringList &uris, const QString &color) {
    set(uris, color, state(uris, color) != 2);
}
void TagStore::set(const QStringList &uris, const QString &color, bool on) {
    if (!colors().contains(color))
        return;
    bool any = false;
    for (const auto &uri : uris) {
        if (uri.isEmpty() || !uri.startsWith("file:"))
            continue;
        auto tags = m_items.value(uri);
        if (tags.contains(color) == on)
            continue;
        if (on) {
            tags << color;
            // Kept in the sidebar's order, so dots and chips line up the same everywhere.
            std::sort(tags.begin(), tags.end(), [this](const QString &a, const QString &b) {
                return m_order.indexOf(a) < m_order.indexOf(b);
            });
        } else
            tags.removeAll(color);
        if (tags.isEmpty())
            m_items.remove(uri);
        else
            m_items.insert(uri, tags);
        any = true;
    }
    if (any)
        touch();
}
void TagStore::clear(const QStringList &uris) {
    bool any = false;
    for (const auto &uri : uris)
        any |= m_items.remove(uri) > 0;
    if (any)
        touch();
}
void TagStore::rename(const QString &color, const QString &name) {
    if (!colors().contains(color))
        return;
    const QString trimmed = name.trimmed();
    if (trimmed.isEmpty() || trimmed == DefaultNames.value(color))
        m_names.remove(color);
    else
        m_names.insert(color, trimmed.left(40));
    touch();
}
void TagStore::clearColor(const QString &color) {
    set(urisWith(color), color, false);
}
void TagStore::move(const QString &color, int to) {
    const int from = m_order.indexOf(color);
    to = qBound(0, to, int(m_order.size()) - 1);
    if (from < 0 || from == to)
        return;
    m_order.move(from, to);
    touch();
}
QStringList TagStore::urisWith(const QString &color) const {
    QStringList result;
    for (auto it = m_items.begin(); it != m_items.end(); ++it)
        if (it.value().contains(color))
            result << it.key();
    result.sort();
    return result;
}
void TagStore::moved(const QString &from, const QString &to) {
    if (from.isEmpty() || to.isEmpty() || from == to)
        return;
    QHash<QString, QStringList> carried;
    for (auto it = m_items.begin(); it != m_items.end();) {
        if (within(it.key(), from)) {
            carried.insert(to + it.key().mid(from.size()), it.value());
            it = m_items.erase(it);
        } else
            ++it;
    }
    if (carried.isEmpty())
        return;
    for (auto it = carried.begin(); it != carried.end(); ++it)
        m_items.insert(it.key(), it.value());
    touch();
}
void TagStore::removed(const QString &uri) {
    bool any = false;
    for (auto it = m_items.begin(); it != m_items.end();) {
        if (within(it.key(), uri)) {
            it = m_items.erase(it);
            any = true;
        } else
            ++it;
    }
    if (any)
        touch();
}
