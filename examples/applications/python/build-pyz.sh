#!/usr/bin/env bash
# Builds xml-splitter.pyz: xml_splitter.py and euclid-pdk in one zipapp, which any python3 runs as
# it is - "python3 xml-splitter.pyz" - with nothing installed. euclid-pdk has no dependencies of its
# own, so the archive holds pure Python only and runs on the Alpine workers of examples/docker as on
# anything else.
#
#   OUT_DIR       where the archive goes (default: ../../docker/apps, next to the other applications)
#   PDK_VERSION   the euclid-pdk release to bundle (default: 0.17.0)
set -euo pipefail

here="$(cd "$(dirname "$0")" && pwd)"
OUT_DIR="${OUT_DIR:-$here/../../docker/apps}"
PDK_VERSION="${PDK_VERSION:-0.17.0}"
mkdir -p "$OUT_DIR"

build="$(mktemp -d)"
trap 'rm -rf "$build"' EXIT

python3 -m pip install --quiet --no-compile --target "$build" "euclid-pdk==$PDK_VERSION"
cp "$here/xml_splitter.py" "$build/__main__.py"
python3 -m zipapp "$build" --output "$OUT_DIR/xml-splitter.pyz" --python "/usr/bin/env python3"

echo "Done: $OUT_DIR/xml-splitter.pyz"
