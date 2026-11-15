#!/usr/bin/env bash
# Renders setup-infrastructure.png from setup-infrastructure.py in a container, so neither Graphviz
# nor the `diagrams` package has to be installed here - only docker.
set -euo pipefail

export MSYS_NO_PATHCONV=1 MSYS2_ARG_CONV_EXCL='*'

here="$(cd "$(dirname "$0")" && pwd)"

docker run --rm -v "$here:/work" -w /work -e OWNER="$(id -u):$(id -g)" python:3.12-slim sh -ec '
    apt-get update -qq >/dev/null
    apt-get install -y -qq graphviz >/dev/null 2>&1
    pip install -q --root-user-action=ignore diagrams==0.25.1
    python setup-infrastructure.py
    chown "$OWNER" setup-infrastructure.png
'

echo "Done: $here/setup-infrastructure.png"
