#!/usr/bin/env bash
# deko3d backend smoke test on Eden on the Ryzen: like eden_smoke.sh, with
# config/renderer = deko3d on Eden's SD only during the test (removed at the end,
# and the SD's original .nro comes back). Fails if the game doesn't get past MIN_FRAMES
# frames, if Eden closes, if boot fails or if logs/deko3d.txt doesn't show
# the device created. Saves a screenshot to $OUT/screen.png.
#   tests/switch/eden_deko3d.sh path/to.nro [seconds]
#   REMOTE_NRO=C:/Users/flavin/x.nro tests/switch/eden_deko3d.sh - [seconds]  (already on the Ryzen)
set -euo pipefail
nro="${1:?path to the .nro (or - with REMOTE_NRO)}"; seconds="${2:-150}"
host="${EDEN_HOST:-ryzen}"; MIN_FRAMES="${MIN_FRAMES:-300}"
OUT="${OUT:-$(mktemp -d)}"
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ssh_() { ssh "$host" "$@" 2> >(grep -v -E 'WARNING|quantum|upgraded|store now' >&2); }
# The Ryzen's RAM is faulty: Eden and a build together rebooted it (24/10).
if ssh_ "wsl -d Ubuntu -u root -- pgrep -x ninja" 2>/dev/null | grep -q '[0-9]'; then
    echo "  FAIL: a build (ninja) is running on the Ryzen; Eden doesn't open alongside it"; exit 1
fi
if [ "$nro" != "-" ]; then
    scp -q -l 40000 "$nro" "$host:eden_deko3d.nro"; remote='$env:USERPROFILE\eden_deko3d.nro'
else
    remote="${REMOTE_NRO:?REMOTE_NRO}"
fi
scp -q "$here/eden_smoke.ps1" "$host:eden_smoke.ps1"
ssh_ "Get-Process eden -EA 0 | Stop-Process -Force -EA 0;
  \$sd=\"\$env:APPDATA\\eden\\sdmc\\switch\\oot3dre\";
  if (-not (Test-Path \$env:USERPROFILE\\eden_deko3d_orig.nro)) { Copy-Item \"\$sd\\oot3d_native_game.nro\" \$env:USERPROFILE\\eden_deko3d_orig.nro -Force }
  Copy-Item \"$remote\" \"\$sd\\oot3d_native_game.nro\" -Force;
  Set-Content -NoNewline -Path \"\$sd\\config\\renderer\" -Value 'deko3d';
  Remove-Item \$env:USERPROFILE\\eden-smoke\\done.txt -EA 0;
  schtasks /Create /F /TN EdenDeko3d /SC ONCE /ST 23:59 /IT /TR \"powershell -ExecutionPolicy Bypass -File \$env:USERPROFILE\\eden_smoke.ps1 -Seconds $seconds -Screenshot\" | Out-Null;
  schtasks /Run /TN EdenDeko3d | Out-Null"
for _ in $(seq $(( (seconds + 90) / 10 ))); do
    sleep 10
    ssh_ 'Test-Path $env:USERPROFILE\eden-smoke\done.txt' | grep -q True && break
done
ssh_ "\$sd=\"\$env:APPDATA\\eden\\sdmc\\switch\\oot3dre\";
  Remove-Item \"\$sd\\config\\renderer\" -EA 0;
  Copy-Item \$env:USERPROFILE\\eden_deko3d_orig.nro \"\$sd\\oot3d_native_game.nro\" -Force;
  Remove-Item \$env:USERPROFILE\\eden_deko3d_orig.nro;
  schtasks /Delete /F /TN EdenDeko3d | Out-Null"
for f in result.txt progress.txt boot-status.txt logs/deko3d.txt logs/perf.txt screen.png; do
    scp -q "$host:eden-smoke/$f" "$OUT/$(basename "$f")" 2>/dev/null || true
done
sed 's/^/  /' "$OUT/result.txt" "$OUT/progress.txt" 2>/dev/null | tr -d '\r' | tail -6
failed=0
grep -q '^status=failed' "$OUT/boot-status.txt" 2>/dev/null && { echo "  FAIL: boot-status status=failed: $(grep '^detail=' "$OUT/boot-status.txt" | cut -c8-)"; failed=1; }
grep -q 'alive_at_end=True' "$OUT/result.txt" 2>/dev/null || { echo "  FAIL: Eden closed early"; failed=1; }
grep -q 'deko3d: ready' "$OUT/deko3d.txt" 2>/dev/null || { echo "  FAIL: logs/deko3d.txt without 'deko3d: ready'"; failed=1; }
frames="$(tr -d '\r' < "$OUT/progress.txt" 2>/dev/null | grep -oE 'host_frames=[0-9]+' | tail -1 | cut -d= -f2)"
(( ${frames:-0} >= MIN_FRAMES )) || { echo "  FAIL: only ${frames:-0} frames (minimum $MIN_FRAMES)"; failed=1; }
echo "  output in $OUT"
((failed)) && exit 1
echo "  ok: ${frames} frames in ${seconds} s on deko3d"
