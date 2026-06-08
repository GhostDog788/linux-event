# Generic helper for reverting a VMware Workstation VM to a named snapshot
# before each debug session, so each run starts from a known-clean state.
#
# This is optional and intended as a starting point. Wire it into your own
# pre-debug routine if you find it useful.
#
# Example:
#   .\restore-snapshot.ps1 -VmxPath 'C:\VMs\target\target.vmx' `
#                          -Snapshot clean-debug `
#                          -StartAfterRestore

param(
    [Parameter(Mandatory = $true)]
    [string]$VmxPath,

    [Parameter(Mandatory = $true)]
    [string]$Snapshot,

    [string]$VmrunPath = "C:\Program Files (x86)\VMware\VMware Workstation\vmrun.exe",

    [switch]$StartAfterRestore
)

$ErrorActionPreference = "Stop"

if (-not (Test-Path -LiteralPath $VmrunPath)) {
    throw "vmrun.exe was not found at '$VmrunPath'"
}

if (-not (Test-Path -LiteralPath $VmxPath)) {
    throw "VMX path does not exist: '$VmxPath'"
}

Write-Host "Stopping VM if it is running..."
& $VmrunPath -T ws stop $VmxPath soft
if ($LASTEXITCODE -ne 0) {
    Write-Host "Soft stop failed or VM was not running; continuing to snapshot revert."
}

Write-Host "Reverting VM to snapshot '$Snapshot'..."
& $VmrunPath -T ws revertToSnapshot $VmxPath $Snapshot
if ($LASTEXITCODE -ne 0) {
    throw "Snapshot revert failed with exit code $LASTEXITCODE"
}

if ($StartAfterRestore) {
    Write-Host "Starting VM..."
    & $VmrunPath -T ws start $VmxPath
    if ($LASTEXITCODE -ne 0) {
        throw "VM start failed with exit code $LASTEXITCODE"
    }
}

Write-Host "Snapshot restore complete."
