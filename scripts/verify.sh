#!/usr/bin/env bash
set -euo pipefail
cmake --build --preset dev
ctest --preset dev --output-on-failure
if command -v clang-format >/dev/null 2>&1; then cmake --build build/dev --target format; fi
./scripts/scan-secrets.sh --mode all
echo 'EdgeFleet build, CTest, and secret scan passed.'
