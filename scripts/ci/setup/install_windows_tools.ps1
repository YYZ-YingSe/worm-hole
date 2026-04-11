param(
  [Parameter(Mandatory = $true)]
  [ValidateSet("build")]
  [string]$Profile
)

$ErrorActionPreference = "Stop"

function Test-CommandAvailable {
  param(
    [Parameter(Mandatory = $true)]
    [string]$Name
  )

  return $null -ne (Get-Command $Name -ErrorAction SilentlyContinue)
}

function Install-ChocoPackageIfMissing {
  param(
    [Parameter(Mandatory = $true)]
    [string]$CommandName,
    [Parameter(Mandatory = $true)]
    [string]$PackageName
  )

  if (Test-CommandAvailable -Name $CommandName) {
    Write-Host "[install-windows-tools] SKIP $PackageName ($CommandName already available)"
    return
  }

  choco install -y $PackageName
  if ($LASTEXITCODE -ne 0) {
    throw "[install-windows-tools] FAIL installing $PackageName"
  }
}

switch ($Profile) {
  "build" {
    Install-ChocoPackageIfMissing -CommandName "clang++" -PackageName "llvm"
    Install-ChocoPackageIfMissing -CommandName "ninja" -PackageName "ninja"
  }
}

$llvmBin = "C:\Program Files\LLVM\bin"
if (Test-Path $llvmBin) {
  if (-not ($env:Path -split ";" | Where-Object { $_ -eq $llvmBin })) {
    $env:Path = "$llvmBin;$env:Path"
  }
}

if ($env:GITHUB_PATH -and (Test-Path $llvmBin)) {
  $llvmBin | Out-File -FilePath $env:GITHUB_PATH -Encoding utf8 -Append
}

if (-not (Test-CommandAvailable -Name "clang++")) {
  throw "[install-windows-tools] FAIL clang++ not found after setup"
}

if (-not (Test-CommandAvailable -Name "ninja")) {
  throw "[install-windows-tools] FAIL ninja not found after setup"
}

Write-Host "[install-windows-tools] PASS $Profile"
