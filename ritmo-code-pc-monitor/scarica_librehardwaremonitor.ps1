# Scarica LibreHardwareMonitor 0.9.6 (portatile, MPL-2.0) nella cartella LibreHardwareMonitor\
# mantenendo il file di configurazione di Ritmo Code (sensori completi, web server su 127.0.0.1:8085).
$ErrorActionPreference = "Stop"
$dir = Join-Path $PSScriptRoot "LibreHardwareMonitor"
$zip = Join-Path $env:TEMP "LibreHardwareMonitor-0.9.6.zip"
$cfg = Join-Path $dir "LibreHardwareMonitor.config"
$keep = if (Test-Path $cfg) { Get-Content $cfg -Raw } else { $null }
Invoke-WebRequest -UseBasicParsing "https://github.com/LibreHardwareMonitor/LibreHardwareMonitor/releases/download/v0.9.6/LibreHardwareMonitor.zip" -OutFile $zip
Expand-Archive -Path $zip -DestinationPath $dir -Force
Invoke-WebRequest -UseBasicParsing "https://raw.githubusercontent.com/LibreHardwareMonitor/LibreHardwareMonitor/v0.9.6/LICENSE" -OutFile (Join-Path $dir "LICENSE.txt")
if ($keep) { Set-Content -Path $cfg -Value $keep -Encoding UTF8 }
Write-Host "LibreHardwareMonitor pronto in $dir"
