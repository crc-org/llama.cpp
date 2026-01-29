# VirtGPU Backend Service Connectivity Testing

This document explains how to test connectivity between WSL and the Windows VirtGPU Backend Service.

## Overview

The VirtGPU Backend Service runs on Windows (port 4660) and needs to be accessible from WSL2 environments. Two test scripts are provided to verify connectivity and troubleshoot issues.

## Test Scripts

### 1. Windows Firewall Test (`test-windows-firewall.ps1`)

**Run this on the Windows host** to check firewall configuration and service status.

```powershell
# Basic test
.\test-windows-firewall.ps1

# Test specific port
.\test-windows-firewall.ps1 -Port 4661

# Automatically fix firewall rules (run as Administrator)
.\test-windows-firewall.ps1 -Fix
```

**What it checks:**
- ✅ Service is running
- ✅ Port is listening
- ✅ Windows Firewall rules
- ✅ Local connectivity
- ✅ Network profile settings

### 2. WSL Connectivity Test (`test-wsl-connectivity.sh`)

**Run this from WSL** to test connectivity to the Windows host.

```bash
# Auto-detect Windows host IP and test
./test-wsl-connectivity.sh

# Test specific Windows host IP
./test-wsl-connectivity.sh 192.168.1.100
```

**What it checks:**
- ✅ Windows host reachability
- ✅ Port 4660 accessibility
- ✅ Shared memory directory (`/mnt/c/temp`)
- ✅ Basic service API communication

## Common Issues and Solutions

### Issue: Port 4660 Access Denied (Error 10048)

**Cause:** Another service is already using port 4660

**Solution:**
```powershell
# Check what's using the port
netstat -ano | findstr :4660

# Kill the process if needed
tasklist /FI "PID eq <process_id>"
taskkill /F /PID <process_id>
```

### Issue: WSL Cannot Connect to Windows Host

**Cause:** Windows Firewall blocking connections

**Solution:**
```powershell
# Run as Administrator to fix firewall
.\test-windows-firewall.ps1 -Fix

# Or manually create rule
New-NetFirewallRule -DisplayName "VirtGPU Backend Service" -Direction Inbound -Protocol TCP -LocalPort 4660 -Action Allow
```

### Issue: Network Profile is Public

**Cause:** Windows network set to Public (more restrictive)

**Solution:**
1. Go to Settings > Network & Internet
2. Select your network connection (Ethernet/WiFi)
3. Change Network profile to "Private"

### Issue: Shared Memory Directory Not Accessible

**Cause:** WSL cannot access Windows `C:\temp` directory

**Solution:**
```cmd
# Create directory on Windows
mkdir C:\temp

# Test from WSL
ls -la /mnt/c/temp
echo "test" > /mnt/c/temp/test.txt
```

## Testing Workflow

1. **Start the Windows service:**
   ```powershell
   .\VirtGPUWindowsBackend.exe console
   ```

2. **Test Windows-side configuration:**
   ```powershell
   .\test-windows-firewall.ps1
   ```

3. **Test from WSL:**
   ```bash
   ./test-wsl-connectivity.sh
   ```

4. **Fix issues as needed:**
   ```powershell
   # Fix firewall (run as Administrator)
   .\test-windows-firewall.ps1 -Fix
   ```

5. **Verify end-to-end connectivity:**
   ```bash
   # From WSL - should work after fixes
   ./test-wsl-connectivity.sh
   ```

## Expected Output

### Successful Windows Test:
```
=== Windows Firewall Test for VirtGPU Backend Service ===

1. Checking if VirtGPU Backend Service is running...
   [PASS] Service process found: VirtGPUWindowsBackend (PID: 1234)

2. Checking if port 4660 is listening...
   [PASS] Port 4660 is listening (PID: 1234)

3. Checking Windows Firewall rules for port 4660...
   [INFO] Found firewall rules for port 4660

4. Testing local connectivity to port 4660...
   [PASS] Can connect to localhost:4660

[SUCCESS] VirtGPU Backend Service appears to be running correctly
```

### Successful WSL Test:
```
=== WSL to Windows VirtGPU Backend Connectivity Test ===

1. Detecting Windows host IP from WSL...
   [INFO] Windows host IP detected: 192.168.1.100

2. Testing basic connectivity to Windows host...
   [PASS] Windows host is reachable via ping

3. Testing TCP connection to port 4660...
   [PASS] Port 4660 is open and accessible

4. Testing shared memory directory access...
   [PASS] Shared directory /mnt/c/temp exists
   [PASS] Can write to shared directory

5. Testing VirtGPU service communication...
   [PASS] Service responds correctly to JSON API calls

[SUCCESS] VirtGPU Windows Backend Service is accessible from WSL
          WSL clients can connect to: 192.168.1.100:4660
```

## Manual Testing Commands

### Windows (Command Prompt):
```cmd
# Test local connection
telnet localhost 4660

# Check listening ports
netstat -ano | findstr :4660

# Check firewall rules
netsh advfirewall firewall show rule name="VirtGPU Backend Service"
```

### WSL (Bash):
```bash
# Get Windows host IP
ip route show | grep default | awk '{print $3}'

# Test TCP connection
nc -z <windows_host_ip> 4660

# Test with timeout
timeout 5 nc -z <windows_host_ip> 4660 && echo "Connected" || echo "Failed"

# Manual telnet test
telnet <windows_host_ip> 4660
```