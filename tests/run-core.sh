#!/usr/bin/env bash
set -euo pipefail
test_root=$(mktemp -d "$PWD/core-session-XXXXXX")
trap 'rm -rf -- "$test_root"' EXIT
mkdir -p "$test_root/config" "$test_root/data" "$test_root/cache"
export XDG_CONFIG_HOME="$test_root/config" XDG_DATA_HOME="$test_root/data" XDG_CACHE_HOME="$test_root/cache"
export NOOK_TEST_ENV_PRESET=1
exec_status=0
dbus-run-session -- "$@" || exec_status=$?
exit "$exec_status"
