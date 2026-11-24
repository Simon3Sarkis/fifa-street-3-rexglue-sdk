/**
 * @file        input/input_script.h
 * @brief       Timed controller input played back from a text file, for repeatable test runs.
 *
 * Enabled with the `input_script` setting (a path; relative paths resolve next to the exe).
 * Times are milliseconds since the input system was created (about the start of the process).
 * The scripted pad is merged into the guest's pad after the window-focus gate, so a scripted run
 * works while the window is in the background. Off (no file) changes nothing.
 *
 * Format, one event per line, '#' starts a comment:
 *
 *   <time> <command> [arguments]
 *
 *   time        12000 (absolute ms) or +1500 (ms after the previous line's time)
 *   press B [ms]          buttons down for ms (default 120). B: A B X Y START BACK LB RB LS RS
 *                         UP DOWN LEFT RIGHT GUIDE, joined with '+' (A+B)
 *   hold B ms             same as press, duration required
 *   stick L|R x y ms      stick at x,y in -1..1 (y up) for ms
 *   trigger L|R v ms      trigger at v in 0..1 for ms
 *   user N                following events go to guest user N (0-3, default 0)
 *   log text...           writes "input_script: text" to the log at that time
 *   waitpixel X Y R G B tol ms
 *                         holds the script until the game's output pixel X,Y (in 1280x720 coordinates,
 *                         scaled to the real output) is within tol of R,G,B, or ms pass; every later
 *                         event shifts by the time waited
 *   pressuntil B period X Y R G B tol ms
 *                         like waitpixel, pressing buttons B every period ms while waiting (e.g. START
 *                         until the main menu shows: can't miss the title or overshoot the menu)
 *   snap name             saves the game's output frame as <input_script_snap_dir>/<name>.bmp
 *                         (from the presenter, so other windows can't cover it)
 *   set name value        changes a setting at that time (rex::cvar::SetFlagByName), e.g. to turn
 *                         a debug log on for the frames that matter
 *   quit                  flushes the log and exits the process (exit code 0)
 */
#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <rex/input/input.h>

namespace rex::input {

class InputScript {
 public:
  /// Parses the file. Returns null (and logs why) if it can't be read or has no events.
  static std::unique_ptr<InputScript> Load(const std::filesystem::path& path);

  /// Applies the events active now for user_index to pad. Returns how many times the scripted
  /// state of this user has changed so far (added to the packet number so the guest sees changes).
  uint32_t Apply(uint32_t user_index, X_INPUT_GAMEPAD& pad);

  /// True if any event targets user_index (a scripted user counts as connected).
  bool HasUser(uint32_t user_index) const;

  /// Called (outside the script's lock) for each `snap` event, with its name. Set by the app.
  using SnapCallback = std::function<void(const std::string& name)>;
  void SetSnapCallback(SnapCallback callback) { snap_callback_ = std::move(callback); }

  /// Reads the game's output pixel at (x, y) in 1280x720 coordinates (scaled to the real output) into
  /// rgb; false if no frame is available. Set by the app; called outside the script's lock.
  using PixelProbe = std::function<bool(float x, float y, uint8_t rgb[3])>;
  void SetPixelProbe(PixelProbe probe) { pixel_probe_ = std::move(probe); }

 private:
  enum class Kind { kButtons, kStick, kTrigger, kLog, kSet, kSnap, kWait, kQuit };
  struct Event {
    int64_t start_ms = 0;
    int64_t end_ms = 0;
    uint32_t user = 0;
    Kind kind = Kind::kButtons;
    uint16_t buttons = 0;
    bool right = false;
    float x = 0, y = 0;
    std::string text;
    std::string value;
    // waitpixel / pressuntil
    uint8_t rgb[3] = {};
    int tolerance = 0;
    int64_t timeout_ms = 0;
    int64_t period_ms = 0;
    int line = 0;
    bool started = false;
  };

  InputScript() = default;
  uint32_t ApplyLocked(uint32_t user_index, X_INPUT_GAMEPAD& pad, std::vector<std::string>& snaps,
                       int& probe_event);
  int64_t NowMs() const;

  std::vector<Event> events_;
  SnapCallback snap_callback_;
  PixelProbe pixel_probe_;
  // Script time = NowMs() - offset_ms_; waits grow the offset so later events shift.
  int64_t offset_ms_ = 0;
  int active_wait_ = -1;
  int64_t wait_begin_ms_ = 0;
  int64_t last_probe_ms_ = 0;
  bool probe_matched_ = false;
  int64_t epoch_ms_ = 0;
  std::mutex mutex_;
  struct UserState {
    uint64_t last_signature = 0;
    uint32_t changes = 0;
  } users_[4];
};

}  // namespace rex::input
