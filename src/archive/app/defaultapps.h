#pragma once
#include <QObject>
#include <QStringList>

// "设为默认压缩软件": every file type nook解压缩's desktop file claims, who opens each now, and
// taking them over (Q25: only when the user asks; Q40: all of them). The user's mimeapps.list is
// backed up first and the earlier defaults are remembered, so that they can be put back.
class DefaultApps : public QObject {
    Q_OBJECT
    // unavailable (the desktop file is not installed), none, some, all
    Q_PROPERTY(QString state READ state NOTIFY changed)
    Q_PROPERTY(int typeCount READ typeCount NOTIFY changed)
    Q_PROPERTY(int ownedCount READ ownedCount NOTIFY changed)
    // The application that opens most of the types nook解压缩 does not ("归档管理器").
    Q_PROPERTY(QString otherName READ otherName NOTIFY changed)
    Q_PROPERTY(bool canRestore READ canRestore NOTIFY changed)
    Q_PROPERTY(QString lastBackup READ lastBackup NOTIFY changed)
    Q_PROPERTY(QString mimeappsPath READ mimeappsPath CONSTANT)
  public:
    explicit DefaultApps(const QString &desktopId = QStringLiteral("local.nook.archive.desktop"), QObject *parent = nullptr);

    QString state() const { return m_state; }
    int typeCount() const { return int(m_types.size()); }
    int ownedCount() const { return m_owned; }
    QString otherName() const { return m_otherName; }
    bool canRestore() const;
    QString lastBackup() const;
    QString mimeappsPath() const;
    // Each claimed type once (aliases resolved).
    QStringList types() const { return m_types; }
    // The desktop id of what opens type now ("" when nothing does).
    QString defaultFor(const QString &type) const;

    // Reads who opens what again (another program may have changed it).
    Q_INVOKABLE void refresh();
    // Backs up mimeapps.list, remembers the current defaults and takes every type: "" or what went wrong.
    Q_INVOKABLE QString setAsDefault();
    // Puts back what opened each type before setAsDefault: "" or what went wrong.
    Q_INVOKABLE QString restore();

  Q_SIGNALS:
    void changed();

  private:
    QString recordPath() const;
    QString m_id;
    QStringList m_types;
    QString m_state = QStringLiteral("unavailable");
    int m_owned = 0;
    QString m_otherName;
};
