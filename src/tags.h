#pragma once
#include <QHash>
#include <QObject>
#include <QStringList>
#include <QTimer>
#include <QVariantList>

// Colour tags, as in Pulse: each of seven colours is one tag, and the user may give it a name
// ("红色" can become "紧急"). Items are kept by URI in nook's data folder (tags.json), so a tag
// works on any file system; nook's own moves and renames carry an item's tags along, and
// deleting it for good takes them off. Items that went missing elsewhere are kept (a disk may
// come back) but not counted or listed.
class TagStore : public QObject {
    Q_OBJECT
    // [{id, name, count}] in the order the user arranged them (the palette's at first).
    Q_PROPERTY(QVariantList tags READ tags NOTIFY changed)
  public:
    static TagStore *instance();
    // The palette, in the order the sidebar and menus show it.
    static const QStringList &colors();
    QVariantList tags() const;
    Q_INVOKABLE QStringList tagsOf(const QString &uri) const;
    Q_INVOKABLE QString nameOf(const QString &color) const;
    // How many of these items carry the colour: 0 none, 1 some, 2 all.
    Q_INVOKABLE int state(const QStringList &uris, const QString &color) const;
    // Every item gets the colour, or, when all of them already have it, loses it.
    Q_INVOKABLE void toggle(const QStringList &uris, const QString &color);
    Q_INVOKABLE void set(const QStringList &uris, const QString &color, bool on);
    Q_INVOKABLE void clear(const QStringList &uris);
    // An empty name gives the colour its own name back.
    Q_INVOKABLE void rename(const QString &color, const QString &name);
    // Takes the colour off every item.
    Q_INVOKABLE void clearColor(const QString &color);
    // Dragged in the sidebar: the colour goes to this place in the list.
    Q_INVOKABLE void move(const QString &color, int to);
    QStringList urisWith(const QString &color) const;
    // From nook's own file operations; a folder's contents follow it.
    void moved(const QString &from, const QString &to);
    void removed(const QString &uri);
    // Reads tags.json again (tests use a fresh profile per case).
    void reload();
    void flush();

  Q_SIGNALS:
    void changed();

  private:
    explicit TagStore(QObject *parent = nullptr);
    ~TagStore() override;
    void touch();
    QString m_file;
    QHash<QString, QStringList> m_items;
    QHash<QString, QString> m_names;
    QStringList m_order;
    QTimer m_saveTimer;
    mutable QVariantList m_cache;
    mutable bool m_dirty = true;
};
