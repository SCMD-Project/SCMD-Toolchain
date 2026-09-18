#!/usr/bin/env bash
# Examples smoke: compile every example and run the self-contained ones
# through scmdsim. Usage: examples_smoke.sh <dist-dir-with-binaries>
set -euo pipefail

DIST="${1:?usage: examples_smoke.sh <dist-dir-with-binaries>}"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

"$DIST/scmdc" --version

names=()
for src in "$ROOT"/examples/*.scmd; do
    name="$(basename "$src" .scmd)"
    echo "[smoke] compile $name"
    "$DIST/scmdc" "$src" -o "$WORK/$name.cfg" >/dev/null
    names+=("$name")
done

for proj in "$ROOT"/examples/*/*.scmdproj; do
    [ -e "$proj" ] || continue
    echo "[smoke] compile project $(basename "$proj")"
    (cd "$(dirname "$proj")" && "$DIST/scmdc" build "$(basename "$proj")" >/dev/null)
done

for name in "${names[@]}"; do
    echo "[smoke] run $name"
    if ! out="$("$DIST/scmdsim" "$WORK" --exec "$name" --no-interactive 2>&1)"; then
        printf '%s\n' "$out"
        echo "::error::example $name failed in scmdsim"
        exit 1
    fi
    if grep -qiE 'unknown command' <<<"$out"; then
        printf '%s\n' "$out"
        echo "::error::example $name produced 'Unknown command'"
        exit 1
    fi
    printf '  last: %s\n' "$(tail -n 1 <<<"$out")"
done

if ! grep -q 'FULL ADDER 1+1+1: PASS' \
    <("$DIST/scmdsim" "$WORK" --exec full_adder --no-interactive 2>&1); then
    echo "::error::full_adder PASS marker missing"
    exit 1
fi

echo "[smoke] all examples passed"
