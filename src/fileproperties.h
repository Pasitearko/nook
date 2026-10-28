#pragma once
#include <QObject>
#include <QVariantMap>
#include <memory>

class FileProperties : public QObject {
    Q_OBJECT
    Q_PROPERTY(QVariantMap details READ details NOTIFY changed)
    Q_PROPERTY(bool busy READ busy NOTIFY changed)
  public:
    explicit FileProperties(QObject *parent = nullptr);
    ~FileProperties() override;
    // Timestamps are Unix seconds, with additional modifiedText/createdText display strings.
    // path is display-only; permission operations retain GIO's original filename bytes.
    QVariantMap details() const;
    bool busy() const;
    Q_INVOKABLE void inspect(const QString &uri);
    Q_INVOKABLE void applyPermissions(int mode);
    // Dot names are renamed without the dot to un-hide; others use the parent's .hidden list.
    Q_INVOKABLE void setHidden(bool hidden);
  Q_SIGNALS:
    void changed();
    void error(const QString &message);
    void permissionsApplied();
    void hiddenApplied(bool hidden);

  private:
    struct Private;
    std::unique_ptr<Private> d;
};
