#include "worker.h"
#include "../archivenames.h"
#include <QDateTime>
#include <QFileInfo>
#include <QRegularExpression>
#include <QStandardPaths>
#include <cerrno>
#include <csignal>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <sys/mman.h>
#include <sys/statvfs.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

namespace nook::archive::worker {
namespace {

QString program() {
    const QString configured = qEnvironmentVariable("NOOK_7Z");
    if (!configured.isEmpty())
        return configured;
    for (const auto *name : {"7z", "7zz", "7za"}) {
        const QString found = QStandardPaths::findExecutable(name);
        if (!found.isEmpty())
            return found;
    }
    return {};
}

struct Run {
    int exitCode = -1;
    bool cancelled = false;
    bool started = false;
    QString errors; // "ERROR:" lines and other stderr text, capped
};

// A list file the child reads as /proc/self/fd/N; it never touches the disk.
class ListFile {
  public:
    explicit ListFile(const QStringList &lines) {
        if (lines.isEmpty())
            return;
        m_fd = ::memfd_create("nook-list", 0);
        const QByteArray data = lines.join(u'\n').toUtf8() + '\n';
        if (m_fd >= 0 && ::write(m_fd, data.constData(), size_t(data.size())) != data.size()) {
            ::close(m_fd);
            m_fd = -1;
        }
    }
    ~ListFile() {
        if (m_fd >= 0)
            ::close(m_fd);
    }
    QString argument() const { return m_fd >= 0 ? QStringLiteral("@/proc/self/fd/%1").arg(m_fd) : QString(); }

  private:
    int m_fd = -1;
};

Run run(const QStringList &arguments, const QByteArray &input, const std::function<void(const QByteArray &)> &onLine,
        const std::function<void(const QByteArray &)> &onProgress) {
    Run result;
    const QString executable = program();
    if (executable.isEmpty()) {
        result.errors = "找不到 7-Zip（7z）程序";
        return result;
    }
    std::vector<QByteArray> storage{executable.toLocal8Bit()};
    for (const auto &argument : arguments)
        storage.push_back(argument.toUtf8());
    std::vector<char *> argv;
    for (auto &value : storage)
        argv.push_back(value.data());
    argv.push_back(nullptr);
    std::vector<QByteArray> environment;
    for (char **entry = environ; *entry; ++entry) {
        if (std::strncmp(*entry, "LC_ALL=", 7) != 0 && std::strncmp(*entry, "LANG=", 5) != 0)
            environment.emplace_back(*entry);
    }
    environment.emplace_back("LC_ALL=C.UTF-8");
    std::vector<char *> envp;
    for (auto &value : environment)
        envp.push_back(value.data());
    envp.push_back(nullptr);

    int in[2], out[2], err[2];
    if (::pipe2(in, O_CLOEXEC) != 0 || ::pipe2(out, O_CLOEXEC) != 0 || ::pipe2(err, O_CLOEXEC) != 0) {
        result.errors = QString::fromLocal8Bit(std::strerror(errno));
        return result;
    }
    const pid_t pid = ::fork();
    if (pid == 0) {
        ::dup2(in[0], 0);
        ::dup2(out[1], 1);
        ::dup2(err[1], 2);
        ::execve(argv[0], argv.data(), envp.data());
        ::_exit(127);
    }
    ::close(in[0]);
    ::close(out[1]);
    ::close(err[1]);
    if (pid < 0) {
        ::close(in[1]);
        ::close(out[0]);
        ::close(err[0]);
        result.errors = QString::fromLocal8Bit(std::strerror(errno));
        return result;
    }
    result.started = true;
    ::signal(SIGPIPE, SIG_IGN);
    if (!input.isEmpty()) {
        const ssize_t written = ::write(in[1], input.constData(), size_t(input.size()));
        Q_UNUSED(written);
    }
    ::close(in[1]);

    auto &channel = Channel::instance();
    QByteArray outBuffer, errBuffer;
    bool outOpen = true, errOpen = true;
    qint64 killedAt = 0;
    const auto drain = [&](QByteArray &buffer, bool isErr) {
        if (!isErr) {
            int newline;
            while ((newline = int(buffer.indexOf('\n'))) >= 0) {
                const QByteArray line = buffer.left(newline);
                buffer.remove(0, newline + 1);
                if (onLine)
                    onLine(line);
            }
            return;
        }
        // Progress is redrawn with backspaces; errors arrive as whole lines.
        for (;;) {
            qsizetype cut = -1;
            for (qsizetype i = 0; i < buffer.size(); ++i) {
                const char c = buffer.at(i);
                if (c == '\n' || c == '\b' || c == '\r') {
                    cut = i;
                    break;
                }
            }
            if (cut < 0)
                break;
            const QByteArray token = buffer.left(cut).trimmed();
            buffer.remove(0, cut + 1);
            if (token.isEmpty())
                continue;
            if (onProgress && token.contains('%') && !token.startsWith("ERROR"))
                onProgress(token);
            else if (result.errors.size() < 65536)
                result.errors += QString::fromUtf8(token) + u'\n';
        }
    };
    while (outOpen || errOpen) {
        pollfd fds[2] = {{outOpen ? out[0] : -1, POLLIN, 0}, {errOpen ? err[0] : -1, POLLIN, 0}};
        ::poll(fds, 2, 100);
        char chunk[16384];
        if (fds[0].revents) {
            const ssize_t n = ::read(out[0], chunk, sizeof chunk);
            if (n > 0) {
                outBuffer.append(chunk, n);
                drain(outBuffer, false);
            } else if (n == 0 || errno != EINTR) {
                outOpen = false;
            }
        }
        if (fds[1].revents) {
            const ssize_t n = ::read(err[0], chunk, sizeof chunk);
            if (n > 0) {
                errBuffer.append(chunk, n);
                drain(errBuffer, true);
            } else if (n == 0 || errno != EINTR) {
                errOpen = false;
            }
        }
        if (channel.cancelled()) {
            const qint64 now = QDateTime::currentMSecsSinceEpoch();
            if (!killedAt) {
                result.cancelled = true;
                killedAt = now;
                ::kill(pid, SIGTERM);
            } else if (now - killedAt > 2000) {
                ::kill(pid, SIGKILL);
            }
        }
    }
    if (!outBuffer.isEmpty() && onLine)
        onLine(outBuffer);
    if (!errBuffer.trimmed().isEmpty())
        result.errors += QString::fromUtf8(errBuffer.trimmed());
    ::close(out[0]);
    ::close(err[0]);
    int status = 0;
    while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) {
    }
    result.exitCode = WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
    return result;
}

double percent(const QByteArray &token) {
    static const QRegularExpression pattern(QStringLiteral("(\\d+)%"));
    const auto match = pattern.match(QString::fromUtf8(token));
    return match.hasMatch() ? match.captured(1).toDouble() / 100.0 : -1;
}

enum class Outcome { Ok, NeedPassword, WrongPassword, Failed, Cancelled };

// 7-Zip prints its password prompt even when it reads one from stdin; only "Break signaled"
// (stdin ended while it was asking) means no password was available.
Outcome outcomeOf(const Run &run) {
    if (run.cancelled)
        return Outcome::Cancelled;
    if (run.errors.contains("Wrong password", Qt::CaseInsensitive))
        return Outcome::WrongPassword;
    if (run.errors.contains("Break signaled"))
        return Outcome::NeedPassword;
    return run.exitCode == 0 || run.exitCode == 1 ? Outcome::Ok : Outcome::Failed;
}

QJsonObject errorOf(const Run &run, bool split) {
    const QString text = run.errors;
    if (!run.started)
        return failure("missing-tool", text);
    if (text.contains("No space left", Qt::CaseInsensitive))
        return failure("no-space", "磁盘空间不足");
    if (split && (text.contains("Missing volume", Qt::CaseInsensitive) || text.contains("Unexpected end", Qt::CaseInsensitive)))
        return failure("missing-volume", "分卷不完整，请把所有分卷放在同一文件夹");
    if (text.contains("Can not open the file as archive", Qt::CaseInsensitive) ||
        text.contains("Cannot open the file as archive", Qt::CaseInsensitive))
        return failure("unsupported", "无法识别的压缩包格式");
    if (text.contains("Unsupported", Qt::CaseInsensitive))
        return failure("unsupported", "不支持的压缩方式");
    if (text.contains("Data Error", Qt::CaseInsensitive) || text.contains("CRC Failed", Qt::CaseInsensitive) ||
        text.contains("Headers Error", Qt::CaseInsensitive) || text.contains("Unexpected end", Qt::CaseInsensitive))
        return failure("corrupt", "压缩包已损坏或不完整");
    if (run.exitCode == 8)
        return failure("memory", "内存不足");
    QString last;
    for (const auto &line : text.split(u'\n')) {
        if (line.startsWith(u"ERROR:"))
            last = line.mid(6).trimmed();
    }
    return failure("failed", QStringLiteral("7-Zip 出错（代码 %1）%2").arg(run.exitCode).arg(last.isEmpty() ? QString() : "：" + last));
}

// Entries and archive properties from "7z l -slt".
struct Listing {
    QJsonObject properties;
    QJsonArray items;
    bool encrypted = false;
    qint64 total = 0;
    int count = 0;
};

class ListParser {
  public:
    explicit ListParser(bool emitEntries, bool keep = false) : m_emit(emitEntries), m_keep(keep) {}
    void line(const QByteArray &raw) {
        const QString text = QString::fromUtf8(raw).trimmed();
        if (text == u"--") {
            m_section = Section::Archive;
            return;
        }
        if (text == u"----------") {
            m_section = Section::Entries;
            return;
        }
        if (text.isEmpty()) {
            flush();
            return;
        }
        const int equals = int(text.indexOf(u" = "));
        const QString key = equals > 0 ? text.left(equals) : text.endsWith(u" =") ? text.chopped(2) : QString();
        const QString value = equals > 0 ? text.mid(equals + 3) : QString();
        if (key.isEmpty())
            return;
        if (m_section == Section::Archive)
            m_listing.properties.insert(key, value);
        else if (m_section == Section::Entries)
            m_entry.insert(key, value);
    }
    void finish() {
        flush();
        if (m_emit && !m_batch.isEmpty())
            Channel::instance().send({{"event", "entries"}, {"items", m_batch}});
        m_batch = {};
    }
    Listing &listing() { return m_listing; }

  private:
    void flush() {
        if (m_entry.isEmpty())
            return;
        const QString attributes = m_entry.value("Attributes");
        const QString unixMode = attributes.section(u' ', -1);
        const bool hasUnix = unixMode.size() == 10 && QStringLiteral("-dlcbps").contains(unixMode.at(0));
        const bool directory = m_entry.value("Folder") == u"+" || attributes.startsWith(u'D') || (hasUnix && unixMode.at(0) == u'd');
        const bool link = hasUnix && unixMode.at(0) == u'l';
        mode_t mode = directory ? 0755 : 0644;
        if (hasUnix) {
            mode = 0;
            static const char *bits = "rwxrwxrwx";
            for (int i = 0; i < 9; ++i) {
                const QChar c = unixMode.at(i + 1);
                if (c == QLatin1Char(bits[i]) || ((i == 2 || i == 5) && c == u's') || (i == 8 && c == u't'))
                    mode |= mode_t(1) << (8 - i);
            }
        }
        const QStringList components = safeComponents(m_entry.value("Path"), false);
        const QString path = components.isEmpty() ? m_entry.value("Path") : components.join(u'/');
        const QString sizeText = m_entry.value("Size");
        const qint64 size = sizeText.isEmpty() ? -1 : sizeText.toLongLong();
        const QString packedText = m_entry.value("Packed Size");
        const QDateTime modified = QDateTime::fromString(m_entry.value("Modified").left(19), "yyyy-MM-dd HH:mm:ss");
        const bool encrypted = m_entry.value("Encrypted") == u"+";
        QJsonObject item{{"path", path},
                         {"type", directory ? "dir" : link ? "link" : "file"},
                         {"size", double(size)},
                         {"mtime", modified.isValid() ? double(modified.toSecsSinceEpoch()) : 0.0},
                         {"mode", int(mode)}};
        if (!packedText.isEmpty())
            item.insert("packed", packedText.toDouble());
        if (encrypted)
            item.insert("encrypted", true);
        if (!m_entry.value("Method").isEmpty())
            item.insert("method", m_entry.value("Method"));
        if (m_entry.contains("Symbolic Link") && !m_entry.value("Symbolic Link").isEmpty())
            item.insert("link", m_entry.value("Symbolic Link"));
        if (components.isEmpty())
            item.insert("unsafe", true);
        if (m_keep)
            m_listing.items << item;
        m_listing.encrypted = m_listing.encrypted || encrypted;
        m_listing.total += std::max<qint64>(size, 0);
        ++m_listing.count;
        if (m_emit) {
            m_batch << item;
            if (m_batch.size() == 1000) {
                Channel::instance().send({{"event", "entries"}, {"items", m_batch}});
                m_batch = {};
            }
        }
        m_entry = {};
    }

    enum class Section { Header, Archive, Entries } m_section = Section::Header;
    bool m_emit;
    bool m_keep;
    QHash<QString, QString> m_entry;
    QJsonArray m_batch;
    Listing m_listing;
};

QByteArray passwordInput(const std::optional<QString> &password, int times = 1) {
    if (!password)
        return {};
    QByteArray input;
    for (int i = 0; i < times; ++i)
        input += password->toUtf8() + '\n';
    return input;
}

QStringList common() { return {"-sccUTF-8", "-scsUTF-8", "-bso1", "-bse2"}; }

struct Prepared {
    bool ok = false;
    bool headerEncrypted = false;
    std::optional<QString> password;
    Listing listing;
    QJsonObject done; // set when !ok
};

// Lists the package (asking for a header password if needed) before testing or extracting it.
Prepared prepare(const Request &request, bool emitEntries) {
    Prepared prepared;
    if (request.hasPassword)
        prepared.password = request.password;
    const QString archive = volumesOf(request.archive).first;
    const bool split = volumesOf(request.archive).split;
    for (;;) {
        ListParser parser(emitEntries);
        const Run result = run(QStringList{"l", "-slt", "-bsp0"} + common() + QStringList{"--", archive},
                               passwordInput(prepared.password),
                               [&](const QByteArray &line) { parser.line(line); }, {});
        parser.finish();
        const Outcome outcome = outcomeOf(result);
        if (outcome == Outcome::NeedPassword || outcome == Outcome::WrongPassword) {
            prepared.headerEncrypted = true;
            const auto answer = askPassword(prepared.password.has_value());
            if (!answer) {
                prepared.done = {{"event", "done"}, {"ok", false}, {"cancelled", true}};
                return prepared;
            }
            prepared.password = answer;
            continue;
        }
        if (outcome == Outcome::Cancelled) {
            prepared.done = {{"event", "done"}, {"ok", false}, {"cancelled", true}};
            return prepared;
        }
        if (outcome == Outcome::Failed && parser.listing().count == 0) {
            prepared.done = {{"event", "done"}, {"ok", false}, {"error", errorOf(result, split)}};
            return prepared;
        }
        prepared.listing = parser.listing();
        prepared.ok = true;
        return prepared;
    }
}

}

QJsonArray sevenZipEntries(const QString &archive) {
    ListParser parser(false, true);
    const Run result = run(QStringList{"l", "-slt", "-bsp0"} + common() + QStringList{"--", archive}, {},
                           [&](const QByteArray &line) { parser.line(line); }, {});
    parser.finish();
    return result.exitCode == 0 ? parser.listing().items : QJsonArray();
}

QJsonObject sevenZipList(const Request &request) {
    auto &channel = Channel::instance();
    // Entries stream out while 7-Zip lists; info follows once the archive properties are known.
    Prepared prepared = prepare(request, true);
    if (!prepared.ok)
        return prepared.done;
    const auto &properties = prepared.listing.properties;
    const Volumes volumes = volumesOf(request.archive);
    channel.send({{"event", "info"},
                  {"backend", "7z"},
                  {"format", properties.value("Type").toString()},
                  {"filters", QJsonArray()},
                  {"encoding", "UTF-8"},
                  {"legacyNames", false},
                  {"encrypted", prepared.listing.encrypted || prepared.headerEncrypted},
                  {"headerEncrypted", prepared.headerEncrypted},
                  {"solid", properties.value("Solid").toString() == "+"},
                  {"split", volumes.split},
                  {"volumes", QJsonArray::fromStringList(volumes.parts)},
                  {"stream", false}});
    return {{"event", "done"}, {"ok", true}, {"count", prepared.listing.count}, {"totalSize", double(prepared.listing.total)}};
}

QJsonObject sevenZipTest(const Request &request) {
    auto &channel = Channel::instance();
    Prepared prepared = prepare(request, false);
    if (!prepared.ok)
        return prepared.done;
    if (prepared.listing.encrypted && !prepared.password) {
        prepared.password = askPassword(false);
        if (!prepared.password)
            return {{"event", "done"}, {"ok", false}, {"cancelled", true}};
    }
    const Volumes volumes = volumesOf(request.archive);
    for (;;) {
        const Run result = run(QStringList{"t", "-bsp2"} + common() + QStringList{"--", volumes.first},
                               passwordInput(prepared.password), {}, [&](const QByteArray &token) {
                                   const double fraction = percent(token);
                                   if (fraction >= 0)
                                       channel.progress({{"phase", "test"}, {"fraction", fraction}});
                               });
        const Outcome outcome = outcomeOf(result);
        if (outcome == Outcome::Cancelled)
            return {{"event", "done"}, {"ok", false}, {"cancelled", true}};
        if (outcome == Outcome::WrongPassword || outcome == Outcome::NeedPassword) {
            prepared.password = askPassword(true);
            if (!prepared.password)
                return {{"event", "done"}, {"ok", false}, {"cancelled", true}};
            continue;
        }
        QJsonArray errors;
        for (const auto &line : result.errors.split(u'\n')) {
            if (line.startsWith(u"ERROR:"))
                errors << QJsonObject{{"path", line.section(u':', -1).trimmed()}, {"message", line.mid(6).trimmed()}};
        }
        if (outcome == Outcome::Failed && errors.isEmpty())
            errors << QJsonObject{{"path", QString()}, {"message", errorOf(result, volumes.split).value("message")}};
        return {{"event", "done"}, {"ok", outcome == Outcome::Ok && errors.isEmpty()},
                {"tested", prepared.listing.count}, {"errors", errors}};
    }
}

QJsonObject sevenZipExtract(const Request &request) {
    auto &channel = Channel::instance();
    const QJsonObject &json = request.json;
    const QString destination = destinationOf(json);
    const int destinationFd = ::open(destination.toLocal8Bit().constData(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (destinationFd < 0)
        return {{"event", "done"}, {"ok", false}, {"error", failure("destination", "无法打开目标文件夹：" + QString::fromLocal8Bit(std::strerror(errno)))}};
    struct Closer {
        int fd;
        ~Closer() { ::close(fd); }
    } closer{destinationFd};
    Prepared prepared = prepare(request, false);
    if (!prepared.ok)
        return prepared.done;
    struct statvfs space{};
    if (::fstatvfs(destinationFd, &space) == 0 &&
        qint64(space.f_bavail) * qint64(space.f_frsize) < prepared.listing.total + (64ll << 20))
        return {{"event", "done"}, {"ok", false}, {"error", failure("no-space", "磁盘空间不足，解压需要约 " + QString::number(prepared.listing.total >> 20) + " MB")}};
    if (prepared.listing.encrypted && !prepared.password) {
        prepared.password = askPassword(false);
        if (!prepared.password)
            return {{"event", "done"}, {"ok", false}, {"cancelled", true}};
    }
    const Volumes volumes = volumesOf(request.archive);
    QStringList selection;
    for (const auto &value : json.value("entries").toArray())
        selection << safeComponents(value.toString(), false).join(u'/');
    const QStringList prefix = safeComponents(json.value("prefix").toString(), false);
    const bool flatten = json.value("flatten").toBool();
    for (;;) {
        auto stage = std::make_unique<Stage>(destinationFd);
        QString stageError;
        if (!stage->create(&stageError))
            return {{"event", "done"}, {"ok", false}, {"error", failure("destination", "无法写入目标文件夹：" + stageError)}};
        const QString stagePath = stage->path(destination);
        channel.send({{"event", "staging"}, {"path", stagePath}});
        ListFile list(selection);
        QStringList arguments{flatten ? "e" : "x", "-o" + stagePath, "-aou", "-y", "-spd", "-bb1", "-bsp2"};
        arguments << common() << volumes.first;
        if (!list.argument().isEmpty())
            arguments << list.argument();
        QString current;
        int files = 0;
        const Run result = run(arguments, passwordInput(prepared.password),
                               [&](const QByteArray &line) {
                                   if (line.startsWith("- ")) {
                                       current = QString::fromUtf8(line.mid(2));
                                       ++files;
                                   }
                               },
                               [&](const QByteArray &token) {
                                   const double fraction = percent(token);
                                   if (fraction >= 0)
                                       channel.progress({{"phase", "extract"}, {"fraction", fraction}, {"files", files},
                                                         {"current", current}});
                               });
        const Outcome outcome = outcomeOf(result);
        if (outcome == Outcome::WrongPassword || outcome == Outcome::NeedPassword) {
            prepared.password = askPassword(prepared.password.has_value());
            if (!prepared.password)
                return {{"event", "done"}, {"ok", false}, {"cancelled", true}};
            continue; // the stage and anything half-written in it go away with this attempt
        }
        if (outcome == Outcome::Cancelled && !current.isEmpty()) {
            // The file 7-Zip was writing when it stopped is incomplete.
            QStringList components = safeComponents(current, false);
            if (flatten && !components.isEmpty())
                components = {components.last()};
            if (!components.isEmpty()) {
                const int parent = stage->directory(components.mid(0, components.size() - 1), false);
                struct stat st{};
                const QByteArray name = encodePath(components.last());
                if (parent >= 0 && ::fstatat(parent, name.constData(), &st, AT_SYMLINK_NOFOLLOW) == 0 && !S_ISDIR(st.st_mode))
                    ::unlinkat(parent, name.constData(), 0);
                if (parent >= 0)
                    ::close(parent);
            }
        }
        channel.progress({{"phase", "commit"}, {"fraction", 1.0}, {"files", files}}, true);
        const QJsonObject error = outcome == Outcome::Failed ? errorOf(result, volumes.split) : QJsonObject();
        return finishExtract(*stage, json, flatten ? QStringList() : prefix, outcome == Outcome::Cancelled, error);
    }
}

QJsonObject sevenZipCreate(const Request &request, const QString &temporaryPath) {
    auto &channel = Channel::instance();
    const QJsonObject &json = request.json;
    const QJsonObject options = json.value("options").toObject();
    const QString format = json.value("format").toString();
    const bool sevenZip = format == "7z";
    QStringList arguments{"a", sevenZip ? "-t7z" : "-tzip", "-spd", "-bsp2"};
    arguments << common();
    const auto number = [&](const char *key, double low, double high, double *value) {
        if (!options.contains(key))
            return false;
        *value = options.value(key).toDouble(-1);
        return *value >= low && *value <= high;
    };
    double value = 0;
    if (number("level", 0, 9, &value))
        arguments << QStringLiteral("-mx=%1").arg(int(value));
    static const QStringList sevenZipMethods{"LZMA2", "LZMA", "PPMd", "BZip2", "Deflate", "Copy"};
    static const QStringList zipMethods{"Deflate", "Deflate64", "BZip2", "LZMA", "PPMd", "Copy"};
    const QString method = options.value("method").toString();
    if (!method.isEmpty()) {
        if (!(sevenZip ? sevenZipMethods : zipMethods).contains(method))
            return {{"event", "done"}, {"ok", false}, {"error", failure("options", "不支持的压缩方法：" + method)}};
        arguments << (sevenZip ? "-m0=" : "-mm=") + method;
    }
    if (number("dictionary", 4096, 1536.0 * (1 << 20), &value))
        arguments << QStringLiteral("-md=%1b").arg(qint64(value));
    if (number("wordSize", 4, 273, &value))
        arguments << QStringLiteral("-mfb=%1").arg(int(value));
    if (sevenZip && options.contains("solid")) {
        const QJsonValue solid = options.value("solid");
        if (solid.isBool())
            arguments << (solid.toBool() ? "-ms=on" : "-ms=off");
        else if (solid.toDouble() >= 1 << 20)
            arguments << QStringLiteral("-ms=%1m").arg(qint64(solid.toDouble()) >> 20);
    }
    if (number("threads", 1, 256, &value))
        arguments << QStringLiteral("-mmt=%1").arg(int(value));
    if (sevenZip && number("volumeSize", 65536, 1e15, &value))
        arguments << QStringLiteral("-v%1b").arg(qint64(value));
    if (sevenZip)
        arguments << "-snl"; // links are stored as links in 7z, as their contents in ZIP
    for (const auto &pattern : options.value("exclude").toArray()) {
        const QString text = pattern.toString();
        if (!text.isEmpty() && !text.contains(u'\n'))
            arguments << "-xr!" + text;
    }
    std::optional<QString> password;
    if (request.hasPassword && !request.password.isEmpty()) {
        password = request.password;
        arguments << "-p";
        if (sevenZip && options.value("encryptNames").toBool(true))
            arguments << "-mhe=on";
        if (!sevenZip)
            arguments << (options.value("zipEncryption").toString("AES256") == "ZipCrypto" ? "-mem=ZipCrypto" : "-mem=AES256");
    }
    QStringList items;
    for (const auto &item : json.value("items").toArray())
        items << QFileInfo(item.toString()).absoluteFilePath();
    ListFile list(items);
    arguments << temporaryPath << list.argument();
    const Run result = run(arguments, passwordInput(password, 2), {}, [&](const QByteArray &token) {
        const double fraction = percent(token);
        if (fraction >= 0)
            channel.progress({{"phase", "create"}, {"fraction", fraction}});
    });
    if (result.cancelled)
        return {{"event", "done"}, {"ok", false}, {"cancelled", true}};
    if (result.exitCode != 0 && result.exitCode != 1)
        return {{"event", "done"}, {"ok", false}, {"error", errorOf(result, false)}};
    if (result.exitCode == 1) {
        for (const auto &line : result.errors.split(u'\n')) {
            if (!line.trimmed().isEmpty() && !line.startsWith(u"Scanning"))
                warn("create", {}, line.trimmed());
        }
    }
    return {{"event", "done"}, {"ok", true}};
}

}
