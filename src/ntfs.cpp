#include "ntfs.h"
#include "files.h"
#include <QDir>
#include <QFileInfo>
#include <algorithm>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

namespace nook::ntfs {
namespace {
constexpr std::uint64_t kRecordMask = 0xFFFFFFFFFFFFull;
constexpr std::uint64_t kRoot = 5, kExtend = 11, kFirstUser = 16;
constexpr std::uint32_t kStandardInformation = 0x10, kAttributeList = 0x20, kFileName = 0x30, kData = 0x80,
                        kEnd = 0xFFFFFFFF;
constexpr std::uint8_t kDosNamespace = 2;

template <typename T> T get(const char *p) {
    T value;
    std::memcpy(&value, p, sizeof value);
    return value;
}
QString utf16(const char *p, int units) {
    QString text(units, Qt::Uninitialized);
    std::memcpy(text.data(), p, units * 2);
    return text;
}
bool fixupRecord(char *data, qint64 size) {
    if (size < 512 || std::memcmp(data, "FILE", 4) != 0)
        return false;
    const auto offset = get<std::uint16_t>(data + 4), count = get<std::uint16_t>(data + 6);
    if (count != size / 512 + 1 || offset < 8 || offset % 2 || offset + count * 2 > size)
        return false;
    const auto check = get<std::uint16_t>(data + offset);
    for (int i = 1; i < count; ++i) {
        char *tail = data + i * 512 - 2;
        if (get<std::uint16_t>(tail) != check)
            return false;
        std::memcpy(tail, data + offset + i * 2, 2);
    }
    return true;
}
struct Header {
    std::uint16_t seq = 0, flags = 0;
    int first = 0, used = 0;
    std::uint64_t base = 0;
};
bool header(const char *data, qint64 size, Header &h) {
    h.seq = get<std::uint16_t>(data + 0x10);
    h.first = get<std::uint16_t>(data + 0x14);
    h.flags = get<std::uint16_t>(data + 0x16);
    h.used = int(std::min<std::uint32_t>(get<std::uint32_t>(data + 0x18), std::uint32_t(size)));
    h.base = get<std::uint64_t>(data + 0x20) & kRecordMask;
    return h.first >= 0x18 && h.first + 8 <= h.used;
}
// Calls visit(type, name, attribute, length) for each well-formed attribute in the record.
template <typename Visit> void eachAttribute(const char *data, const Header &h, Visit &&visit) {
    int offset = h.first;
    while (offset + 16 <= h.used) {
        const auto type = get<std::uint32_t>(data + offset);
        if (type == kEnd)
            break;
        const auto length = get<std::uint32_t>(data + offset + 4);
        if (length < 16 || length % 8 || offset + qint64(length) > h.used)
            break;
        const char *attribute = data + offset;
        const int nameUnits = std::uint8_t(attribute[9]);
        const int nameOffset = get<std::uint16_t>(attribute + 10);
        QString name;
        if (nameUnits && nameOffset + nameUnits * 2 <= int(length))
            name = utf16(attribute + nameOffset, nameUnits);
        visit(type, name, attribute, int(length));
        offset += int(length);
    }
}
// Resident value of an attribute, or nullptr.
const char *residentValue(const char *attribute, int length, int &valueLength) {
    if (attribute[8] != 0 || length < 0x18)
        return nullptr;
    valueLength = int(get<std::uint32_t>(attribute + 0x10));
    const int offset = get<std::uint16_t>(attribute + 0x14);
    if (offset < 0x18 || valueLength < 0 || offset + qint64(valueLength) > length)
        return nullptr;
    return attribute + offset;
}
bool nonResident(const char *attribute, int length) {
    return attribute[8] == 1 && length >= 0x40;
}
struct ParsedName {
    std::uint64_t parent = 0;
    std::uint8_t space = 0;
    QString text;
};
bool parseFileName(const char *attribute, int length, ParsedName &out) {
    int valueLength = 0;
    const char *value = residentValue(attribute, length, valueLength);
    if (!value || valueLength < 0x42)
        return false;
    const int units = std::uint8_t(value[0x40]);
    if (!units || 0x42 + units * 2 > valueLength)
        return false;
    out.parent = get<std::uint64_t>(value);
    out.space = std::uint8_t(value[0x41]);
    out.text = utf16(value + 0x42, units);
    // A name never contains '/' or NUL on a healthy volume; refuse paths that would.
    return !out.text.contains(QLatin1Char('/')) && !out.text.contains(QChar(0)) && out.text != "." &&
           out.text != "..";
}
} // namespace

qint64 unixTime(std::uint64_t fileTime) {
    return fileTime ? qint64(fileTime / 10000000ull) - 11644473600LL : 0;
}

bool decodeRuns(const char *data, int length, std::int64_t firstVcn, std::vector<Run> &runs) {
    int pos = 0;
    std::int64_t vcn = firstVcn, lcn = 0;
    while (pos < length) {
        const auto head = std::uint8_t(data[pos]);
        if (!head)
            return true;
        const int lengthBytes = head & 0x0F, offsetBytes = head >> 4;
        if (!lengthBytes || lengthBytes > 8 || offsetBytes > 8 || pos + 1 + lengthBytes + offsetBytes > length)
            return false;
        std::uint64_t count = 0;
        for (int i = lengthBytes - 1; i >= 0; --i)
            count = (count << 8) | std::uint8_t(data[pos + 1 + i]);
        if (!count || count > (1ull << 48))
            return false;
        bool sparse = offsetBytes == 0;
        if (!sparse) {
            std::int64_t delta = std::int8_t(data[pos + lengthBytes + offsetBytes]); // sign of the top byte
            for (int i = offsetBytes - 2; i >= 0; --i)
                delta = delta * 256 + std::uint8_t(data[pos + 1 + lengthBytes + i]);
            lcn += delta;
            if (lcn < 0)
                return false;
        }
        runs.push_back({vcn, sparse ? -1 : lcn, std::int64_t(count)});
        vcn += std::int64_t(count);
        pos += 1 + lengthBytes + offsetBytes;
    }
    return true;
}

Volume::Volume(int fd) : m_fd(fd) {}
Volume::~Volume() {
    if (m_fd >= 0)
        ::close(m_fd);
}

bool Volume::read(qint64 offset, void *buffer, qint64 length) const {
    auto *out = static_cast<char *>(buffer);
    while (length > 0) {
        const auto got = ::pread(m_fd, out, size_t(length), off_t(offset));
        if (got < 0 && errno == EINTR)
            continue;
        if (got <= 0)
            return false;
        out += got;
        offset += got;
        length -= got;
    }
    return true;
}

bool Volume::readStream(const std::vector<Run> &runs, qint64 size, qint64 offset, char *buffer,
                        qint64 length) const {
    if (offset < 0 || length < 0 || offset + length > size)
        return false;
    while (length > 0) {
        const std::int64_t vcn = offset / m_clusterSize;
        auto run = std::upper_bound(runs.begin(), runs.end(), vcn,
                                    [](std::int64_t v, const Run &r) { return v < r.vcn; });
        if (run == runs.begin())
            return false;
        --run;
        if (vcn >= run->vcn + run->length)
            return false;
        const qint64 within = offset % m_clusterSize;
        const qint64 chunk = std::min(length, (run->vcn + run->length - vcn) * m_clusterSize - within);
        if (run->lcn < 0)
            std::memset(buffer, 0, size_t(chunk));
        else if (!read((run->lcn + (vcn - run->vcn)) * m_clusterSize + within, buffer, chunk))
            return false;
        buffer += chunk;
        offset += chunk;
        length -= chunk;
    }
    return true;
}

bool Volume::fixup(QByteArray &record) const {
    return fixupRecord(record.data(), record.size());
}

bool Volume::readRecord(std::uint64_t record, QByteArray &buffer) const {
    if (record >= m_records)
        return false;
    buffer.resize(m_recordSize);
    return readStream(m_mftRuns, m_mftSize, qint64(record) * m_recordSize, buffer.data(), m_recordSize) &&
           fixup(buffer);
}

bool Volume::open(QString *error) {
    char boot[512];
    auto fail = [error](const QString &message) {
        if (error)
            *error = message;
        return false;
    };
    if (!read(0, boot, sizeof boot) || std::memcmp(boot + 3, "NTFS    ", 8) != 0)
        return fail(QStringLiteral("不是 NTFS 卷"));
    const int sectorSize = get<std::uint16_t>(boot + 0x0B);
    const int rawCluster = std::uint8_t(boot[0x0D]);
    const qint64 sectorsPerCluster = rawCluster <= 0x80 ? rawCluster : (rawCluster >= 0xF4 ? 1LL << (256 - rawCluster) : 0);
    if (sectorSize < 256 || sectorSize > 4096 || (sectorSize & (sectorSize - 1)) || !sectorsPerCluster)
        return fail(QStringLiteral("NTFS 引导扇区无效"));
    m_clusterSize = sectorSize * sectorsPerCluster;
    const auto perRecord = std::int8_t(boot[0x40]);
    m_recordSize = perRecord > 0 ? perRecord * m_clusterSize : (perRecord <= -9 && perRecord >= -16 ? 1LL << -perRecord : 0);
    const auto mftCluster = get<std::uint64_t>(boot + 0x30);
    if (m_clusterSize > (2 << 20) || m_recordSize < 512 || m_recordSize > 65536 || m_recordSize % 512 ||
        mftCluster > (1ull << 48))
        return fail(QStringLiteral("NTFS 引导扇区无效"));
    // Bootstrap: $MFT describes itself in record 0.
    QByteArray first(m_recordSize, Qt::Uninitialized);
    Header h;
    if (!read(qint64(mftCluster) * m_clusterSize, first.data(), m_recordSize) || !fixup(first) ||
        !header(first.constData(), m_recordSize, h))
        return fail(QStringLiteral("无法读取 $MFT"));
    eachAttribute(first.constData(), h, [&](std::uint32_t type, const QString &name, const char *a, int length) {
        if (type == kData && name.isEmpty() && nonResident(a, length)) {
            const auto start = get<std::int64_t>(a + 0x10);
            const int runs = get<std::uint16_t>(a + 0x20);
            if (runs < length && decodeRuns(a + runs, length - runs, start, m_mftRuns) && start == 0)
                m_mftSize = get<std::int64_t>(a + 0x30);
        }
    });
    std::sort(m_mftRuns.begin(), m_mftRuns.end(), [](const Run &a, const Run &b) { return a.vcn < b.vcn; });
    m_records = m_mftSize > 0 ? std::uint64_t(m_mftSize / m_recordSize) : 0;
    if (m_mftRuns.empty() || !m_records)
        return fail(QStringLiteral("无法读取 $MFT"));
    // A very fragmented $MFT continues in extension records listed in $ATTRIBUTE_LIST.
    std::vector<Run> all;
    qint64 size = 0;
    if (streamRuns(0, kData, {}, all, size, nullptr) && !all.empty() && size >= m_mftSize) {
        m_mftRuns = std::move(all);
        m_mftSize = size;
        m_records = std::uint64_t(m_mftSize / m_recordSize);
    }
    return true;
}

bool Volume::attributes(std::uint64_t record,
                        const std::function<void(std::uint32_t, const QString &, const char *, int)> &visit) {
    QByteArray base;
    Header h;
    if (!readRecord(record, base) || !header(base.constData(), base.size(), h))
        return false;
    std::vector<std::uint64_t> extensions;
    eachAttribute(base.constData(), h, [&](std::uint32_t type, const QString &name, const char *a, int length) {
        if (type != kAttributeList) {
            visit(type, name, a, length);
            return;
        }
        QByteArray list;
        int valueLength = 0;
        if (const char *value = residentValue(a, length, valueLength))
            list = QByteArray(value, valueLength);
        else if (nonResident(a, length)) {
            std::vector<Run> runs;
            const int at = get<std::uint16_t>(a + 0x20);
            const auto size = get<std::int64_t>(a + 0x30);
            if (at < length && size > 0 && size <= (16 << 20) && decodeRuns(a + at, length - at, 0, runs)) {
                list.resize(size);
                if (!readStream(runs, size, 0, list.data(), size))
                    list.clear();
            }
        }
        for (int at = 0; at + 0x1A <= list.size();) {
            const int entryLength = get<std::uint16_t>(list.constData() + at + 4);
            if (entryLength < 0x1A || at + entryLength > list.size())
                break;
            const auto target = get<std::uint64_t>(list.constData() + at + 0x10) & kRecordMask;
            if (target != record && extensions.size() < 4096 &&
                std::find(extensions.begin(), extensions.end(), target) == extensions.end())
                extensions.push_back(target);
            at += (entryLength + 7) & ~7;
        }
    });
    for (const auto extension : extensions) {
        QByteArray data;
        Header eh;
        if (!readRecord(extension, data) || !header(data.constData(), data.size(), eh) || eh.base != record)
            continue;
        eachAttribute(data.constData(), eh, [&](std::uint32_t type, const QString &name, const char *a, int length) {
            if (type != kAttributeList)
                visit(type, name, a, length);
        });
    }
    return true;
}

bool Volume::streamRuns(std::uint64_t record, std::uint32_t wanted, const QString &wantedName,
                        std::vector<Run> &runs, qint64 &size, QByteArray *resident) {
    bool found = false;
    size = 0;
    const bool ok = attributes(record, [&](std::uint32_t type, const QString &name, const char *a, int length) {
        if (type != wanted || name != wantedName)
            return;
        int valueLength = 0;
        if (const char *value = residentValue(a, length, valueLength)) {
            if (resident)
                *resident = QByteArray(value, valueLength);
            size = valueLength;
            found = true;
        } else if (nonResident(a, length)) {
            const auto start = get<std::int64_t>(a + 0x10);
            const int at = get<std::uint16_t>(a + 0x20);
            if (at < length && decodeRuns(a + at, length - at, start, runs)) {
                found = true;
                if (start == 0)
                    size = get<std::int64_t>(a + 0x30);
            }
        }
    });
    std::sort(runs.begin(), runs.end(), [](const Run &a, const Run &b) { return a.vcn < b.vcn; });
    return ok && found && size >= 0;
}

Volume::Info Volume::info(std::uint64_t record) {
    Info result;
    QByteArray data;
    Header h;
    if (!readRecord(record, data) || !header(data.constData(), data.size(), h) || h.base)
        return result;
    result.valid = true;
    result.seq = h.seq;
    result.inUse = h.flags & 1;
    result.folder = h.flags & 2;
    if (!result.inUse)
        return result;
    attributes(record, [&](std::uint32_t type, const QString &name, const char *a, int length) {
        int valueLength = 0;
        if (type == kStandardInformation) {
            if (const char *value = residentValue(a, length, valueLength); value && valueLength >= 0x10)
                result.mtime = unixTime(get<std::uint64_t>(value + 0x08));
        } else if (type == kFileName) {
            ParsedName parsed;
            if (parseFileName(a, length, parsed))
                result.names.push_back({parsed.parent & kRecordMask, std::uint16_t(parsed.parent >> 48),
                                        parsed.space, parsed.text});
        } else if (type == kData && name.isEmpty()) {
            if (residentValue(a, length, valueLength))
                result.size = valueLength;
            else if (nonResident(a, length) && get<std::int64_t>(a + 0x10) == 0)
                result.size = get<std::int64_t>(a + 0x30);
        }
    });
    return result;
}

bool Volume::directoryPath(std::uint64_t record, std::uint16_t seq, QString &path, int depth) {
    if (record == kRoot) {
        path.clear();
        return true;
    }
    if (record < kFirstUser || depth > 255)
        return false;
    if (auto cached = m_dirCache.find(record); cached != m_dirCache.end()) {
        // A parent reference names a sequence number: a reused record is a different folder.
        if (seq && cached->second.first != seq)
            return false;
        path = cached->second.second;
        return !path.isNull();
    }
    const Info folder = info(record);
    const Name *name = nullptr;
    for (const auto &candidate : folder.names)
        if (candidate.space != kDosNamespace) {
            name = &candidate;
            break;
        }
    QString parent;
    bool ok = folder.inUse && folder.folder && (!seq || folder.seq == seq) && name && name->parent != record &&
              directoryPath(name->parent, name->parentSeq, parent, depth + 1) &&
              !(name->parent == kRoot && windowsSystemEntry(name->text.toUtf8()));
    path = ok ? (parent.isEmpty() ? name->text : parent + '/' + name->text) : QString();
    m_dirCache[record] = {folder.seq, path};
    return ok;
}

std::vector<Entry> Volume::current(std::uint64_t record) {
    std::vector<Entry> out;
    if (record < kFirstUser)
        return out;
    const Info item = info(record);
    if (!item.inUse)
        return out;
    for (const auto &name : item.names) {
        if (name.space == kDosNamespace)
            continue;
        if (name.parent == kRoot && windowsSystemEntry(name.text.toUtf8()))
            continue;
        QString parent;
        if (!directoryPath(name.parent, name.parentSeq, parent))
            continue;
        out.push_back({parent.isEmpty() ? name.text : parent + '/' + name.text, record, item.folder,
                       item.folder ? 0 : item.size, item.mtime});
    }
    return out;
}

bool Volume::enumerate(const std::function<void(const Entry &)> &visit, Journal &journal, const std::atomic_bool *stop,
                       QString *error) {
    const std::uint64_t n = m_records;
    if (n > (1ull << 32)) {
        if (error)
            *error = QStringLiteral("$MFT 过大");
        return false;
    }
    std::vector<std::uint16_t> seq(n);
    std::vector<std::uint8_t> flags(n); // 1 in use, 2 folder
    std::vector<qint64> sizes(n), times(n);
    // Names share one UTF-16 pool: a QString per name would triple the memory on a big drive.
    struct RawName {
        std::uint64_t parent;
        std::uint32_t target, offset;
        std::uint8_t length, space;
    };
    std::vector<RawName> names;
    std::vector<char16_t> pool;
    auto text = [&pool](const RawName &name) {
        return QString(reinterpret_cast<const QChar *>(pool.data() + name.offset), name.length);
    };
    const qint64 perChunk = std::max<qint64>(1, (4 << 20) / m_recordSize);
    QByteArray chunk;
    for (std::uint64_t start = 0; start < n; start += std::uint64_t(perChunk)) {
        if (stop && *stop)
            return false;
        const qint64 count = std::min<qint64>(perChunk, qint64(n - start));
        chunk.resize(count * m_recordSize);
        if (!readStream(m_mftRuns, m_mftSize, qint64(start) * m_recordSize, chunk.data(), chunk.size())) {
            if (error)
                *error = QStringLiteral("读取 $MFT 失败");
            return false;
        }
        for (qint64 i = 0; i < count; ++i) {
            char *data = chunk.data() + i * m_recordSize;
            Header h;
            if (!fixupRecord(data, m_recordSize) || !header(data, m_recordSize, h) || !(h.flags & 1))
                continue;
            const std::uint64_t record = start + std::uint64_t(i);
            const std::uint64_t target = h.base ? h.base : record;
            if (target >= n)
                continue;
            if (!h.base) {
                seq[record] = h.seq;
                flags[record] = std::uint8_t(1 | ((h.flags & 2) ? 2 : 0));
            }
            eachAttribute(data, h, [&](std::uint32_t type, const QString &name, const char *a, int length) {
                int valueLength = 0;
                if (type == kStandardInformation) {
                    if (const char *value = residentValue(a, length, valueLength); value && valueLength >= 0x10)
                        times[target] = unixTime(get<std::uint64_t>(value + 0x08));
                } else if (type == kFileName) {
                    ParsedName parsed;
                    if (parseFileName(a, length, parsed) && pool.size() < (1ull << 32) - 256) {
                        names.push_back({parsed.parent, std::uint32_t(target), std::uint32_t(pool.size()),
                                         std::uint8_t(parsed.text.size()), parsed.space});
                        pool.insert(pool.end(), reinterpret_cast<const char16_t *>(parsed.text.utf16()),
                                    reinterpret_cast<const char16_t *>(parsed.text.utf16()) + parsed.text.size());
                    }
                } else if (type == kData && name.isEmpty()) {
                    if (residentValue(a, length, valueLength))
                        sizes[target] = valueLength;
                    else if (nonResident(a, length) && get<std::int64_t>(a + 0x10) == 0)
                        sizes[target] = get<std::int64_t>(a + 0x30);
                }
            });
        }
    }
    // Folder paths, resolved once each through the parent references.
    std::vector<std::int64_t> primary(n, -1);
    for (size_t i = 0; i < names.size(); ++i) {
        const auto &name = names[i];
        if (name.space != kDosNamespace && (flags[name.target] & 2) && primary[name.target] < 0)
            primary[name.target] = std::int64_t(i);
        if ((name.parent & kRecordMask) == kExtend && text(name) == QLatin1String("$UsnJrnl"))
            journal.record = name.target;
    }
    std::vector<std::uint8_t> state(n); // 0 unknown, 1 resolved, 2 unusable
    std::vector<QString> paths(n);
    auto valid = [&](std::uint64_t parentRef) {
        const auto parent = parentRef & kRecordMask;
        const auto parentSeq = std::uint16_t(parentRef >> 48);
        return parent < n && (flags[parent] & 3) == 3 && (!parentSeq || seq[parent] == parentSeq);
    };
    auto resolve = [&](std::uint64_t folder) -> bool {
        std::vector<std::uint64_t> chain;
        std::uint64_t at = folder;
        while (at != kRoot && state[at] == 0) {
            if (at < kFirstUser || primary[at] < 0 || chain.size() > 255) {
                state[at] = 2;
                break;
            }
            chain.push_back(at);
            const auto parentRef = names[size_t(primary[at])].parent;
            if (!valid(parentRef)) {
                state[at] = 2;
                break;
            }
            at = parentRef & kRecordMask;
            if (std::find(chain.begin(), chain.end(), at) != chain.end()) {
                state[at] = 2;
                break;
            }
        }
        bool ok = at == kRoot || state[at] == 1;
        QString base = at == kRoot ? QString() : paths[at];
        for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
            if (state[*it] != 0) {
                ok = state[*it] == 1;
                base = paths[*it];
                continue;
            }
            const auto &name = names[size_t(primary[*it])];
            const QString part = text(name);
            if (ok && (name.parent & kRecordMask) == kRoot && windowsSystemEntry(part.toUtf8()))
                ok = false;
            if (ok)
                base = base.isEmpty() ? part : base + '/' + part;
            state[*it] = ok ? 1 : 2;
            if (ok)
                paths[*it] = base;
        }
        return folder == kRoot || state[folder] == 1;
    };
    for (const auto &name : names) {
        if (stop && *stop)
            return false;
        if (name.space == kDosNamespace || name.target < kFirstUser || !(flags[name.target] & 1) || !valid(name.parent))
            continue;
        const auto parent = name.parent & kRecordMask;
        const QString part = text(name);
        if (parent == kRoot ? windowsSystemEntry(part.toUtf8()) : !resolve(parent))
            continue;
        const QString &base = parent == kRoot ? QString() : paths[parent];
        const bool folder = flags[name.target] & 2;
        visit({base.isEmpty() ? part : base + '/' + part, name.target, folder, folder ? 0 : sizes[name.target],
               times[name.target]});
    }
    if (journal.record) {
        QString ignored;
        if (!this->journal(journal.record, journal, &ignored))
            journal = {};
    }
    return true;
}

bool Volume::journal(std::uint64_t record, Journal &out, QString *error) {
    out = {};
    const Info item = info(record);
    bool named = false;
    for (const auto &name : item.names)
        named = named || (name.parent == kExtend && name.text == QLatin1String("$UsnJrnl"));
    std::vector<Run> runs;
    qint64 size = 0;
    QByteArray max;
    if (!item.inUse || !named || !streamRuns(record, kData, QStringLiteral("$Max"), runs, size, &max) ||
        max.size() < 0x20) {
        if (error)
            *error = QStringLiteral("此卷没有启用 USN 变更日志");
        return false;
    }
    runs.clear();
    qint64 end = 0;
    if (!streamRuns(record, kData, QStringLiteral("$J"), runs, end, nullptr)) {
        if (error)
            *error = QStringLiteral("无法读取 USN 变更日志");
        return false;
    }
    // $Max: MaximumSize, AllocationDelta, UsnJournalID, LowestValidUsn.
    std::uint64_t fields[4] = {};
    std::memcpy(fields, max.constData(), std::min<size_t>(sizeof fields, size_t(max.size())));
    out.present = true;
    out.record = record;
    out.id = fields[2];
    // Windows releases old journal space by making the head of $J sparse; the real oldest
    // record starts at the first allocated cluster (what FSCTL_QUERY_USN_JOURNAL calls FirstUsn).
    qint64 firstAllocated = end;
    for (const auto &run : runs)
        if (run.lcn >= 0) {
            firstAllocated = run.vcn * m_clusterSize;
            break;
        }
    out.lowest = std::min(end, std::max(qint64(fields[3]), firstAllocated));
    out.next = end;
    return true;
}

bool Volume::changes(const Journal &journal, qint64 from, qint64 to, std::vector<Change> &out, QString *error) {
    std::vector<Run> runs;
    qint64 size = 0;
    if (!streamRuns(journal.record, kData, QStringLiteral("$J"), runs, size, nullptr)) {
        if (error)
            *error = QStringLiteral("无法读取 USN 变更日志");
        return false;
    }
    from = std::max(from, journal.lowest);
    to = std::min(to, size);
    constexpr qint64 kPage = 4096, kChunk = 1 << 20;
    QByteArray buffer;
    qint64 pos = from;
    while (pos < to) {
        const qint64 length = std::min(kChunk, to - pos);
        buffer.resize(length);
        if (!readStream(runs, size, pos, buffer.data(), length)) {
            if (error)
                *error = QStringLiteral("读取 USN 变更日志失败");
            return false;
        }
        qint64 at = 0;
        while (at + 8 <= length) {
            const char *r = buffer.constData() + at;
            const auto recordLength = get<std::uint32_t>(r);
            if (recordLength == 0 || recordLength % 8 || recordLength < 0x3C || recordLength > 0x10000) {
                at = ((pos + at) / kPage + 1) * kPage - pos; // padding or damage: next page
                continue;
            }
            if (at + recordLength > length) {
                if (pos + at + recordLength > to)
                    at = length; // incomplete tail
                break;           // re-read from this record
            }
            const auto major = get<std::uint16_t>(r + 4);
            Change change;
            int nameLength = 0, nameOffset = 0;
            if (major == 2) {
                change.record = get<std::uint64_t>(r + 8) & kRecordMask;
                change.parent = get<std::uint64_t>(r + 16) & kRecordMask;
                change.reason = get<std::uint32_t>(r + 40);
                nameLength = get<std::uint16_t>(r + 56);
                nameOffset = get<std::uint16_t>(r + 58);
            } else if (major == 3 && recordLength >= 0x4C) {
                change.record = get<std::uint64_t>(r + 8) & kRecordMask;
                change.parent = get<std::uint64_t>(r + 24) & kRecordMask;
                change.reason = get<std::uint32_t>(r + 56);
                nameLength = get<std::uint16_t>(r + 72);
                nameOffset = get<std::uint16_t>(r + 74);
            } else {
                at += recordLength;
                continue;
            }
            if (nameLength % 2 == 0 && nameOffset + nameLength <= int(recordLength))
                change.name = utf16(r + nameOffset, nameLength / 2);
            out.push_back(std::move(change));
            at += recordLength;
        }
        if (at <= 0)
            return false; // cannot make progress
        pos += at;
    }
    return true;
}

QString deviceUuid(const QString &device) {
    const QString target = QFileInfo(device).canonicalFilePath();
    if (target.isEmpty())
        return {};
    for (const auto &link : QDir(QStringLiteral("/dev/disk/by-uuid")).entryInfoList(QDir::System | QDir::Files))
        if (link.canonicalFilePath() == target)
            return link.fileName();
    return {};
}

int openVolume(const QString &uuid, QString *error) {
    auto fail = [error](const QString &message) {
        if (error)
            *error = message;
        return -1;
    };
    static const char *helper = "/usr/lib/nook/nook-volume-helper";
    if (::access(helper, X_OK) != 0)
        return fail(QStringLiteral("未安装 NTFS 读取助手（需通过软件包安装）"));
    int channel[2], diagnostics[2];
    if (::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, channel) != 0)
        return fail(QString::fromLocal8Bit(strerror(errno)));
    if (::pipe2(diagnostics, O_CLOEXEC) != 0) {
        ::close(channel[0]);
        ::close(channel[1]);
        return fail(QString::fromLocal8Bit(strerror(errno)));
    }
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_addopen(&actions, 0, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_adddup2(&actions, channel[1], 1); // the helper writes the descriptor here
    posix_spawn_file_actions_adddup2(&actions, diagnostics[1], 2);
    const QByteArray id = uuid.toLatin1();
    const char *argv[] = {"/usr/bin/pkexec", "--disable-internal-agent", helper, id.constData(), nullptr};
    pid_t pid = 0;
    const int spawned = posix_spawn(&pid, argv[0], &actions, nullptr, const_cast<char **>(argv), environ);
    posix_spawn_file_actions_destroy(&actions);
    ::close(channel[1]);
    ::close(diagnostics[1]);
    if (spawned != 0) {
        ::close(channel[0]);
        ::close(diagnostics[0]);
        return fail(QStringLiteral("无法启动 pkexec"));
    }
    int fd = -1;
    // Up to two minutes: the first use may wait for a password.
    pollfd ready{channel[0], POLLIN, 0};
    if (::poll(&ready, 1, 120000) == 1) {
        char greeting[32] = {};
        iovec part{greeting, sizeof greeting - 1};
        alignas(cmsghdr) char control[CMSG_SPACE(sizeof(int))] = {};
        msghdr message{};
        message.msg_iov = &part;
        message.msg_iovlen = 1;
        message.msg_control = control;
        message.msg_controllen = sizeof control;
        constexpr char kVolumeGreeting[] = "NOOK-VOLUME 1\n";
        const auto received = ::recvmsg(channel[0], &message, MSG_CMSG_CLOEXEC);
        if (received > 0)
            for (auto *c = CMSG_FIRSTHDR(&message); c; c = CMSG_NXTHDR(&message, c))
                if (c->cmsg_level == SOL_SOCKET && c->cmsg_type == SCM_RIGHTS && c->cmsg_len == CMSG_LEN(sizeof(int)))
                    std::memcpy(&fd, CMSG_DATA(c), sizeof fd);
        if (fd >= 0 && (received != sizeof(kVolumeGreeting) - 1 ||
                        std::memcmp(greeting, kVolumeGreeting, sizeof(kVolumeGreeting) - 1) != 0)) {
            ::close(fd);
            fd = -1;
        }
    } else {
        ::kill(pid, SIGTERM);
    }
    ::close(channel[0]);
    int status = 0;
    while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) {
    }
    char reason[512] = {};
    const auto got = ::read(diagnostics[0], reason, sizeof reason - 1);
    ::close(diagnostics[0]);
    if (fd >= 0)
        return fd;
    const int code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    if (code == 126 || code == 127)
        return fail(QStringLiteral("未获得读取 NTFS 分区的授权"));
    return fail(got > 0 ? QString::fromLocal8Bit(reason, got).trimmed() : QStringLiteral("NTFS 读取助手没有返回分区"));
}

} // namespace nook::ntfs
