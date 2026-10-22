# Teste de fumaça do oot3dre no Eden (Windows). Roda na sessão interativa
# (tarefa agendada com /IT), abre o .nro, espera N s, fecha e junta os logs.
param([int]$Seconds = 180)
$eden = "D:\Emuladores\Eden-Windows-v0.2.1-amd64-msvc-standard\eden.exe"
$sd = "$env:APPDATA\eden\sdmc\switch\oot3dre"
$out = "$env:USERPROFILE\eden-smoke"
Remove-Item -Recurse -Force $out -ErrorAction SilentlyContinue
New-Item -ItemType Directory $out | Out-Null
Remove-Item -Recurse -Force "$sd\logs", "$sd\boot-status.txt" -ErrorAction SilentlyContinue
# O jogo só grava logs/perf.txt se a pasta existir.
New-Item -ItemType Directory "$sd\logs" | Out-Null
$p = Start-Process $eden -ArgumentList "-g `"$sd\oot3d_native_game.nro`"" -PassThru
$t0 = Get-Date
while (((Get-Date) - $t0).TotalSeconds -lt $Seconds -and -not $p.HasExited) {
  Start-Sleep 15
  $hf = (Select-String -Path "$sd\logs\perf.txt" -Pattern "^host_frames=" -ErrorAction SilentlyContinue |
         Select-Object -Last 1).Line
  if ($hf) { $hf = $hf.Substring(0, [Math]::Min(120, $hf.Length)) }
  "$([int]((Get-Date) - $t0).TotalSeconds)s $hf" | Out-File -Append "$out\progress.txt"
  $p.Refresh()
}
$alive = -not $p.HasExited
"alive_at_end=$alive seconds=$([int]((Get-Date) - $t0).TotalSeconds) exit=$(if (-not $alive) { $p.ExitCode })" |
  Out-File "$out\result.txt"
if ($alive) { Stop-Process $p -Force }
Start-Sleep 2
Copy-Item "$sd\boot-status.txt" $out -ErrorAction SilentlyContinue
Copy-Item -Recurse "$sd\logs" $out -ErrorAction SilentlyContinue
"done" | Out-File "$out\done.txt"
