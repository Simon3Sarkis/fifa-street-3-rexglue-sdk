/**
 * @file        rex/ui/overlay/launcher_overlay.h
 *
 * @brief       Pre-runtime launcher: pick the game's disc image, choose display settings from
 *              dropdowns, then play. Mouse, keyboard and controller.
 */

#pragma once

#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <rex/ui/imgui_dialog.h>
#include <rex/ui/window.h>

namespace rex::ui {

class LauncherDialog final : public ImGuiDialog {
 public:
  // Filled in by the app's update check (any thread, possibly after the dialog is gone): when
  // `version` is set the launcher shows one line with it and a button that opens `url` in the
  // browser. Nothing is downloaded.
  struct UpdateNotice {
    std::mutex mutex;
    std::string version;
    std::string url;
  };
  struct GameCheck {
    bool ok = false;
    std::string message;  // "Ready: ..." or what is wrong with the file
  };
  struct Options {
    std::string title;     // big heading
    std::string subtitle;  // version line under it
    std::filesystem::path game_path;      // current game location (disc image or folder), may be empty
    std::filesystem::path settings_path;  // the user's settings file; choices are saved there
    std::string picker_hint;              // shown when no game is set up yet
    // In-game options menu instead of the pre-runtime launcher: no disc image section, "Back to
    // game" instead of Play (PlayCallback), "Quit game" (QuitCallback), B / Esc / Start + Back
    // closes. Settings the running game can't change (game resolution, render scale, vsync) only
    // go to the settings file, for the next start; each row says whether it applies now or on
    // restart.
    bool in_game = false;
    // Footer line for the in-game menu (e.g. that the game is paused).
    std::string in_game_note;
    // Launcher only: a newer release, if the app's check found one.
    std::shared_ptr<UpdateNotice> update_notice;
    // A bool cvar that turns the app's update check on / off; shows a "Check for updates" row
    // (Display tab) when set. Empty = no row (e.g. RecompLauncher updates the game itself).
    std::string update_check_cvar;
  };
  using CheckGameCallback = std::function<GameCheck(const std::filesystem::path&)>;
  using PlayCallback = std::function<void(const std::filesystem::path& game_path)>;
  using QuitCallback = std::function<void()>;

  LauncherDialog(ImGuiDrawer* drawer, Window* window, Options options, CheckGameCallback check_game,
                 PlayCallback play, QuitCallback quit);
  ~LauncherDialog() override;

 protected:
  void OnDraw(ImGuiIO& io) override;
  void OnClose() override;

 private:
  struct PickerState;
  struct Choice {
    std::string label;
    std::string tooltip;
  };

  void LoadFromSettings();
  void SetGame(const std::filesystem::path& path);
  void OpenPicker();
  void PollPicker();
  void ApplyDisplay();
  // The options that change while the game runs (upscaling filter, sharpening, texture
  // filtering, FXAA, FPS counter): set their cvars now.
  void ApplyLive();
  void SaveAll();
  void SaveForNextStart();
  void PollGamepad(ImGuiIO& io);
  bool DrawCombo(const char* id, int& index, const std::vector<Choice>& choices, bool enabled,
                 float width);
  std::vector<Choice> RefreshChoices() const;

  Window* window_;
  Options options_;
  CheckGameCallback check_game_;
  PlayCallback play_;
  QuitCallback quit_;

  std::filesystem::path game_path_;
  GameCheck game_check_;
  std::shared_ptr<PickerState> picker_;

  std::vector<DisplayInfo> displays_;
  int display_index_ = 0;      // into displays_
  int screen_mode_index_ = 1;  // 0 windowed, 1 borderless, 2 exclusive
  int refresh_index_ = 0;      // 0 = the display's current rate, then displays_[i].refresh_rates
  int vsync_index_ = 0;        // 0 off, 1 on
  int resolution_index_ = 0;   // see kResolutions in the .cpp
  int scale_index_ = 0;        // render scale - 1 (1x..4x)
  int frame_rate_index_ = 0;   // 60 only
  int upscale_index_ = 0;      // see kUpscalers in the .cpp (0 = bilinear, the original)
  int sharpness_index_ = 3;    // see kSharpness in the .cpp
  int aniso_index_ = 0;        // 0 = the game's own filtering, then 2x..16x
  int fxaa_index_ = 0;         // 0 off, 1 FXAA, 2 FXAA (extreme)
  int fps_index_ = 0;          // 0 off, 1 FPS, 2 FPS + frame time, 3 detailed
  int tab_ = 0;                // Display, Graphics, Audio, Controls
  int tab_request_ = -1;       // tab to select on the next frame (LB / RB, PgUp / PgDn)
  int update_check_index_ = 1; // 0 off, 1 on (Options::update_check_cvar)

  bool pad_opened_ = false;
  bool first_frame_ = true;
  int saved_config_flags_ = 0;
  int saved_backend_flags_ = 0;
  bool played_ = false;
  double shown_time_ = 0.0;
  bool pad_b_was_down_ = true;  // a B already held when the menu opens doesn't close it
  bool pad_chord_was_down_ = true;  // same for Start + Back
  bool pad_lb_was_down_ = true;
  bool pad_rb_was_down_ = true;
  bool close_requested_ = false;
};

}  // namespace rex::ui
