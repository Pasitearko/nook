#include "passwordstore.h"
#include <QCollator>
#include <QCoreApplication>
#include <algorithm>
#include <QPointer>
#include <QThreadPool>
#include <libsecret/secret.h>
#include <memory>

namespace {

const SecretSchema *schema() {
    static const SecretSchema value = {"local.nook.archive.Password",
                                       SECRET_SCHEMA_NONE,
                                       {{"application", SECRET_SCHEMA_ATTRIBUTE_STRING},
                                        {"name", SECRET_SCHEMA_ATTRIBUTE_STRING},
                                        {nullptr, SECRET_SCHEMA_ATTRIBUTE_STRING}},
                                       0, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr};
    return &value;
}

constexpr const char *Application = "nook-archive";

QString take(GError *error) {
    const QString message = error ? QString::fromUtf8(error->message) : QString();
    if (error)
        g_error_free(error);
    return message;
}

void sortByName(QVariantList &entries) {
    QCollator collator;
    collator.setNumericMode(true);
    std::sort(entries.begin(), entries.end(), [&](const QVariant &a, const QVariant &b) {
        return collator.compare(a.toMap().value("name").toString(), b.toMap().value("name").toString()) < 0;
    });
}

// Every saved password of this application, with its secret.
QString search(QVariantList *entries) {
    GError *error = nullptr;
    GList *items = secret_password_search_sync(
        schema(), SecretSearchFlags(SECRET_SEARCH_ALL | SECRET_SEARCH_UNLOCK | SECRET_SEARCH_LOAD_SECRETS), nullptr,
        &error, "application", Application, nullptr);
    if (error)
        return take(error);
    for (GList *item = items; item; item = item->next) {
        auto *retrievable = SECRET_RETRIEVABLE(item->data);
        GHashTable *attributes = secret_retrievable_get_attributes(retrievable);
        const char *name = static_cast<const char *>(g_hash_table_lookup(attributes, "name"));
        SecretValue *value = secret_retrievable_retrieve_secret_sync(retrievable, nullptr, nullptr);
        if (name && value) {
            const char *text = secret_value_get_text(value);
            if (text)
                entries->append(QVariantMap{{"name", QString::fromUtf8(name)}, {"password", QString::fromUtf8(text)}});
        }
        if (value)
            secret_value_unref(value);
        g_hash_table_unref(attributes);
    }
    g_list_free_full(items, g_object_unref);
    return {};
}

} // namespace

PasswordStore::PasswordStore(QObject *parent) : QObject(parent) {}

QStringList PasswordStore::passwords() const {
    QStringList result;
    for (const auto &entry : m_entries)
        result << entry.toMap().value("password").toString();
    return result;
}

void PasswordStore::setMemoryOnly(bool memoryOnly) {
    m_memoryOnly = memoryOnly;
    m_loaded = memoryOnly;
    m_available = true;
    Q_EMIT changed();
}

void PasswordStore::run(std::function<QString()> work, std::function<void(const QString &)> done) {
    ++m_busy;
    Q_EMIT changed();
    QPointer<PasswordStore> self(this);
    // libsecret's synchronous calls run their own main loop; off the GUI thread they cannot
    // stall the window while the keyring asks to be unlocked.
    QThreadPool::globalInstance()->start([self, work = std::move(work), done = std::move(done)] {
        const QString error = work();
        QMetaObject::invokeMethod(
            qApp, [self, done, error] {
                if (!self)
                    return;
                --self->m_busy;
                done(error);
                Q_EMIT self->changed();
            },
            Qt::QueuedConnection);
    });
}

void PasswordStore::apply(const QVariantList &entries, bool available) {
    m_entries = entries;
    sortByName(m_entries);
    m_available = available;
    m_loaded = true;
}

void PasswordStore::load(bool force) {
    if (m_memoryOnly || (m_loaded && !force) || m_busy)
        return;
    auto found = std::make_shared<QVariantList>();
    run([found] { return search(found.get()); },
        [this, found](const QString &error) {
            // No Secret Service at all: nothing can be kept, which the dialogs say.
            apply(*found, error.isEmpty());
        });
}

void PasswordStore::save(const QString &name, const QString &password) {
    if (name.trimmed().isEmpty() || password.isEmpty())
        return;
    const QString key = name.trimmed();
    QVariantList entries;
    for (const auto &entry : std::as_const(m_entries))
        if (entry.toMap().value("name").toString() != key)
            entries << entry;
    entries << QVariantMap{{"name", key}, {"password", password}};
    if (m_memoryOnly) {
        apply(entries, true);
        Q_EMIT changed();
        return;
    }
    run(
        [key, password] {
            GError *error = nullptr;
            const QByteArray label = ("nook解压缩：" + key).toUtf8();
            secret_password_store_sync(schema(), SECRET_COLLECTION_DEFAULT, label.constData(),
                                       password.toUtf8().constData(), nullptr, &error, "application", Application,
                                       "name", key.toUtf8().constData(), nullptr);
            return take(error);
        },
        [this, entries](const QString &error) {
            if (error.isEmpty())
                apply(entries, true);
            else
                Q_EMIT failed("无法把密码保存到密钥环：" + error);
        });
}

void PasswordStore::remove(const QString &name) {
    QVariantList entries;
    for (const auto &entry : std::as_const(m_entries))
        if (entry.toMap().value("name").toString() != name)
            entries << entry;
    if (m_memoryOnly) {
        apply(entries, true);
        Q_EMIT changed();
        return;
    }
    run(
        [name] {
            GError *error = nullptr;
            secret_password_clear_sync(schema(), nullptr, &error, "application", Application, "name",
                                       name.toUtf8().constData(), nullptr);
            return take(error);
        },
        [this, entries](const QString &error) {
            if (error.isEmpty())
                apply(entries, m_available);
            else
                Q_EMIT failed("无法从密钥环删除密码：" + error);
        });
}
