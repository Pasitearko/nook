#pragma once
#include <QString>
#include <QProcessEnvironment>

namespace nook {
// Redirect only nook's state, never desktop-wide XDG paths used by IME/theme plugins.
enum class Storage { Config, Data, Cache };
// Called once at startup, before any worker threads. Consume the optional environment
// variable so unrelated applications and terminal shells do not inherit the profile.
void initializeProfile(const QString &directory);
bool customProfile();
QProcessEnvironment workerEnvironment();
QString storageDirectory(Storage location);
} // namespace nook
