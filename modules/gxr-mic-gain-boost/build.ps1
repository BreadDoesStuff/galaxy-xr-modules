# Packages src\ into the KernelSU module zip (LF line endings, '/' entry paths).
#   -Deploy : push + direct-deploy to /data/adb/modules via ADB (device ksud fork rejects `module install`).
param([switch]$Deploy)
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.IO.Compression, System.IO.Compression.FileSystem
$src = "$PSScriptRoot\module"
$zip = "$PSScriptRoot\gxr-mic-gain-boost.zip"
if (Test-Path $zip) { Remove-Item $zip }
$z = [IO.Compression.ZipFile]::Open($zip, 'Create')
try {
    Get-ChildItem $src -Recurse -File | Where-Object Name -ne 'deploy.sh' | ForEach-Object {
        $name = $_.FullName.Substring($src.Length + 1).Replace('\', '/')
        $text = [IO.File]::ReadAllText($_.FullName) -replace "`r`n", "`n"
        $w = New-Object IO.StreamWriter($z.CreateEntry($name).Open(), (New-Object Text.UTF8Encoding $false))
        $w.Write($text); $w.Close()
    }
} finally { $z.Dispose() }
Write-Host "built $zip"

if ($Deploy) {
    $adb = Resolve-Path "$PSScriptRoot\..\..\tools\platform-tools\adb.exe"
    $tmp = '/data/local/tmp/gxr-mic-gain-boost'
    & $adb shell "mkdir -p $tmp" | Out-Null
    & $adb push $zip "$tmp/m.zip" | Out-Null
    $ds = "$env:TEMP\gxr-mic-deploy.sh"
    [IO.File]::WriteAllText($ds, ([IO.File]::ReadAllText("$src\deploy.sh") -replace "`r`n", "`n"))
    & $adb push $ds "$tmp/deploy.sh" | Out-Null
    Remove-Item $ds
    & $adb shell "su -c 'ASH_STANDALONE=1 /data/adb/ksu/bin/busybox sh $tmp/deploy.sh $tmp/m.zip; rm -rf $tmp'"
}
