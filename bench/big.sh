#!/usr/bin/env bash
# Bigger-binary benchmark: exec latency ELF vs SELF memfd vs SELF native
# across a real size range (hello .. gdb), reporting mean AND stddev so we
# can see whether the gap is signal or variance. Emits bench/big.csv +
# bench/big.md. Run in `nix develop`.
set -euo pipefail

repo="$(cd "$(dirname "$0")/.." && pwd)"
export PYTHONPATH="$repo/converter"
SELF_EXEC="$repo/loader/self-exec"
csv="$repo/bench/big.csv"
md="$repo/bench/big.md"
work="$(mktemp -d)"; trap 'rm -rf "$work"' EXIT; cd "$work"
make -C "$repo/loader" >/dev/null

printf '#include <stdio.h>\nint main(void){puts("hi");return 0;}\n' > h.c
cc -O2 h.c -o hello

# name  path  args-to-make-it-exit-fast
rows=(
  "hello|$work/hello|"
  "git|$(readlink -f "$(type -P git)")|--version"
  "curl|$(readlink -f "$(type -P curl)")|--version"
  "gdb|$(readlink -f "$(type -P gdb)")|--version"
)

mean() { python3 -c "import json;r=json.load(open('$1'))['results'][0];print(f\"{r['mean']*1000:.3f},{r['stddev']*1000:.3f}\")"; }

echo "subject,libs,elf_bytes,self_bytes,mode,mean_ms,stddev_ms" > "$csv"
for row in "${rows[@]}"; do
  IFS='|' read -r name src args <<<"$row"
  # coreutils dispatches on basename(argv[0]); name the .self so it matches
  base=$([ "$name" = coreutils ] && echo ls || echo "$name")
  python -m selfconv.elf2self "$src" "$base.self" >/dev/null 2>&1
  libs=$(ldd "$src" 2>/dev/null | grep -c "=>" || true)
  esz=$(stat -c%s "$src"); ssz=$(stat -c%s "$base.self")
  hyperfine -N --warmup 10 --min-runs 60 --export-json elf.json    "$src $args" >/dev/null
  hyperfine -N --warmup 10 --min-runs 60 --export-json memfd.json  "env SELF_MODE=memfd  $SELF_EXEC $work/$base.self $args" >/dev/null
  hyperfine -N --warmup 10 --min-runs 60 --export-json native.json "env SELF_MODE=native $SELF_EXEC $work/$base.self $args" >/dev/null
  for m in elf memfd native; do
    echo "$name,$libs,$esz,$ssz,$m,$(mean $m.json)" >> "$csv"
  done
  echo "done: $name (libs=$libs elf=$esz self=$ssz)" >&2
done

# render a compact markdown table too
{
  echo "# Bigger-binary exec latency"
  echo
  echo "Host \`$(uname -mrs)\`; hyperfine -N warmup=10 min-runs=60. mean ± stddev (ms)."
  echo
  printf '| subject | libs | ELF size | SELF size | ELF | memfd (M1) | native (M2) |\n'
  printf '|---|--:|--:|--:|--:|--:|--:|\n'
  python3 - "$csv" <<'PY'
import csv,sys,collections
rows=list(csv.DictReader(open(sys.argv[1])))
by=collections.defaultdict(dict)
meta={}
for r in rows:
    by[r['subject']][r['mode']]=(r['mean_ms'],r['stddev_ms'])
    meta[r['subject']]=(r['libs'],int(r['elf_bytes']),int(r['self_bytes']))
def fmt(t): return f"{t[0]} ± {t[1]}"
for s in ['hello','git','curl','gdb']:
    if s not in by: continue
    libs,e,sf=meta[s]
    print(f"| {s} | {libs} | {e/1024:.0f} KiB | {sf/1024:.0f} KiB | {fmt(by[s]['elf'])} | {fmt(by[s]['memfd'])} | {fmt(by[s]['native'])} |")
PY
} > "$md"
echo "wrote $csv and $md" >&2
cat "$md"
