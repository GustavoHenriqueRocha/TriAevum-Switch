#!/usr/bin/env bash
# Teste de fumaça no Eden da Ryzen: envia o .nro, abre por N s na sessão do Windows e reprova se
#  - o SD do Eden não bate com o SD de referência (SD corrompido, não é bug do build);
#  - o Eden fechou sozinho ou o jogo não passou de MIN_FRAMES quadros;
#  - o boot-status.txt terminou em status=failed (mostra o detail=);
#  - o jogo alterou arquivos que só deveria ler (config/, game/, resources.o2r).
#   tests/switch/eden_smoke.sh caminho/do.nro [segundos]
# SD de referência: $SD_REF (padrão ~/oot3d-work/sd/switch/oot3dre).
set -euo pipefail
nro="${1:?caminho do .nro}"; seconds="${2:-180}"
host="${EDEN_HOST:-ryzen}"; MIN_FRAMES="${MIN_FRAMES:-300}"
SD_REF="${SD_REF:-$HOME/oot3d-work/sd/switch/oot3dre}"
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
work="$(mktemp -d)"; trap 'rm -rf "$work"' EXIT
ssh_() { ssh "$host" "$@" 2> >(grep -v -E 'WARNING|quantum|upgraded' >&2); }

# Arquivos que o jogo só lê: md5 em maiúsculas + caminho relativo, ordenados.
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
    echo "  FALHA: SD do Eden difere da referência (SD corrompido; recopiar antes de testar):"
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
    echo "  FALHA: boot-status status=failed: $(echo "$boot" | grep '^detail=' | cut -c8-)"; failed=1
fi
echo "$report" | grep -q 'alive_at_end=True' || { echo "  FALHA: o Eden fechou antes do tempo"; failed=1; }
frames="$(echo "$report" | grep -oE 'host_frames=[0-9]+' | tail -1 | cut -d= -f2)"
(( ${frames:-0} >= MIN_FRAMES )) || { echo "  FALHA: só ${frames:-0} quadros (mínimo $MIN_FRAMES)"; failed=1; }
remote_md5 > "$work/after.md5"
if ! diff -q "$work/before.md5" "$work/after.md5" > /dev/null; then
    echo "  FALHA: o jogo alterou arquivos que só deveria ler:"
    diff "$work/before.md5" "$work/after.md5" | grep '^>' | head -5 | sed 's/^/    /'
    failed=1
fi
((failed)) && exit 1
echo "  ok: ${frames} quadros em ${seconds} s, SD intacto"
