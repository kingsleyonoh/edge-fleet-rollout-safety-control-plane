#!/usr/bin/env bash
set -euo pipefail

cmake --preset dev
cmake --build --preset dev
printf '%s\n' 'EdgeFleet development build is ready at build/dev/edgefleet.'
