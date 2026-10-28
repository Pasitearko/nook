#include "storage.h"
#include <QDir>
#include <QStandardPaths>

namespace nook {
namespace {
QString selectedProfile;
QString profileDirectory() {
    return selectedProfile.isEmpty() ? qEnvironmentVariable("NOOK_PROFILE_DIR") : selectedProfile;
}
} // namespace
void initializeProfile(const QString &directory) {
    selectedProfile = directory.isEmpty() ? QString() : QDir(directory).absolutePath();
    qunsetenv("NOOK_PROFILE_DIR");
}
bool customProfile() {
    return !profileDirectory().isEmpty();
}
QProcessEnvironment workerEnvironment() {
    auto environment = QProcessEnvironment::systemEnvironment();
    const auto profile = profileDirectory();
    if (!profile.isEmpty())
        environment.insert("NOOK_PROFILE_DIR", QDir(profile).absolutePath());
    return environment;
}
QString storageDirectory(Storage location) {
    const QString profile = profileDirectory();
    if (!profile.isEmpty()) {
        const auto root = QDir(profile).absolutePath();
        switch (location) {
        case Storage::Config: return root + "/config";
        case Storage::Data: return root + "/data";
        case Storage::Cache: return root + "/cache";
        }
    }
    switch (location) {
    case Storage::Config:
        return QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation) + "/nook";
    case Storage::Data:
        return QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
    case Storage::Cache:
        return QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
    }
    Q_UNREACHABLE();
}
} // namespace nook
