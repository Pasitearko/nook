#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
root="$PWD"
stage="$root/build/package"
mkdir -p "$stage"
git ls-files -c -o --exclude-standard -z | tar --null -T - --transform='s,^,nook-0.1/,' -czf "$stage/nook-0.1.tar.gz"
cp packaging/PKGBUILD "$stage/PKGBUILD"
python3 - "$stage" <<'PY'
from pathlib import Path
import hashlib,sys
stage=Path(sys.argv[1])
digest=hashlib.sha256((stage/'nook-0.1.tar.gz').read_bytes()).hexdigest()
p=stage/'PKGBUILD'
p.write_text(p.read_text().replace('REPLACED_BY_PACKAGE_SCRIPT',digest))
PY
cd "$stage"
makepkg --cleanbuild --noconfirm --force
