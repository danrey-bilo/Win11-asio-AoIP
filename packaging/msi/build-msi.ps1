param([Parameter(Mandatory)][string]$AsioSdkDir, [string]$BuildDirectory, [string]$OutputDirectory)
$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
if (-not $BuildDirectory) { $BuildDirectory = Join-Path $repo 'build/windows' }
if (-not $OutputDirectory) { $OutputDirectory = Join-Path $repo dist }
$native = Join-Path $BuildDirectory bin
$payload = Join-Path $repo 'build/msi/payload'
New-Item -ItemType Directory -Force $payload,$OutputDirectory | Out-Null
foreach ($file in 'PiAoipAsio.dll','PiAoipControl.exe','smoke_host.exe') {
    Copy-Item -LiteralPath (Join-Path $native $file) -Destination $payload
}
Copy-Item -LiteralPath (Join-Path $AsioSdkDir LICENSE.txt) -Destination (Join-Path $payload ASIO-SDK-LICENSE.txt)
Copy-Item -LiteralPath (Join-Path $repo LICENSE) -Destination (Join-Path $payload PROJECT-LICENSE.txt)
$body = (ConvertFrom-Markdown -Path (Join-Path $repo docs/BUILD.md)).Html
$body += (ConvertFrom-Markdown -Path (Join-Path $repo docs/ASIO-SDK.md)).Html
$html = @"
<!doctype html><html lang="ru"><meta charset="utf-8"><meta name="viewport" content="width=device-width, initial-scale=1">
<title>PiAoIP — Windows 11 x64</title><style>body{font:16px/1.65 Segoe UI,Arial,sans-serif;max-width:960px;margin:40px auto;padding:0 24px;color:#1c2940;background:#f6f8fc}h1,h2{line-height:1.25;color:#34235b}pre{white-space:pre-wrap;background:#e9edf5;padding:18px;border-radius:10px}table{border-collapse:collapse;width:100%}td,th{border:1px solid #ccd5e3;padding:9px;text-align:left}a{color:#563d93}</style>$body</html>
"@
[IO.File]::WriteAllText((Join-Path $payload INSTALL-RU.html),$html,[Text.UTF8Encoding]::new($false))
$wix = Join-Path ${env:ProgramFiles} 'WiX Toolset v7.0/bin/wix.exe'
if (-not (Test-Path $wix)) { $wix = (Get-Command wix -ErrorAction Stop).Source }
$msi = Join-Path $OutputDirectory PiAoIP-2.1.0-Windows11-x64.msi
& $wix build (Join-Path $PSScriptRoot PiAoIP.wxs) -arch x64 -culture ru-RU `
  -loc (Join-Path $PSScriptRoot ru-RU.wxl) -ext WixToolset.UI.wixext -ext WixToolset.Firewall.wixext `
  -d "Payload=$payload" -d "NoticeRtf=$(Join-Path $PSScriptRoot package-notice.rtf)" `
  -pdb (Join-Path $repo build/msi/PiAoIP.wixpdb) -o $msi
if ($LASTEXITCODE) { throw 'MSI build failed' }
Write-Output "Created $msi"
Get-FileHash -LiteralPath $msi
