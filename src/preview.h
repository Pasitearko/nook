#pragma once
#include <QObject>
#include <QProcess>
#include <QQuickAsyncImageProvider>
#include <QThreadPool>
#include <QTimer>
#include <QVariantMap>

class Preview : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool visible READ visible NOTIFY changed)
    Q_PROPERTY(bool loading READ loading NOTIFY changed)
    Q_PROPERTY(QVariantMap content READ content NOTIFY changed)
    Q_PROPERTY(QString uri READ uri NOTIFY changed)
    Q_PROPERTY(int page READ page NOTIFY changed)
  public:
    explicit Preview(QObject *parent = nullptr);
    ~Preview() override;
    bool visible() const {
        return m_visible;
    }
    bool loading() const {
        return m_loading;
    }
    QVariantMap content() const {
        return m_content;
    }
    QString uri() const {
        return m_uri;
    }
    int page() const {
        return m_page;
    }
    void setWorkerExecutable(QString path) {
        m_executable = std::move(path);
    }
    // Metadata only: accepts entryMap fields plus optional mimeType/mime/contentType.
    Q_INVOKABLE bool hoverEligible(const QVariantMap &entry) const;
    Q_INVOKABLE void open(const QString &uri, int page = 1);
    Q_INVOKABLE void close();
  Q_SIGNALS:
    void changed();

  private:
    void stopProcess();
    QString m_uri;
    int m_page = 1;
    quint64 m_generation = 0;
    bool m_visible = false, m_loading = false;
    QVariantMap m_content;
    QProcess *m_process = nullptr;
    QString m_executable;
};
QVariantMap makePreview(const QString &uri, int page, bool thumbnail = false);
class ThumbnailProvider : public QQuickAsyncImageProvider {
  public:
    // The worker is nook itself unless named (tests run from another program).
    explicit ThumbnailProvider(QString workerExecutable = {});
    ~ThumbnailProvider() override;
    // id: encodeURIComponent(fileUri) + '|' + bytes + modified (opaque revision suffix).
    QQuickImageResponse *requestImageResponse(const QString &id, const QSize &size) override;

  private:
    QThreadPool m_pool;
    QString m_executable;
};
