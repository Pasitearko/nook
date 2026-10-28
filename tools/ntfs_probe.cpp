// Development probe: reads an NTFS volume's $MFT and USN journal read-only and prints what the
// index would see. Not installed. Usage: pkexec build/nook-ntfs-probe /dev/nvme0n1p4
// With --index DEVICE DATABASE it opens the device, drops back to the pkexec caller, and runs
// the real index (file-table build, then a journal replay) into DATABASE, timing both passes.
#include "ntfs.h"
#include "searchindex.h"
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QTimer>
#include <cstdio>
#include <fcntl.h>
#include <grp.h>
#include <map>
#include <pwd.h>
#include <unistd.h>

namespace {
int index(int fd, const char *database, int argc, char **argv) {
    if (const char *caller = getenv("PKEXEC_UID")) {
        const passwd *user = getpwuid(uid_t(atoi(caller)));
        if (!user || setgroups(0, nullptr) != 0 || setgid(user->pw_gid) != 0 || setuid(user->pw_uid) != 0) {
            std::perror("drop privileges");
            return 1;
        }
    }
    QCoreApplication app(argc, argv);
    SearchIndex search(QString::fromLocal8Bit(database));
    const QString root = QStringLiteral("/mnt/nook-ntfs-probe");
    search.setRoots({root});
    search.setNtfsJournal(true);
    search.setVolumeOpener([fd](const QString &, QString *) { return ::fcntl(fd, F_DUPFD_CLOEXEC, 0); });
    QElapsedTimer clock;
    int pass = 0;
    QObject::connect(&search, &SearchIndex::scanFinished, &app, [&] {
        std::printf("pass %d: %lld ms, %lld entries, %s\n", ++pass, clock.elapsed(), search.indexed(),
                    qPrintable(search.status()));
        for (const auto &warning : search.warnings())
            if (!warning.contains(QStringLiteral("监听")))
                std::printf("  warning: %s\n", qPrintable(warning));
        if (pass == 2) {
            search.stop();
            app.quit();
            return;
        }
        QTimer::singleShot(0, &app, [&] {
            clock.restart();
            search.start();
        });
    });
    clock.start();
    search.start();
    return app.exec();
}
} // namespace

int main(int argc, char **argv) {
    const bool indexing = argc == 4 && qstrcmp(argv[1], "--index") == 0;
    if (argc != 2 && !indexing) {
        std::fprintf(stderr, "usage: %s [--index] <ntfs block device or image> [database]\n", argv[0]);
        return 2;
    }
    const int fd = ::open(argv[indexing ? 2 : 1], O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) {
        std::perror("open");
        return 1;
    }
    if (indexing)
        return index(fd, argv[3], argc, argv);
    nook::ntfs::Volume volume(fd);
    QString error;
    QElapsedTimer clock;
    clock.start();
    if (!volume.open(&error)) {
        std::fprintf(stderr, "open: %s\n", qPrintable(error));
        return 1;
    }
    std::vector<nook::ntfs::Entry> entries;
    nook::ntfs::Journal journal;
    if (!volume.enumerate([&](const nook::ntfs::Entry &e) { entries.push_back(e); }, journal, nullptr, &error)) {
        std::fprintf(stderr, "enumerate: %s\n", qPrintable(error));
        return 1;
    }
    const auto enumerated = clock.elapsed();
    qint64 folders = 0, bytes = 0;
    std::map<QString, int> top;
    for (const auto &entry : entries) {
        folders += entry.folder;
        bytes += entry.folder ? 0 : entry.size;
        top[entry.path.section('/', 0, 0)]++;
    }
    std::printf("MFT records: %llu\nentries: %zu (folders %lld, files %lld, %.1f GiB)\nenumerate: %lld ms\n",
                static_cast<unsigned long long>(volume.recordCount()), entries.size(), folders,
                qint64(entries.size()) - folders, bytes / 1073741824.0, enumerated);
    std::printf("top level:\n");
    for (const auto &[name, count] : top)
        std::printf("  %-40s %d\n", qPrintable(name), count);
    for (size_t i = 0; i < entries.size() && i < 8; ++i)
        std::printf("sample: %s\n", qPrintable(entries[i * (entries.size() / 8)].path));
    if (!journal.present) {
        std::printf("USN journal: not present\n");
        return 0;
    }
    std::printf("USN journal: id %llx, lowest %lld, next %lld (%.1f MiB kept)\n",
                static_cast<unsigned long long>(journal.id), journal.lowest, journal.next,
                (journal.next - journal.lowest) / 1048576.0);
    clock.restart();
    std::vector<nook::ntfs::Change> changes;
    const qint64 from = std::max(journal.lowest, journal.next - (8 << 20));
    if (!volume.changes(journal, from, journal.next, changes, &error)) {
        std::fprintf(stderr, "changes: %s\n", qPrintable(error));
        return 1;
    }
    std::printf("last 8 MiB of journal: %zu records in %lld ms\n", changes.size(), clock.elapsed());
    for (size_t i = changes.size() > 5 ? changes.size() - 5 : 0; i < changes.size(); ++i) {
        const auto current = volume.current(changes[i].record);
        std::printf("  record %llu reason %08x %s -> %s\n", static_cast<unsigned long long>(changes[i].record),
                    changes[i].reason, qPrintable(changes[i].name),
                    current.empty() ? "(gone or excluded)" : qPrintable(current.front().path));
    }
    return 0;
}
