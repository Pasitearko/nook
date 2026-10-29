#pragma once
#include <QByteArray>
#include <QJsonArray>
#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <optional>
#include <sys/stat.h>
#include <sys/types.h>

// The unprivileged process that reads and writes packages for nook解压缩. It takes one JSON request
// on the first stdin line, then answers questions (passwords, conflicts) and a cancel command on
// later lines, and reports events as JSON lines on stdout. See docs/ARCHIVE-WORKER.md.
namespace nook::archive::worker {

class Channel {
  public:
    static Channel &instance();
    QJsonObject readRequest();
    void listen();
    void send(const QJsonObject &event);
    // Rate-limited to one event per 100 ms unless forced.
    void progress(QJsonObject event, bool force = false);
    bool cancelled() const { return m_cancelled.load(); }
    void cancel() { m_cancelled = true; }
    // Waits for a command such as {"cmd":"password"}; empty when cancelled or stdin closed.
    std::optional<QJsonObject> await(const QString &command);

  private:
    std::mutex m_mutex;
    std::condition_variable m_ready;
    std::deque<QJsonObject> m_commands;
    std::atomic<bool> m_cancelled = false;
    bool m_closed = false;
    std::mutex m_writeMutex;
    qint64 m_lastProgress = 0;
};

void warn(const QString &code, const QString &path, const QString &message);
QJsonObject failure(const QString &code, const QString &message);
// Asks the client for a password; wrong says the previous one was rejected.
std::optional<QString> askPassword(bool wrong);

// Landlock: after this only writableDir (and nothing, if empty) may be modified by this process
// and the programs it starts. Returns false where the kernel has no Landlock.
bool restrictWrites(const QString &writableDir);

enum class Choice { Overwrite, Skip, Rename };

struct Conflicts {
    QString policy = "ask"; // ask, overwrite, skip, rename
    // Answers conflicts on paths relative to the destination; remembers "apply to all".
    Choice resolve(const QString &path, const struct stat &existing, const struct stat &incoming);
    int counter = 0;
};

// A private directory inside the destination that receives everything first; commit() moves it
// into place. Nothing is ever written through a symbolic link found on the way.
class Stage {
  public:
    explicit Stage(int destinationFd) : m_destination(destinationFd) {}
    ~Stage();
    bool create(QString *error);
    QString path(const QString &destinationPath) const { return destinationPath + u'/' + m_name; }
    int fd() const { return m_fd; }
    // A directory under the stage root, created (0700) as needed. -1 with errno on failure, ELOOP
    // when a component is a symbolic link.
    int directory(const QStringList &components, bool create) const;
    // Removes unsafe links and special files, strips set-id bits and keeps directories writable.
    void sanitize(const QStringList &root);
    struct Result {
        QStringList created;
        int skipped = 0;
    };
    // Moves the contents of root into the destination. subfolder is "smart", "always" or "never".
    Result commit(const QStringList &root, const QString &subfolder, const QString &folderName,
                  Conflicts &conflicts);

  private:
    void commitTree(int sourceFd, int targetFd, const QString &relative, Conflicts &conflicts,
                    Result &result);
    int m_destination;
    int m_fd = -1;
    QByteArray m_name;
};

// Whether a link at location (path components under root) to target stays inside root, following
// links it passes through that already exist under rootFd.
bool linkStaysInside(int rootFd, const QStringList &location, const QByteArray &target, int depth = 0);
void removeTree(int dirFd, const QByteArray &name);
// Splits an archive path into safe components; empty when it climbs out with "..".
QStringList safeComponents(QString path, bool backslashes);
mode_t currentUmask();
QByteArray encodePath(const QString &path);

struct Request {
    QJsonObject json;
    QString op;
    QString archive;
    QString password;
    bool hasPassword = false;
    QString encoding = "auto";
};

// Libarchive: ZIP (any name encoding, ZipCrypto and AES), tar and compressed tar, single streams,
// ISO, CAB, cpio, ar/deb, RPM, XAR, LHA. 7-Zip: 7z, RAR, split volumes, WIM and the rest.
enum class Backend { Libarchive, SevenZip };
Backend backendFor(const QString &path);

QJsonObject libarchiveList(const Request &request, bool *fallback);
QJsonObject libarchiveTest(const Request &request, bool *fallback);
QJsonObject libarchiveExtract(const Request &request, bool *fallback);
QJsonObject sevenZipList(const Request &request);
// 7-Zip's entries for a package that needs no password to list (packed sizes, methods).
QJsonArray sevenZipEntries(const QString &archive);
QJsonObject sevenZipTest(const Request &request);

// Changing ZIP and 7z packages goes through 7-Zip on a copy (see modify.cpp).
struct EditListing {
    bool ok = false;
    QJsonObject done; // when !ok
    QJsonArray entries;
    std::optional<QString> password;
    bool encrypted = false;
    bool headerEncrypted = false;
};
// Lists archive (asking for a header password); with needPassword, also makes sure the password
// for encrypted entries is known and right.
EditListing sevenZipEditListing(const Request &request, const QString &archive, bool needPassword);
// Runs 7-Zip with arguments (and a list file of listLines) in directory; asks again for a
// rejected password. Empty on success, else a done event.
QJsonObject sevenZipEdit(const QStringList &arguments, const QStringList &listLines, const QString &directory,
                         const QString &phase, std::optional<QString> &password);
// The switches every 7-Zip call takes (UTF-8 names and console, output on stdout/stderr).
QStringList sevenZipSwitches();
QJsonObject sevenZipVerify(const QString &archive, const std::optional<QString> &password);
// add, delete, rename on ZIP, 7z and tar packages; convert to another format.
QJsonObject modify(const Request &request);
QJsonObject convert(const Request &request);
QJsonObject sevenZipExtract(const Request &request);
QJsonObject create(const Request &request);
QJsonObject sevenZipCreate(const Request &request, const QString &temporaryPath);

// Shared by both extract paths once files sit in the stage.
QJsonObject finishExtract(Stage &stage, const QJsonObject &request, const QStringList &root,
                          bool cancelled, const QJsonObject &error,
                          const std::function<void()> &beforeCommit = {});
QString destinationOf(const QJsonObject &request);

}
