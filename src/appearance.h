#pragma once
#include <QDBusVariant>
#include <QObject>

// Whether the desktop prefers dark windows. The portal's appearance setting is read without
// blocking: the file chooser is started by the portal itself and must never wait on it. Qt's
// own reading (styleHints) is the fallback when the portal cannot be reached.
class SystemAppearance : public QObject {
    Q_OBJECT
  public:
    explicit SystemAppearance(QObject *parent = nullptr);
    bool dark() const;
  Q_SIGNALS:
    void changed();

  private Q_SLOTS:
    void settingChanged(const QString &group, const QString &key, const QDBusVariant &value);

  private:
    void read(bool readOne);
    void apply(const QVariant &value);
    int m_portal = -1; // 0 no preference, 1 dark, 2 light; -1 unknown
};
