# Enable TCP + Shared Memory Mode for Windows API Remoting
# This script sets up file-based shared memory for zero-copy transfers

param(
    [string]$SharedMemoryPath = "C:\temp\winapi_shared_memory",
    [int]$SharedMemorySize = 8388608  # 8MB (8 * 1024 * 1024)
)

Write-Host "Setting up TCP + Shared Memory mode for Windows API Remoting..." -ForegroundColor Green
Write-Host "Shared memory file: $SharedMemoryPath" -ForegroundColor Yellow
Write-Host "Shared memory size: $($SharedMemorySize / 1024 / 1024) MB" -ForegroundColor Yellow

# Create temp directory if it doesn't exist
$tempDir = Split-Path $SharedMemoryPath -Parent
if (!(Test-Path $tempDir)) {
    Write-Host "Creating directory: $tempDir" -ForegroundColor Yellow
    New-Item -ItemType Directory -Path $tempDir -Force | Out-Null
    Write-Host "Created directory: $tempDir" -ForegroundColor Green
}

# Create the shared memory file
Write-Host "Creating shared memory file..." -ForegroundColor Yellow
try {
    # Create file with exact size needed
    $file = [System.IO.File]::Create($SharedMemoryPath)
    $file.SetLength($SharedMemorySize)
    $file.Close()

    Write-Host "Created shared memory file: $SharedMemoryPath" -ForegroundColor Green

    # Verify file size
    $actualSize = (Get-Item $SharedMemoryPath).Length
    Write-Host "File size: $actualSize bytes ($($actualSize / 1024 / 1024) MB)" -ForegroundColor Green

    # Set full permissions for the current user and SYSTEM
    $acl = Get-Acl $SharedMemoryPath
    $accessRule = New-Object System.Security.AccessControl.FileSystemAccessRule(
        $env:USERNAME, "FullControl", "Allow"
    )
    $acl.SetAccessRule($accessRule)

    # Add SYSTEM permission
    $systemAccessRule = New-Object System.Security.AccessControl.FileSystemAccessRule(
        "SYSTEM", "FullControl", "Allow"
    )
    $acl.SetAccessRule($systemAccessRule)

    Set-Acl -Path $SharedMemoryPath -AclObject $acl
    Write-Host "Set file permissions for shared memory access" -ForegroundColor Green

} catch {
    Write-Host "Error creating shared memory file: $_" -ForegroundColor Red
    exit 1
}

# Check WSL2 accessibility
Write-Host "`nChecking WSL2 accessibility..." -ForegroundColor Yellow
$wslPath = "/mnt/c/temp/winapi_shared_memory"
try {
    # Test if WSL can access the file
    $wslResult = wsl test -f $wslPath
    if ($LASTEXITCODE -eq 0) {
        Write-Host "WSL2 can access shared memory file at: $wslPath" -ForegroundColor Green
    } else {
        Write-Host "WSL2 cannot access file at: $wslPath" -ForegroundColor Red
        Write-Host "Make sure WSL2 is running and C: drive is mounted" -ForegroundColor Yellow
    }

    # Get file info from WSL perspective
    $wslStat = wsl stat $wslPath 2>$null
    if ($LASTEXITCODE -eq 0) {
        Write-Host "WSL2 file info:" -ForegroundColor Cyan
        Write-Host $wslStat -ForegroundColor Gray
    }
} catch {
    Write-Host "Could not test WSL2 accessibility: $_" -ForegroundColor Yellow
    Write-Host "This is normal if WSL2 is not currently running" -ForegroundColor Gray
}

Write-Host "`n=== Setup Complete ===" -ForegroundColor Green
Write-Host "TCP + Shared Memory mode is now enabled!" -ForegroundColor Green
Write-Host "`nNext steps:" -ForegroundColor Cyan
Write-Host "1. Restart the Windows API Remoting service" -ForegroundColor White
Write-Host "2. Test from WSL2 with: ./test_client" -ForegroundColor White
Write-Host "`nExpected behavior:" -ForegroundColor Cyan
Write-Host "- Client will fall back to TCP connection" -ForegroundColor White
Write-Host "- Shared memory will be detected: 'TCP + shared memory hybrid'" -ForegroundColor White
Write-Host "- Zero-copy transfers will be available for large buffers" -ForegroundColor White