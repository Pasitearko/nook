#include "searchindex.h"
#include "storage.h"
#include "ntfs.h"
#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QPointer>
#include <QStandardPaths>
#include <QStorageInfo>
#include <QUrl>
#include <QtConcurrent>
#include <algorithm>
#include <set>
#include <climits>
#include <deque>
#include <dirent.h>
#include <fcntl.h>
#include <gio/gio.h>
#include <malloc.h>
#include <sqlite3.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

namespace {
// Rows are found by a 64-bit hash of their path (FNV-1a): an index of these is a fraction of
// one over the paths themselves. Lookups compare the path too, so a collision costs nothing.
sqlite3_int64 pathKey(const QByteArray &path) {
    quint64 hash = 14695981039346656037ULL;
    for (const char c : path) {
        hash ^= quint8(c);
        hash *= 1099511628211ULL;
    }
    return sqlite3_int64(hash);
}
// inotify watches cost kernel memory (~1 KiB each); unwatched directories fall back to the sweep.
constexpr int kWatchBudget = 8192;
struct Database {
    sqlite3 *db = nullptr;
    explicit Database(const QString &path, bool readOnly = false) {
        if (sqlite3_open_v2(QFile::encodeName(path).constData(), &db,
                            readOnly ? SQLITE_OPEN_READONLY : SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE,
                            nullptr) != SQLITE_OK) {
            if (db)
                sqlite3_close(db);
            db = nullptr;
        }
        if (db) {
            sqlite3_busy_timeout(db, 2500);
            sqlite3_exec(db, "PRAGMA cache_size=-8192; PRAGMA temp_store=FILE;", nullptr, nullptr, nullptr);
            sqlite3_create_function(
                db, "path_key", 1, SQLITE_UTF8 | SQLITE_DETERMINISTIC, nullptr,
                [](sqlite3_context *context, int, sqlite3_value **values) {
                    const auto *data = static_cast<const char *>(sqlite3_value_blob(values[0]));
                    sqlite3_result_int64(context, pathKey(QByteArray::fromRawData(data, sqlite3_value_bytes(values[0]))));
                },
                nullptr, nullptr);
        }
    }
    ~Database() {
        if (db)
            sqlite3_close(db);
    }
    bool exec(const char *sql) {
        return sqlite3_exec(db, sql, nullptr, nullptr, nullptr) == SQLITE_OK;
    }
};
void textBind(sqlite3_stmt *stmt, int at, const QString &value) {
    QByteArray data = value.toUtf8();
    sqlite3_bind_text(stmt, at, data.constData(), data.size(), SQLITE_TRANSIENT);
}
// Paths are stored as the file system gives them (raw bytes), not as percent-encoded URIs.
void pathBind(sqlite3_stmt *stmt, int at, const QByteArray &path) {
    sqlite3_bind_text(stmt, at, path.constData(), int(path.size()), SQLITE_TRANSIENT);
}
QByteArray pathColumn(sqlite3_stmt *stmt, int n) {
    const auto *value = reinterpret_cast<const char *>(sqlite3_column_text(stmt, n));
    return value ? QByteArray(value, sqlite3_column_bytes(stmt, n)) : QByteArray();
}
// "dir/" for prefix matches; "/" stays "/".
QByteArray treePrefix(QByteArray path) {
    if (!path.endsWith('/'))
        path += '/';
    return path;
}
QString column(sqlite3_stmt *stmt, int n) {
    const auto *value = sqlite3_column_text(stmt, n);
    return value ? QString::fromUtf8(reinterpret_cast<const char *>(value)) : QString();
}
bool schema(Database &db) {
    return db.exec(
        "PRAGMA journal_mode=WAL; PRAGMA synchronous=NORMAL;"
        // pinyin/initials are empty unless they differ from name_fold (names with Chinese).
        "CREATE TABLE IF NOT EXISTS entries(id INTEGER PRIMARY KEY,path TEXT NOT NULL,key INTEGER NOT NULL,"
        "root TEXT NOT NULL,name TEXT NOT NULL,"
        "name_fold TEXT NOT NULL,pinyin TEXT NOT NULL,initials TEXT NOT NULL,folder INTEGER,size "
        "INTEGER,mtime INTEGER,icon TEXT,seen INTEGER,ref INTEGER);"
        "CREATE TABLE IF NOT EXISTS meta(key TEXT PRIMARY KEY,value INTEGER);"
        "CREATE INDEX IF NOT EXISTS entries_key ON entries(key);"
        "CREATE INDEX IF NOT EXISTS entries_ref ON entries(root,ref);"
        "CREATE VIRTUAL TABLE IF NOT EXISTS entries_fts USING "
        "fts5(name_fold,pinyin,initials,content=entries,content_rowid=id,tokenize=trigram);"
        "CREATE TRIGGER IF NOT EXISTS entries_ai AFTER INSERT ON entries BEGIN INSERT INTO "
        "entries_fts(rowid,name_fold,pinyin,initials) VALUES(new.id,new.name_fold,new.pinyin,new.initials); "
        "END;"
        "CREATE TRIGGER IF NOT EXISTS entries_ad AFTER DELETE ON entries BEGIN INSERT INTO "
        "entries_fts(entries_fts,rowid,name_fold,pinyin,initials) "
        "VALUES('delete',old.id,old.name_fold,old.pinyin,old.initials); END;"
        "CREATE TRIGGER IF NOT EXISTS entries_au AFTER UPDATE OF name_fold,pinyin,initials ON entries BEGIN "
        "INSERT INTO entries_fts(entries_fts,rowid,name_fold,pinyin,initials) "
        "VALUES('delete',old.id,old.name_fold,old.pinyin,old.initials);"
        "INSERT INTO entries_fts(rowid,name_fold,pinyin,initials) "
        "VALUES(new.id,new.name_fold,new.pinyin,new.initials); END;");
}
struct Statement {
    sqlite3_stmt *stmt = nullptr;
    Statement(sqlite3 *db, const char *sql) {
        sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr);
    }
    ~Statement() {
        sqlite3_finalize(stmt);
    }
    Statement(const Statement &) = delete;
    Statement &operator=(const Statement &) = delete;
    operator sqlite3_stmt *() const {
        return stmt;
    }
};
qint64 metaValue(sqlite3 *db, const QByteArray &key) {
    Statement stmt(db, "SELECT value FROM meta WHERE key=?");
    if (!stmt)
        return 0;
    sqlite3_bind_text(stmt, 1, key.constData(), key.size(), SQLITE_TRANSIENT);
    return sqlite3_step(stmt) == SQLITE_ROW ? sqlite3_column_int64(stmt, 0) : 0;
}
void setMetaValue(sqlite3 *db, const QByteArray &key, qint64 value) {
    Statement stmt(db, "INSERT OR REPLACE INTO meta(key,value) VALUES(?,?)");
    if (!stmt)
        return;
    sqlite3_bind_text(stmt, 1, key.constData(), key.size(), SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 2, value);
    sqlite3_step(stmt);
}
// Binds every column of the INSERT used by the walk and by the NTFS reader.
constexpr const char *kInsert =
    "INSERT INTO entries(path,key,root,name,name_fold,pinyin,initials,folder,size,mtime,icon,seen,ref) "
    "VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?)";
constexpr const char *kLookup = "SELECT id,folder,size,mtime,root,ref FROM entries WHERE key=?1 AND path=?2";
void bindLookup(sqlite3_stmt *lookup, const QByteArray &path) {
    sqlite3_bind_int64(lookup, 1, pathKey(path));
    pathBind(lookup, 2, path);
}
void bindInsert(sqlite3_stmt *insert, const QByteArray &path, const QString &root, const QString &display, bool folder,
                qint64 size, qint64 mtime, qint64 generation, qint64 ref) {
    bool nonAscii = false;
    for (auto ch : display)
        if (ch.unicode() > 127) {
            nonAscii = true;
            break;
        }
    const QString fold = nook::folded(display);
    // Pinyin only where it says something the name does not: most names would store theirs
    // three times over, in the table and in the full-text index.
    auto keys = nonAscii ? nook::pinyinKeys(display) : QPair<QString, QString>();
    if (keys.first == fold)
        keys.first.clear();
    if (keys.second == fold)
        keys.second.clear();
    pathBind(insert, 1, path);
    sqlite3_bind_int64(insert, 2, pathKey(path));
    textBind(insert, 3, root);
    textBind(insert, 4, display);
    textBind(insert, 5, fold);
    textBind(insert, 6, keys.first);
    textBind(insert, 7, keys.second);
    sqlite3_bind_int(insert, 8, folder);
    sqlite3_bind_int64(insert, 9, size);
    sqlite3_bind_int64(insert, 10, mtime);
    textBind(insert, 11, nook::iconFor(display, folder));
    sqlite3_bind_int64(insert, 12, generation);
    sqlite3_bind_int64(insert, 13, ref);
}
QString uriOf(const QByteArray &path) {
    GFile *file = g_file_new_for_path(path.constData());
    char *value = g_file_get_uri(file);
    QString uri = QString::fromUtf8(value);
    g_free(value);
    g_object_unref(file);
    return uri;
}
int userVersion(Database &db) {
    Statement version(db.db, "PRAGMA user_version");
    return version && sqlite3_step(version) == SQLITE_ROW ? sqlite3_column_int(version, 0) : 0;
}
// v3 stores paths instead of URIs and keeps pinyin only where it differs. Older indexes are
// emptied rather than converted: the next scan rebuilds them (NTFS from its file table), and
// the space they held is given back. Every process opening the index runs this; the first
// one does the work under an exclusive lock.
void migrate(Database &db) {
    constexpr int kVersion = 3;
    if (userVersion(db) >= kVersion)
        return;
    sqlite3_busy_timeout(db.db, 60000);
    if (db.exec("BEGIN EXCLUSIVE;")) {
        if (userVersion(db) < kVersion) {
            db.exec("DROP TABLE IF EXISTS entries_fts; DROP TABLE IF EXISTS entries; DROP TABLE IF EXISTS meta;");
            db.exec("PRAGMA user_version=3;");
            db.exec("COMMIT;");
            db.exec("VACUUM;");
        } else
            db.exec("COMMIT;");
    }
    sqlite3_busy_timeout(db.db, 2500);
}
QString literalLike(QString text) {
    text.replace('\\', "\\\\");
    text.replace('%', "\\%");
    text.replace('_', "\\_");
    return "%" + text + "%";
}
} // namespace
SearchIndex::SearchIndex(const QString &database, QObject *parent) : QObject(parent), m_results(this) {
    m_database = database.isEmpty() ? nook::storageDirectory(nook::Storage::Data) + "/index.sqlite"
                                    : database;
    QDir().mkpath(QFileInfo(m_database).absolutePath());
    {
        Database db(m_database);
        if (db.db)
            migrate(db);
        if (!db.db || !schema(db))
            m_status = QStringLiteral("索引数据库初始化失败（需要 SQLite FTS5）");
    }
    m_sweep.setInterval(30 * 60 * 1000);
    connect(&m_sweep, &QTimer::timeout, this, &SearchIndex::start);
    m_debounce.setInterval(10000);
    m_debounce.setSingleShot(true);
    connect(&m_debounce, &QTimer::timeout, this, [this] {
        if (m_scanning) {
            m_debounce.start();
            return;
        }
        auto scopes = m_dirty.values();
        m_dirty.clear();
        if (!scopes.isEmpty())
            scan(scopes);
    });
    connect(&m_watcher, &QFileSystemWatcher::directoryChanged, this, [this](const QString &path) {
        if (m_stop)
            return;
        m_dirty.insert(path);
        m_debounce.start();
    });
}
SearchIndex::~SearchIndex() {
    if (!m_scanSource)
        stop();
    if (m_thread.joinable())
        m_thread.join();
    for (const auto &[key, fd] : m_volumes)
        ::close(fd);
    if (m_queryCancel)
        *m_queryCancel = true;
}
SearchIndex *SearchIndex::createSession(QObject *parent) {
    auto *session = new SearchIndex(m_database, parent);
    session->m_scanSource = this;
    auto sync = [this, session] {
        session->m_scanning = m_scanning;
        session->m_status = m_status;
        session->m_indexed = m_indexed;
        session->m_warnings = m_warnings;
        session->m_roots = m_roots;
        session->m_exclusions = m_exclusions;
        Q_EMIT session->changed();
        Q_EMIT session->rootsChanged();
    };
    connect(this, &SearchIndex::changed, session, sync);
    connect(this, &SearchIndex::rootsChanged, session, sync);
    connect(this, &SearchIndex::scanFinished, session, &SearchIndex::scanFinished);
    sync();
    return session;
}
void SearchIndex::setRoots(QStringList roots) {
    m_roots = std::move(roots);
    Q_EMIT rootsChanged();
}
void SearchIndex::setExclusions(QStringList exclusions) {
    m_exclusions = std::move(exclusions);
    Q_EMIT rootsChanged();
}
void SearchIndex::stop() {
    if (m_scanSource) {
        m_scanSource->stop();
        return;
    }
    m_stop = true;
    m_sweep.stop();
    m_debounce.stop();
    m_dirty.clear();
    if (!m_watcher.directories().isEmpty())
        m_watcher.removePaths(m_watcher.directories());
}
void SearchIndex::start() {
    if (m_scanSource)
        m_scanSource->start();
    else
        scan({});
}
void SearchIndex::scan(QStringList scopes) {
    if (m_scanning)
        return;
    if (m_thread.joinable())
        m_thread.join();
    m_scanning = true;
    m_stop = false;
    m_warnings.clear();
    m_status = QStringLiteral("正在后台更新文件名索引…");
    Q_EMIT changed();
    m_sweep.start();
    const bool full = scopes.isEmpty();
    if (full && !m_watcher.directories().isEmpty())
        m_watcher.removePaths(m_watcher.directories());
    QStringList roots = m_roots.isEmpty() ? nook::localRoots() : m_roots;
    if (full)
        scopes = roots;
    QStringList excludes = m_exclusions;
    if (!m_indexSystem)
        excludes.append(nook::systemFolders());
    const bool skipNoise = !m_indexNoise;
    excludes.append(nook::storageDirectory(nook::Storage::Cache));
    excludes.append(QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) + "/Trash");
    excludes.append(QFileInfo(m_database).absolutePath());
    const auto dbPath = m_database;
    const bool configuredRoots = !m_roots.isEmpty();
    const int watchBudget = qMax(0, kWatchBudget - m_watcher.directories().size());
    // Directory mtimes change whenever an entry is added, removed or renamed on these file
    // systems (NTFS too, including changes made while booted into Windows), so an unchanged
    // folder can be verified from its listing alone. FAT/exFAT do not keep folder times.
    QSet<QString> trusted, windowsVolumes;
    for (const auto &root : roots) {
        const QStorageInfo volume(root);
        const QByteArray fs = volume.fileSystemType();
        const auto clean = QDir::cleanPath(root);
        if (fs != "vfat" && fs != "exfat" && fs != "msdos")
            trusted.insert(clean);
        if (QDir::cleanPath(volume.rootPath()) == clean &&
            (fs == "ntfs" || fs == "ntfs3" || fs == "fuseblk" || fs == "vfat" || fs == "exfat"))
            windowsVolumes.insert(clean);
    }
    // Whole NTFS partitions, by the key that names them in the meta table (the partition UUID).
    QHash<QString, QString> ntfsKeys;
    if (m_ntfsJournal && full)
        for (const auto &root : roots) {
            const auto clean = QDir::cleanPath(root);
            if (m_volumeOpener) {
                ntfsKeys.insert(clean, clean);
                continue;
            }
            const QStorageInfo volume(root);
            const QByteArray fs = volume.fileSystemType();
            if ((fs == "ntfs" || fs == "ntfs3") && QDir::cleanPath(volume.rootPath()) == clean)
                if (const auto uuid = nook::ntfs::deviceUuid(QFile::decodeName(volume.device())); !uuid.isEmpty())
                    ntfsKeys.insert(clean, uuid);
        }
    const VolumeOpener opener = m_volumeOpener ? m_volumeOpener : VolumeOpener(nook::ntfs::openVolume);
    m_thread = std::thread([this, roots, scopes, excludes, dbPath, watchBudget, full, configuredRoots, trusted,
                            windowsVolumes, ntfsKeys, opener, skipNoise] {
        // Priority applies to this Linux task, not the UI/render threads. The idle I/O
        // class lets the background service yield the disk to anything interactive.
        setpriority(PRIO_PROCESS, 0, 10);
        constexpr int ioprioWhoProcess = 1, ioprioClassIdle = 3, ioprioClassShift = 13;
        ::syscall(SYS_ioprio_set, ioprioWhoProcess, 0, ioprioClassIdle << ioprioClassShift);
        Database db(dbPath);
        QStringList warnings, watches;
        qint64 count = 0;
        if (!db.db || !schema(db))
            warnings.append(QStringLiteral("数据库无法打开或缺少 FTS5 支持"));
        else {
            // Every file is re-examined (size and date edits included) at most once a day;
            // other passes trust unchanged folder times and only look at directories.
            constexpr qint64 kVerifyEvery = 24LL * 60 * 60 * 1000;
            auto meta = [&db](const char *key) { return metaValue(db.db, key); };
            auto setMeta = [&db](const char *key, qint64 value) { setMetaValue(db.db, key, value); };
            const qint64 startedAt = QDateTime::currentMSecsSinceEpoch();
            const bool verify = full && startedAt - meta("verified") > kVerifyEvery;
            // Folder times have one-second resolution: only a folder last changed clearly before
            // the previous complete pass began can have been listed in its final state.
            const qint64 trustBefore = meta("scanned") / 1000 - 2;
            bool everythingComplete = true;
            if (full) {
                sqlite3_stmt *known = nullptr;
                sqlite3_prepare_v2(db.db, "SELECT DISTINCT root FROM entries", -1, &known, nullptr);
                QStringList obsolete;
                if (known) {
                    while (sqlite3_step(known) == SQLITE_ROW) {
                        QString previous = column(known, 0);
                        if (!roots.contains(previous)) {
                            if (configuredRoots)
                                obsolete.append(previous);
                            else
                                warnings.append(
                                    QStringLiteral("位置离线或不再挂载，保留旧结果：%1").arg(previous));
                        }
                    }
                    sqlite3_finalize(known);
                }
                for (const auto &root : obsolete) {
                    sqlite3_stmt *remove = nullptr;
                    sqlite3_prepare_v2(db.db, "DELETE FROM entries WHERE root=?", -1, &remove, nullptr);
                    if (remove) {
                        textBind(remove, 1, root);
                        sqlite3_step(remove);
                        sqlite3_finalize(remove);
                    }
                }
                for (const auto &path : excludes) {
                    if (path.isEmpty())
                        continue;
                    const QByteArray base = QFile::encodeName(QDir::cleanPath(path)), prefix = treePrefix(base);
                    if (base == "/")
                        continue;
                    sqlite3_stmt *remove = nullptr;
                    sqlite3_prepare_v2(db.db, "DELETE FROM entries WHERE path=? OR substr(path,1,length(?))=?",
                                       -1, &remove, nullptr);
                    if (remove) {
                        pathBind(remove, 1, base);
                        pathBind(remove, 2, prefix);
                        pathBind(remove, 3, prefix);
                        sqlite3_step(remove);
                        sqlite3_finalize(remove);
                    }
                }
            }
            // Look up before writing: a re-scan of an unchanged tree only reads, so the
            // database pages (and the SSD) are not rewritten every time the index is verified.
            sqlite3_stmt *lookup = nullptr, *update = nullptr, *insert = nullptr, *remove = nullptr;
            const bool prepared =
                sqlite3_prepare_v2(db.db, kLookup, -1, &lookup, nullptr) == SQLITE_OK &&
                sqlite3_prepare_v2(db.db,
                                   "UPDATE entries SET root=?,folder=?,size=?,mtime=?,icon=?,ref=? WHERE id=?", -1,
                                   &update, nullptr) == SQLITE_OK &&
                sqlite3_prepare_v2(db.db, kInsert, -1, &insert, nullptr) == SQLITE_OK &&
                sqlite3_prepare_v2(db.db, "DELETE FROM entries WHERE id=?", -1, &remove, nullptr) == SQLITE_OK;
            if (!prepared) {
                warnings.append(QString::fromUtf8(sqlite3_errmsg(db.db)));
                for (auto *stmt : {lookup, update, insert, remove})
                    sqlite3_finalize(stmt);
            } else {
                QElapsedTimer progress;
                progress.start();
                const qint64 generation = QDateTime::currentMSecsSinceEpoch();
                QSet<QByteArray> boundaries;
                for (const auto &volume : QStorageInfo::mountedVolumes())
                    boundaries.insert(QFile::encodeName(volume.rootPath()));
                // Partitions are kept open for the session, so the helper (and any password
                // prompt) runs once rather than on every sweep.
                if (full)
                    for (auto it = m_volumes.begin(); it != m_volumes.end();) {
                        if (ntfsKeys.key(it->first).isEmpty()) {
                            ::close(it->second);
                            it = m_volumes.erase(it);
                        } else
                            ++it;
                    }
                QSet<QString> journaled;
                QStringList ntfsWatches;
                for (auto it = ntfsKeys.cbegin(); it != ntfsKeys.cend() && !m_stop; ++it) {
                    const QString &rootString = it.key();
                    if (nook::excludedPath(QFile::encodeName(rootString), excludes))
                        continue;
                    QString error;
                    if (syncNtfs(db.db, opener, it.value(), rootString, excludes, skipNoise, verify, generation,
                                 count, ntfsWatches, &error))
                        journaled.insert(rootString);
                    else if (!error.isEmpty())
                        warnings.append(QStringLiteral("%1 改为逐目录扫描：%2").arg(rootString, error));
                }
                for (const auto &scopeString : scopes) {
                    QString rootString;
                    for (const auto &candidate : roots) {
                        const auto clean = QDir::cleanPath(candidate);
                        if ((scopeString == clean ||
                             scopeString.startsWith(clean == "/" ? "/" : clean + '/')) &&
                            clean.size() > rootString.size())
                            rootString = clean;
                    }
                    if (rootString.isEmpty() || journaled.contains(QDir::cleanPath(scopeString)))
                        continue;
                    QByteArray root = QFile::encodeName(QDir::cleanPath(scopeString));
                    if (nook::excludedPath(root, excludes))
                        continue;
                    const bool trustFolderTimes = !verify && trusted.contains(rootString);
                    const bool windowsVolume = windowsVolumes.contains(rootString);
                    const QByteArray volumeRoot = QFile::encodeName(rootString);
                    // (path, quick): a quick folder kept its mtime since it was indexed, so its
                    // names are unchanged and only its subfolders need a stat.
                    std::deque<std::pair<QByteArray, bool>> pending{{root, false}};
                    std::vector<sqlite3_int64> seen;
                    bool complete = true, databaseOk = db.exec("BEGIN IMMEDIATE;");
                    if (!databaseOk) {
                        warnings.append(QString::fromUtf8(sqlite3_errmsg(db.db)));
                        break;
                    }
                    int transactionCount = 0;
                    auto step = [&](sqlite3_stmt *stmt) {
                        const int rc = sqlite3_step(stmt);
                        if (rc != SQLITE_DONE && rc != SQLITE_ROW) {
                            complete = false;
                            databaseOk = false;
                            warnings.append(QString::fromUtf8(sqlite3_errmsg(db.db)));
                        }
                        return rc;
                    };
                    auto entryDone = [&] {
                        ++count;
                        if (transactionCount >= 512) {
                            databaseOk = db.exec("COMMIT; BEGIN IMMEDIATE;");
                            transactionCount = 0;
                            std::this_thread::sleep_for(std::chrono::milliseconds(2));
                        }
                        if (progress.elapsed() > 400) {
                            QMetaObject::invokeMethod(
                                this,
                                [this, count] {
                                    m_indexed = count;
                                    Q_EMIT changed();
                                },
                                Qt::QueuedConnection);
                            progress.restart();
                        }
                    };
                    while (!pending.empty() && !m_stop && databaseOk) {
                        auto [path, quick] = std::move(pending.front());
                        pending.pop_front();
                        DIR *dir = opendir(path.constData());
                        if (!dir) {
                            complete = false;
                            if (warnings.size() < 30)
                                warnings.append(QStringLiteral("无法读取：%1").arg(QFile::decodeName(path)));
                            continue;
                        }
                        if (watches.size() < watchBudget)
                            watches.append(QFile::decodeName(path));
                        const bool atVolumeRoot = windowsVolume && path == volumeRoot;
                        errno = 0;
                        while (auto *item = readdir(dir)) {
                            if (m_stop || !databaseOk)
                                break;
                            QByteArray name(item->d_name);
                            if (name == "." || name == "..")
                                continue;
                            if (atVolumeRoot && nook::windowsSystemEntry(name))
                                continue;
                            QByteArray child = path + (path == "/" ? "" : "/") + name;
                            if (nook::excludedPath(child, excludes))
                                continue;
                            // Unchanged folder: a known file is confirmed from the listing alone,
                            // with no inode read. Folders are still examined to decide their turn.
                            if (quick && item->d_type != DT_UNKNOWN && item->d_type != DT_DIR) {
                                bindLookup(lookup, child);
                                const bool known = step(lookup) == SQLITE_ROW;
                                if (known)
                                    seen.push_back(sqlite3_column_int64(lookup, 0));
                                sqlite3_reset(lookup);
                                if (known) {
                                    entryDone();
                                    errno = 0;
                                    continue;
                                }
                            }
                            struct stat info{};
                            if (fstatat(dirfd(dir), name.constData(), &info, AT_SYMLINK_NOFOLLOW) != 0) {
                                complete = false;
                                continue;
                            }
                            bool folder = S_ISDIR(info.st_mode);
                            // Left out with everything in it; rows from before are pruned below.
                            if (folder && skipNoise && nook::noiseFolder(dirfd(dir), name)) {
                                errno = 0;
                                continue;
                            }
                            bool boundary = child != root && boundaries.contains(child);
                            bool unchanged = false;
                            bindLookup(lookup, child);
                            if (step(lookup) == SQLITE_ROW) {
                                const auto id = sqlite3_column_int64(lookup, 0);
                                // A folder's size is file-system bookkeeping, not something to search by.
                                const bool changed = sqlite3_column_int(lookup, 1) != int(folder) ||
                                                     (!folder && sqlite3_column_int64(lookup, 2) != info.st_size) ||
                                                     sqlite3_column_int64(lookup, 3) != info.st_mtime ||
                                                     column(lookup, 4) != rootString ||
                                                     sqlite3_column_int64(lookup, 5) != sqlite3_int64(info.st_ino);
                                sqlite3_reset(lookup);
                                seen.push_back(id);
                                unchanged = !changed;
                                if (changed) {
                                    textBind(update, 1, rootString);
                                    sqlite3_bind_int(update, 2, folder);
                                    sqlite3_bind_int64(update, 3, info.st_size);
                                    sqlite3_bind_int64(update, 4, info.st_mtime);
                                    textBind(update, 5, nook::iconFor(QString::fromUtf8(name), folder));
                                    sqlite3_bind_int64(update, 6, sqlite3_int64(info.st_ino));
                                    sqlite3_bind_int64(update, 7, id);
                                    step(update);
                                    sqlite3_reset(update);
                                    ++transactionCount;
                                }
                            } else if (databaseOk) {
                                sqlite3_reset(lookup);
                                bindInsert(insert, child, rootString, QString::fromUtf8(name), folder, info.st_size,
                                           info.st_mtime, generation, qint64(info.st_ino));
                                if (step(insert) == SQLITE_DONE)
                                    seen.push_back(sqlite3_last_insert_rowid(db.db));
                                sqlite3_reset(insert);
                                sqlite3_clear_bindings(insert);
                                ++transactionCount;
                            }
                            if (!databaseOk)
                                break;
                            if (folder && !boundary)
                                pending.emplace_back(child, trustFolderTimes && unchanged && info.st_mtime < trustBefore);
                            entryDone();
                            errno = 0;
                        }
                        if (errno)
                            complete = false;
                        closedir(dir);
                    }
                    // Never prune a root after cancellation, permission loss, or
                    // a failed transaction: unseen does not mean deleted.
                    if (complete && databaseOk && !m_stop) {
                        std::sort(seen.begin(), seen.end());
                        sqlite3_stmt *stale = nullptr;
                        sqlite3_prepare_v2(db.db,
                                           "SELECT id FROM entries WHERE root=? AND substr(path,1,length(?))=?", -1,
                                           &stale, nullptr);
                        std::vector<sqlite3_int64> obsolete;
                        if (stale) {
                            textBind(stale, 1, rootString);
                            const QByteArray prefix = treePrefix(QFile::encodeName(QDir::cleanPath(scopeString)));
                            pathBind(stale, 2, prefix);
                            pathBind(stale, 3, prefix);
                            while (sqlite3_step(stale) == SQLITE_ROW) {
                                const auto id = sqlite3_column_int64(stale, 0);
                                if (!std::binary_search(seen.begin(), seen.end(), id))
                                    obsolete.push_back(id);
                            }
                            sqlite3_finalize(stale);
                        }
                        for (const auto id : obsolete) {
                            sqlite3_bind_int64(remove, 1, id);
                            sqlite3_step(remove);
                            sqlite3_reset(remove);
                        }
                    }
                    if (!db.exec("COMMIT;")) {
                        complete = false;
                        warnings.append(QString::fromUtf8(sqlite3_errmsg(db.db)));
                    }
                    everythingComplete = everythingComplete && complete;
                    if (m_stop)
                        break;
                }
                sqlite3_finalize(lookup);
                sqlite3_finalize(update);
                sqlite3_finalize(insert);
                sqlite3_finalize(remove);
                // Walked folders first: the NTFS partitions also have the change journal.
                for (const auto &path : ntfsWatches)
                    if (watches.size() < watchBudget)
                        watches.append(path);
                if (!ntfsKeys.isEmpty())
                    malloc_trim(0); // the file-table pass leaves large freed buffers behind
                if (full && everythingComplete && !m_stop) {
                    setMeta("scanned", startedAt);
                    if (verify)
                        setMeta("verified", startedAt);
                }
                sqlite3_stmt *total = nullptr;
                if (sqlite3_prepare_v2(db.db, "SELECT count(*) FROM entries", -1, &total, nullptr) ==
                        SQLITE_OK &&
                    sqlite3_step(total) == SQLITE_ROW)
                    count = sqlite3_column_int64(total, 0);
                sqlite3_finalize(total);
            }
        }
        bool stopped = m_stop;
        QMetaObject::invokeMethod(
            this,
            [this, count, warnings, watches, stopped] {
                m_scanning = false;
                m_indexed = count;
                m_warnings = warnings;
                m_status = stopped ? QStringLiteral("索引已暂停，已有结果可继续搜索")
                                   : QStringLiteral("已索引 %1 项 · %2")
                                         .arg(count)
                                         .arg(warnings.isEmpty() ? "已更新" : "部分位置未覆盖");
                if (!stopped && !m_stop) {
                    QStringList newWatches;
                    const auto existing = m_watcher.directories();
                    for (const auto &path : watches)
                        if (!existing.contains(path))
                            newWatches.append(path);
                    QStringList failed =
                        newWatches.isEmpty() ? QStringList() : m_watcher.addPaths(newWatches);
                    if (!failed.isEmpty())
                        m_warnings.append(
                            QStringLiteral("%1 个目录监听不可用，将定期核对").arg(failed.size()));
                    if (m_watcher.directories().size() >= kWatchBudget)
                        m_warnings.append(
                            QStringLiteral("已达到 %1 个目录的监听预算，其余目录每 30 分钟核对").arg(kWatchBudget));
                }
                Q_EMIT changed();
                Q_EMIT scanFinished();
            },
            Qt::QueuedConnection);
    });
}
bool SearchIndex::syncNtfs(sqlite3 *db, const VolumeOpener &opener, const QString &key, const QString &root,
                           const QStringList &excludes, bool skipNoise, bool rebuild, qint64 generation, qint64 &count,
                           QStringList &watches, QString *error) {
    using namespace nook::ntfs;
    // Replaying more than this is slower than one sequential pass over the file table.
    constexpr size_t kMaxReplay = 20000;
    auto cached = m_volumes.find(key);
    if (cached == m_volumes.end()) {
        const int fd = opener(key, error);
        if (fd < 0)
            return false;
        cached = m_volumes.emplace(key, fd).first;
    }
    const int fd = ::fcntl(cached->second, F_DUPFD_CLOEXEC, 0);
    if (fd < 0)
        return false;
    Volume volume(fd);
    if (!volume.open(error)) {
        ::close(cached->second);
        m_volumes.erase(cached);
        return false;
    }
    const QByteArray meta = "ntfs:" + key.toUtf8() + ':';
    rebuild = rebuild || !metaValue(db, meta + "built");
    Journal journal;
    std::vector<std::uint64_t> records;
    if (!rebuild) {
        const auto record = std::uint64_t(metaValue(db, meta + "jrec"));
        const qint64 usn = metaValue(db, meta + "usn");
        if (!record)
            return false; // no journal on this partition: between file-table passes the walk keeps up
        std::vector<Change> changes;
        // A recreated or wrapped journal can no longer account for everything since the last pass.
        rebuild = !volume.journal(record, journal, nullptr) ||
                  journal.id != std::uint64_t(metaValue(db, meta + "id")) || usn < journal.lowest ||
                  usn > journal.next || !volume.changes(journal, usn, journal.next, changes, nullptr);
        std::set<std::uint64_t> unique;
        for (const auto &change : changes) {
            unique.insert(change.record);
            if (change.parent)
                unique.insert(change.parent); // its folder time changed too
        }
        rebuild = rebuild || unique.size() > kMaxReplay;
        records.assign(unique.begin(), unique.end());
    }
    Statement lookup(db, kLookup),
        update(db, "UPDATE entries SET root=?,folder=?,size=?,mtime=?,icon=?,ref=? WHERE id=?"),
        insert(db, kInsert),
        remove(db, "DELETE FROM entries WHERE id=?"), byRef(db, "SELECT id,path,folder FROM entries WHERE root=? AND ref=?"),
        occupied(db, "SELECT 1 FROM entries WHERE root=?1 AND (path=?2 OR substr(path,1,length(?3))=?3) AND ref IS NOT ?4 "
                     "LIMIT 1"),
        dropTree(db, "DELETE FROM entries WHERE root=?1 AND substr(path,1,length(?2))=?2"),
        moveTree(db, "UPDATE entries SET path=?1||substr(path,length(?2)+1),key=path_key(?1||substr(path,length(?2)+1)) "
                     "WHERE root=?3 AND substr(path,1,length(?2))=?2"),
        all(db, "SELECT id FROM entries WHERE root=?"),
        folders(db, "SELECT path FROM entries WHERE root=? AND folder=1 ORDER BY length(path) LIMIT ?");
    for (sqlite3_stmt *stmt : {lookup.stmt, update.stmt, insert.stmt, remove.stmt, byRef.stmt, occupied.stmt,
                               dropTree.stmt, moveTree.stmt, all.stmt, folders.stmt})
        if (!stmt) {
            if (error)
                *error = QString::fromUtf8(sqlite3_errmsg(db));
            return false;
        }
    bool ok = sqlite3_exec(db, "BEGIN IMMEDIATE;", nullptr, nullptr, nullptr) == SQLITE_OK;
    int written = 0;
    QElapsedTimer progress;
    progress.start();
    auto step = [&](sqlite3_stmt *stmt) {
        const int rc = sqlite3_step(stmt);
        if (rc != SQLITE_DONE && rc != SQLITE_ROW) {
            ok = false;
            if (error && error->isEmpty())
                *error = QString::fromUtf8(sqlite3_errmsg(db));
        }
        return rc;
    };
    auto wrote = [&] {
        if (++written < 512)
            return;
        ok = ok && sqlite3_exec(db, "COMMIT; BEGIN IMMEDIATE;", nullptr, nullptr, nullptr) == SQLITE_OK;
        written = 0;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    };
    auto tick = [&] {
        ++count;
        if (progress.elapsed() > 400) {
            QMetaObject::invokeMethod(
                this,
                [this, count] {
                    m_indexed = count;
                    Q_EMIT changed();
                },
                Qt::QueuedConnection);
            progress.restart();
        }
    };
    auto pathOf = [&root](const Entry &entry) {
        return QFile::encodeName(root == "/" ? "/" + entry.path : root + '/' + entry.path);
    };
    // The file table cannot be looked into folder by folder: noise is recognised by name.
    auto skipped = [&](const Entry &entry, const QByteArray &path) {
        return nook::excludedPath(path, excludes) || (skipNoise && nook::noisePath(entry.path.toUtf8()));
    };
    // Same comparison as the walk, so rows written by either are left alone by the other.
    auto upsert = [&](const QByteArray &path, const Entry &entry) -> sqlite3_int64 {
        sqlite3_int64 id = 0;
        bindLookup(lookup, path);
        if (step(lookup) == SQLITE_ROW) {
            id = sqlite3_column_int64(lookup, 0);
            const bool changed = sqlite3_column_int(lookup, 1) != int(entry.folder) ||
                                 (!entry.folder && sqlite3_column_int64(lookup, 2) != entry.size) ||
                                 sqlite3_column_int64(lookup, 3) != entry.mtime || column(lookup, 4) != root ||
                                 sqlite3_column_int64(lookup, 5) != sqlite3_int64(entry.record);
            sqlite3_reset(lookup);
            if (changed) {
                textBind(update, 1, root);
                sqlite3_bind_int(update, 2, entry.folder);
                sqlite3_bind_int64(update, 3, entry.size);
                sqlite3_bind_int64(update, 4, entry.mtime);
                textBind(update, 5, nook::iconFor(entry.path.section('/', -1), entry.folder));
                sqlite3_bind_int64(update, 6, sqlite3_int64(entry.record));
                sqlite3_bind_int64(update, 7, id);
                step(update);
                sqlite3_reset(update);
                wrote();
            }
        } else if (ok) {
            sqlite3_reset(lookup);
            bindInsert(insert, path, root, entry.path.section('/', -1), entry.folder, entry.size, entry.mtime,
                       generation, qint64(entry.record));
            if (step(insert) == SQLITE_DONE)
                id = sqlite3_last_insert_rowid(db);
            sqlite3_reset(insert);
            sqlite3_clear_bindings(insert);
            wrote();
        }
        return id;
    };
    // Each changed record is looked up again on disk and its rows made to match. Folders go
    // first, outermost first, so a renamed folder carries its contents along before they are
    // visited themselves.
    auto replay = [&]() -> bool {
        struct Item {
            std::uint64_t record;
            std::vector<std::pair<QByteArray, Entry>> now;
            int depth = INT_MAX;
        };
        std::vector<Item> items;
        for (const auto record : records) {
            Item item{record, {}};
            for (auto &entry : volume.current(record)) {
                const QByteArray path = pathOf(entry);
                if (!skipped(entry, path))
                    item.now.emplace_back(path, std::move(entry));
            }
            textBind(byRef, 1, root);
            sqlite3_bind_int64(byRef, 2, sqlite3_int64(record));
            while (step(byRef) == SQLITE_ROW)
                item.depth = std::min(item.depth, int(sqlite3_column_bytes(byRef, 1)));
            sqlite3_reset(byRef);
            items.push_back(std::move(item));
        }
        std::stable_sort(items.begin(), items.end(), [](const Item &a, const Item &b) { return a.depth < b.depth; });
        for (const auto &item : items) {
            if (!ok || m_stop)
                return false;
            const std::pair<QByteArray, Entry> *folder = nullptr;
            for (const auto &now : item.now)
                if (now.second.folder)
                    folder = &now;
            struct Row {
                sqlite3_int64 id;
                QByteArray path;
                bool folder;
            };
            std::vector<Row> rows;
            textBind(byRef, 1, root);
            sqlite3_bind_int64(byRef, 2, sqlite3_int64(item.record));
            while (step(byRef) == SQLITE_ROW)
                rows.push_back({sqlite3_column_int64(byRef, 0), pathColumn(byRef, 1), sqlite3_column_int(byRef, 2) != 0});
            sqlite3_reset(byRef);
            for (const auto &row : rows) {
                if (std::any_of(item.now.begin(), item.now.end(), [&](const auto &now) { return now.first == row.path; }))
                    continue;
                if (row.folder) {
                    const QByteArray from = row.path + '/';
                    if (folder) {
                        const QByteArray to = folder->first + '/';
                        // Something else already sits at the destination (swapped names, say):
                        // replaying record by record would be ambiguous.
                        textBind(occupied, 1, root);
                        pathBind(occupied, 2, folder->first);
                        pathBind(occupied, 3, to);
                        sqlite3_bind_int64(occupied, 4, sqlite3_int64(item.record));
                        const bool taken = step(occupied) == SQLITE_ROW;
                        sqlite3_reset(occupied);
                        if (taken)
                            return false;
                        pathBind(moveTree, 1, to);
                        pathBind(moveTree, 2, from);
                        textBind(moveTree, 3, root);
                        step(moveTree);
                        sqlite3_reset(moveTree);
                    } else {
                        textBind(dropTree, 1, root);
                        pathBind(dropTree, 2, from);
                        step(dropTree);
                        sqlite3_reset(dropTree);
                    }
                }
                sqlite3_bind_int64(remove, 1, row.id);
                step(remove);
                sqlite3_reset(remove);
            }
            for (const auto &[path, entry] : item.now) {
                upsert(path, entry);
                tick();
            }
        }
        return ok;
    };
    if (!rebuild && !replay()) {
        if (!ok || m_stop) {
            sqlite3_exec(db, "COMMIT;", nullptr, nullptr, nullptr);
            return m_stop; // the journal position is kept, so the next pass replays again
        }
        rebuild = true;
    }
    if (rebuild) {
        std::vector<sqlite3_int64> seen;
        const bool read = volume.enumerate(
            [&](const Entry &entry) {
                if (!ok)
                    return;
                const QByteArray path = pathOf(entry);
                if (skipped(entry, path))
                    return;
                if (const auto id = upsert(path, entry))
                    seen.push_back(id);
                tick();
            },
            journal, &m_stop, error);
        // Never prune after a stop or an unreadable table: unseen does not mean deleted.
        if (!read || !ok || m_stop) {
            sqlite3_exec(db, "COMMIT;", nullptr, nullptr, nullptr);
            return m_stop;
        }
        std::sort(seen.begin(), seen.end());
        std::vector<sqlite3_int64> obsolete;
        textBind(all, 1, root);
        while (step(all) == SQLITE_ROW)
            if (const auto id = sqlite3_column_int64(all, 0); !std::binary_search(seen.begin(), seen.end(), id))
                obsolete.push_back(id);
        for (const auto id : obsolete) {
            sqlite3_bind_int64(remove, 1, id);
            step(remove);
            sqlite3_reset(remove);
            wrote();
        }
        setMetaValue(db, meta + "built", QDateTime::currentMSecsSinceEpoch());
    }
    if (!ok) {
    } else if (!rebuild && records.empty()) {
        // Nothing happened since the last pass: nothing is written.
    } else if (journal.present) {
        setMetaValue(db, meta + "jrec", qint64(journal.record));
        setMetaValue(db, meta + "id", qint64(journal.id));
        setMetaValue(db, meta + "usn", journal.next);
    } else
        setMetaValue(db, meta + "jrec", 0);
    if (sqlite3_exec(db, "COMMIT;", nullptr, nullptr, nullptr) != SQLITE_OK || !ok) {
        if (error && error->isEmpty())
            *error = QString::fromUtf8(sqlite3_errmsg(db));
        return false;
    }
    // inotify still reports changes made from Linux, which ntfs3 does not journal.
    watches.append(root);
    textBind(folders, 1, root);
    sqlite3_bind_int(folders, 2, kWatchBudget);
    while (step(folders) == SQLITE_ROW)
        watches.append(QFile::decodeName(pathColumn(folders, 0)));
    return true;
}
void SearchIndex::query(const QString &text, const QString &scope, const QString &type, qint64 minBytes,
                        qint64 maxBytes, qint64 after, qint64 before) {
    m_text = text.trimmed();
    m_scope = scope;
    m_type = type;
    m_min = minBytes;
    m_max = maxBytes;
    m_after = after;
    m_before = before;
    m_offset = 0;
    runQuery(false);
}
void SearchIndex::nextPage() {
    if (m_more && !m_results.loading()) {
        m_offset += 200;
        runQuery(true);
    }
}
void SearchIndex::runQuery(bool append) {
    if (m_queryCancel)
        *m_queryCancel = true;
    m_queryCancel = std::make_shared<std::atomic_bool>(false);
    auto cancel = m_queryCancel;
    const auto generation = ++m_queryGeneration;
    if (!append)
        m_results.replaceEntries({});
    if (m_text.isEmpty()) {
        m_more = false;
        m_results.setBusy(false);
        Q_EMIT resultsChanged();
        return;
    }
    m_results.setBusy(true);
    QString text = nook::folded(m_text), scope = m_scope, type = m_type, database = m_database;
    qint64 min = m_min, max = m_max, after = m_after, before = m_before;
    int offset = m_offset;
    QPointer<SearchIndex> guard(this);
    (void)QtConcurrent::run([guard, cancel, generation, text, scope, type, database, min, max, after, before,
                             offset, append] {
        QList<nook::Entry> rows;
        QString error;
        Database db(database, true);
        if (!db.db)
            error = QStringLiteral("无法读取索引数据库");
        else {
            sqlite3_progress_handler(
                db.db, 1000,
                [](void *value) { return static_cast<std::atomic_bool *>(value)->load() ? 1 : 0; },
                cancel.get());
            bool trigram = text.toUcs4().size() >= 3;
            QString sql = "SELECT path,name,folder,size,mtime,icon FROM entries WHERE ";
            sql += trigram ? "id IN (SELECT rowid FROM entries_fts WHERE entries_fts MATCH ?)"
                           : "(name_fold LIKE ? ESCAPE '\\' OR pinyin LIKE ? ESCAPE '\\' OR initials LIKE ? "
                             "ESCAPE '\\')";
            if (!scope.isEmpty())
                sql += " AND (path=? OR substr(path,1,length(?))=?)";
            if (type == "folder")
                sql += " AND folder=1";
            else if (type == "file")
                sql += " AND folder=0";
            else if (type != "all")
                sql += " AND icon=?";
            if (min > 0)
                sql += " AND size>=?";
            if (max > 0)
                sql += " AND size<=?";
            if (after > 0)
                sql += " AND mtime>=?";
            if (before > 0)
                sql += " AND mtime<=?";
            sql += " ORDER BY folder DESC,name_fold,path LIMIT 201 OFFSET ?";
            sqlite3_stmt *stmt = nullptr;
            if (sqlite3_prepare_v2(db.db, sql.toUtf8().constData(), -1, &stmt, nullptr) != SQLITE_OK)
                error = QString::fromUtf8(sqlite3_errmsg(db.db));
            else {
                int bind = 1;
                if (trigram) {
                    QString escaped = text;
                    escaped.replace('"', "\"\"");
                    textBind(stmt, bind++, "\"" + escaped + "\"");
                } else
                    for (int i = 0; i < 3; ++i)
                        textBind(stmt, bind++, literalLike(text));
                if (!scope.isEmpty()) {
                    const QByteArray base = QFile::encodeName(nook::localPath(scope)), prefix = treePrefix(base);
                    pathBind(stmt, bind++, base);
                    pathBind(stmt, bind++, prefix);
                    pathBind(stmt, bind++, prefix);
                }
                if (type != "all" && type != "folder" && type != "file")
                    textBind(stmt, bind++, type);
                for (auto value : {min, max, after, before})
                    if (value > 0)
                        sqlite3_bind_int64(stmt, bind++, value);
                sqlite3_bind_int(stmt, bind, offset);
                int rc = SQLITE_DONE;
                while (!cancel->load() && (rc = sqlite3_step(stmt)) == SQLITE_ROW) {
                    nook::Entry e;
                    const QByteArray path = pathColumn(stmt, 0);
                    e.uri = uriOf(path);
                    e.name = column(stmt, 1);
                    e.directory = sqlite3_column_int(stmt, 2);
                    e.size = sqlite3_column_int64(stmt, 3);
                    e.modified = sqlite3_column_int64(stmt, 4);
                    e.icon = column(stmt, 5);
                    e.hidden = e.name.startsWith('.');
                    e.kind = e.directory ? QStringLiteral("文件夹") : QFileInfo(e.name).suffix().toUpper();
                    struct stat info{};
                    if (::lstat(path.constData(), &info) != 0)
                        e.kind = QStringLiteral("位置不可用");
                    rows.append(e);
                }
                if (rc != SQLITE_DONE && rc != SQLITE_ROW && !cancel->load())
                    error = QString::fromUtf8(sqlite3_errmsg(db.db));
                sqlite3_finalize(stmt);
            }
        }
        if (cancel->load())
            return;
        QMetaObject::invokeMethod(
            qApp,
            [guard, generation, rows = std::move(rows), error, append]() mutable {
                if (!guard || generation != guard->m_queryGeneration)
                    return;
                guard->m_more = rows.size() > 200;
                if (guard->m_more)
                    rows.removeLast();
                if (append)
                    guard->m_results.appendEntries(rows);
                else
                    guard->m_results.replaceEntries(rows);
                guard->m_results.setBusy(false, error);
                Q_EMIT guard->resultsChanged();
            },
            Qt::QueuedConnection);
    });
}
