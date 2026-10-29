#include "archivenames.h"
#include <QDir>
#include <QFileInfo>
#include <QRegularExpression>
#include <QSet>
#include <memory>
#include <unicode/ucnv.h>
#include <unicode/ucsdet.h>

namespace nook::archive {
namespace {

QString partName(const QString &prefix, int number, int width, const QString &suffix) {
    return prefix + QString::number(number).rightJustified(width, u'0') + suffix;
}

// Collects prefix<n>suffix for n = from, from+1 … while the files exist.
QStringList run(const QString &prefix, int from, int width, const QString &suffix) {
    QStringList parts;
    for (int n = from; n < from + 100000; ++n) {
        const QString path = partName(prefix, n, width, suffix);
        if (!QFileInfo::exists(path))
            break;
        parts << path;
    }
    return parts;
}

struct Converter {
    UConverter *handle = nullptr;
    explicit Converter(const QString &name) {
        UErrorCode status = U_ZERO_ERROR;
        handle = ucnv_open(name.toLatin1().constData(), &status);
        if (U_FAILURE(status))
            handle = nullptr;
    }
    ~Converter() {
        if (handle)
            ucnv_close(handle);
    }
};

bool strictDecode(const QByteArray &raw, const QString &encoding, QString *out) {
    Converter converter(encoding);
    if (!converter.handle)
        return false;
    UErrorCode status = U_ZERO_ERROR;
    ucnv_setToUCallBack(converter.handle, UCNV_TO_U_CALLBACK_STOP, nullptr, nullptr, nullptr, &status);
    QString result(raw.size() * 2 + 2, Qt::Uninitialized);
    status = U_ZERO_ERROR;
    const int length = ucnv_toUChars(converter.handle, reinterpret_cast<UChar *>(result.data()),
                                     int(result.size()), raw.constData(), int(raw.size()), &status);
    if (U_FAILURE(status))
        return false;
    result.truncate(length);
    *out = result;
    return true;
}

}

Volumes volumesOf(const QString &path) {
    Volumes volumes;
    volumes.first = path;
    volumes.parts = {path};
    const QFileInfo info(path);
    const QString dir = info.absolutePath() + u'/';
    const QString name = info.fileName();
    static const QRegularExpression numbered(QStringLiteral("^(.*\\.)(\\d{3})$"));
    static const QRegularExpression rarPart(QStringLiteral("^(.*\\.part)(\\d+)(\\.rar)$"),
                                            QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression oldRar(QStringLiteral("^(.*\\.)[rR](\\d{2,})$"));
    static const QRegularExpression zipPart(QStringLiteral("^(.*\\.)[zZ](\\d{2,})$"));
    if (auto m = numbered.match(name); m.hasMatch()) {
        const int width = int(m.captured(2).size());
        volumes.split = true;
        volumes.first = dir + partName(m.captured(1), 1, width, {});
        volumes.parts = run(dir + m.captured(1), 1, width, {});
    } else if (auto m = rarPart.match(name); m.hasMatch()) {
        const int width = int(m.captured(2).size());
        volumes.split = true;
        volumes.first = dir + partName(m.captured(1), 1, width, m.captured(3));
        volumes.parts = run(dir + m.captured(1), 1, width, m.captured(3));
    } else if (auto m = oldRar.match(name); m.hasMatch() || name.endsWith(u".rar", Qt::CaseInsensitive)) {
        const QString stem = m.hasMatch() ? m.captured(1) : name.chopped(3);
        const QString rar = dir + stem + (QFileInfo::exists(dir + stem + "RAR") ? "RAR" : "rar");
        const QStringList rest = run(dir + stem + u'r', 0, 2, {});
        if (m.hasMatch() || !rest.isEmpty()) {
            volumes.split = true;
            volumes.first = rar;
            volumes.parts = QStringList{rar} + rest;
        }
    } else if (auto m = zipPart.match(name); m.hasMatch() || name.endsWith(u".zip", Qt::CaseInsensitive)) {
        const QString stem = m.hasMatch() ? m.captured(1) : name.chopped(3);
        const QStringList rest = run(dir + stem + u'z', 1, 2, {});
        if (m.hasMatch() || !rest.isEmpty()) {
            volumes.split = true;
            volumes.first = dir + stem + "zip";
            volumes.parts = rest + QStringList{volumes.first};
        }
    }
    if (volumes.parts.isEmpty() || !volumes.parts.contains(volumes.first)) {
        // The opened part is all that exists; keep it so the caller can report the missing first one.
        if (!QFileInfo::exists(volumes.first))
            volumes.parts = QStringList{path};
    }
    return volumes;
}

QString stemOf(const QString &fileName) {
    QString name = QFileInfo(fileName).fileName();
    static const QRegularExpression numbered(QStringLiteral("\\.\\d{3}$"));
    static const QRegularExpression rarPart(QStringLiteral("\\.part\\d+\\.rar$"),
                                            QRegularExpression::CaseInsensitiveOption);
    name.remove(rarPart);
    name.remove(numbered);
    static const QStringList compound{".tar.gz", ".tar.bz2", ".tar.xz", ".tar.zst", ".tar.lz4",
                                      ".tar.lzma", ".tar.lz", ".tar.z", ".tgz", ".tbz2", ".tbz",
                                      ".txz", ".tzst", ".tlz"};
    for (const auto &suffix : compound) {
        if (name.endsWith(suffix, Qt::CaseInsensitive) && name.size() > suffix.size())
            return name.chopped(suffix.size());
    }
    const int dot = int(name.lastIndexOf(u'.'));
    return dot > 0 ? name.left(dot) : name;
}

QString numberedName(const QString &name, int number, bool directory) {
    const int dot = directory ? -1 : int(name.lastIndexOf(u'.'));
    const QString counter = QStringLiteral(" (%1)").arg(number);
    if (dot <= 0)
        return name + counter;
    return name.left(dot) + counter + name.mid(dot);
}

bool isUtf8(const QByteArray &bytes) {
    const auto *p = reinterpret_cast<const unsigned char *>(bytes.constData());
    const auto *end = p + bytes.size();
    while (p < end) {
        const unsigned char c = *p;
        int extra = c < 0x80 ? 0 : (c >> 5) == 0x6 ? 1 : (c >> 4) == 0xe ? 2 : (c >> 3) == 0x1e ? 3 : -1;
        if (extra < 0 || (extra == 1 && c < 0xc2) || end - p <= extra)
            return false;
        for (int i = 1; i <= extra; ++i) {
            if ((p[i] & 0xc0) != 0x80)
                return false;
        }
        if (extra == 2 && ((c == 0xe0 && p[1] < 0xa0) || (c == 0xed && p[1] >= 0xa0)))
            return false;
        if (extra == 3 && ((c == 0xf0 && p[1] < 0x90) || (c == 0xf4 && p[1] >= 0x90) || c > 0xf4))
            return false;
        p += extra + 1;
    }
    return true;
}

QStringList filenameEncodings() {
    return {"UTF-8", "GB18030", "Big5", "Shift_JIS", "EUC-JP", "EUC-KR", "IBM437", "IBM866",
            "windows-1251", "windows-1252", "KOI8-R", "ISO-8859-1"};
}

QString fallbackEncoding() {
    const QByteArray language = !qgetenv("LC_ALL").isEmpty()    ? qgetenv("LC_ALL")
                                : !qgetenv("LC_CTYPE").isEmpty() ? qgetenv("LC_CTYPE")
                                                                 : qgetenv("LANG");
    if (language.startsWith("zh_TW") || language.startsWith("zh_HK") || language.startsWith("zh_MO"))
        return "Big5";
    if (language.startsWith("zh"))
        return "GB18030";
    if (language.startsWith("ja"))
        return "Shift_JIS";
    if (language.startsWith("ko"))
        return "EUC-KR";
    if (language.startsWith("ru") || language.startsWith("uk") || language.startsWith("be"))
        return "IBM866";
    return "IBM437";
}

QString detectEncoding(const QList<QByteArray> &names) {
    QList<QByteArray> legacy;
    QByteArray joined;
    for (const auto &name : names) {
        if (!isUtf8(name)) {
            legacy << name;
            joined += name;
            joined += '\n';
        }
    }
    if (legacy.isEmpty())
        return "UTF-8";
    const auto decodesAll = [&](const QString &encoding) {
        QString ignored;
        for (const auto &name : legacy) {
            if (!strictDecode(name, encoding, &ignored))
                return false;
        }
        return true;
    };
    const QString fallback = fallbackEncoding();
    QString detected;
    int confidence = 0;
    UErrorCode status = U_ZERO_ERROR;
    std::unique_ptr<UCharsetDetector, decltype(&ucsdet_close)> detector(ucsdet_open(&status), ucsdet_close);
    if (U_SUCCESS(status)) {
        ucsdet_setText(detector.get(), joined.constData(), int(joined.size()), &status);
        int count = 0;
        const UCharsetMatch **matches = ucsdet_detectAll(detector.get(), &count, &status);
        static const QSet<QString> offered{"GB18030", "Big5", "Shift_JIS", "EUC-JP", "EUC-KR",
                                           "windows-1251", "windows-1252", "KOI8-R", "ISO-8859-1"};
        for (int i = 0; U_SUCCESS(status) && i < count; ++i) {
            const QString name = QString::fromLatin1(ucsdet_getName(matches[i], &status));
            if (offered.contains(name) && decodesAll(name)) {
                detected = name;
                confidence = ucsdet_getConfidence(matches[i], &status);
                break;
            }
        }
    }
    // Short names give the detector little to go on: trust the desktop's language unless the
    // detector is confident about something else.
    if (decodesAll(fallback) && (detected.isEmpty() || detected == fallback || confidence < 60))
        return fallback;
    if (!detected.isEmpty())
        return detected;
    return decodesAll("GB18030") ? QStringLiteral("GB18030") : fallback;
}

QString decodeName(const QByteArray &raw, const QString &encoding, bool *ok) {
    if (ok)
        *ok = true;
    if (encoding.isEmpty() || encoding.compare(u"UTF-8", Qt::CaseInsensitive) == 0) {
        if (ok)
            *ok = isUtf8(raw);
        return QString::fromUtf8(raw);
    }
    QString decoded;
    if (strictDecode(raw, encoding, &decoded))
        return decoded;
    if (ok)
        *ok = false;
    return QString::fromUtf8(raw);
}

}
