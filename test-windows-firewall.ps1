# Test script to check Windows Firewall configuration for VirtGPU Backend Service
# Run this on the Windows host to verify firewall settings

param(
    [int]$Port = 4660,
    [switch]$Fix
)

Write-Host "=== Windows Firewall Test for VirtGPU Backend Service ===" -ForegroundColor Cyan
Write-Host ""

# Test 1: Check if service is running
Write-Host "1. Checking if VirtGPU Backend Service is running..." -ForegroundColor Yellow
$ServiceProcess = Get-Process | Where-Object {$_.ProcessName -like "*VirtGPU*" -or $_.ProcessName -like "*WinApi*"}
if ($ServiceProcess) {
    Write-Host "   [PASS] Service process found: $($ServiceProcess.ProcessName) (PID: $($ServiceProcess.Id))" -ForegroundColor Green
    $ServiceRunning = $true
} else {
    Write-Host "   [WARN] No VirtGPU service process found" -ForegroundColor Red
    Write-Host "          Start with: .\VirtGPUWindowsBackend.exe console" -ForegroundColor Gray
    $ServiceRunning = $false
}
Write-Host ""

# Test 2: Check if port is listening
Write-Host "2. Checking if port $Port is listening..." -ForegroundColor Yellow
$ListeningPort = Get-NetTCPConnection -LocalPort $Port -ErrorAction SilentlyContinue | Where-Object {$_.State -eq "Listen"}
if ($ListeningPort) {
    Write-Host "   [PASS] Port $Port is listening (PID: $($ListeningPort.OwningProcess))" -ForegroundColor Green
    $PortListening = $true
} else {
    Write-Host "   [FAIL] Port $Port is not listening" -ForegroundColor Red
    $PortListening = $false
}
Write-Host ""

# Test 3: Check Windows Firewall rules
Write-Host "3. Checking Windows Firewall rules for port $Port..." -ForegroundColor Yellow

try {
    # Check for existing rules
    $ExistingRules = Get-NetFirewallRule | Where-Object {
        $_.DisplayName -like "*VirtGPU*" -or
        $_.DisplayName -like "*WinAPI*" -or
        $_.DisplayName -like "*$Port*"
    }

    if ($ExistingRules) {
        Write-Host "   [INFO] Found existing firewall rules:" -ForegroundColor Green
        foreach ($rule in $ExistingRules) {
            $ruleDetails = Get-NetFirewallPortFilter -AssociatedNetFirewallRule $rule -ErrorAction SilentlyContinue
            Write-Host "          - $($rule.DisplayName) ($($rule.Direction), $($rule.Action), Port: $($ruleDetails.LocalPort))" -ForegroundColor Gray
        }
    } else {
        Write-Host "   [WARN] No specific firewall rules found for VirtGPU service" -ForegroundColor Yellow
    }

    # Check if port is blocked by firewall
    $InboundRule = Get-NetFirewallRule -Direction Inbound | Get-NetFirewallPortFilter | Where-Object {$_.LocalPort -eq $Port}
    $OutboundRule = Get-NetFirewallRule -Direction Outbound | Get-NetFirewallPortFilter | Where-Object {$_.LocalPort -eq $Port}

    if ($InboundRule -or $OutboundRule) {
        Write-Host "   [INFO] Found firewall rules for port $Port" -ForegroundColor Green
        $FirewallConfigured = $true
    } else {
        Write-Host "   [WARN] No specific firewall rules for port $Port" -ForegroundColor Yellow
        $FirewallConfigured = $false
    }

} catch {
    Write-Host "   [ERROR] Cannot check firewall rules: $($_.Exception.Message)" -ForegroundColor Red
    Write-Host "          Try running as Administrator" -ForegroundColor Gray
    $FirewallConfigured = $false
}
Write-Host ""

# Test 4: Test local connectivity
Write-Host "4. Testing local connectivity to port $Port..." -ForegroundColor Yellow
try {
    $tcpClient = New-Object System.Net.Sockets.TcpClient
    $tcpClient.ReceiveTimeout = 3000
    $tcpClient.SendTimeout = 3000
    $tcpClient.Connect("127.0.0.1", $Port)

    if ($tcpClient.Connected) {
        Write-Host "   [PASS] Can connect to localhost:$Port" -ForegroundColor Green
        $tcpClient.Close()
        $LocalConnectivity = $true
    } else {
        Write-Host "   [FAIL] Cannot connect to localhost:$Port" -ForegroundColor Red
        $LocalConnectivity = $false
    }
} catch {
    Write-Host "   [FAIL] Cannot connect to localhost:$Port - $($_.Exception.Message)" -ForegroundColor Red
    $LocalConnectivity = $false
}
Write-Host ""

# Test 5: Check Windows network profile
Write-Host "5. Checking network profile settings..." -ForegroundColor Yellow
try {
    $NetworkProfile = Get-NetConnectionProfile
    foreach ($profile in $NetworkProfile) {
        $profileColor = if ($profile.NetworkCategory -eq "Public") { "Red" } else { "Green" }
        Write-Host "   [INFO] Network: $($profile.Name) - Category: $($profile.NetworkCategory)" -ForegroundColor $profileColor
    }

    # Check if any network is set to Public (more restrictive)
    $PublicNetworks = $NetworkProfile | Where-Object {$_.NetworkCategory -eq "Public"}
    if ($PublicNetworks) {
        Write-Host "   [WARN] Public networks detected - may block incoming connections" -ForegroundColor Yellow
        Write-Host "          Consider changing to Private for WSL connectivity" -ForegroundColor Gray
    }
} catch {
    Write-Host "   [WARN] Cannot check network profiles" -ForegroundColor Yellow
}
Write-Host ""

# Summary and recommendations
Write-Host "=== Summary and Recommendations ===" -ForegroundColor Cyan

$overallSuccess = $ServiceRunning -and $PortListening -and $LocalConnectivity

if ($overallSuccess) {
    Write-Host "[SUCCESS] VirtGPU Backend Service appears to be running correctly" -ForegroundColor Green
    Write-Host ""
    Write-Host "To test from WSL, run:" -ForegroundColor Gray
    Write-Host "   chmod +x test-wsl-connectivity.sh" -ForegroundColor Gray
    Write-Host "   ./test-wsl-connectivity.sh" -ForegroundColor Gray
} else {
    Write-Host "[ISSUES DETECTED] Service may not be accessible from WSL" -ForegroundColor Red
    Write-Host ""

    if (!$ServiceRunning) {
        Write-Host "• Start the service:" -ForegroundColor Yellow
        Write-Host "  .\VirtGPUWindowsBackend.exe console" -ForegroundColor Gray
        Write-Host ""
    }

    if (!$FirewallConfigured -or $Fix) {
        Write-Host "• Configure Windows Firewall:" -ForegroundColor Yellow
        Write-Host "  New-NetFirewallRule -DisplayName 'VirtGPU Backend Service' -Direction Inbound -Protocol TCP -LocalPort $Port -Action Allow" -ForegroundColor Gray
        Write-Host ""

        if ($Fix) {
            Write-Host "Applying firewall fix..." -ForegroundColor Yellow
            try {
                New-NetFirewallRule -DisplayName "VirtGPU Backend Service" -Direction Inbound -Protocol TCP -LocalPort $Port -Action Allow -ErrorAction Stop
                Write-Host "[APPLIED] Inbound firewall rule created for port $Port" -ForegroundColor Green
            } catch {
                Write-Host "[ERROR] Failed to create firewall rule: $($_.Exception.Message)" -ForegroundColor Red
                Write-Host "        Try running as Administrator" -ForegroundColor Gray
            }
        }
    }

    Write-Host "• Check if Windows is in Private network mode:" -ForegroundColor Yellow
    Write-Host "  Settings > Network & Internet > Ethernet/WiFi > Network profile: Private" -ForegroundColor Gray
    Write-Host ""
}

# Manual test instructions
Write-Host "=== Manual Test Commands ===" -ForegroundColor Cyan
Write-Host "Test from Windows command prompt:" -ForegroundColor Gray
Write-Host "   telnet localhost $Port" -ForegroundColor Gray
Write-Host ""
Write-Host "Test from WSL:" -ForegroundColor Gray
Write-Host "   # Get Windows IP: ip route show | grep default | awk '{print `$3}'" -ForegroundColor Gray
Write-Host "   # Test connection: nc -z <windows_ip> $Port" -ForegroundColor Gray
Write-Host ""

if ($Fix) {
    Write-Host "Re-run this script without -Fix to verify changes" -ForegroundColor Yellow
}