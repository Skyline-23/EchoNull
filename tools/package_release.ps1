[CmdletBinding()]
param(
  [Parameter(Mandatory = $true)]
  [string]$SdkRoot,

  [Parameter(Mandatory = $true)]
  [string]$BuildRoot,

  [string]$OutputRoot = ".",

  [ValidateRange(1, 4)]
  [int]$ThrottleLimit = 4
)

$ErrorActionPreference = "Stop"

$sdkPath = (Resolve-Path -LiteralPath $SdkRoot).Path
$buildPath = (Resolve-Path -LiteralPath $BuildRoot).Path
$outputPath = (Resolve-Path -LiteralPath $OutputRoot).Path
$packerPath = Join-Path $buildPath "echonull_bundle_packer.exe"
$pluginBasePath = Join-Path $buildPath "EchoNullPlugin.dll"
$setupStubPath = Join-Path $buildPath "EchoNullSetupStub.exe"
$smokeTestPath = Join-Path $buildPath "echonull_plugin_smoke_tests.exe"

foreach ($file in @(
    $packerPath,
    $pluginBasePath,
    $setupStubPath,
    $smokeTestPath
  )) {
  if (-not (Test-Path -LiteralPath $file -PathType Leaf)) {
    throw "A release packaging tool or base binary is missing: $file"
  }
}

$runtimeFiles = @(
  Get-ChildItem -LiteralPath (Join-Path $sdkPath "bin") `
    -Recurse -Filter *.dll -File
  Get-ChildItem -LiteralPath (Join-Path $sdkPath "features") `
    -Recurse -Filter *.dll -File |
    Where-Object { $_.Directory.Name -ieq "bin" }
)
$duplicateRuntimeNames = $runtimeFiles |
  Group-Object -Property Name |
  Where-Object { $_.Count -gt 1 }
if ($duplicateRuntimeNames) {
  throw "NvAFX runtime DLL names must be unique: $($duplicateRuntimeNames.Name -join ', ')"
}
$runtimeAssets = @($runtimeFiles | ForEach-Object {
    [pscustomobject]@{ Name = $_.Name; Path = $_.FullName }
  })

$licenseAssets = @(
  Join-Path $sdkPath "license/NVIDIA-Software-License-Agreement-2025.05.05.pdf"
  Join-Path $sdkPath "license/product-specific-terms-for-nvidia-ai-products-2025.05.05.pdf"
  Join-Path $sdkPath "features/nvafxaec/license/NVIDIA-Models-Community-License-2025-04-15.pdf"
) | Where-Object { Test-Path -LiteralPath $_ -PathType Leaf } |
  ForEach-Object {
    [pscustomobject]@{ Name = Split-Path -Leaf $_; Path = $_ }
  }

$variants = @(
  [pscustomobject]@{ Label = "Turing-RTX20"; Architecture = "turing" }
  [pscustomobject]@{ Label = "Ampere-RTX30"; Architecture = "ampere" }
  [pscustomobject]@{ Label = "Ada-RTX40"; Architecture = "ada" }
  [pscustomobject]@{ Label = "Blackwell-RTX50"; Architecture = "blackwell" }
)

$variants | ForEach-Object -Parallel {
  $ErrorActionPreference = "Stop"
  $variant = $_
  $sdk = $using:sdkPath
  $output = $using:outputPath
  $packer = $using:packerPath
  $pluginBase = $using:pluginBasePath
  $setupStub = $using:setupStubPath
  $smokeTest = $using:smokeTestPath
  $runtimes = $using:runtimeAssets
  $licenses = $using:licenseAssets
  $label = $variant.Label
  $architecture = $variant.Architecture
  $plugin = Join-Path $output "EchoNullPlugin-$label.dll"
  $setup = Join-Path $output "EchoNullSetup-$label.exe"

  Copy-Item -LiteralPath $pluginBase -Destination $plugin -Force
  $arguments = @($plugin)
  foreach ($runtime in $runtimes) {
    $arguments += @($runtime.Name, $runtime.Path)
  }
  $arguments += @(
    "aec_48k_$architecture.trtpkg",
    (Join-Path $sdk "features/nvafxaec/models/$architecture/aec_48k.trtpkg"),
    "denoiser_48k_$architecture.trtpkg",
    (Join-Path $sdk "features/nvafxdenoiser/models/$architecture/denoiser_48k.trtpkg")
  )
  foreach ($license in $licenses) {
    $arguments += @($license.Name, $license.Path)
  }

  & $packer @arguments
  if ($LASTEXITCODE -ne 0) {
    throw "Failed to package the $architecture plug-in."
  }
  & $smokeTest $plugin
  if ($LASTEXITCODE -ne 0) {
    throw "The $architecture plug-in failed its smoke test."
  }

  Copy-Item -LiteralPath $setupStub -Destination $setup -Force
  & $packer $setup "EchoNullPlugin.dll" $plugin
  if ($LASTEXITCODE -ne 0) {
    throw "Failed to package the $architecture installer."
  }
  Remove-Item -LiteralPath $plugin -Force
  $hash = (Get-FileHash -LiteralPath $setup -Algorithm SHA256).Hash.ToLowerInvariant()
  "$hash *$(Split-Path -Leaf $setup)" |
    Set-Content -LiteralPath "$setup.sha256" -Encoding ascii -NoNewline
  Write-Output "Packaged $label"
} -ThrottleLimit $ThrottleLimit

$expectedOutputs = foreach ($variant in $variants) {
  Join-Path $outputPath "EchoNullSetup-$($variant.Label).exe"
  Join-Path $outputPath "EchoNullSetup-$($variant.Label).exe.sha256"
}
foreach ($file in $expectedOutputs) {
  if (-not (Test-Path -LiteralPath $file -PathType Leaf)) {
    throw "A release output is missing: $file"
  }
}
