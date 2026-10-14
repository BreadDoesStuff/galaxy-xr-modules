# Builds the mic boost daemon (src\ -> micd.dex) and packages module\ into the KernelSU module zip.
# Needs a JDK 11+ (JAVA_HOME or javac on PATH) and the Android SDK (android.jar + build-tools d8.jar).
# Text files get LF line endings; entry paths use '/'. Does not touch the headset.
param(
    [string]$Sdk = $(if ($env:ANDROID_HOME) { $env:ANDROID_HOME } else { "$env:LOCALAPPDATA\Android\Sdk" })
)
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.IO.Compression, System.IO.Compression.FileSystem
$root = $PSScriptRoot

$javac = if ($env:JAVA_HOME -and (Test-Path "$env:JAVA_HOME\bin\javac.exe")) { "$env:JAVA_HOME\bin\javac.exe" } else { (Get-Command javac.exe).Source }
$java  = Join-Path (Split-Path $javac) 'java.exe'
$androidJar = Get-ChildItem "$Sdk\platforms\android-*\android.jar" | Sort-Object FullName | Select-Object -Last 1
$d8Jar      = Get-ChildItem "$Sdk\build-tools\*\lib\d8.jar" | Sort-Object FullName | Select-Object -Last 1
if (-not $androidJar -or -not $d8Jar) { throw "android.jar / d8.jar not found under $Sdk" }

$out = "$root\out"
Remove-Item -Recurse -Force $out -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force "$out\stubs", "$out\classes" | Out-Null

# Hidden framework API stubs: compile-only, never packaged (real classes come from the framework).
& $javac -nowarn -Xlint:-options --release 8 -cp $androidJar.FullName -d "$out\stubs" `
    (Get-ChildItem -Recurse "$root\stubs" -Filter *.java | ForEach-Object FullName)
if ($LASTEXITCODE) { throw "javac stubs failed" }
& $javac -nowarn -Xlint:-options --release 8 -cp "$out\stubs;$($androidJar.FullName)" -d "$out\classes" `
    (Get-ChildItem -Recurse "$root\src" -Filter *.java | ForEach-Object FullName)
if ($LASTEXITCODE) { throw "javac failed" }
& $java -cp $d8Jar.FullName com.android.tools.r8.D8 --release --min-api 34 `
    --lib $androidJar.FullName --classpath "$out\stubs" --output $out `
    (Get-ChildItem -Recurse "$out\classes" -Filter *.class | ForEach-Object FullName)
if ($LASTEXITCODE) { throw "d8 failed" }

$src = "$root\module"
$zip = "$root\gxr-mic-gain-boost.zip"
if (Test-Path $zip) { Remove-Item $zip }
$z = [IO.Compression.ZipFile]::Open($zip, 'Create')
try {
    Get-ChildItem $src -Recurse -File | Where-Object Name -ne 'deploy.sh' | ForEach-Object {
        $name = $_.FullName.Substring($src.Length + 1).Replace('\', '/')
        $text = [IO.File]::ReadAllText($_.FullName) -replace "`r`n", "`n"
        $w = New-Object IO.StreamWriter($z.CreateEntry($name).Open(), (New-Object Text.UTF8Encoding $false))
        $w.Write($text); $w.Close()
    }
    [void][IO.Compression.ZipFileExtensions]::CreateEntryFromFile($z, "$out\classes.dex", 'micd.dex')
} finally { $z.Dispose() }
Write-Host "built $zip"
