#!/usr/bin/env bash
# Run the whole SELF test suite (M0..M3b). Use: nix develop -c bash tests/all.sh
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"

for t in roundtrip loader audit selfld preload closure; do
	printf '\n\033[1m=== tests/%s.sh ===\033[0m\n' "$t"
	bash "$here/$t.sh"
done
printf '\n\033[32mALL SELF TESTS PASSED (M0 M1 M2 M3a M3b)\033[0m\n'
