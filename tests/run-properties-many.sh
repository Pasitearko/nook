#!/usr/bin/env bash
# Focused standalone build: does not touch the shared application/CMake build tree.
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
work=$(mktemp -d "${TMPDIR:-/tmp}/nook-properties-many.XXXXXX")
trap 'rm -rf -- "$work"' EXIT
moc="$(qmake6 -query QT_INSTALL_LIBEXECS)/moc"
packages=(Qt6Test Qt6Concurrent Qt6Gui gio-2.0 icu-i18n icu-uc)
read -r -a cflags <<< "$(pkg-config --cflags "${packages[@]}")"
read -r -a libs <<< "$(pkg-config --libs "${packages[@]}")"
"$moc" -DQT_NO_KEYWORDS "$root/src/fileproperties.h" -o "$work/moc_fileproperties.cpp"
"$moc" -DQT_NO_KEYWORDS "$root/tests/properties_many_test.cpp" -o "$work/properties_many_test.moc"
"${CXX:-c++}" -std=c++20 -fPIC -pthread -Wall -Wextra -Wpedantic -DQT_NO_KEYWORDS \
    "${cflags[@]}" -I"$root/src" -I"$work" \
    "$root/tests/properties_many_test.cpp" "$root/src/fileproperties.cpp" \
    "$root/src/files.cpp" "$work/moc_fileproperties.cpp" \
    "${libs[@]}" -o "$work/properties-many-test"
"$work/properties-many-test" "$@"
