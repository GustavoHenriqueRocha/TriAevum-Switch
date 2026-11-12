#!/usr/bin/env bash
# Smoke test on Eden on the Ryzen: sends the .nro, opens it for N s in the Windows session and fails if
#  - Eden's SD doesn't match the reference SD (corrupted SD, not a build bug);
#  - Eden closed on its own or the game didn't get past MIN_FRAMES frames;
#  - boot-status.txt ended in status=failed (shows the detail=);
#  - the game changed files it should only read (config/, game/, resources.o2r).
#   tests/switch/eden_smoke.sh path/to.nro [seconds]
# Reference SD: $SD_REF (default ~/oot3d-work/sd/switch/oot3dre).
set -euo pipefail
nro="${1:?path to the .nro}"; seconds="${2:-180}"
host="${EDEN_HOST:-ryzen}"; MIN_FRAMES="${MIN_FRAMES:-300}"
SD_REF="${SD_REF:-$HOME/oot3d-work/sd/switch/oot3dre}"
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
work="$(mktemp -d)"; trap 'rm -rf "$work"' EXIT
ssh_() { ssh "$host" "$@" 2> >(grep -v -E 'WARNING|quantum|upgraded' >&2); }

# Files the game only reads: uppercase md5 + relative path, sorted.
remote_md5() {
    ssh_ '$b="$env:APPDATA\eden\sdmc\switch\oot3dre"; Get-ChildItem $b -Recurse -File |
      ? { $_.FullName -match "\\(config|game)\\|resources\.o2r$" -and $_.Name -ne "fps_counter" } |
      % { "{0} {1}" -f (Get-FileHash -Algorithm MD5 $_.FullName).Hash, ($_.FullName.Substring($b.Length+1) -replace "\\","/") }' |
      tr -d '\r' | sort -k2
}
(cd "$SD_REF" && find config game resources.o2r -type f ! -name fps_counter -exec md5sum {} + |
    awk '{print toupper($1)" "$2}' | sort -k2) > "$work/ref.md5"

remote_md5 > "$work/before.md5"
if ! diff -q "$work/ref.md5" "$work/before.md5" > /dev/null; then
    echo "  FAIL: Eden's SD differs from the reference (corrupted SD; recopy before testing):"
    diff "$work/ref.md5" "$work/before.md5" | grep '^[<>]' | head -5 | sed 's/^/    /'
    exit 1
fi

scp -q "$here/eden_smoke.ps1" "$host:eden_smoke.ps1"
scp -q "$nro" "$host:eden_smoke.nro"
ssh_ "Get-Process eden -EA 0 | Stop-Process -Force -EA 0;
  Copy-Item \$env:USERPROFILE\\eden_smoke.nro \"\$env:APPDATA\\eden\\sdmc\\switch\\oot3dre\\oot3d_native_game.nro\" -Force;
  Remove-Item \$env:USERPROFILE\\eden-smoke\\done.txt -EA 0;
  schtasks /Create /F /TN EdenSmoke /SC ONCE /ST 23:59 /IT /TR \"powershell -ExecutionPolicy Bypass -File \$env:USERPROFILE\\eden_smoke.ps1 -Seconds $seconds\" | Out-Null;
  schtasks /Run /TN EdenSmoke | Out-Null"
for _ in $(seq $(( (seconds + 90) / 10 ))); do
    sleep 10
    ssh_ 'Test-Path $env:USERPROFILE\eden-smoke\done.txt' | grep -q True && break
done
report="$(ssh_ 'Get-Content $env:USERPROFILE\eden-smoke\result.txt,$env:USERPROFILE\eden-smoke\progress.txt -EA 0')"
boot="$(ssh_ 'Get-Content $env:USERPROFILE\eden-smoke\boot-status.txt -EA 0' | tr -d '\r')"
echo "$report" | sed 's/^/  /'

failed=0
if echo "$boot" | grep -q '^status=failed'; then
    echo "  FAIL: boot-status status=failed: $(echo "$boot" | grep '^detail=' | cut -c8-)"; failed=1
fi
echo "$report" | grep -q 'alive_at_end=True' || { echo "  FAIL: Eden closed early"; failed=1; }
frames="$(echo "$report" | grep -oE 'host_frames=[0-9]+' | tail -1 | cut -d= -f2)"
(( ${frames:-0} >= MIN_FRAMES )) || { echo "  FAIL: only ${frames:-0} frames (minimum $MIN_FRAMES)"; failed=1; }
remote_md5 > "$work/after.md5"
if ! diff -q "$work/before.md5" "$work/after.md5" > /dev/null; then
    echo "  FAIL: the game changed files it should only read:"
    diff "$work/before.md5" "$work/after.md5" | grep '^>' | head -5 | sed 's/^/    /'
    failed=1
fi
((failed)) && exit 1
echo "  ok: ${frames} frames in ${seconds} s, SD intact"
