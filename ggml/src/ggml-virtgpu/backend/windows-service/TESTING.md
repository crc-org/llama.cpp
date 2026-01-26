# Windows API Remoting Testing Guide

This document describes how to validate the critical fixes implemented for Windows frontend<>backend API remoting.

## Critical Fixes Implemented

### ✅ Fix 1: Response Buffer Return
**Problem**: Backend was losing all APIR computation results
**Solution**: Backend now writes response data to `*_response.dat` files that clients read

**Test**: `test_response_data_handling()` - Verifies response files are created and contain data

### ✅ Fix 2: Buffer ID Collision Prevention
**Problem**: Multiple clients corrupted each other's buffer mappings
**Solution**: Per-client session management with `{session_id, buffer_id}` namespacing

**Test**: `test_concurrent_clients()` - Multiple clients using same buffer IDs simultaneously

### ✅ Fix 3: Dynamic Command Type Support
**Problem**: All APIR commands hardcoded as type 22
**Solution**: Extract command type from APIR binary data header

**Test**: `test_dynamic_command_types()` - Sends different command types (0, 5, 10, 22)

## Test Files

### Integration Test Suite
- **`test-windows-api-remoting.cpp`** - Comprehensive integration tests
- **`test-CMakeLists.txt`** - Build configuration for tests
- **`test-basic.cmd`** - Basic connectivity and setup verification

### Test Coverage

| Test | Purpose | Validates |
|------|---------|-----------|
| `test_basic_connectivity()` | Service connectivity | TCP socket, JSON protocol |
| `test_dynamic_command_types()` | Command type extraction | Dynamic APIR cmd_type parsing |
| `test_concurrent_clients()` | Multi-client safety | Buffer ID namespace isolation |
| `test_response_data_handling()` | Response data flow | Response file creation/cleanup |

## Running Tests

### Prerequisites
1. **Windows 10+** with Visual Studio 2019+
2. **vcpkg** with jsoncpp package installed:
   ```cmd
   vcpkg install jsoncpp:x64-windows
   ```
3. **VirtGPUWindowsBackend service** running on port 4660

### Basic Test
```cmd
# Quick connectivity check
test-basic.cmd
```

### Full Integration Tests
```cmd
# Build integration tests
mkdir build-tests
cd build-tests
cmake -f test-CMakeLists.txt .. -DCMAKE_TOOLCHAIN_FILE=C:/vcpkg/scripts/buildsystems/vcpkg.cmake
cmake --build . --config Release

# Run tests
./test-windows-api-remoting.exe
```

## Test Scenarios

### Scenario 1: Single Client APIR Processing
1. **Connect** to service on port 4660
2. **Create** shared memory file with APIR data
3. **Send** JSON request with dynamic command type
4. **Receive** JSON response with status and response file path
5. **Read** binary response data from response file
6. **Cleanup** request and response files

### Scenario 2: Concurrent Multi-Client Processing
1. **Launch** 3-5 clients simultaneously
2. **Each client** uses same buffer_id but different session
3. **Verify** no cross-client buffer corruption
4. **Verify** each client gets correct response
5. **Verify** session cleanup on disconnect

### Scenario 3: Error Handling Validation
1. **Test** invalid file paths
2. **Test** malformed JSON requests
3. **Test** oversized APIR data
4. **Test** network disconnection during processing
5. **Verify** graceful error responses

## Expected Test Results

### Success Criteria
- ✅ **All 4 integration tests PASS**
- ✅ **No buffer ID collision errors**
- ✅ **Response data correctly returned**
- ✅ **Dynamic command types processed**
- ✅ **Clean session cleanup on disconnect**

### Performance Benchmarks
- **Connection establishment**: < 100ms
- **APIR command processing**: < 1s for simple commands
- **Response file I/O**: < 50ms for typical response sizes
- **Session cleanup**: < 10ms per session

## Troubleshooting

### Common Issues

**Test fails with "Failed to connect to service"**
- Verify service is running: `sc query VirtGPUBackend`
- Check port availability: `netstat -an | findstr :4660`
- Start service: `sc start VirtGPUBackend`

**"Failed to create shared memory file"**
- Ensure `C:\temp\` directory exists and is writable
- Check disk space availability
- Verify WSL2 filesystem bridge: `ls /mnt/c/temp/` (from WSL2)

**"Command type mismatch"**
- Verify APIR data format is correct
- Check endianness of command type header
- Validate memcpy offset in client code

**"Buffer ID collision detected"**
- Check server logs for session management
- Verify concurrent client isolation
- Test with different buffer_id values

### Debug Mode

**Service Debug Mode**:
```cmd
# Stop service and run in console mode
sc stop VirtGPUBackend
VirtGPUWindowsBackend.exe console
```

**Client Debug Mode**:
```cpp
// Enable verbose logging in test code
#define DEBUG_VERBOSE 1
```

## Validation Checklist

Before marking fixes as complete:

- [ ] All 4 integration tests pass consistently
- [ ] No memory leaks detected (run with Application Verifier)
- [ ] Concurrent clients work without interference
- [ ] Response data integrity verified with checksum
- [ ] Session cleanup verified with handle/memory monitoring
- [ ] Error conditions handled gracefully
- [ ] Performance meets benchmark requirements
- [ ] Service starts/stops cleanly
- [ ] File cleanup removes all temporary files

## Integration with Linux Implementation

### Behavioral Parity Testing

Compare Windows vs Linux implementations:

1. **Same APIR commands** produce identical results
2. **Same error conditions** return equivalent error codes
3. **Same performance characteristics** within reasonable variance
4. **Same memory usage patterns** for equivalent operations

### Cross-Platform Test Data

Use identical test vectors on both platforms:
- Same APIR binary command data
- Same expected response formats
- Same error injection scenarios
- Same concurrent load patterns

This ensures the Windows backend provides equivalent functionality to the Linux VirGL renderer backend.