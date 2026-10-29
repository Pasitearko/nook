#pragma once
#include <QByteArray>
#include <QList>
#include <QString>
#include <QStringList>

// Naming rules shared by the archive worker and the application: split volumes, the folder name a
// package extracts into, and the legacy filename encodings found in old ZIP and tar packages.
namespace nook::archive {

struct Volumes {
    QString first;       // the part an extractor must open
    QStringList parts;   // existing parts in order, beginning with first
    bool split = false;  // the name follows a split-volume pattern
};

// Finds the first part and its siblings for name.7z.001, name.part2.rar, name.r00, name.z01 ….
Volumes volumesOf(const QString &path);
// "photos.tar.gz" -> "photos", "pack.part1.rar" -> "pack", "data.7z.001" -> "data".
QString stemOf(const QString &fileName);
// "report.txt" -> "report (2).txt"; directories keep their whole name before the counter.
QString numberedName(const QString &name, int number, bool directory);

// The glob patterns of packages by name ("*.zip", "*.tar.gz", "*.part1.rar" …).
const QStringList &archivePatterns();
// Whether a file name looks like a package (by its suffix; the worker decides by content).
bool looksLikeArchive(const QString &fileName);
// The name, without suffix, of a new package for these items: a single item's own name (a file
// without its extension), or for several the name of the folder that holds the first.
QString suggestedName(const QStringList &paths);

bool isUtf8(const QByteArray &bytes);
// Encodings offered for filenames, as ICU converter names; "UTF-8" first.
QStringList filenameEncodings();
// The legacy encoding most likely for this desktop's language (GB18030 for zh_CN).
QString fallbackEncoding();
// Picks an encoding for names that are not valid UTF-8; "UTF-8" when all of them are.
QString detectEncoding(const QList<QByteArray> &names);
// Decodes strictly; ok is false (and the bytes are decoded as lossy UTF-8) if the encoding rejects them.
QString decodeName(const QByteArray &raw, const QString &encoding, bool *ok = nullptr);

}
