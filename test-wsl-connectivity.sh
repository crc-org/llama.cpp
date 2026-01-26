#!/bin/bash
# Test script to check if Windows VirtGPU Backend Service is accessible from WSL

set -e

echo "=== WSL to Windows VirtGPU Backend Connectivity Test ==="
echo

# Configuration
WINDOWS_HOST_IP=""
WINDOWS_PORT="4660"
TEST_TIMEOUT="5"

# Auto-detect Windows host IP from WSL
echo "1. Detecting Windows host IP from WSL..."
if command -v ip >/dev/null 2>&1; then
    # Method 1: Use ip route (most reliable)
    WINDOWS_HOST_IP=$(ip route show | grep default | awk '{print $3}' | head -n1)
elif [ -f /etc/resolv.conf ]; then
    # Method 2: Parse resolv.conf (fallback)
    WINDOWS_HOST_IP=$(grep nameserver /etc/resolv.conf | awk '{print $2}' | head -n1)
fi

if [ -z "$WINDOWS_HOST_IP" ]; then
    echo "   [ERROR] Could not auto-detect Windows host IP"
    echo "   Please specify manually: $0 <windows_host_ip>"
    exit 1
fi

echo "   [INFO] Windows host IP detected: $WINDOWS_HOST_IP"
echo

# Allow manual override
if [ $# -eq 1 ]; then
    WINDOWS_HOST_IP="$1"
    echo "   [INFO] Using manually specified IP: $WINDOWS_HOST_IP"
    echo
fi

# Test 1: Basic ping test
echo "2. Testing basic connectivity to Windows host..."
if ping -c 3 -W 2 "$WINDOWS_HOST_IP" >/dev/null 2>&1; then
    echo "   [PASS] Windows host is reachable via ping"
else
    echo "   [WARN] Windows host ping failed (may be normal if ping disabled)"
fi
echo

# Test 2: Port connectivity test
echo "3. Testing TCP connection to port $WINDOWS_PORT..."
if command -v nc >/dev/null 2>&1; then
    # Using netcat
    if timeout "$TEST_TIMEOUT" nc -z "$WINDOWS_HOST_IP" "$WINDOWS_PORT" 2>/dev/null; then
        echo "   [PASS] Port $WINDOWS_PORT is open and accessible"
        CONNECTION_SUCCESS=true
    else
        echo "   [FAIL] Port $WINDOWS_PORT is not accessible"
        CONNECTION_SUCCESS=false
    fi
elif command -v telnet >/dev/null 2>&1; then
    # Using telnet as fallback
    if timeout "$TEST_TIMEOUT" bash -c "echo quit | telnet $WINDOWS_HOST_IP $WINDOWS_PORT" >/dev/null 2>&1; then
        echo "   [PASS] Port $WINDOWS_PORT is open and accessible"
        CONNECTION_SUCCESS=true
    else
        echo "   [FAIL] Port $WINDOWS_PORT is not accessible"
        CONNECTION_SUCCESS=false
    fi
else
    # Manual socket test using bash
    if timeout "$TEST_TIMEOUT" bash -c "exec 3<>/dev/tcp/$WINDOWS_HOST_IP/$WINDOWS_PORT && echo 'Connection successful' >&3 && exec 3>&-" >/dev/null 2>&1; then
        echo "   [PASS] Port $WINDOWS_PORT is open and accessible"
        CONNECTION_SUCCESS=true
    else
        echo "   [FAIL] Port $WINDOWS_PORT is not accessible"
        CONNECTION_SUCCESS=false
    fi
fi
echo

# Test 3: Check shared memory directory
echo "4. Testing shared memory directory access..."
SHARED_DIR="/mnt/c/temp"
if [ -d "$SHARED_DIR" ]; then
    echo "   [PASS] Shared directory $SHARED_DIR exists"

    # Test write access
    TEST_FILE="$SHARED_DIR/wsl_test_$(date +%s).tmp"
    if echo "test" > "$TEST_FILE" 2>/dev/null; then
        echo "   [PASS] Can write to shared directory"
        rm -f "$TEST_FILE" 2>/dev/null
    else
        echo "   [WARN] Cannot write to shared directory (check permissions)"
    fi
else
    echo "   [WARN] Shared directory $SHARED_DIR not found"
    echo "          Windows C:\\temp may not be accessible via WSL"
fi
echo

# Test 4: Basic service communication test
echo "5. Testing VirtGPU service communication..."
if [ "$CONNECTION_SUCCESS" = true ]; then
    # Simple JSON echo test
    JSON_REQUEST='{"api":"echo","request_id":1,"input":"WSL connectivity test"}'
    JSON_LENGTH=$(printf "%08x" ${#JSON_REQUEST} | sed 's/\(..\)/\\x\1/g')

    if command -v nc >/dev/null 2>&1; then
        RESPONSE=$(printf "$JSON_LENGTH$JSON_REQUEST" | nc -w 3 "$WINDOWS_HOST_IP" "$WINDOWS_PORT" 2>/dev/null | head -c 1024)
        if echo "$RESPONSE" | grep -q "WSL connectivity test"; then
            echo "   [PASS] Service responds correctly to JSON API calls"
        else
            echo "   [INFO] Service is listening but response format differs"
            echo "          Response: $(echo "$RESPONSE" | head -c 100)..."
        fi
    else
        echo "   [SKIP] Service communication test (nc not available)"
    fi
else
    echo "   [SKIP] Service communication test (port not accessible)"
fi
echo

# Summary
echo "=== Test Summary ==="
if [ "$CONNECTION_SUCCESS" = true ]; then
    echo "[SUCCESS] VirtGPU Windows Backend Service is accessible from WSL"
    echo "          WSL clients can connect to: $WINDOWS_HOST_IP:$WINDOWS_PORT"
else
    echo "[FAILURE] VirtGPU Windows Backend Service is NOT accessible from WSL"
    echo ""
    echo "Troubleshooting steps:"
    echo "1. Ensure the Windows service is running:"
    echo "   .\\VirtGPUWindowsBackend.exe console"
    echo ""
    echo "2. Check Windows Firewall (run on Windows host):"
    echo "   .\\test-windows-firewall.ps1"
    echo ""
    echo "3. Manually test with telnet from WSL:"
    echo "   telnet $WINDOWS_HOST_IP $WINDOWS_PORT"
fi
echo