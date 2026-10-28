#pragma once
#include <QByteArray>
#include <QString>
#include <atomic>
#include <cstdint>
#include <functional>
#include <unordered_map>
#include <vector>

// Read-only NTFS metadata reader, the way Everything indexes Windows drives: the whole file
// table ($MFT) is read sequentially once, and afterwards only the change journal
// ($Extend/$UsnJrnl:$J) is consulted. It runs unprivileged on a file descriptor handed over by
// nook-volume-helper, and treats every on-disk byte as untrusted (all reads are bounds-checked).
namespace nook::ntfs {

struct Entry {
    QString path;              // relative to the volume root, '/'-separated, no leading slash
    std::uint64_t record = 0;  // MFT record number (what ntfs3 reports as the inode number)
    bool folder = false;
    qint64 size = 0, mtime = 0; // mtime in Unix seconds
};

struct Journal {
    bool present = false;
    std::uint64_t record = 0; // MFT record of $UsnJrnl
    std::uint64_t id = 0;     // changes whenever the journal is recreated
    qint64 lowest = 0;        // oldest USN still stored (Windows frees old journal space)
    qint64 next = 0;          // USN the next record will get (end of $J)
};

struct Change {
    std::uint64_t record = 0, parent = 0;
    std::uint32_t reason = 0;
    QString name;
};

struct Run {
    std::int64_t vcn = 0, lcn = -1, length = 0; // lcn < 0: sparse
};

class Volume {
  public:
    // Takes ownership of fd (a block device or image opened read-only).
    explicit Volume(int fd);
    ~Volume();
    Volume(const Volume &) = delete;
    Volume &operator=(const Volume &) = delete;

    bool open(QString *error);
    // Every in-use file and folder, with Windows system areas and metafiles left out, handed to
    // visit() one by one (nothing per-entry is kept, so a million-file drive stays small).
    bool enumerate(const std::function<void(const Entry &)> &visit, Journal &journal, const std::atomic_bool *stop,
                   QString *error);
    // Journal state, given the record found by a previous enumerate().
    bool journal(std::uint64_t record, Journal &out, QString *error);
    // Journal records with USN in [from, to).
    bool changes(const Journal &journal, qint64 from, qint64 to, std::vector<Change> &out, QString *error);
    // The record's current names (one per hard link), read from disk; empty if gone or excluded.
    std::vector<Entry> current(std::uint64_t record);

    std::uint64_t recordCount() const { return m_records; }

  private:
    struct Name {
        std::uint64_t parent = 0;
        std::uint16_t parentSeq = 0;
        std::uint8_t space = 0;
        QString text;
    };
    struct Info {
        bool valid = false, inUse = false, folder = false;
        std::uint16_t seq = 0;
        qint64 size = 0, mtime = 0;
        std::vector<Name> names;
    };
    bool read(qint64 offset, void *buffer, qint64 length) const;
    bool readStream(const std::vector<Run> &runs, qint64 size, qint64 offset, char *buffer, qint64 length) const;
    bool readRecord(std::uint64_t record, QByteArray &buffer) const;
    bool fixup(QByteArray &record) const;
    // Calls visit(type, name, attribute bytes) for every attribute of the record, following
    // $ATTRIBUTE_LIST into extension records.
    bool attributes(std::uint64_t record, const std::function<void(std::uint32_t, const QString &, const char *, int)> &visit);
    bool streamRuns(std::uint64_t record, std::uint32_t type, const QString &name, std::vector<Run> &runs,
                    qint64 &size, QByteArray *resident);
    Info info(std::uint64_t record);
    bool directoryPath(std::uint64_t record, std::uint16_t seq, QString &path, int depth = 0);

    int m_fd = -1;
    qint64 m_clusterSize = 0, m_recordSize = 0, m_mftSize = 0;
    std::uint64_t m_records = 0;
    std::vector<Run> m_mftRuns;
    std::unordered_map<std::uint64_t, std::pair<std::uint16_t, QString>> m_dirCache; // record -> (seq, path)
};

// Asks nook-volume-helper (through pkexec) for a read-only descriptor of the NTFS partition
// with this UUID, received over a socket. Blocks until the helper answers (or a password prompt
// is dismissed); returns -1 with a reason on failure.
int openVolume(const QString &uuid, QString *error);
// UUID under /dev/disk/by-uuid of a block device such as /dev/nvme0n1p4, or empty.
QString deviceUuid(const QString &device);

// Mapping pairs of a non-resident attribute.
bool decodeRuns(const char *data, int length, std::int64_t firstVcn, std::vector<Run> &runs);
qint64 unixTime(std::uint64_t fileTime);

} // namespace nook::ntfs
