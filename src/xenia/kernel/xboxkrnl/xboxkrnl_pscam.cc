/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

// PsCamDeviceRequest, answered with a Kinect v1 camera on the host (see
// hid/kinect/usb_camera.h). Calibration math is from libfreenect
// (src/registration.c, Apache-2.0).

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "xenia/base/cvar.h"
#include "xenia/base/logging.h"
#include "xenia/base/memory.h"
#include "xenia/base/mutex.h"
#include "xenia/base/threading.h"
#include "xenia/cpu/processor.h"
#include "xenia/hid/kinect/usb_camera.h"
#include "xenia/kernel/kernel_state.h"
#include "xenia/kernel/util/shim_utils.h"
#include "xenia/kernel/xboxkrnl/xboxkrnl_private.h"
#include "xenia/kernel/xthread.h"
#include "xenia/xbox.h"

DEFINE_bool(allow_kinect_camera, false,
            "Let titles use a connected Kinect for Xbox 360 (v1) camera.",
            "HID");

DEFINE_bool(kinect_use_usbdk, true,
            "Access a Kinect v1 camera through UsbDk, which borrows it from "
            "its Windows driver while the emulator runs. When disabled, the "
            "camera must use the WinUSB driver.",
            "HID");

namespace xe {
namespace kernel {
namespace xboxkrnl {

namespace {

// NUI skeleton frame structures
struct NUI_SKELETON_JOINT {
  xe::be<float> x;
  xe::be<float> y;
  xe::be<float> z;
  xe::be<float> w;
};

struct NUI_SKELETON_DATA {
  xe::be<uint32_t> tracking_state;
  xe::be<uint32_t> tracking_id;
  xe::be<uint32_t> enrollment_index;
  xe::be<uint32_t> user_index;
  uint8_t padding[16];
  NUI_SKELETON_JOINT joints[20];
  uint8_t quality_flags[256];
};

struct NUI_SKELETON_FRAME_HEADER {
  xe::be<uint64_t> timestamp;
  xe::be<uint32_t> frame_number;
  xe::be<uint32_t> flags;
  xe::be<float> floor_plane[4];
  uint8_t reserved[16];
};

enum class CameraRequestCode : uint32_t {
  kGetState = 0,
  kStop = 2,
  kReadFrame = 5,
  kVersion = 13,
  kRegistration = 14,
  kTimeSync = 15,
  kDepthTable = 24,
  kTiming = 25,
  kConfigure = 27,
  kUnknown33 = 33,
  kSetControls = 35,
  kGetControls = 36,
};

enum class CameraStream : uint32_t {
  kColor = 2,
  kDepth = 4,
};

// Device states reported through code 0.
constexpr uint32_t kDeviceStateAbsent = 0;
constexpr uint32_t kDeviceStateStreaming = 2;
constexpr uint32_t kDeviceStateReady = 4;
constexpr uint32_t kNoCompletionRoutine = 0xFFFFFFFF;
constexpr uint32_t kFrameWidth = 640;
constexpr uint32_t kFrameHeight = 480;
constexpr uint32_t kDepthFrameBytes = kFrameWidth * kFrameHeight * 2;
constexpr uint32_t kColorFrameBytes = kFrameWidth * kFrameHeight;
constexpr uint32_t kDepthTableEntries = 8192;
constexpr uint16_t kNoDepthRaw = 2047;
// Frame period in microseconds, returned for both streams by code 25.
constexpr uint32_t kFramePeriodUs = 33333;
// Host-side choices, not console values.
constexpr uint32_t kWorkerStackSize = 128 * 1024;
constexpr auto kWorkerWaitTimeout = std::chrono::milliseconds(100);
// Reported through code 13; 5.1 or later selects the registration request.
constexpr uint8_t kDriverVersionMajor = 5;
constexpr uint8_t kDriverVersionMinor = 1;

// libfreenect registration constants (src/registration.c).
constexpr uint16_t kShiftToDepthPixelConst = 10;
constexpr double kShiftToDepthConstOffset = 0.375;
constexpr double kParameterCoefficient = 4.0;
constexpr double kShiftScale = 10.0;
constexpr double kMaxDepthMillimetres = 10000.0;

// Request header. Codes 14 and 25 reuse +0x08 onwards for their replies.
struct X_PSCAM_REQUEST {
  xe::be<uint32_t> code;
  xe::be<uint32_t> unknown_04[3];
  xe::be<uint32_t> context;
  // Called with (request, status, bytes), 0 or kNoCompletionRoutine for none.
  xe::be<uint32_t> completion_routine;
};
static_assert_size(X_PSCAM_REQUEST, 0x18);

struct X_PSCAM_STATE_REQUEST {
  xe::be<uint32_t> code;
  xe::be<uint32_t> state;
};
static_assert_size(X_PSCAM_STATE_REQUEST, 0x8);

struct X_PSCAM_READ_FRAME_REQUEST {
  X_PSCAM_REQUEST header;
  xe::be<uint32_t> stream;
  xe::be<uint32_t> buffer_ptr;
  xe::be<uint32_t> buffer_size;
  xe::be<uint32_t> unknown_24;
  xe::be<uint32_t> frame_counter_ptr;
  // Color frames only.
  uint8_t settings_generation;
  uint8_t unknown_2D[3];
};
static_assert_size(X_PSCAM_READ_FRAME_REQUEST, 0x30);

struct X_PSCAM_CONTROLS_REQUEST {
  X_PSCAM_REQUEST header;
  xe::be<uint32_t> settings_generation;
};
static_assert_size(X_PSCAM_CONTROLS_REQUEST, 0x1C);

struct X_PSCAM_VERSION_REQUEST {
  xe::be<uint32_t> code;
  uint8_t major;
  uint8_t minor;
  uint8_t unknown_06[2];
};
static_assert_size(X_PSCAM_VERSION_REQUEST, 0x8);

struct X_PSCAM_REGISTRATION_REQUEST {
  xe::be<uint32_t> code;
  xe::be<uint32_t> unknown_04;
  xe::be<uint32_t> registration[29];
  uint8_t unknown_7C[0x100C];
  // Depth-to-color shift parameters, used like libfreenect's
  // freenect_init_depth_to_rgb.
  xe::be<double> reference_distance;
  xe::be<double> reference_pixel_size;
  // The title divides by this; libfreenect's equivalent factor is 1.
  xe::be<uint16_t> frame_width;
  xe::be<uint16_t> shift_to_depth_pixel_const;
  uint8_t unknown_109C[4];
  xe::be<double> shift_to_depth_const_offset;
  xe::be<double> dcmos_rcmos_distance;
};
static_assert_size(X_PSCAM_REGISTRATION_REQUEST, 0x10B0);

struct X_PSCAM_DEPTH_TABLE_REQUEST {
  xe::be<uint32_t> code;
  xe::be<uint32_t> unknown_04;
  xe::be<uint32_t> table_ptr;
  xe::be<uint32_t> entry_count;
};
static_assert_size(X_PSCAM_DEPTH_TABLE_REQUEST, 0x10);

struct X_PSCAM_TIMING_REQUEST {
  xe::be<uint32_t> code;
  // Microseconds.
  xe::be<uint32_t> depth_frame_period;
  xe::be<uint32_t> unknown_08[4];
  // Microseconds.
  xe::be<uint32_t> color_frame_period;
  xe::be<uint32_t> unknown_1C[2];
  // 0 disables time sync (code 15).
  xe::be<uint32_t> time_sync;
  xe::be<uint32_t> unknown_28[5];
};
static_assert_size(X_PSCAM_TIMING_REQUEST, 0x3C);

struct PendingRequest {
  uint32_t request_ptr = 0;
  // Read at request time, the title may overwrite the field afterwards. 0 for
  // none.
  uint32_t completion = 0;
  X_STATUS status = X_STATUS_SUCCESS;
  uint32_t bytes = 0;
  // Camera settings request that sets this generation once completed.
  bool new_settings = false;
  uint8_t generation = 0;
  // Frame reads that were queued when streaming was stopped (code 2), completed
  // as cancelled before this request.
  std::vector<PendingRequest> cancelled_reads;
};

class CameraDevice {
 public:
  explicit CameraDevice(KernelState* kernel_state)
      : kernel_state_(kernel_state), memory_(kernel_state->memory()) {
    if (!cvars::allow_kinect_camera) {
      return;
    }
    camera_ = hid::kinect::UsbCamera::Open(cvars::kinect_use_usbdk,
                                           [this]() { Notify(); });
    if (!camera_) {
      XELOGI("PsCamDeviceRequest: no Kinect camera found");
      return;
    }
    worker_thread_ = object_ref<XHostThread>(new XHostThread(
        kernel_state, kWorkerStackSize, 0,
        [this]() {
          WorkerMain();
          return 0;
        },
        kernel_state->GetSystemProcess()));
    worker_thread_->set_can_debugger_suspend(true);
    worker_thread_->set_name("Camera Worker");
    worker_thread_->Create();
  }

  ~CameraDevice() {
    worker_running_ = false;
    Notify();
    if (worker_thread_) {
      worker_thread_->Wait(0, 0, 0, nullptr);
      worker_thread_.reset();
    }
    // Closing the camera stops its streams and returns it to its driver.
    camera_.reset();
  }

  X_STATUS Request(uint32_t request_ptr) {
    if (!IsGuestRange(request_ptr, sizeof(X_PSCAM_REQUEST))) {
      return X_STATUS_INVALID_PARAMETER;
    }
    auto request = Guest<X_PSCAM_REQUEST>(request_ptr);
    const auto code = static_cast<CameraRequestCode>(uint32_t(request->code));
    if (!IsGuestRange(request_ptr, GetRequestSize(code))) {
      return X_STATUS_INVALID_PARAMETER;
    }
    uint32_t completion = request->completion_routine;
    if (completion == kNoCompletionRoutine) {
      completion = 0;
    }
    LogRequest(code, request_ptr);

    if (!camera_) {
      if (code == CameraRequestCode::kGetState) {
        uint32_t bytes = 0;
        return Handle(code, request_ptr, bytes);
      }
      // TODO(knuckleslee): Find the status the console returns without a
      // camera.
      return X_STATUS_DEVICE_NOT_CONNECTED;
    }

    if (code == CameraRequestCode::kReadFrame) {
      auto read = Guest<X_PSCAM_READ_FRAME_REQUEST>(request_ptr);
      const auto stream = static_cast<CameraStream>(uint32_t(read->stream));
      const uint32_t counter_ptr = read->frame_counter_ptr;
      if (!completion ||
          (stream != CameraStream::kDepth && stream != CameraStream::kColor) ||
          (counter_ptr && !IsGuestRange(counter_ptr, sizeof(uint32_t)))) {
        if (diag_failures_++ < 20) {
          XELOGE(
              "PsCam diag: read-frame request rejected: completion={:08X} "
              "stream={} counter_ptr={:08X}",
              completion, static_cast<uint32_t>(stream), counter_ptr);
        }
        return X_STATUS_INVALID_PARAMETER;
      }
      PendingRequest pending;
      pending.request_ptr = request_ptr;
      pending.completion = completion;
      {
        std::lock_guard<xe_mutex> lock(mutex_);
        streaming_ = true;
        (stream == CameraStream::kDepth ? depth_reads_ : color_reads_)
            .push_back(pending);
      }
      Notify();
      return X_STATUS_PENDING;
    }

    uint32_t bytes = 0;
    const X_STATUS status = Handle(code, request_ptr, bytes);
    if (!XSUCCEEDED(status) && diag_failures_++ < 20) {
      XELOGE("PsCam diag: code {} returned {:08X}",
             static_cast<uint32_t>(code), static_cast<uint32_t>(status));
    }
    PendingRequest pending;
    pending.request_ptr = request_ptr;
    pending.completion = completion;
    pending.status = status;
    pending.bytes = bytes;
    pending.new_settings =
        code == CameraRequestCode::kSetControls && XSUCCEEDED(status);
    pending.generation = requested_generation_;
    {
      std::lock_guard<xe_mutex> lock(mutex_);
      if (code == CameraRequestCode::kStop) {
        // TODO(knuckleslee): Find how the console completes queued reads on a
        // stop. They are completed with X_STATUS_CANCELLED here.
        pending.cancelled_reads.assign(depth_reads_.begin(),
                                       depth_reads_.end());
        pending.cancelled_reads.insert(pending.cancelled_reads.end(),
                                       color_reads_.begin(),
                                       color_reads_.end());
        depth_reads_.clear();
        color_reads_.clear();
        streaming_ = false;
        ++stop_count_;
      }
      if (!completion && pending.cancelled_reads.empty()) {
        return status;
      }
      completions_.push_back(std::move(pending));
    }
    Notify();
    return completion ? X_STATUS_PENDING : status;
  }

 private:
  template <typename T>
  T* Guest(uint32_t address) const {
    return memory_->TranslateVirtual<T*>(address);
  }

  // Checks that the first and last byte of a guest range are mapped.
  // TODO(knuckleslee): Find whether the console validates request ranges.
  // X_STATUS_INVALID_PARAMETER is returned for bad ones here.
  bool IsGuestRange(uint32_t address, size_t size) const {
    if (!address || !size || size > UINT32_MAX) {
      return false;
    }
    const uint32_t last_address = address + static_cast<uint32_t>(size - 1);
    return last_address >= address && memory_->LookupHeap(address) &&
           memory_->LookupHeap(last_address);
  }

  static size_t GetRequestSize(CameraRequestCode code) {
    size_t size = 0;
    switch (code) {
      case CameraRequestCode::kGetState:
        size = sizeof(X_PSCAM_STATE_REQUEST);
        break;
      case CameraRequestCode::kReadFrame:
        size = sizeof(X_PSCAM_READ_FRAME_REQUEST);
        break;
      case CameraRequestCode::kVersion:
        size = sizeof(X_PSCAM_VERSION_REQUEST);
        break;
      case CameraRequestCode::kRegistration:
        size = sizeof(X_PSCAM_REGISTRATION_REQUEST);
        break;
      case CameraRequestCode::kDepthTable:
        size = sizeof(X_PSCAM_DEPTH_TABLE_REQUEST);
        break;
      case CameraRequestCode::kTiming:
        size = sizeof(X_PSCAM_TIMING_REQUEST);
        break;
      case CameraRequestCode::kSetControls:
      case CameraRequestCode::kGetControls:
        size = sizeof(X_PSCAM_CONTROLS_REQUEST);
        break;
      default:
        break;
    }
    return std::max(size, sizeof(X_PSCAM_REQUEST));
  }

  // DIAGNOSTIC: logs the first three requests of each code (and of each frame
  // stream) with their raw words, so the fields the title fills in are visible.
  void LogRequest(CameraRequestCode code, uint32_t request_ptr) {
    uint32_t slot = static_cast<uint32_t>(code);
    if (code == CameraRequestCode::kReadFrame) {
      slot = 48 + (static_cast<uint32_t>(
                       Guest<X_PSCAM_READ_FRAME_REQUEST>(request_ptr)->stream) &
                   7);
    }
    if (slot >= xe::countof(request_counts_)) {
      return;
    }
    const uint32_t count = ++request_counts_[slot];
    if (count > 3) {
      return;
    }
    uint32_t size = 0x40;
    while (size > sizeof(X_PSCAM_REQUEST) && !IsGuestRange(request_ptr, size)) {
      size -= 4;
    }
    std::string words;
    for (uint32_t i = 0; i < size / 4; ++i) {
      char text[16];
      std::snprintf(text, sizeof(text), "%08X ",
                    static_cast<unsigned>(xe::load_and_swap<uint32_t>(
                        Guest<uint8_t>(request_ptr) + i * 4)));
      words += text;
    }
    XELOGI("PsCam diag: code={} slot={} #{} words: {}",
           static_cast<uint32_t>(code), slot, count, words);
  }

  // DIAGNOSTIC: the state of the title's own tilt manager, read from its
  // global data (address taken from the disassembly of Dance Central's tilt
  // callback, 545607D3 only). Logged when it changes.
  void LogTiltManager() {
    constexpr uint32_t kBase = 0x82969E30;
    if (!IsGuestRange(kBase, 0x1F0)) {
      return;
    }
    auto word = [&](uint32_t offset) {
      return static_cast<uint32_t>(*Guest<xe::be<uint32_t>>(kBase + offset));
    };
    char text[256];
    std::snprintf(text, sizeof(text),
                  "+50=%08X +18C=%08X +1A0=%08X | +60..84=%08X %08X %08X %08X "
                  "%08X %08X %08X %08X %08X %08X | +1D4..1EC=%08X %08X %08X "
                  "%08X %08X %08X %08X",
                  word(0x50), word(0x18C), word(0x1A0), word(0x60), word(0x64),
                  word(0x68), word(0x6C), word(0x70), word(0x74), word(0x78),
                  word(0x7C), word(0x80), word(0x84), word(0x1D4), word(0x1D8),
                  word(0x1DC), word(0x1E0), word(0x1E4), word(0x1E8),
                  word(0x1EC));
    if (tilt_manager_log_ != text || tilt_manager_logs_++ < 3) {
      tilt_manager_log_ = text;
      XELOGI("Kinect diag: tilt manager {}", text);
    }
  }

  // DIAGNOSTIC: raw depth statistics of the frame just written to the title.
  void LogDepthStats(uint32_t buffer_ptr) {
    const auto* depth = Guest<xe::be<uint16_t>>(buffer_ptr);
    const uint32_t pixels = kFrameWidth * kFrameHeight;
    uint32_t valid = 0;
    uint16_t min_raw = 0xFFFF;
    uint16_t max_raw = 0;
    for (uint32_t i = 0; i < pixels; ++i) {
      const uint16_t raw = depth[i];
      if (raw) {
        ++valid;
        min_raw = std::min(min_raw, raw);
        max_raw = std::max(max_raw, raw);
      }
    }
    XELOGI(
        "PsCam diag: depth frame #{} valid_pixels={} ({}%) raw_min={} "
        "raw_max={} reads_requested depth={} color={}",
        depth_delivered_, valid, valid * 100 / pixels, min_raw, max_raw,
        request_counts_[48 + static_cast<uint32_t>(CameraStream::kDepth)].load(),
        request_counts_[48 + static_cast<uint32_t>(CameraStream::kColor)]
            .load());
  }

  // Wakes the worker for a new request or camera frame.
  void Notify() { work_event_->Set(); }

  void InjectSkeletonData() {
    uint32_t address = cvars::kinect_skeleton_address;
    if (!IsGuestRange(address, sizeof(NUI_SKELETON_FRAME_HEADER) + 6 * sizeof(NUI_SKELETON_DATA))) {
      return;
    }

    NUI_SKELETON_FRAME_HEADER header;
    header.timestamp = xe::load_and_swap<uint64_t>(0); // Simplified timestamp
    header.frame_number = xe::load_and_swap<uint32_t>(++skeleton_frame_count_);
    header.flags = xe::load_and_swap<uint32_t>(0);
    header.floor_plane[0] = xe::load_and_swap<float>(0.0f);
    header.floor_plane[1] = xe::load_and_swap<float>(1.0f);
    header.floor_plane[2] = xe::load_and_swap<float>(0.0f);
    header.floor_plane[3] = xe::load_and_swap<float>(0.0f);
    std::memset(header.reserved, 0, sizeof(header.reserved));

    memory_->Write(address, &header, sizeof(header));

    for (int i = 0; i < 6; ++i) {
      NUI_SKELETON_DATA skeleton;
      std::memset(&skeleton, 0, sizeof(skeleton));

      // Only set the first skeleton as tracked for initial verification
      if (i == 0) {
        skeleton.tracking_state = xe::load_and_swap<uint32_t>(2); // Tracked
        skeleton.tracking_id = xe::load_and_swap<uint32_t>(1);

        for (int j = 0; j < 20; ++j) {
          skeleton.joints[j].x = xe::load_and_swap<float>(0.0f);
          skeleton.joints[j].y = xe::load_and_swap<float>(0.0f);
          skeleton.joints[j].z = xe::load_and_swap<float>(1.0f);
          skeleton.joints[j].w = xe::load_and_swap<float>(1.0f);
        }
      }

      memory_->Write(address + sizeof(NUI_SKELETON_FRAME_HEADER) + i * sizeof(NUI_SKELETON_DATA),
                     &skeleton, sizeof(skeleton));
    }
  }

  X_STATUS Handle(CameraRequestCode code, uint32_t request_ptr,
                  uint32_t& bytes) {
    switch (code) {
      case CameraRequestCode::kGetState:
        Guest<X_PSCAM_STATE_REQUEST>(request_ptr)->state =
            !camera_     ? kDeviceStateAbsent
            : streaming_ ? kDeviceStateStreaming
                         : kDeviceStateReady;
        return X_STATUS_SUCCESS;
      case CameraRequestCode::kSetControls:
        // Color frames carry the generation once this request has completed
        // (see Complete).
        // TODO(knuckleslee): The settings are not applied to the sensor.
        Guest<X_PSCAM_CONTROLS_REQUEST>(request_ptr)->settings_generation =
            ++requested_generation_;
        return X_STATUS_SUCCESS;
      case CameraRequestCode::kStop:
      case CameraRequestCode::kConfigure:
      case CameraRequestCode::kUnknown33:
      case CameraRequestCode::kGetControls:
      case CameraRequestCode::kTimeSync:
        return X_STATUS_SUCCESS;
      case CameraRequestCode::kVersion: {
        auto version = Guest<X_PSCAM_VERSION_REQUEST>(request_ptr);
        version->major = kDriverVersionMajor;
        version->minor = kDriverVersionMinor;
        return X_STATUS_SUCCESS;
      }
      case CameraRequestCode::kRegistration:
        WriteRegistration(request_ptr);
        return X_STATUS_SUCCESS;
      case CameraRequestCode::kDepthTable:
        return WriteDepthTable(request_ptr);
      case CameraRequestCode::kTiming: {
        auto timing = Guest<X_PSCAM_TIMING_REQUEST>(request_ptr);
        timing->depth_frame_period = kFramePeriodUs;
        std::memset(timing->unknown_08, 0, sizeof(timing->unknown_08));
        timing->color_frame_period = kFramePeriodUs;
        std::memset(timing->unknown_1C, 0, sizeof(timing->unknown_1C));
        timing->time_sync = 0;
        std::memset(timing->unknown_28, 0, sizeof(timing->unknown_28));
        return X_STATUS_SUCCESS;
      }
      default:
        XELOGE("PsCamDeviceRequest: unsupported code {}",
               static_cast<uint32_t>(code));
        return X_STATUS_NOT_SUPPORTED;
    }
  }

  // DIAGNOSTIC: looks in committed guest memory for the title's skeleton data.
  // The title's NUI library fills NUI_SKELETON_FRAME structures (layout taken
  // from the public Dance Central 3 decompilation): a 0x30 byte header (64-bit
  // timestamp, frame number, flags, floor plane, gravity normal) followed by 6
  // NUI_SKELETON_DATA of 0x1C0 bytes: tracking state, tracking id, enrollment
  // index, user index, position, then 20 joint positions as big-endian float
  // vectors (x, y, z, w = 1.0, meters), 20 joint tracking states and quality
  // flags. Joint order is that of the Kinect SDK (0 hip, 3 head, 7 left hand,
  // 11 right hand, 15 left foot, 19 right foot).
  struct SkeletonCandidate {
    uint32_t positions;  // Address of the 20 joint positions.
    uint32_t region_start;
    uint32_t region_end;
  };

  static float GuestFloat(const uint8_t* data) {
    const uint32_t bits = xe::load_and_swap<uint32_t>(data);
    float value;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
  }

  // Returns the number of plausible joints, or -1 if this isn't a skeleton.
  int EvaluateSkeleton(uint32_t address) {
    int plausible = 0;
    float hip_y = 0.0f;
    float head_y = 0.0f;
    for (uint32_t joint = 0; joint < 20; ++joint) {
      const uint8_t* vector = Guest<uint8_t>(address + joint * 16);
      if (xe::load_and_swap<uint32_t>(vector + 12) != 0x3F800000) {
        continue;
      }
      const float x = GuestFloat(vector);
      const float y = GuestFloat(vector + 4);
      const float z = GuestFloat(vector + 8);
      if (x > -2.5f && x < 2.5f && y > -2.0f && y < 2.5f && z > 0.5f &&
          z < 5.0f) {
        ++plausible;
      }
      if (joint == 0) {
        hip_y = y;
      } else if (joint == 3) {
        head_y = y;
      }
    }
    const float height = head_y - hip_y;
    if (plausible >= 12 && height > 0.3f && height < 1.0f) {
      return plausible;
    }
    return -1;
  }

  void ScanSkeletonCandidates() {
    skeleton_candidates_.clear();
    uint64_t scanned = 0;
    uint32_t address = 0x00010000;
    while (address < 0xA0000000 && skeleton_candidates_.size() < 8) {
      BaseHeap* heap = memory_->LookupHeap(address);
      if (!heap) {
        address += 0x10000;
        continue;
      }
      HeapAllocationInfo info = {};
      if (!heap->QueryRegionInfo(address, &info) || !info.region_size) {
        address += 0x1000;
        continue;
      }
      const uint64_t region_end =
          static_cast<uint64_t>(info.base_address) + info.region_size;
      const bool readable = (info.state & kMemoryAllocationCommit) &&
                            (info.protect & kMemoryProtectRead);
      if (readable) {
        scanned += info.region_size;
        const uint64_t first = std::max<uint64_t>(address, info.base_address);
        for (uint64_t at = (first + 15) & ~15ull; at + 0x1A0 <= region_end;
             at += 16) {
          const uint32_t candidate = static_cast<uint32_t>(at);
          if (xe::load_and_swap<uint32_t>(Guest<uint8_t>(candidate + 12)) !=
              0x3F800000) {
            continue;
          }
          if (EvaluateSkeleton(candidate) >= 0) {
            skeleton_candidates_.push_back(
                {candidate, info.base_address,
                 static_cast<uint32_t>(
                     std::min<uint64_t>(region_end, 0xFFFFFFFFull))});
            if (skeleton_candidates_.size() >= 8) {
              break;
            }
            at += 0x1A0 - 16;
          }
        }
      }
      address = region_end > address ? static_cast<uint32_t>(std::min<uint64_t>(
                                           region_end, 0xFFFFFFFFull))
                                     : address + 0x1000;
    }
    XELOGI("Skeleton diag: scanned {} MB, {} candidate(s)", scanned >> 20,
           skeleton_candidates_.size());
  }

  // Finds the NUI_SKELETON_FRAME header that contains this skeleton, trying
  // each of the 6 skeleton slots; a header is accepted if its floor plane has
  // a unit normal. Returns 0 if none is found.
  uint32_t FindFrameHeader(const SkeletonCandidate& candidate,
                           uint32_t* slot_out) {
    const uint32_t data = candidate.positions - 0x20;
    for (uint32_t slot = 0; slot < 6; ++slot) {
      if (data < 0x30 + slot * 0x1C0) {
        break;
      }
      const uint32_t header = data - 0x30 - slot * 0x1C0;
      if (header < candidate.region_start ||
          static_cast<uint64_t>(header) + 0xAB0 > candidate.region_end) {
        continue;
      }
      const uint8_t* bytes = Guest<uint8_t>(header);
      const float a = GuestFloat(bytes + 0x10);
      const float b = GuestFloat(bytes + 0x14);
      const float c = GuestFloat(bytes + 0x18);
      const float norm = std::sqrt(a * a + b * b + c * c);
      if (norm > 0.9f && norm < 1.1f) {
        *slot_out = slot;
        return header;
      }
    }
    return 0;
  }

  void LogSkeleton() {
    if (depth_delivered_ % 900 == 90 || (skeleton_candidates_.empty() &&
                                         depth_delivered_ % 180 == 90)) {
      ScanSkeletonCandidates();
    }
    if (skeleton_candidates_.empty() || depth_delivered_ % 30 != 0) {
      return;
    }
    for (size_t i = 0; i < skeleton_candidates_.size() && i < 3; ++i) {
      const SkeletonCandidate& candidate = skeleton_candidates_[i];
      const uint32_t base = candidate.positions;
      auto joint = [&](uint32_t index) {
        const uint8_t* vector = Guest<uint8_t>(base + index * 16);
        char text[64];
        std::snprintf(text, sizeof(text), "(%.2f %.2f %.2f)",
                      GuestFloat(vector), GuestFloat(vector + 4),
                      GuestFloat(vector + 8));
        return std::string(text);
      };
      uint32_t tracked = 0;
      uint32_t inferred = 0;
      for (uint32_t j = 0; j < 20; ++j) {
        const uint32_t state =
            xe::load_and_swap<uint32_t>(Guest<uint8_t>(base + 0x140 + j * 4));
        tracked += state == 2;
        inferred += state == 1;
      }
      const uint8_t* data = Guest<uint8_t>(base - 0x20);
      std::string header_text = "no frame header found";
      uint32_t slot = 0;
      if (const uint32_t header = FindFrameHeader(candidate, &slot)) {
        const uint8_t* bytes = Guest<uint8_t>(header);
        char text[200];
        std::snprintf(
            text, sizeof(text),
            "header@%08X slot=%u frame=%u time=%lld flags=%08X floor=(%.2f "
            "%.2f %.2f %.2f)",
            header, slot, xe::load_and_swap<uint32_t>(bytes + 8),
            static_cast<long long>(xe::load_and_swap<uint64_t>(bytes)),
            xe::load_and_swap<uint32_t>(bytes + 12), GuestFloat(bytes + 0x10),
            GuestFloat(bytes + 0x14), GuestFloat(bytes + 0x18),
            GuestFloat(bytes + 0x1C));
        header_text = text;
      }
      XELOGI(
          "Skeleton diag: t={}s @{:08X} state={} id={} quality={:08X} "
          "joints tracked={} inferred={} | hip={} head={} handL={} handR={} "
          "footL={} footR={} | {}",
          depth_delivered_ / 30, base, xe::load_and_swap<uint32_t>(data),
          xe::load_and_swap<uint32_t>(data + 4),
          xe::load_and_swap<uint32_t>(Guest<uint8_t>(base + 0x190)), tracked,
          inferred, joint(0), joint(3), joint(7), joint(11), joint(15),
          joint(19), header_text);
    }
  }

  void WriteRegistration(uint32_t request_ptr) {
    const hid::kinect::UsbCameraCalibration& calibration =
        camera_->calibration();
    auto request = Guest<X_PSCAM_REGISTRATION_REQUEST>(request_ptr);
    static_assert(sizeof(request->registration) ==
                  sizeof(calibration.registration));
    for (size_t i = 0; i < xe::countof(request->registration); ++i) {
      request->registration[i] =
          xe::load<uint32_t>(calibration.registration.data() + i * 4);
    }
    request->reference_distance = calibration.reference_distance;
    request->reference_pixel_size = calibration.reference_pixel_size;
    request->frame_width = static_cast<uint16_t>(kFrameWidth);
    request->shift_to_depth_pixel_const = kShiftToDepthPixelConst;
    request->shift_to_depth_const_offset = kShiftToDepthConstOffset;
    request->dcmos_rcmos_distance = calibration.dcmos_rcmos_distance;
  }

  X_STATUS WriteDepthTable(uint32_t request_ptr) {
    auto request = Guest<X_PSCAM_DEPTH_TABLE_REQUEST>(request_ptr);
    const uint32_t table_ptr = request->table_ptr;
    const uint32_t entries = request->entry_count;
    if (entries != kDepthTableEntries ||
        !IsGuestRange(table_ptr, entries * sizeof(uint16_t))) {
      return X_STATUS_INVALID_PARAMETER;
    }
    const hid::kinect::UsbCameraCalibration& calibration =
        camera_->calibration();
    auto table = Guest<xe::be<uint16_t>>(table_ptr);
    for (uint32_t raw = 0; raw < entries; ++raw) {
      uint16_t millimetres = 0;
      // Raw 0 is used for pixels without depth, see UnpackDepth.
      if (raw != 0 && raw < kNoDepthRaw) {
        // libfreenect freenect_raw_to_mm.
        const double fixed_ref_x =
            (raw - kParameterCoefficient * calibration.const_shift) /
                kParameterCoefficient -
            kShiftToDepthConstOffset;
        const double metric = fixed_ref_x * calibration.reference_pixel_size;
        const double value =
            kShiftScale * (metric * calibration.reference_distance /
                               (calibration.dcmos_emitter_distance - metric) +
                           calibration.reference_distance);
        if (value > 0 && value < kMaxDepthMillimetres) {
          millimetres = static_cast<uint16_t>(value);
        }
      }
      table[raw] = millimetres;
    }
    return X_STATUS_SUCCESS;
  }

  // Unpacks big-endian 11-bit samples into big-endian 16-bit words.
  static void UnpackDepth(const std::vector<uint8_t>& packed, uint8_t* out,
                          size_t pixels) {
    uint32_t bits = 0;
    int bit_count = 0;
    size_t in = 0;
    for (size_t i = 0; i < pixels; ++i) {
      while (bit_count < 11) {
        bits = (bits << 8) | packed[in++];
        bit_count += 8;
      }
      bit_count -= 11;
      uint16_t raw = static_cast<uint16_t>((bits >> bit_count) & 0x7FF);
      // Pixels without depth are passed as 0 rather than 2047.
      // TODO(knuckleslee): Verify what the console's camera driver returns.
      if (raw == kNoDepthRaw) {
        raw = 0;
      }
      xe::store_and_swap<uint16_t>(out + i * 2, raw);
    }
  }

  bool CompleteFrame(const PendingRequest& read, bool depth) {
    const uint32_t frame_bytes = depth ? kDepthFrameBytes : kColorFrameBytes;
    uint32_t& last_sequence = depth ? depth_sequence_ : color_sequence_;
    const uint32_t sequence =
        depth ? camera_->CopyDepthFrame(last_sequence, frame_)
              : camera_->CopyColorFrame(last_sequence, frame_);
    if (!sequence) {
      return false;
    }
    last_sequence = sequence;

    PendingRequest done = read;
    auto request = Guest<X_PSCAM_READ_FRAME_REQUEST>(read.request_ptr);
    const uint32_t buffer = request->buffer_ptr;
    if (request->buffer_size < frame_bytes ||
        !IsGuestRange(buffer, frame_bytes)) {
      if (diag_invalid_++ < 10) {
        XELOGE(
            "PsCam diag: {} frame request rejected: buffer_size={} needed={} "
            "buffer={:08X}",
            depth ? "depth" : "color",
            static_cast<uint32_t>(request->buffer_size), frame_bytes, buffer);
      }
      done.status = X_STATUS_INVALID_PARAMETER;
      Complete(done);
      return true;
    }
    if (depth) {
      UnpackDepth(frame_, Guest<uint8_t>(buffer), frame_bytes / 2);
      InjectSkeletonData();
    } else {
      std::memcpy(Guest<uint8_t>(buffer), frame_.data(), frame_bytes);
      request->settings_generation = settings_generation_;
    }
    const uint32_t counter_ptr = request->frame_counter_ptr;
    if (counter_ptr) {
      *Guest<xe::be<uint32_t>>(counter_ptr) = sequence;
    }
    if (depth) {
      ++depth_delivered_;
      if (depth_delivered_ <= 3 || depth_delivered_ % 90 == 0) {
        LogDepthStats(buffer);
        LogTiltManager();
      }
      LogSkeleton();
    } else {
      ++color_delivered_;
      if (color_delivered_ <= 3 || color_delivered_ % 90 == 0) {
        XELOGI("PsCam diag: color frames delivered={}", color_delivered_);
      }
    }
    done.bytes = frame_bytes;
    Complete(done);
    return true;
  }

  // Does not read the request, which the title may have reused.
  void Complete(const PendingRequest& request) {
    if (request.completion) {
      uint64_t args[] = {request.request_ptr,
                         static_cast<uint32_t>(request.status), request.bytes};
      kernel_state_->processor()->Execute(worker_thread_->thread_state(),
                                          request.completion, args,
                                          xe::countof(args));
    }
    if (request.new_settings) {
      settings_generation_ = request.generation;
    }
  }

  void WorkerMain() {
    while (worker_running_) {
      std::deque<PendingRequest> completions;
      PendingRequest depth_read;
      PendingRequest color_read;
      uint32_t stop_count;
      xe::threading::Wait(work_event_.get(), false, kWorkerWaitTimeout);
      {
        std::lock_guard<xe_mutex> lock(mutex_);
        completions.swap(completions_);
        // Taken out of the queues so that a stop can't cancel them meanwhile.
        if (!depth_reads_.empty()) {
          depth_read = depth_reads_.front();
          depth_reads_.pop_front();
        }
        if (!color_reads_.empty()) {
          color_read = color_reads_.front();
          color_reads_.pop_front();
        }
        stop_count = stop_count_;
      }
      for (PendingRequest& request : completions) {
        for (PendingRequest& read : request.cancelled_reads) {
          read.status = X_STATUS_CANCELLED;
          Complete(read);
        }
        if (request.completion) {
          Complete(request);
        }
      }
      for (bool depth : {true, false}) {
        PendingRequest& read = depth ? depth_read : color_read;
        if (!read.request_ptr || CompleteFrame(read, depth)) {
          continue;
        }
        // No new frame yet: requeue, unless streaming was stopped meanwhile.
        bool cancelled;
        {
          std::lock_guard<xe_mutex> lock(mutex_);
          cancelled = stop_count != stop_count_;
          if (!cancelled) {
            (depth ? depth_reads_ : color_reads_).push_front(read);
          }
        }
        if (cancelled) {
          read.status = X_STATUS_CANCELLED;
          Complete(read);
        }
      }
    }
  }

  KernelState* kernel_state_;
  Memory* memory_;
  xe_mutex mutex_;
  std::unique_ptr<xe::threading::Event> work_event_ =
      xe::threading::Event::CreateAutoResetEvent(false);
  std::deque<PendingRequest> completions_;
  // Incremented by every stop (code 2).
  uint32_t stop_count_ = 0;
  std::deque<PendingRequest> depth_reads_;
  std::deque<PendingRequest> color_reads_;
  std::unique_ptr<hid::kinect::UsbCamera> camera_;
  object_ref<XHostThread> worker_thread_;
  std::atomic<bool> worker_running_ = true;
  uint32_t depth_sequence_ = 0;
  uint32_t color_sequence_ = 0;
  std::atomic<bool> streaming_ = false;
  // Generation of the latest camera settings request, and of the latest one
  // whose completion has run, which color frames are tagged with.
  uint8_t requested_generation_ = 0;
  std::atomic<uint8_t> settings_generation_ = 0;
  std::vector<uint8_t> frame_;
  uint32_t skeleton_frame_count_ = 0;
  // DIAGNOSTIC counters.
  std::atomic<uint32_t> request_counts_[64] = {};
  std::atomic<uint32_t> diag_failures_{0};
  uint32_t depth_delivered_ = 0;
  uint32_t color_delivered_ = 0;
  uint32_t diag_invalid_ = 0;
  std::string tilt_manager_log_;
  uint32_t tilt_manager_logs_ = 0;
  std::vector<SkeletonCandidate> skeleton_candidates_;
};

xe_mutex g_camera_device_mutex;
std::unique_ptr<CameraDevice> g_camera_device;

CameraDevice* GetCameraDevice() {
  std::lock_guard<xe_mutex> lock(g_camera_device_mutex);
  if (!g_camera_device) {
    g_camera_device = std::make_unique<CameraDevice>(kernel_state());
  }
  return g_camera_device.get();
}

}  // namespace

void ShutdownCameraDevice() {
  std::unique_ptr<CameraDevice> camera_device;
  {
    std::lock_guard<xe_mutex> lock(g_camera_device_mutex);
    camera_device = std::move(g_camera_device);
  }
  // Destroyed unlocked, as its worker may still be completing a request.
  camera_device.reset();
}

dword_result_t PsCamDeviceRequest_entry(dword_t request_ptr) {
  if (!request_ptr) {
    return X_STATUS_INVALID_PARAMETER;
  }
  return GetCameraDevice()->Request(request_ptr);
}
DECLARE_XBOXKRNL_EXPORT1(PsCamDeviceRequest, kNone, kSketchy);

}  // namespace xboxkrnl
}  // namespace kernel
}  // namespace xe

DECLARE_XBOXKRNL_EMPTY_REGISTER_EXPORTS(PsCam);
