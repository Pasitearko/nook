#pragma once
#include <QObject>
#include <QVariantList>
#include <functional>

// Passwords the user chose to keep, in the desktop keyring (Secret Service, through libsecret),
// each under a name of the user's choosing. Nothing is read until first needed, because opening
// a locked keyring asks the user to unlock it.
class PasswordStore : public QObject {
    Q_OBJECT
    // False when there is no keyring to keep passwords in (then nothing can be saved).
    Q_PROPERTY(bool available READ available NOTIFY changed)
    Q_PROPERTY(bool loaded READ loaded NOTIFY changed)
    Q_PROPERTY(bool busy READ busy NOTIFY changed)
    // [{name, password}], sorted by name.
    Q_PROPERTY(QVariantList entries READ entries NOTIFY changed)
  public:
    explicit PasswordStore(QObject *parent = nullptr);
    bool available() const { return m_available; }
    bool loaded() const { return m_loaded; }
    bool busy() const { return m_busy > 0; }
    QVariantList entries() const { return m_entries; }
    QStringList passwords() const;

    // Reads the keyring once; later calls only reread it when force is set.
    Q_INVOKABLE void load(bool force = false);
    Q_INVOKABLE void save(const QString &name, const QString &password);
    Q_INVOKABLE void remove(const QString &name);
    // Tests and machines without a keyring: keep everything in memory only.
    void setMemoryOnly(bool memoryOnly);

  Q_SIGNALS:
    void changed();
    void failed(const QString &message);

  private:
    void run(std::function<QString()> work, std::function<void(const QString &)> done);
    void apply(const QVariantList &entries, bool available);
    QVariantList m_entries;
    bool m_available = true;
    bool m_loaded = false;
    bool m_memoryOnly = false;
    int m_busy = 0;
};
