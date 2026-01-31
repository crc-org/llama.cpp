# Cache Coherency Analysis - Current State

## Summary
- The control path of the API Remoting is working correctly, command and reply.
- The inference overall isn't working. This means that the data of the data buffers isn't properly reaching the host side, or the host data isn't reaching the guest side.
- The command/reply buffers are opened and closed just around the API Remoting query.

## Proposed Solution for Data Buffers

We should have implemented a similar mechanism for the data buffers:

1. **Buffer Allocation Phase:**
   - The shared files are created when the app requests the buffer allocation, and memory mapped
   - The app uses memcpy to read/write into the buffers

2. **Computation Phase (backend_backend_graph_compute):**
   - **Guest side:** Unmap the buffer from the guest, close the FD
   - **Host side:** Open the files in the host, memory map them
   - **Host side:** Call the actual graph_compute method
   - **Host side:** Flush the files and unmap them from the host
   - **Guest side:** Remap them in the guest, and let the app read/write into it

3. **Other Operations:**
   - This same pattern applies to a few other operations that touch the buffers on the host side

## Key Insight
The data buffers should follow the same temporary file pattern as command/reply buffers - complete unmapping on one side before mapping on the other side to ensure cache coherency.

---

## ANALYSIS: What We Actually Implemented vs Proposed Solution

### ✅ What Works (Matches Proposal):
1. **Buffer Allocation**: Files are created when app requests buffer allocation
2. **App Usage**: App uses memcpy through buffer operations (via on-demand mapping)

### ❌ Key Discrepancies:

#### 1. **Buffer Allocation Phase**
- **Proposed**: Files created and immediately memory mapped
- **Actual**: Files created but NOT immediately mapped (`mapped_memory = NULL`)
- **Impact**: Deferred mapping instead of immediate mapping

#### 2. **Computation Phase - Missing Guest/Host Coordination**
- **Proposed**: Guest unmaps + closes FD, then Host opens fresh + maps
- **Actual**: Only Host-side unmap/remap within same process
- **Missing**: No coordination between Linux guest (WSL2) and Windows host
- **Missing**: No "close FD on guest side" step
- **Missing**: No "open fresh files on host side" step

#### 3. **Post-Computation**
- **Proposed**: Host flushes + unmaps, then Guest remaps
- **Actual**: No post-computation unmapping on host
- **Missing**: No guest-side remapping coordination

### 🎯 Root Cause Identified:

**We implemented unmap/remap within the Windows host process only, but the proposed solution requires coordination between:**
- **Linux Guest (WSL2)**: Where llama.cpp app runs and creates `/tmp/` files
- **Windows Host**: Where backend service runs and accesses `C:\temp\` files

**The cache coherency issue is between WSL2 filesystem bridge and Windows filesystem, NOT within Windows process itself.**

### 🚨 Critical Missing Piece:
We need to implement the **guest/host handoff pattern** like command/reply buffers:
- Guest closes file descriptors to flush to disk
- Host opens fresh file handles to see flushed data
- This forces cache coherency across the WSL2/Windows filesystem boundary