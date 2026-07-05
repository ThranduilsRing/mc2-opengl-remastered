# sign_release.ps1 — sign MC2 release binaries with Azure Artifact Signing
# (formerly Trusted Signing).
#
# One-time machine setup (run once, needs winget + az cli):
#   winget install Microsoft.Azure.TrustedSigningClientTools
#   winget install Microsoft.AzureCLI          # if not present
#   az login                                    # browser auth, once per token lifetime
#
# Fill scripts/installer/signing-metadata.json (template below) with your
# account values, then:
#   powershell -File scripts\installer\sign_release.ps1 -Files <exe1>,<exe2>
#
# Typical release: sign mc2.exe + mc2-launcher.exe + "Mission Editor.exe"
# BEFORE compiling the installer, then sign mc2-setup.exe after ISCC.

param(
    [Parameter(Mandatory=$true)] [string[]]$Files,
    [string]$Metadata = "$PSScriptRoot\signing-metadata.json"
)

$ErrorActionPreference = "Stop"

if (-not (Test-Path $Metadata)) {
    Write-Error @"
Missing $Metadata. Create it with your Artifact Signing values:
{
  "Endpoint": "https://<region>.codesigning.azure.net",
  "CodeSigningAccountName": "<signing account name>",
  "CertificateProfileName": "<certificate profile name>"
}
"@
}

# Locate signtool (Windows SDK) and the Trusted Signing dlib.
$signtool = Get-ChildItem "C:\Program Files (x86)\Windows Kits\10\bin\*\x64\signtool.exe" |
    Sort-Object FullName -Descending | Select-Object -First 1
if (-not $signtool) { Write-Error "signtool.exe not found - install Windows 10/11 SDK (winget install Microsoft.WindowsSDK.10.0.26100)" }

$dlib = Get-ChildItem "$env:ProgramFiles\Microsoft Visual Studio\*\*\MSBuild\Microsoft\VisualStudio\v*\TrustedSigning\*\Azure.CodeSigning.Dlib.dll",
                      "${env:ProgramFiles(x86)}\Microsoft SDKs\TrustedSigning\*\Azure.CodeSigning.Dlib.dll",
                      "$env:LOCALAPPDATA\Microsoft\MicrosoftTrustedSigningClientTools\Azure.CodeSigning.Dlib.dll" -ErrorAction SilentlyContinue |
    Select-Object -First 1
if (-not $dlib) { Write-Error "Azure.CodeSigning.Dlib.dll not found - winget install Microsoft.Azure.TrustedSigningClientTools" }

foreach ($f in $Files) {
    if (-not (Test-Path $f)) { Write-Error "not found: $f" }
    & $signtool.FullName sign /v /fd SHA256 /tr "http://timestamp.acs.microsoft.com" /td SHA256 `
        /dlib $dlib.FullName /dmdf $Metadata $f
    if ($LASTEXITCODE -ne 0) { Write-Error "signing failed: $f" }
    & $signtool.FullName verify /pa /v $f
    if ($LASTEXITCODE -ne 0) { Write-Error "verify failed: $f" }
    Write-Host "SIGNED OK: $f"
}
