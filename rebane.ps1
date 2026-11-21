<#
.SYNOPSIS
Builds and manually tests a selected RebaneOS ISO on Windows.
.DESCRIPTION
Build and Test delegate compilation to an existing WSL Linux distribution through
rebane.sh. Run launches an existing ISO without building. VirtualBox and Python 3
run natively on Windows through scripts/test-vm.py. No dependencies are installed
and no existing VM is deleted or forcibly powered off.

Standard uses the graphical installer: install manually, shut down the guest, then
let the helper inspect the disk and boot it. Linux and Browser are live ISO tests;
they do not install or claim installed-disk verification. Resume and Boot use the
recorded run metadata and do not require WSL or any build tools.
.PARAMETER Action
Build, Test (default: build then test), Run (existing ISO), Resume, or Boot.
.PARAMETER Variant
Standard, Linux, or Browser. When omitted for Build, Test, or Run, an interactive
console prompts; redirected input defaults to Standard.
.PARAMETER Jobs
Positive build parallelism, default 2.
.PARAMETER RunDir
Existing run directory required for Resume and Boot.
.PARAMETER TestRoot
Directory for new VM runs. Defaults to REBANE_TEST_ROOT, or build/vm-tests in this
repository. Relative paths are resolved against the caller's current directory.
.PARAMETER Distribution
Optional existing WSL distribution used for Build and Test.
.PARAMETER MakeArgs
Array of additional make arguments. Each element is passed unchanged through WSL
to rebane.sh after --. Only valid with Build and Test.
.PARAMETER Help
Displays help without checking or launching any dependencies.
.EXAMPLE
.\rebane.ps1 -Action Test -Variant Standard
.EXAMPLE
.\rebane.ps1 -Action Test -Variant Browser -Distribution Ubuntu -Jobs 4
.EXAMPLE
.\rebane.ps1 -Action Build -Variant Linux -MakeArgs @('-B', 'VARIABLE=value with spaces')
.EXAMPLE
.\rebane.ps1 -Action Run -Variant Linux -TestRoot 'D:\VM tests'
.EXAMPLE
.\rebane.ps1 -Action Resume -RunDir 'D:\VM tests\run-example'
.EXAMPLE
Get-Help .\rebane.ps1 -Full
#>
[CmdletBinding()]
param(
    [ValidateSet('Build', 'Test', 'Run', 'Resume', 'Boot')]
    [string]$Action = 'Test',

    [ValidateSet('Standard', 'Linux', 'Browser')]
    [string]$Variant,

    [ValidateRange(1, 2147483647)]
    [int]$Jobs = 2,

    [string]$RunDir,
    [string]$TestRoot,
    [string]$Distribution,

    [AllowEmptyCollection()]
    [AllowEmptyString()]
    [string[]]$MakeArgs = @(),

    [switch]$Help
)

if ($Help) {
    Get-Help -Name $PSCommandPath -Full
    exit 0
}

function Find-NativeApplication {
    param([string]$Name)
    $command = Get-Command -Name $Name -CommandType Application -ErrorAction SilentlyContinue |
        Select-Object -First 1
    if ($null -ne $command) {
        return $command.Source
    }
    return $null
}

function ConvertTo-WindowsArgument {
    param([AllowEmptyString()][string]$Value)
    if ($Value.Length -gt 0 -and $Value -notmatch '[\s"]') {
        return $Value
    }

    # Windows PowerShell 5.1 lacks ProcessStartInfo.ArgumentList. Quote using
    # Windows argv rules, preserving empty strings, quotes and trailing slashes.
    $quoted = New-Object System.Text.StringBuilder
    [void]$quoted.Append('"')
    $slashes = 0
    foreach ($character in $Value.ToCharArray()) {
        if ($character -eq [char]92) {
            $slashes++
            continue
        }
        if ($character -eq [char]34) {
            [void]$quoted.Append(('\' * (2 * $slashes + 1)))
        } else {
            [void]$quoted.Append(('\' * $slashes))
        }
        [void]$quoted.Append($character)
        $slashes = 0
    }
    [void]$quoted.Append(('\' * (2 * $slashes)))
    [void]$quoted.Append('"')
    return $quoted.ToString()
}

function Invoke-NativeApplication {
    param(
        [string]$Executable,
        [AllowEmptyCollection()][AllowEmptyString()][string[]]$Arguments,
        [string]$WorkingDirectory
    )

    $start = New-Object System.Diagnostics.ProcessStartInfo
    $start.FileName = $Executable
    $start.UseShellExecute = $false
    $start.WorkingDirectory = $WorkingDirectory
    if ($null -ne $start.PSObject.Properties['ArgumentList']) {
        foreach ($argument in $Arguments) {
            [void]$start.ArgumentList.Add($argument)
        }
    } else {
        $quoted = foreach ($argument in $Arguments) {
            ConvertTo-WindowsArgument -Value $argument
        }
        $start.Arguments = $quoted -join ' '
    }

    $process = New-Object System.Diagnostics.Process
    $process.StartInfo = $start
    try {
        if (-not $process.Start()) {
            throw "Could not start $Executable"
        }
        $process.WaitForExit()
        return $process.ExitCode
    } finally {
        $process.Dispose()
    }
}

function Find-Python3 {
    param([string]$WorkingDirectory)
    $candidates = @(
        @{ Name = 'py.exe'; Prefix = @('-3') },
        @{ Name = 'python3.exe'; Prefix = @() },
        @{ Name = 'python.exe'; Prefix = @() },
        @{ Name = 'python3'; Prefix = @() },
        @{ Name = 'python'; Prefix = @() }
    )
    foreach ($candidate in $candidates) {
        $executable = Find-NativeApplication -Name $candidate.Name
        if ($null -eq $executable) {
            continue
        }
        $probe = @($candidate.Prefix) + @('-c', 'import sys; sys.exit(0 if sys.version_info[0] == 3 else 1)')
        $status = Invoke-NativeApplication -Executable $executable -Arguments $probe -WorkingDirectory $WorkingDirectory
        if ($status -eq 0) {
            return [pscustomobject]@{
                Executable = $executable
                Prefix = @($candidate.Prefix)
            }
        }
    }
    throw 'Native Python 3 was not found. Make Python 3 (py -3, python3, or python) available on the Windows PATH; this launcher does not install it.'
}

function Get-AbsoluteFileSystemPath {
    param([string]$Path)
    $provider = $null
    $drive = $null
    $absolute = $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath(
        $Path, [ref]$provider, [ref]$drive)
    if ($provider.Name -ne 'FileSystem') {
        throw "Expected a filesystem path: $Path"
    }
    return $absolute
}

function Select-IsoVariant {
    $interactive = $false
    try {
        $interactive = [Environment]::UserInteractive -and -not [Console]::IsInputRedirected
    } catch {
        $interactive = $false
    }
    if (-not $interactive) {
        return 'standard'
    }
    while ($true) {
        $selection = Read-Host 'ISO variant: [1] Standard (default), [2] Linux, [3] Browser'
        switch ($selection.Trim()) {
            '' { return 'standard' }
            '1' { return 'standard' }
            '2' { return 'linux' }
            '3' { return 'browser' }
            default { Write-Host 'Choose 1, 2, or 3.' }
        }
    }
}

try {
    $repository = $PSScriptRoot
    $actionName = $Action.ToLowerInvariant()
    $isExistingRun = $actionName -eq 'resume' -or $actionName -eq 'boot'
    $needsBuild = $actionName -eq 'build' -or $actionName -eq 'test'

    if ($isExistingRun) {
        if ([string]::IsNullOrEmpty($RunDir)) {
            throw '-RunDir is required for Resume and Boot.'
        }
    } elseif ($PSBoundParameters.ContainsKey('RunDir')) {
        throw '-RunDir is only valid for Resume and Boot.'
    }
    if (-not $needsBuild -and $MakeArgs.Count -gt 0) {
        throw '-MakeArgs is only valid for Build and Test.'
    }

    if (-not $isExistingRun) {
        if ($PSBoundParameters.ContainsKey('Variant')) {
            $variantName = $Variant.ToLowerInvariant()
        } else {
            $variantName = Select-IsoVariant
        }
    }

    if ($needsBuild) {
        $wsl = Find-NativeApplication -Name 'wsl.exe'
        if ($null -eq $wsl) {
            throw 'wsl.exe was not found. Build and Test need an existing WSL Linux distribution with the RebaneOS build dependencies. Use -Distribution to select it; this launcher does not install or configure WSL.'
        }
        $buildArguments = @()
        if ($PSBoundParameters.ContainsKey('Distribution')) {
            if ([string]::IsNullOrWhiteSpace($Distribution)) {
                throw '-Distribution must name an existing WSL distribution.'
            }
            $buildArguments += @('--distribution', $Distribution)
        }
        $buildArguments += @('--cd', $repository, '--exec', 'bash', './rebane.sh', 'build',
            '--variant', $variantName, '--jobs', $Jobs.ToString([Globalization.CultureInfo]::InvariantCulture), '--')
        $buildArguments += $MakeArgs
        $status = Invoke-NativeApplication -Executable $wsl -Arguments $buildArguments -WorkingDirectory $repository
        if ($status -ne 0) {
            exit $status
        }
    }

    if (-not $isExistingRun) {
        $isoRelative = switch ($variantName) {
            'standard' { 'rockosdualbootv.1.iso' }
            'linux' { 'build/rebane-linux.iso' }
            'browser' { 'build/rebane-browser.iso' }
        }
        $iso = Join-Path -Path $repository -ChildPath $isoRelative
        if (-not (Test-Path -LiteralPath $iso -PathType Leaf)) {
            throw "Selected ISO is missing: $iso. Build it with -Action Build -Variant $variantName."
        }
        if ((Get-Item -LiteralPath $iso -ErrorAction Stop).Length -eq 0) {
            throw "Selected ISO is empty: $iso"
        }
        if ($actionName -eq 'build') {
            exit 0
        }
    }

    $helper = Join-Path -Path $repository -ChildPath 'scripts/test-vm.py'
    if (-not (Test-Path -LiteralPath $helper -PathType Leaf)) {
        throw "VM helper is missing: $helper. Restore scripts/test-vm.py from this repository."
    }
    if ($isExistingRun) {
        $runPath = Get-AbsoluteFileSystemPath -Path $RunDir
        if (-not (Test-Path -LiteralPath $runPath -PathType Container)) {
            throw "Run directory does not exist: $runPath"
        }
        $helperArguments = @($helper, $actionName, $runPath)
    } else {
        if ($PSBoundParameters.ContainsKey('TestRoot')) {
            if ([string]::IsNullOrEmpty($TestRoot)) {
                throw '-TestRoot must not be empty.'
            }
            $root = Get-AbsoluteFileSystemPath -Path $TestRoot
        } elseif (-not [string]::IsNullOrEmpty($env:REBANE_TEST_ROOT)) {
            $root = Get-AbsoluteFileSystemPath -Path $env:REBANE_TEST_ROOT
        } else {
            $root = Join-Path -Path $repository -ChildPath 'build/vm-tests'
        }
        $helperArguments = @($helper, 'new', '--variant', $variantName, '--iso', $iso, '--test-root', $root)
    }
    $python = Find-Python3 -WorkingDirectory $repository
    $status = Invoke-NativeApplication -Executable $python.Executable `
        -Arguments (@($python.Prefix) + $helperArguments) -WorkingDirectory $repository
    exit $status
} catch {
    [Console]::Error.WriteLine('ERROR: ' + $_.Exception.Message)
    exit 1
}
