# Implementation Plan: Kinect Data Bridge for Dance Central

## Goal
Implement a "Data Bridge" in Xenia to inject forged `NUI_SKELETON_FRAME` data into guest memory, enabling dance scoring in titles like Dance Central.

## Technical Requirements
- **Header**: 0x30 bytes. Must contain a valid floor plane normal (e.g., 0, 1, 0, 0) and a monotonically increasing frame number.
- **Skeleton Data**: 6 blocks of 0x1C0 bytes each.
- **Joints**: 20 joints per skeleton, each a 16-byte vector (x, y, z, w) where w=1.0f. All floats must be big-endian.
- **Synchronization**: Data must be written during depth frame delivery in `CameraDevice::CompleteFrame`.
- **Target**: `src/xenia/kernel/xboxkrnl/xboxkrnl_pscam.cc`.

## Proposed Design

### 1. Data Structures
Define structures that mirror the expected guest memory layout. We will use `xe::be<float>` for big-endian floating point values.

```cpp
struct NUI_SKELETON_JOINT {
    xe::be<float> x, y, z, w;
};

struct NUI_SKELETON_DATA {
    uint32_t tracking_state; // 4 bytes
    uint32_t tracking_id;    // 4 bytes
    uint32_t enrollment_idx;  // 4 bytes
    uint32_t user_idx;        // 4 bytes
    NUI_SKELETON_JOINT joints[20]; // 20 * 16 = 320 bytes (0x140)
    uint32_t joint_states[20];     // 20 * 4 = 80 bytes (0x50)
    // Total: 0x1C0 bytes
};

struct NUI_SKELETON_FRAME_HEADER {
    xe::be<uint64_t> timestamp;      // 8 bytes
    xe::be<uint32_t> frame_number;   // 4 bytes
    uint32_t flags;                  // 4 bytes
    xe::be<float> floor_plane[4];    // 16 bytes (e.g., 0, 1, 0, 0)
    uint8_t padding[16];              // Pad to 0x30 bytes
};
```

### 2. Frame Generation Logic
- **Frame Counter**: Maintain a member variable `uint32_t skeleton_frame_count_` in `CameraDevice`.
- **Timestamp**: Use `std::chrono::steady_clock` converted to the game's expected units or a simple incrementing 64-bit value.
- **Floor Plane**: Fixed to `{0.0f, 1.0f, 0.0f, 0.0f}`.

### 3. Mapping Sensor Data
Since we are "forging" data for scoring, and the real Kinect SDK skeleton tracking is usually handled by the sensor/driver, Xenia needs to decide where the skeleton data comes from.
- For the initial implementation, we will inject a "dummy" valid skeleton (e.g., a standing person) to verify the scoring engine starts counting.
- Future iterations can map actual `UsbCamera` skeleton data if available.

### 4. Injection Point
In `CameraDevice::CompleteFrame`, specifically when `depth == true`:
1. Identify the target address (e.g., `0x43A48CC0`). This should be configurable via a CVAR (e.g., `kinect_skeleton_address`) to support different game versions.
2. Use `memory_->Write` or `xe::store_and_swap` to write the `NUI_SKELETON_FRAME_HEADER` and 6 `NUI_SKELETON_DATA` blocks.

### 5. Implementation Steps
1. **Define Structs**: Add the `NUI` structures to the anonymous namespace in `xboxkrnl_pscam.cc`.
2. **Add State**: Add `uint32_t skeleton_frame_count_` to the `CameraDevice` class.
3. **Implement `InjectSkeletonData`**: Create a private helper method in `CameraDevice` that handles the memory write.
4. **Integrate into `CompleteFrame`**: Call `InjectSkeletonData` inside the `if (depth)` block of `CompleteFrame`.
5. **Verify**: Use the existing `LogSkeleton` diagnostic to confirm the "no frame header found" message is replaced by actual header details.

## Verification Plan
1. **Log Check**: Run Dance Central. Monitor logs for "Skeleton diag".
2. **Header Validation**: Confirm `LogSkeleton` reports a valid header, slot, and frame number instead of "no frame header found".
3. **Gameplay Check**: Confirm that points begin to accumulate during dance sequences.
