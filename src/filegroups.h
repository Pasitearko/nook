#pragma once
#include <QAbstractListModel>
#include <QSet>
#include <QTimer>

class DirectoryModel;

// A visual layout over the directory, never a second collection of files. Every tile keeps
// its source row, so grouping headers cannot be mistaken for operation/selection targets.
class FileGroups : public QAbstractListModel {
    Q_OBJECT
    Q_PROPERTY(QString mode READ mode WRITE setMode NOTIFY modeChanged)
    Q_PROPERTY(bool available READ available NOTIFY modeChanged)
    Q_PROPERTY(bool active READ active NOTIFY modeChanged)
    Q_PROPERTY(int columns READ columns WRITE setColumns NOTIFY columnsChanged)
    Q_PROPERTY(int revision READ revision NOTIFY revisionChanged)
    Q_PROPERTY(bool ready READ ready NOTIFY readyChanged)
  public:
    enum Role { HeaderRole = Qt::UserRole + 1, KeyRole, TitleRole, CountRole,
                PickedRole, CollapsedRole, SourcesRole };
    explicit FileGroups(DirectoryModel *source);
    int rowCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;
    QString mode() const { return m_mode; }
    bool available() const;
    bool active() const { return available() && m_mode != "none"; }
    void setMode(const QString &mode);
    int columns() const { return m_columns; }
    void setColumns(int columns);
    int revision() const { return m_revision; }
    bool ready() const { return m_ready; }
    Q_INVOKABLE void toggle(const QString &key);
    Q_INVOKABLE void expandAll();
    Q_INVOKABLE void collapseAll();
    Q_INVOKABLE void selectGroup(const QString &key);
    Q_INVOKABLE void selectSource(int sourceRow, int modifiers);
    Q_INVOKABLE int moveSource(int current, int step) const;
    Q_INVOKABLE int moveVertical(int current, int direction) const;
    Q_INVOKABLE int sourceForUri(const QString &uri) const;
    Q_INVOKABLE int layoutRow(int sourceRow) const;
    Q_INVOKABLE QVariantMap groupInfo(int layoutRow) const;
    Q_INVOKABLE int layoutForGroup(const QString &key) const;
    Q_INVOKABLE QList<int> sourcesBetweenRows(int first, int last, double x1, double x2, double cellWidth, bool grid) const;
  Q_SIGNALS:
    void modeChanged();
    void columnsChanged();
    void revisionChanged();
    void readyChanged();
  private:
    struct Group { QString key, title; QList<int> sources; };
    struct Row { int group; QList<int> sources; };
    void restore();
    void save() const;
    void rebuild();
    void relayout(bool startReset = true);
    QVariantMap info(int group) const;
    DirectoryModel *m_source;
    QString m_mode = "none", m_anchorUri;
    QSet<QString> m_collapsed;
    QList<Group> m_groups;
    QList<Row> m_rows;
    QList<int> m_visible;
    QHash<int, int> m_sourceLayout;
    QHash<QString, int> m_uriSources;
    int m_columns = 1, m_revision = 0;
    bool m_ready = true;
    QTimer m_rebuildTimer, m_dayTimer;
};
