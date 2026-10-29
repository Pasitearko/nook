#pragma once
#include <QHash>
#include <QObject>
#include <functional>

// Desktop notifications (org.freedesktop.Notifications) with one optional action button.
class Notifier : public QObject {
    Q_OBJECT
  public:
    explicit Notifier(QObject *parent = nullptr);
    // actionLabel empty: no button. onAction runs when the button (or the notification) is clicked.
    void notify(const QString &title, const QString &body, bool problem, const QString &actionLabel = {},
                std::function<void()> onAction = {});
    int sent() const { return m_sent; }

  private Q_SLOTS:
    void invoked(uint id, const QString &action);
    void closed(uint id, uint reason);

  private:
    QHash<uint, std::function<void()>> m_actions;
    int m_sent = 0;
};
