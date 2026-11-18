param(
    [Parameter(Mandatory = $true)][string]$RepoRoot,
    [Parameter(Mandatory = $true)][string]$OutputPath
)

$ErrorActionPreference = 'Stop'

function Invoke-GitQuiet {
    param([string[]]$Arguments)
    $prev = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try {
        $out = & git -C $RepoRoot @Arguments 2>$null
        return [pscustomobject]@{
            ExitCode = $LASTEXITCODE
            Output   = if ($null -eq $out) { '' } else { ($out | Out-String).Trim() }
        }
    } finally {
        $ErrorActionPreference = $prev
    }
}

function Escape-CString([string]$value) {
    return $value.Replace('\', '\\').Replace('"', '\"')
}

$hashRes = Invoke-GitQuiet @('rev-parse', '--short', 'HEAD')
if ($hashRes.ExitCode -ne 0 -or [string]::IsNullOrWhiteSpace($hashRes.Output)) {
    $version = 'dev'
} else {
    $hash = Escape-CString $hashRes.Output
    $tagRes = Invoke-GitQuiet @('describe', '--tags', '--abbrev=0')
    if ($tagRes.ExitCode -eq 0 -and -not [string]::IsNullOrWhiteSpace($tagRes.Output)) {
        $tag = $tagRes.Output
        $aheadRes = Invoke-GitQuiet @('rev-list', "$tag..HEAD", '--count')
        [int]$ahead = 0
        if ($aheadRes.ExitCode -eq 0 -and [int]::TryParse($aheadRes.Output, [ref]$ahead) -and $ahead -gt 0) {
            $tagPart = "$tag+$ahead"
        } else {
            $tagPart = $tag
        }
    } else {
        $tagPart = 'dev'
    }
    # Use \u00b7 so the generated header stays pure 7-bit ASCII regardless of MSVC codepage.
    $version = "$(Escape-CString $tagPart) \u00b7 g$hash"
}

$content = "#pragma once`r`n#define AGSHOT_BUILD_VERSION L`"$version`"`r`n"

$fullOut = [System.IO.Path]::GetFullPath($OutputPath)
$outDir = [System.IO.Path]::GetDirectoryName($fullOut)
if (-not [string]::IsNullOrEmpty($outDir)) {
    [System.IO.Directory]::CreateDirectory($outDir) | Out-Null
}

if (-not (Test-Path $fullOut) -or [System.IO.File]::ReadAllText($fullOut) -ne $content) {
    [System.IO.File]::WriteAllText($fullOut, $content, [System.Text.UTF8Encoding]::new($false))
}
