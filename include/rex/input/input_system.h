#pragma once

#include <atomic>
/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2013 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <functional>
#include <memory>
#include <vector>

#include <rex/input/device_assignment.h>
#include <rex/input/input.h>
#include <rex/input/input_driver.h>
#include <rex/input/input_script.h>
#include <rex/input/state_merge.h>
#include <rex/system/interfaces/input.h>

namespace rex::ui {
class Window;
}

namespace rex::input {

class InputSystem : public system::IInputSystem {
 public:
  explicit InputSystem(rex::ui::Window* window);
  ~InputSystem() override;

  rex::ui::Window* window() const { return window_; }

  X_STATUS Setup() override;
  void Shutdown() override;

  void AddDriver(std::unique_ptr<InputDriver> driver);
  void AttachWindow(rex::ui::Window* window);
  void SetActiveCallback(std::function<bool()> callback);
  void SetMenuChordCallback(std::function<void()> callback);
  /// Call when a host menu closes: every button held at that moment is kept from the guest until
  /// it is released (per button), so the press that closed the menu doesn't reach the game.
  /// Also done by itself whenever the active callback turns from false to true.
  void IgnoreButtonsHeldNow();
  /// Timed test input merged into the guest pad (see input_script.h). Null turns it off.
  void SetScript(std::unique_ptr<InputScript> script) { script_ = std::move(script); }
  /// Hook for the script's `snap` command (no-op without a script).
  void SetScriptSnapCallback(InputScript::SnapCallback callback) {
    if (script_) script_->SetSnapCallback(std::move(callback));
  }
  /// Hook for the script's waitpixel / pressuntil (no-op without a script).
  void SetScriptPixelProbe(InputScript::PixelProbe probe) {
    if (script_) script_->SetPixelProbe(std::move(probe));
  }

  /// Replaces any previous assignment. Call before the guest starts polling.
  void SetDeviceAssignment(std::unique_ptr<DeviceAssignment> assignment);

  X_RESULT GetCapabilities(uint32_t user_index, uint32_t flags, X_INPUT_CAPABILITIES* out_caps);
  X_RESULT GetState(uint32_t user_index, X_INPUT_STATE* out_state);
  // Merged raw pad state for host UI overlays: all connected pads across all
  // drivers and devices, bypassing the is_active suppression and the
  // menu-chord edge tracking of the guest-facing GetState. Safe to call from
  // the UI thread. Returns false when no pad is connected.
  bool GetUiGamepadState(X_INPUT_GAMEPAD* out_gamepad);
  X_RESULT SetState(uint32_t user_index, X_INPUT_VIBRATION* vibration);
  X_RESULT GetKeystroke(uint32_t user_index, uint32_t flags, X_INPUT_KEYSTROKE* out_keystroke);

 private:
  /// Re-enumerates every driver and notifies the assignment when the set
  /// changed.
  void RefreshDevices();
  InputDriver* DriverForDevice(DeviceId id);
  const DeviceInfo* DeviceInfoFor(DeviceId id) const;

  rex::ui::Window* window_ = nullptr;

  std::vector<std::unique_ptr<InputDriver>> drivers_;
  std::function<bool()> active_callback_ = nullptr;
  std::function<void()> menu_chord_callback_ = nullptr;
  bool menu_chord_down_ = false;

  // Per guest user: keeping the menu chord's buttons and buttons held across a host menu away
  // from the guest (FilterGuestButtons).
  struct ButtonFilter {
    bool chord_suppress = false;    // chord completed: its buttons stay hidden until all released
    uint64_t hold_start_ms = 0;     // a chord button went down alone: held back since then
    uint16_t held_back = 0;         // chord buttons held back meanwhile
    uint64_t tap_until_ms = 0;      // released while held back: delivered until then
    uint16_t tap = 0;
    uint16_t ignore_until_released = 0;  // held when a menu closed
    bool was_active = true;
    // The guest may skip a state whose packet number didn't change: count our own changes.
    uint16_t last_in = 0;
    uint16_t last_out = 0;
    uint32_t packet_bias = 0;
  };
  ButtonFilter button_filters_[4];
  std::atomic<bool> ignore_held_requested_[4] = {};
  uint16_t FilterGuestButtons(uint32_t user_index, uint16_t physical, uint16_t delivered,
                              uint16_t chord_mask, bool active);
  std::unique_ptr<DeviceAssignment> assignment_;
  ActiveDeviceTracker active_devices_;

  // Ordered by ordinal. Ordinals are never recycled, so unplugging pad one
  // does not renumber pad two.
  std::vector<DeviceInfo> devices_;
  std::vector<InputDriver*> device_owners_;

  // input_device_refresh_ms: when devices were last enumerated, and capabilities answered since
  // then (they don't change while a device stays connected). Each enumeration / capabilities query
  // is a device I/O round trip under Wine; the game asks for every user every frame.
  uint64_t last_refresh_ms_ = 0;
  bool refreshed_once_ = false;
  struct CachedCapabilities {
    DeviceId id;
    uint32_t flags;
    X_RESULT result;
    X_INPUT_CAPABILITIES caps;
  };
  std::vector<CachedCapabilities> cached_capabilities_;

  std::unique_ptr<InputScript> script_;
};

/// Create a default InputSystem with SDL + NOP drivers.
/// In tool mode, only the NOP driver is added.
std::unique_ptr<InputSystem> CreateDefaultInputSystem(bool tool_mode);

}  // namespace rex::input
