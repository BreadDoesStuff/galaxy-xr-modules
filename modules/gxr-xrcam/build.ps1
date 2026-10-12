# Builds xrcamd and packages the KernelSU module zip.
# Requires Android NDK (r27c used). Set $env:NDK or edit below.
$ErrorActionPreference = 'Stop'
$ndk = if ($env:NDK) { $env:NDK } else { "$PSScriptRoot\tools\android-ndk-r27c" }
$cc = "$ndk\toolchains\llvm\prebuilt\windows-x86_64\bin\aarch64-linux-android34-clang.cmd"
$out = "$PSScriptRoot\module\system\bin"
New-Item -ItemType Directory -Force "$PSScriptRoot\module\bin" | Out-Null
& $cc -O2 -Wall -Wno-misleading-indentation -o "$PSScriptRoot\module\bin\xrcamd" `
    "$PSScriptRoot\src\xrcamd.c" "$PSScriptRoot\src\gpu.c" `
    -ldl -lnativewindow -ljnigraphics -lEGL -lGLESv3 -landroid -llog -pthread
if ($LASTEXITCODE -ne 0) { throw "compile failed" }
$zip = "$PSScriptRoot\gxr-xrcam.zip"
if (Test-Path $zip) { Remove-Item $zip }
& "C:\Program Files\7-Zip\7z.exe" a -tzip $zip "$PSScriptRoot\module\*" | Out-Null
Write-Host "built $zip"
