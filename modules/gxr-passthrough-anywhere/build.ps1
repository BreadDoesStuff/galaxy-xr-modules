# Build gxr_passthrough_anywhere KernelSU module zip
# needs: JDK 11+ (javac/java), r8.jar (com.android.tools:r8 from Google maven)
param(
  [string]$Jdk = "$PSScriptRoot\tools\jdk-25.0.4.1+1\bin",
  [string]$R8  = "$PSScriptRoot\tools\r8.jar"
)
$ErrorActionPreference = "Stop"
$out  = Join-Path $PSScriptRoot "out"
$cls  = Join-Path $out "classes"
Remove-Item -Recurse -Force $out -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force $cls | Out-Null

# framework stubs (compile-only, NOT packaged; real classes come from framework.jar at runtime)
$stubs = Join-Path $out "stubs"
New-Item -ItemType Directory -Force $stubs | Out-Null
& "$Jdk\javac.exe" -nowarn -Xlint:-options --release 8 -d $stubs (Get-ChildItem -Recurse (Join-Path $PSScriptRoot "stubs") -Filter *.java | ForEach-Object FullName)
if ($LASTEXITCODE) { throw "javac stubs failed" }
& "$Jdk\javac.exe" -nowarn -Xlint:-options --release 8 -cp $stubs -d $cls (Join-Path $PSScriptRoot "src\com\gxr\pt\Main.java")
if ($LASTEXITCODE) { throw "javac failed" }
$classFiles = Get-ChildItem -Recurse $cls -Filter *.class | ForEach-Object FullName
& "$Jdk\java.exe" -cp $R8 com.android.tools.r8.D8 --release --min-api 30 --classpath $stubs --output $out $classFiles
if ($LASTEXITCODE) { throw "d8 failed" }

$stage = Join-Path $out "module"
Copy-Item -Recurse (Join-Path $PSScriptRoot "module") $stage
Copy-Item (Join-Path $out "classes.dex") (Join-Path $stage "ptd.dex")

# zip with forward slashes + LF scripts
Get-ChildItem $stage -Filter *.sh | ForEach-Object {
  $t = [IO.File]::ReadAllText($_.FullName) -replace "`r`n", "`n"
  [IO.File]::WriteAllText($_.FullName, $t)
}
foreach ($f in "module.prop","config.conf") {
  $p = Join-Path $stage $f; $t = [IO.File]::ReadAllText($p) -replace "`r`n", "`n"; [IO.File]::WriteAllText($p, $t)
}
$zip = Join-Path $PSScriptRoot "gxr-passthrough-anywhere.zip"
Remove-Item $zip -ErrorAction SilentlyContinue
Add-Type -AssemblyName System.IO.Compression, System.IO.Compression.FileSystem
$z = [IO.Compression.ZipFile]::Open($zip, "Create")
Get-ChildItem -Recurse -File $stage | ForEach-Object {
  $rel = $_.FullName.Substring($stage.Length + 1).Replace('\', '/')
  [IO.Compression.ZipFileExtensions]::CreateEntryFromFile($z, $_.FullName, $rel) | Out-Null
}
$z.Dispose()
Write-Host "built $zip"
