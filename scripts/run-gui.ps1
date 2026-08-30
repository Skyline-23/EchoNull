param(
    [string]$BuildType = "Release"
)

$ErrorActionPreference = "Stop"

if (-not $env:AFX_SDK_ROOT) {
    throw "AFX_SDK_ROOT must point to the extracted NVIDIA Audio Effects SDK."
}

$afxRoot = [System.IO.Path]::GetFullPath($env:AFX_SDK_ROOT)
$runtimePaths = @(
    (Join-Path $afxRoot "bin"),
    (Join-Path $afxRoot "bin\external\cuda\bin"),
    (Join-Path $afxRoot "bin\external\openssl\bin"),
    (Join-Path $afxRoot "features\nvafxaec\bin")
)
$env:PATH = (($runtimePaths + @($env:PATH)) -join ";")

$exe = Join-Path $PSScriptRoot "..\build\$BuildType\EchoNull.exe"
if (-not (Test-Path -LiteralPath $exe)) {
    $exe = Join-Path $PSScriptRoot "..\build\EchoNull.exe"
}
if (-not (Test-Path -LiteralPath $exe)) {
    throw "EchoNull executable not found. Build the project first."
}

Start-Process -FilePath $exe -WorkingDirectory (Join-Path $PSScriptRoot "..")

