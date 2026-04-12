param(
  [Parameter(Mandatory = $true)]
  [ValidateSet("fast-gates", "build", "analysis", "coverage", "nightly")]
  [string]$Profile
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

if (-not $IsWindows) {
  throw "[install-windows-tools] FAIL Windows runner required"
}

$script:PathExports = [System.Collections.Generic.HashSet[string]]::new(
  [System.StringComparer]::OrdinalIgnoreCase
)
$script:InstalledPackages = [System.Collections.Generic.HashSet[string]]::new(
  [System.StringComparer]::OrdinalIgnoreCase
)

function Get-NormalizedDirectory {
  param(
    [Parameter(Mandatory = $true)]
    [string]$PathValue
  )

  if ([string]::IsNullOrWhiteSpace($PathValue)) {
    return $null
  }

  $separator = [System.IO.Path]::DirectorySeparatorChar
  try {
    return [System.IO.Path]::GetFullPath($PathValue).TrimEnd($separator)
  } catch {
    return $PathValue.TrimEnd($separator)
  }
}

function Get-VisualStudioInstallRoots {
  $roots = [System.Collections.Generic.List[string]]::new()
  foreach ($year in @("2022", "2019")) {
    foreach ($edition in @("Enterprise", "Professional", "Community", "BuildTools")) {
      $root = Join-Path $env:ProgramFiles "Microsoft Visual Studio\$year\$edition"
      if (Test-Path $root -PathType Container) {
        $roots.Add($root)
      }
    }
  }
  return $roots.ToArray()
}

function Add-ToolDirectory {
  param(
    [Parameter(Mandatory = $true)]
    [string]$Directory
  )

  $normalized = Get-NormalizedDirectory -PathValue $Directory
  if ($null -eq $normalized) {
    return
  }

  if (-not (Test-Path $normalized -PathType Container)) {
    return
  }

  if (-not $script:PathExports.Add($normalized)) {
    return
  }

  $pathEntries = @(
    $env:Path -split ";" |
      Where-Object { -not [string]::IsNullOrWhiteSpace($_) } |
      ForEach-Object { Get-NormalizedDirectory -PathValue $_ }
  )
  if (-not ($pathEntries -contains $normalized)) {
    $env:Path = "$normalized;$env:Path"
  }

  if ($env:GITHUB_PATH) {
    $normalized | Out-File -FilePath $env:GITHUB_PATH -Encoding utf8 -Append
  }
}

function Refresh-ProcessPath {
  $segments = [System.Collections.Generic.List[string]]::new()
  foreach ($scope in @("Machine", "User")) {
    $value = [System.Environment]::GetEnvironmentVariable("Path", $scope)
    if (-not [string]::IsNullOrWhiteSpace($value)) {
      $segments.Add($value)
    }
  }
  if (-not [string]::IsNullOrWhiteSpace($env:Path)) {
    $segments.Add($env:Path)
  }
  $env:Path = ($segments -join ";")
}

function Find-Command {
  param(
    [Parameter(Mandatory = $true)]
    [string[]]$Names
  )

  foreach ($name in $Names) {
    $command = Get-Command $name -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($null -ne $command) {
      return $command
    }
  }

  return $null
}

function Invoke-ExternalCommand {
  param(
    [Parameter(Mandatory = $true)]
    [string]$FilePath,
    [Parameter(Mandatory = $true)]
    [string[]]$Arguments
  )

  & $FilePath @Arguments
  if ($LASTEXITCODE -ne 0) {
    throw "[install-windows-tools] FAIL ${FilePath} exited with code $LASTEXITCODE"
  }
}

$visualStudioRoots = Get-VisualStudioInstallRoots
$commonDirectories = @(
  (Join-Path $env:ProgramFiles "LLVM\bin"),
  (Join-Path ${env:ProgramFiles(x86)} "LLVM\bin"),
  (Join-Path $env:ProgramFiles "CMake\bin"),
  (Join-Path $env:ProgramData "chocolatey\bin"),
  (Join-Path $env:LOCALAPPDATA "Microsoft\WinGet\Links")
)
foreach ($vsRoot in $visualStudioRoots) {
  $commonDirectories += @(
    (Join-Path $vsRoot "VC\Tools\Llvm\x64\bin"),
    (Join-Path $vsRoot "Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin"),
    (Join-Path $vsRoot "Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja")
  )
}
foreach ($directory in $commonDirectories) {
  Add-ToolDirectory -Directory $directory
}

$llvmDirectories = @(
  (Join-Path $env:ProgramFiles "LLVM\bin"),
  (Join-Path ${env:ProgramFiles(x86)} "LLVM\bin")
) + ($visualStudioRoots | ForEach-Object {
  Join-Path $_ "VC\Tools\Llvm\x64\bin"
})
$cmakeDirectories = @(
  (Join-Path $env:ProgramFiles "CMake\bin")
) + ($visualStudioRoots | ForEach-Object {
  Join-Path $_ "Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin"
})
$ninjaDirectories = @(
  (Join-Path $env:ProgramData "chocolatey\bin"),
  (Join-Path $env:LOCALAPPDATA "Microsoft\WinGet\Links")
) + ($visualStudioRoots | ForEach-Object {
  Join-Path $_ "Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja"
})
$portableToolDirectories = @(
  (Join-Path $env:ProgramData "chocolatey\bin"),
  (Join-Path $env:LOCALAPPDATA "Microsoft\WinGet\Links")
)
$llvmPackages = @(
  @{ Provider = "winget"; Ids = @("LLVM.LLVM") }
  @{ Provider = "choco"; Ids = @("llvm") }
)

$commandSpecs = @{
  "clang++" = @{
    Names = @("clang++.exe", "clang++")
    Directories = $llvmDirectories
    Packages = $llvmPackages
  }
  "clang-tidy" = @{
    Names = @("clang-tidy.exe", "clang-tidy")
    Directories = $llvmDirectories
    Packages = $llvmPackages
  }
  "llvm-cov" = @{
    Names = @("llvm-cov.exe", "llvm-cov")
    Directories = $llvmDirectories
    Packages = $llvmPackages
  }
  "llvm-profdata" = @{
    Names = @("llvm-profdata.exe", "llvm-profdata")
    Directories = $llvmDirectories
    Packages = $llvmPackages
  }
  "cmake" = @{
    Names = @("cmake.exe", "cmake")
    Directories = $cmakeDirectories
    Packages = @(
      @{ Provider = "winget"; Ids = @("Kitware.CMake") }
      @{ Provider = "choco"; Ids = @("cmake") }
    )
  }
  "ninja" = @{
    Names = @("ninja.exe", "ninja")
    Directories = $ninjaDirectories
    Packages = @(
      @{ Provider = "winget"; Ids = @("Ninja-build.Ninja") }
      @{ Provider = "choco"; Ids = @("ninja") }
    )
  }
  "rg" = @{
    Names = @("rg.exe", "rg")
    Directories = $portableToolDirectories
    Packages = @(
      @{ Provider = "winget"; Ids = @("BurntSushi.ripgrep.MSVC", "BurntSushi.ripgrep") }
      @{ Provider = "choco"; Ids = @("ripgrep") }
    )
  }
  "shellcheck" = @{
    Names = @("shellcheck.exe", "shellcheck")
    Directories = $portableToolDirectories
    Packages = @(
      @{ Provider = "winget"; Ids = @("koalaman.shellcheck", "ShellCheck.ShellCheck") }
      @{ Provider = "choco"; Ids = @("shellcheck") }
    )
  }
}

$profileRequirements = @{
  "fast-gates" = @("rg", "shellcheck")
  "build" = @("clang++", "cmake", "ninja")
  "analysis" = @("clang++", "clang-tidy", "cmake", "ninja")
  "coverage" = @("clang++", "cmake", "llvm-cov", "llvm-profdata", "ninja")
  "nightly" = @("clang++", "cmake", "ninja")
}

function Resolve-CommandRequirement {
  param(
    [Parameter(Mandatory = $true)]
    [string]$CommandName
  )

  $spec = $commandSpecs[$CommandName]
  if ($null -eq $spec) {
    throw "[install-windows-tools] FAIL unknown command requirement: $CommandName"
  }

  foreach ($directory in $spec.Directories) {
    Add-ToolDirectory -Directory $directory
  }

  return Find-Command -Names $spec.Names
}

function Install-PackageViaProvider {
  param(
    [Parameter(Mandatory = $true)]
    [string]$Provider,
    [Parameter(Mandatory = $true)]
    [string]$PackageId
  )

  $cacheKey = "${Provider}:${PackageId}"
  if ($script:InstalledPackages.Contains($cacheKey)) {
    return
  }

  switch ($Provider) {
    "winget" {
      $winget = Get-Command "winget" -ErrorAction SilentlyContinue | Select-Object -First 1
      if ($null -eq $winget) {
        throw "winget unavailable"
      }
      Invoke-ExternalCommand -FilePath $winget.Source -Arguments @(
        "install",
        "--id", $PackageId,
        "--exact",
        "--source", "winget",
        "--accept-package-agreements",
        "--accept-source-agreements"
      )
    }
    "choco" {
      $choco = Get-Command "choco" -ErrorAction SilentlyContinue | Select-Object -First 1
      if ($null -eq $choco) {
        throw "choco unavailable"
      }
      Invoke-ExternalCommand -FilePath $choco.Source -Arguments @(
        "install",
        "-y",
        "--no-progress",
        $PackageId
      )
    }
    default {
      throw "unsupported provider: $Provider"
    }
  }

  $script:InstalledPackages.Add($cacheKey) | Out-Null
  Refresh-ProcessPath
}

function Ensure-CommandRequirement {
  param(
    [Parameter(Mandatory = $true)]
    [string]$CommandName
  )

  $resolved = Resolve-CommandRequirement -CommandName $CommandName
  if ($null -ne $resolved) {
    Write-Host "[install-windows-tools] FOUND $CommandName -> $($resolved.Source)"
    return
  }

  $spec = $commandSpecs[$CommandName]
  $failures = [System.Collections.Generic.List[string]]::new()
  foreach ($packageSpec in $spec.Packages) {
    $provider = [string]$packageSpec.Provider
    foreach ($packageId in $packageSpec.Ids) {
      try {
        Write-Host "[install-windows-tools] TRY $CommandName via $provider:$packageId"
        Install-PackageViaProvider -Provider $provider -PackageId $packageId
        $resolved = Resolve-CommandRequirement -CommandName $CommandName
        if ($null -ne $resolved) {
          Write-Host "[install-windows-tools] READY $CommandName -> $($resolved.Source)"
          return
        }
        $failures.Add("${provider}:${packageId} installed but $CommandName is still unresolved")
      } catch {
        $failures.Add("${provider}:${packageId} => $($_.Exception.Message)")
      }
    }
  }

  throw "[install-windows-tools] FAIL unable to provision $CommandName. Attempts: $($failures -join ' | ')"
}

foreach ($commandName in $profileRequirements[$Profile]) {
  Ensure-CommandRequirement -CommandName $commandName
}

Write-Host "[install-windows-tools] PASS $Profile"
