#pragma once
// Builds a small, valid NTFS image in memory for tests: 4 KiB clusters, 1 KiB records, a $MFT in
// two runs, an attribute list with an extension record, hard links, DOS names and a USN journal.
// Variant 1 is the same volume after a Windows session: renames, a deletion, a new file, and the
// matching journal records on a later page.
#include <QByteArray>
#include <QString>
#include <cstdint>
#include <cstring>
#include <utility>
#include <vector>

namespace testimage {
constexpr qint64 kCluster = 4096, kRecord = 1024, kUnixTime = 1700000000;
constexpr std::uint64_t kFileTime = std::uint64_t(kUnixTime + 11644473600LL) * 10000000ull;

inline int align8(int n) { return (n + 7) & ~7; }
template <typename T> void put(QByteArray &b, int at, T value) { std::memcpy(b.data() + at, &value, sizeof value); }
inline QByteArray wide(const QString &text) {
    return QByteArray(reinterpret_cast<const char *>(text.utf16()), text.size() * 2);
}
inline std::uint64_t ref(std::uint64_t record, std::uint64_t seq = 1) { return record | (seq << 48); }

inline QByteArray runs(const std::vector<std::pair<std::int64_t, std::int64_t>> &list) {
    QByteArray out;
    std::int64_t previous = 0;
    auto bytesFor = [](std::int64_t v, bool isSigned) {
        int n = 1;
        while (n < 8) {
            const std::int64_t lo = isSigned ? -(1LL << (8 * n - 1)) : 0, hi = isSigned ? (1LL << (8 * n - 1)) - 1 : (1LL << (8 * n)) - 1;
            if (v >= lo && v <= hi)
                break;
            ++n;
        }
        return n;
    };
    for (const auto &[length, lcn] : list) {
        const int lengthBytes = bytesFor(length, false);
        const std::int64_t delta = lcn < 0 ? 0 : lcn - previous;
        const int offsetBytes = lcn < 0 ? 0 : bytesFor(delta, true);
        out.append(char((offsetBytes << 4) | lengthBytes));
        for (int i = 0; i < lengthBytes; ++i)
            out.append(char((length >> (8 * i)) & 0xFF));
        for (int i = 0; i < offsetBytes; ++i)
            out.append(char((delta >> (8 * i)) & 0xFF));
        if (lcn >= 0)
            previous = lcn;
    }
    out.append('\0');
    return out;
}

inline QByteArray resident(std::uint32_t type, const QString &name, const QByteArray &value) {
    const QByteArray n = wide(name);
    const int valueOffset = align8(0x18 + n.size());
    QByteArray a(align8(valueOffset + value.size()), '\0');
    put<std::uint32_t>(a, 0, type);
    put<std::uint32_t>(a, 4, a.size());
    a[9] = char(name.size());
    put<std::uint16_t>(a, 10, 0x18);
    put<std::uint32_t>(a, 0x10, value.size());
    put<std::uint16_t>(a, 0x14, valueOffset);
    std::memcpy(a.data() + 0x18, n.constData(), n.size());
    std::memcpy(a.data() + valueOffset, value.constData(), value.size());
    return a;
}

inline QByteArray nonResident(std::uint32_t type, const QString &name, const QByteArray &mapping, std::int64_t clusters,
                              std::int64_t size) {
    const QByteArray n = wide(name);
    const int runOffset = align8(0x40 + n.size());
    QByteArray a(align8(runOffset + mapping.size()), '\0');
    put<std::uint32_t>(a, 0, type);
    put<std::uint32_t>(a, 4, a.size());
    a[8] = 1;
    a[9] = char(name.size());
    put<std::uint16_t>(a, 10, 0x40);
    put<std::int64_t>(a, 0x10, 0);
    put<std::int64_t>(a, 0x18, clusters - 1);
    put<std::uint16_t>(a, 0x20, runOffset);
    put<std::int64_t>(a, 0x28, clusters * kCluster);
    put<std::int64_t>(a, 0x30, size);
    put<std::int64_t>(a, 0x38, size);
    std::memcpy(a.data() + 0x40, n.constData(), n.size());
    std::memcpy(a.data() + runOffset, mapping.constData(), mapping.size());
    return a;
}

inline QByteArray standardInformation() {
    QByteArray v(0x48, '\0');
    for (int at = 0; at < 0x20; at += 8)
        put<std::uint64_t>(v, at, kFileTime);
    return resident(0x10, {}, v);
}

inline QByteArray fileName(std::uint64_t parent, const QString &name, std::uint8_t space = 1, bool folder = false) {
    QByteArray v(0x42 + name.size() * 2, '\0');
    put<std::uint64_t>(v, 0, parent);
    put<std::uint32_t>(v, 0x38, folder ? 0x10000000u : 0u);
    v[0x40] = char(name.size());
    v[0x41] = char(space);
    std::memcpy(v.data() + 0x42, wide(name).constData(), name.size() * 2);
    return resident(0x30, {}, v);
}

// One FILE record with update sequence protection applied.
inline QByteArray record(std::uint16_t seq, std::uint16_t flags, std::uint64_t base, const QByteArray &attributes) {
    QByteArray r(kRecord, '\0');
    std::memcpy(r.data(), "FILE", 4);
    put<std::uint16_t>(r, 4, 0x30);
    put<std::uint16_t>(r, 6, 3);
    put<std::uint16_t>(r, 0x10, seq);
    put<std::uint16_t>(r, 0x12, 1);
    put<std::uint16_t>(r, 0x14, 0x38);
    put<std::uint16_t>(r, 0x16, flags);
    put<std::uint32_t>(r, 0x1C, kRecord);
    put<std::uint64_t>(r, 0x20, base);
    std::memcpy(r.data() + 0x38, attributes.constData(), attributes.size());
    put<std::uint32_t>(r, 0x38 + attributes.size(), 0xFFFFFFFF);
    put<std::uint32_t>(r, 0x18, 0x38 + attributes.size() + 8);
    constexpr std::uint16_t check = 0x0007;
    put<std::uint16_t>(r, 0x30, check);
    for (int i = 1; i <= 2; ++i) {
        std::memcpy(r.data() + 0x30 + i * 2, r.constData() + i * 512 - 2, 2);
        put<std::uint16_t>(r, i * 512 - 2, check);
    }
    return r;
}

inline QByteArray usnV2(std::uint64_t file, std::uint64_t parent, std::uint32_t reason, const QString &name) {
    const QByteArray n = wide(name);
    QByteArray r(align8(0x3C + n.size()), '\0');
    put<std::uint32_t>(r, 0, r.size());
    put<std::uint16_t>(r, 4, 2);
    put<std::uint64_t>(r, 8, file);
    put<std::uint64_t>(r, 16, parent);
    put<std::uint32_t>(r, 40, reason);
    put<std::uint16_t>(r, 56, n.size());
    put<std::uint16_t>(r, 58, 0x3C);
    std::memcpy(r.data() + 0x3C, n.constData(), n.size());
    return r;
}
inline QByteArray usnV3(std::uint64_t file, std::uint64_t parent, std::uint32_t reason, const QString &name) {
    const QByteArray n = wide(name);
    QByteArray r(align8(0x4C + n.size()), '\0');
    put<std::uint32_t>(r, 0, r.size());
    put<std::uint16_t>(r, 4, 3);
    put<std::uint64_t>(r, 8, file);
    put<std::uint64_t>(r, 24, parent);
    put<std::uint32_t>(r, 56, reason);
    put<std::uint16_t>(r, 72, n.size());
    put<std::uint16_t>(r, 74, 0x4C);
    std::memcpy(r.data() + 0x4C, n.constData(), n.size());
    return r;
}

inline QByteArray firstJournalPage() {
    return usnV2(ref(26), ref(25), 0x100, "notes.txt") + usnV2(ref(34), ref(25), 0x200, "gone.txt") +
           usnV3(ref(33), ref(25), 0x8000, "a long name.txt");
}
inline QByteArray secondJournalPage() {
    return usnV2(ref(26), ref(25), 0x2000, "todo.txt") + usnV2(ref(27), ref(25), 0x200, "big.bin") +
           usnV2(ref(38), ref(24), 0x100, "new.docx") + usnV2(ref(25), ref(24), 0x2000, "bob");
}
const qint64 kJournalEnd = 4096 + firstJournalPage().size();
const qint64 kJournalEndAfter = 8192 + secondJournalPage().size();

inline QByteArray build(int variant = 0, std::int64_t recordedLowest = 4096) {
    QByteArray image(128 * kCluster, '\0');
    std::memcpy(image.data() + 3, "NTFS    ", 8);
    put<std::uint16_t>(image, 0x0B, 512);
    image[0x0D] = 8;
    put<std::uint64_t>(image, 0x30, 4);
    image[0x40] = char(-10);
    auto place = [&](int number, const QByteArray &r) {
        const qint64 at = number < 32 ? 4 * kCluster + number * kRecord : 40 * kCluster + (number - 32) * kRecord;
        std::memcpy(image.data() + at, r.constData(), r.size());
    };
    const bool after = variant == 1;
    const auto si = standardInformation();
    place(0, record(1, 1, 0, si + fileName(ref(5, 5), "$MFT", 3) + nonResident(0x80, {}, runs({{8, 4}, {8, 40}}), 16, 64 * kRecord)));
    place(5, record(5, 3, 0, si + fileName(ref(5, 5), ".", 3, true)));
    place(11, record(11, 3, 0, si + fileName(ref(5, 5), "$Extend", 3, true)));
    place(24, record(1, 3, 0, si + fileName(ref(5, 5), "Users", 1, true)));
    place(25, record(1, 3, 0, si + fileName(ref(24), after ? "bob" : "alice", 1, true)));
    place(26, record(1, 1, 0, si + fileName(ref(25), after ? "todo.txt" : "notes.txt") + resident(0x80, {}, "hello")));
    place(27, record(1, after ? 0 : 1, 0, si + fileName(ref(25), "big.bin") + nonResident(0x80, {}, runs({{245, 100}}), 245, 1000000)));
    place(28, record(1, 3, 0, si + fileName(ref(5, 5), "Windows", 1, true)));
    place(29, record(1, 3, 0, si + fileName(ref(28), "system32", 1, true)));
    place(30, record(1, 1, 0, si + fileName(ref(29), "kernel.dll") + resident(0x80, {}, "MZ")));
    place(31, record(1, 3, 0, si + fileName(ref(5, 5), "$Recycle.Bin", 1, true)));
    place(32, record(1, 1, 0, si + fileName(ref(25), "a.txt") + fileName(ref(24), "b.txt") + resident(0x80, {}, "x")));
    place(33, record(1, 1, 0, si + fileName(ref(25), "a long name.txt", 1) + fileName(ref(25), "ALONGN~1.TXT", 2) + resident(0x80, {}, "")));
    place(34, record(1, 0, 0, si + fileName(ref(25), "gone.txt")));
    place(35, record(1, 1, 0, si + fileName(ref(25, 99), "orphan.txt")));
    place(36, record(1, 1, ref(37), fileName(ref(25), "extended.txt")));
    QByteArray list;
    for (const auto &[type, target] : std::vector<std::pair<std::uint32_t, std::uint64_t>>{{0x10, 37}, {0x30, 36}, {0x80, 37}}) {
        QByteArray entry(0x20, '\0');
        put<std::uint32_t>(entry, 0, type);
        put<std::uint16_t>(entry, 4, 0x20);
        entry[7] = 0x1A;
        put<std::uint64_t>(entry, 0x10, ref(target));
        list += entry;
    }
    place(37, record(1, 1, 0, si + resident(0x20, {}, list) + resident(0x80, {}, "ext")));
    if (after)
        place(38, record(1, 1, 0, si + fileName(ref(24), "new.docx") + resident(0x80, {}, "doc")));
    QByteArray max(0x20, '\0');
    put<std::uint64_t>(max, 0, 0x2000000);
    put<std::uint64_t>(max, 8, 0x800000);
    put<std::uint64_t>(max, 0x10, 0x1234);
    put<std::int64_t>(max, 0x18, recordedLowest);
    const qint64 end = after ? kJournalEndAfter : kJournalEnd;
    place(40, record(1, 1, 0, si + fileName(ref(11, 11), "$UsnJrnl") + resident(0x80, "$Max", max) +
                                  nonResident(0x80, "$J", runs({{1, -1}, {2, 60}}), 3, end)));
    const QByteArray first = firstJournalPage();
    std::memcpy(image.data() + 60 * kCluster, first.constData(), first.size());
    if (after) {
        const QByteArray second = secondJournalPage();
        std::memcpy(image.data() + 61 * kCluster, second.constData(), second.size());
    }
    return image;
}
} // namespace testimage
