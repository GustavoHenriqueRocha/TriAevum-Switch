# Teste de fumaça do oot3dre no Eden (Windows). Roda na sessão interativa
# (tarefa agendada com /IT), abre o .nro, espera N s, fecha e junta os logs.
param([int]$Seconds = 180, [switch]$Screenshot)
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
# -Screenshot: a print of the session's screen (Eden's window) before closing.
if ($alive -and $Screenshot) {
  try {
    Add-Type -AssemblyName System.Windows.Forms, System.Drawing
    $b = [System.Windows.Forms.Screen]::PrimaryScreen.Bounds
    $bmp = New-Object System.Drawing.Bitmap $b.Width, $b.Height
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.CopyFromScreen($b.Location, [System.Drawing.Point]::Empty, $b.Size)
    $bmp.Save("$out\screen.png", [System.Drawing.Imaging.ImageFormat]::Png)
    $g.Dispose(); $bmp.Dispose()
  } catch { "screenshot: $_" | Out-File -Append "$out\progress.txt" }
}
if ($alive) { Stop-Process $p -Force }
Start-Sleep 2
Copy-Item "$sd\boot-status.txt" $out -ErrorAction SilentlyContinue
Copy-Item -Recurse "$sd\logs" $out -ErrorAction SilentlyContinue
"done" | Out-File "$out\done.txt"
