param(
  [Parameter(Mandatory = $true)]
  [ValidateSet("build")]
  [string]$Profile
)

$ErrorActionPreference = "Stop"

switch ($Profile) {
  "build" {
    choco install -y ripgrep llvm ninja
  }
}

if ($env:GITHUB_PATH) {
  "C:\Program Files\LLVM\bin" | Out-File -FilePath $env:GITHUB_PATH -Encoding utf8 -Append
}

Write-Host "[install-windows-tools] PASS $Profile"
