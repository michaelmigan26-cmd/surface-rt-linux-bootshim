# Writes a .img.xz directly to a physical USB drive on Windows.
# Hardcoded safety: only writes to SanDisk Cruzer (~7.45 GB) on disk 3.

$ErrorActionPreference = "Stop"
$logPath = "C:\jailbreak\write-image.log"
Start-Transcript -Path $logPath -Force

$src = "C:\jailbreak\linux\2023-05-03-raspios-bullseye-surface-rt-armhf.img.xz"
$xz = "C:\Program Files\Git\mingw64\bin\xz.exe"
$targetDiskNumber = 3
$expectedNamePattern = 'SanDisk Cruzer'
$uncompressedBytes = 4143972352  # 3952 MB

Write-Host "=== Pre-flight checks ==="

if (-not (Test-Path $src)) { Write-Host "ERROR: Source missing: $src" -ForegroundColor Red; Stop-Transcript; exit 1 }
if (-not (Test-Path $xz)) { Write-Host "ERROR: xz missing: $xz" -ForegroundColor Red; Stop-Transcript; exit 1 }

$disk = Get-Disk -Number $targetDiskNumber -ErrorAction SilentlyContinue
if (-not $disk) { Write-Host "ERROR: Disk $targetDiskNumber not found" -ForegroundColor Red; Stop-Transcript; exit 1 }
$actualGB = [math]::Round($disk.Size / 1GB, 2)
Write-Host "Target Disk ${targetDiskNumber}: $($disk.FriendlyName), $actualGB GB, BusType=$($disk.BusType)"
if ($disk.BusType -ne 'USB') { Write-Host "ERROR: Not USB" -ForegroundColor Red; Stop-Transcript; exit 1 }
if ($actualGB -lt 7 -or $actualGB -gt 8) { Write-Host "ERROR: Size out of range" -ForegroundColor Red; Stop-Transcript; exit 1 }
if ($disk.FriendlyName -notmatch $expectedNamePattern) { Write-Host "ERROR: Friendly name doesn't match '$expectedNamePattern'" -ForegroundColor Red; Stop-Transcript; exit 1 }

Write-Host "Pre-flight passed."
Write-Host ""
Write-Host "About to write $src to PhysicalDrive$targetDiskNumber ($($disk.FriendlyName), $actualGB GB)."
Write-Host "This will COMPLETELY ERASE the SanDisk."
Write-Host ""

Write-Host "Disabling automount to prevent mid-write volume re-enumeration..."
"automount disable" | diskpart | Out-Null
Start-Sleep -Seconds 1

Write-Host "Clearing partition table to release volume locks..."
try {
    Clear-Disk -Number $targetDiskNumber -RemoveData -RemoveOEM -Confirm:$false -ErrorAction Stop
    Write-Host "Disk cleared successfully."
    Start-Sleep -Seconds 2
} catch {
    Write-Host "Clear-Disk failed: $_" -ForegroundColor Yellow
    Write-Host "Trying diskpart clean fallback..."
    $dpScript = @"
select disk $targetDiskNumber
clean
exit
"@
    $dpScript | diskpart | Out-Host
    Start-Sleep -Seconds 2
}

Write-Host "Starting xz decompression and raw write..."
$psi = New-Object System.Diagnostics.ProcessStartInfo
$psi.FileName = $xz
$psi.Arguments = "-dc `"$src`""
$psi.UseShellExecute = $false
$psi.RedirectStandardOutput = $true
$psi.RedirectStandardError = $true
$psi.CreateNoWindow = $true

$proc = [System.Diagnostics.Process]::Start($psi)
$stdout = $proc.StandardOutput.BaseStream

$dst = "\\.\PhysicalDrive$targetDiskNumber"
$dstStream = $null
try {
    $dstStream = [System.IO.File]::Open($dst, [System.IO.FileMode]::Open, [System.IO.FileAccess]::Write, [System.IO.FileShare]::ReadWrite)
} catch {
    Write-Host "ERROR opening $dst : $_" -ForegroundColor Red
    Stop-Transcript; exit 1
}

$buffer = New-Object byte[] 4194304  # 4 MB
$totalBytes = 0L
$startTime = Get-Date
$lastReport = $startTime

try {
    while ($true) {
        $read = $stdout.Read($buffer, 0, $buffer.Length)
        if ($read -eq 0) { break }
        $dstStream.Write($buffer, 0, $read)
        $totalBytes += $read
        $now = Get-Date
        if (($now - $lastReport).TotalSeconds -ge 5) {
            $mb = [math]::Round($totalBytes / 1MB, 1)
            $pct = [math]::Round(100 * $totalBytes / $uncompressedBytes, 1)
            $rate = if (($now - $startTime).TotalSeconds -gt 0) { [math]::Round($totalBytes / 1MB / ($now - $startTime).TotalSeconds, 1) } else { 0 }
            Write-Host "[$([math]::Round(($now - $startTime).TotalSeconds))s] $mb MB / ~3952 MB ($pct%) @ $rate MB/s"
            $lastReport = $now
        }
    }
} finally {
    if ($dstStream) { $dstStream.Flush(); $dstStream.Close() }
    if ($stdout) { $stdout.Close() }
}

$proc.WaitForExit()
$mb = [math]::Round($totalBytes / 1MB, 1)
$elapsed = [math]::Round((Get-Date - $startTime).TotalSeconds, 1)
Write-Host ""
Write-Host "WRITE COMPLETE: $mb MB in $elapsed sec (xz exit=$($proc.ExitCode))"
Write-Host ""

Write-Host "Re-enabling automount..."
"automount enable" | diskpart | Out-Null
Start-Sleep -Seconds 2
try { Update-HostStorageCache; Start-Sleep -Seconds 2 } catch { Write-Host "Update-HostStorageCache not available" }

Write-Host ""
Write-Host "=== Post-write partition table ==="
try { Get-Partition -DiskNumber $targetDiskNumber -ErrorAction SilentlyContinue | Format-Table PartitionNumber, DriveLetter, @{N="SizeMB";E={[math]::Round($_.Size/1MB,1)}}, Type -AutoSize } catch { Write-Host "Couldn't read partitions: $_" }

Stop-Transcript
Write-Host ""
Write-Host "Log: $logPath"
Write-Host ""
Write-Host "Window will close in 30 seconds. Press Ctrl+C to keep it open."
Start-Sleep -Seconds 30
