param(
    [Parameter(Mandatory = $true)]
    [string]$SourceRoot
)

$ErrorActionPreference = "Stop"

$innerLayers = @(
    (Join-Path $SourceRoot "domain"),
    (Join-Path $SourceRoot "application")
)
$forbidden = @(
    '#include\s*[<"]Windows\.h[>"]',
    '#include\s*[<"]Audioclient\.h[>"]',
    '#include\s*[<"]Mmdeviceapi\.h[>"]',
    '#include\s*[<"]nvAudioEffects\.h[>"]',
    '#include\s*"infrastructure/'
)

$violations = Get-ChildItem -LiteralPath $innerLayers -Recurse -File -Include *.hpp,*.cpp |
    Select-String -Pattern $forbidden

if ($violations) {
    $violations | ForEach-Object { Write-Error $_.ToString() }
    throw "Clean Architecture dependency boundary violation detected."
}

Write-Output "Clean Architecture dependency boundaries passed."

