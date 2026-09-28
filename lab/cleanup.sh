#!/usr/bin/env bash
set -euo pipefail

lab_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
exec "$lab_dir/weaknet-lab" cleanup
