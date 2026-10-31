#pragma once
#include <QString>
#include <QVariantMap>

namespace nook {
// Called only for full previews, in the isolated preview process. Returns false for other formats.
// Uses the existing text/rich content contract; never extracts package members or loads resources.
bool documentPreview(const QString &path, int page, QVariantMap *result);
}
