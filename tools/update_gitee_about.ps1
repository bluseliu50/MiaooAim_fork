param(
    [Parameter(Mandatory = $false)]
    [string]$Token = $env:GITEE_TOKEN,

    [Parameter(Mandatory = $false)]
    [string]$Owner = "gxp666111",

    [Parameter(Mandatory = $false)]
    [string]$Repo = "miaomiao"
)

$ErrorActionPreference = "Stop"

if ([string]::IsNullOrWhiteSpace($Token)) {
    throw "Missing Gitee token. Set `$env:GITEE_TOKEN first, then rerun this script."
}

$AboutPath = Join-Path (Split-Path -Parent $PSScriptRoot) "docs\gitee-about.md"
$About = Get-Content -LiteralPath $AboutPath -Encoding UTF8 -Raw
$Matches = [regex]::Matches($About, '```text\s*(.*?)\s*```',
                            [System.Text.RegularExpressions.RegexOptions]::Singleline)
if ($Matches.Count -lt 1) {
    throw "Could not find description text block in docs/gitee-about.md"
}
$Description = $Matches[0].Groups[1].Value.Trim()
$Homepage = if ($Matches.Count -ge 2) {
    $Matches[1].Groups[1].Value.Trim()
} else {
    "https://oshwhub.com/team_voosogmo/project_fxbcjhaa"
}

$Body = @{
    access_token = $Token
    description  = $Description
    homepage     = $Homepage
} | ConvertTo-Json -Compress

$Uri = "https://gitee.com/api/v5/repos/$Owner/$Repo"
$Resp = Invoke-RestMethod -Uri $Uri -Method Patch -ContentType "application/json; charset=utf-8" -Body $Body

[pscustomobject]@{
    ok          = $true
    full_name   = $Resp.full_name
    description = $Resp.description
    homepage    = $Resp.homepage
}
