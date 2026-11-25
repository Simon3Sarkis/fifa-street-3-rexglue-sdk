/**
 * @file        ui/overlay/launcher_overlay.cpp
 *
 * @brief       Pre-runtime launcher: game disc image, display settings dropdowns, Play.
 */

#include <rex/ui/overlay/launcher_overlay.h>

#include <algorithm>
#include <climits>
#include <cmath>
#include <fstream>
#include <mutex>

#include <SDL3/SDL.h>
#include <toml++/toml.hpp>
#include <fmt/format.h>
#include <imgui.h>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ui/imgui_drawer.h>
#include <rex/ui/presenter.h>
#include <rex/ui/window_sdl.h>
#include <rex/ui/windowed_app_context.h>

REXCVAR_DEFINE_INT32(launcher_autoplay_ms, 0, "UI",
                     "Testing: press the launcher's Play by itself this many ms after it first "
                     "shows (0 = off)")
    .range(0, 600000);

namespace rex::ui {

namespace {

// Resolution the picture is drawn at. The game has two video modes of its own, 720p and 1080p (as on
// the console; it falls back to 720p for anything else), so 1440p and 4K are those modes drawn
// larger by the GPU emulation (draw_resolution_scale).
struct ResolutionPreset {
  const char* label;
  const char* tooltip;
  const char* resolution;  // the `resolution` cvar: the game's video mode
  int scale;               // draw_resolution_scale_x / _y
};
constexpr ResolutionPreset kResolutions[] = {
    {"1280 x 720 (original)", "The game's 720p mode, as on the console.", "720p", 1},
    {"1920 x 1080", "The game's own 1080p mode.", "1080p", 1},
    {"2560 x 1440", "The 720p mode drawn at twice the size.", "720p", 2},
    {"3840 x 2160", "The 1080p mode drawn at twice the size.", "1080p", 2},
};

int ResolutionHeight(const ResolutionPreset& preset) {
  return (std::string_view(preset.resolution) == "1080p" ? 1080 : 720) * preset.scale;
}

const char* const kScreenModes[] = {"Windowed", "Borderless fullscreen", "Exclusive fullscreen"};

// Colours (linear 0-1 RGBA).
constexpr ImVec4 kBackdropTop{0.035f, 0.045f, 0.075f, 1.0f};
constexpr ImVec4 kBackdropBottom{0.012f, 0.014f, 0.024f, 1.0f};
constexpr ImVec4 kPanel{0.075f, 0.085f, 0.120f, 0.96f};
constexpr ImVec4 kText{0.93f, 0.94f, 0.96f, 1.0f};
constexpr ImVec4 kTextDim{0.58f, 0.61f, 0.68f, 1.0f};
constexpr ImVec4 kAccent{1.00f, 0.47f, 0.10f, 1.0f};
constexpr ImVec4 kAccentHover{1.00f, 0.56f, 0.24f, 1.0f};
constexpr ImVec4 kGood{0.36f, 0.85f, 0.50f, 1.0f};
constexpr ImVec4 kBad{1.00f, 0.42f, 0.40f, 1.0f};
constexpr ImVec4 kField{0.11f, 0.125f, 0.17f, 1.0f};
constexpr ImVec4 kFieldHover{0.15f, 0.17f, 0.23f, 1.0f};

std::string FormatHz(float hz) {
  // 59.94 stays 59.94; 60.00 prints as 60.
  const float rounded = std::round(hz);
  if (std::abs(hz - rounded) < 0.02f) {
    return fmt::format("{} Hz", int(rounded));
  }
  return fmt::format("{:.2f} Hz", hz);
}

}  // namespace

// Shared with SDL's file dialog callback, which may run on another thread and after the dialog
// is gone.
struct LauncherDialog::PickerState {
  std::mutex mutex;
  bool open = false;
  bool done = false;
  std::filesystem::path result;
  std::string error;
};

LauncherDialog::LauncherDialog(ImGuiDrawer* drawer, Window* window, Options options,
                               CheckGameCallback check_game, PlayCallback play, QuitCallback quit)
    : ImGuiDialog(drawer),
      window_(window),
      options_(std::move(options)),
      check_game_(std::move(check_game)),
      play_(std::move(play)),
      quit_(std::move(quit)),
      picker_(std::make_shared<PickerState>()) {
  displays_ = EnumerateDisplays();
  if (displays_.empty()) {
    displays_.push_back({"Default display", 0, 0, 0.0f, {}});
  }
  LoadFromSettings();
  SetGame(options_.game_path);
  if (SDL_InitSubSystem(SDL_INIT_GAMEPAD)) {
    pad_opened_ = true;
  }
}

// The owner destroys the dialog (outside the UI drawer) once Play or Quit has run; Close() isn't
// used, because a closed ImGuiDialog deletes itself.
LauncherDialog::~LauncherDialog() {
  OnClose();
  if (pad_opened_) {
    SDL_QuitSubSystem(SDL_INIT_GAMEPAD);
  }
}

void LauncherDialog::OnClose() {
  if (first_frame_) {
    return;  // never drawn: nothing to restore
  }
  ImGuiIO& io = GetIO();
  io.ConfigFlags = saved_config_flags_;
  io.BackendFlags = saved_backend_flags_;
  first_frame_ = true;
}

void LauncherDialog::LoadFromSettings() {
  const int monitor = rex::cvar::Query<int32_t>("monitor");
  display_index_ = std::clamp(monitor > 0 ? monitor - 1 : 0, 0, int(displays_.size()) - 1);

  const bool fullscreen = rex::cvar::Query<bool>("fullscreen");
  const std::string mode = rex::cvar::Query<std::string>("fullscreen_mode");
  screen_mode_index_ = !fullscreen ? 0 : (mode == "exclusive" ? 2 : 1);

  refresh_index_ = 0;
  const double refresh = rex::cvar::Query<double>("fullscreen_refresh_rate");
  if (refresh > 0.0) {
    const auto& rates = displays_[display_index_].refresh_rates;
    for (size_t i = 0; i < rates.size(); ++i) {
      if (std::abs(rates[i] - float(refresh)) < 0.5f) {
        refresh_index_ = int(i) + 1;
      }
    }
  }

  vsync_index_ = rex::cvar::Query<bool>("vulkan_allow_present_mode_immediate") ? 0 : 1;

  // The preset with the same mode and scale, else the nearest drawn height (an older settings file
  // may hold another scale, like 720p at 3x).
  const std::string resolution = rex::cvar::Query<std::string>("resolution");
  const int scale = std::max(1, rex::cvar::Query<int32_t>("draw_resolution_scale_x"));
  const int height = (resolution == "1080p" ? 1080 : 720) * scale;
  resolution_index_ = 0;
  int best = INT_MAX;
  for (size_t i = 0; i < std::size(kResolutions); ++i) {
    const auto& preset = kResolutions[i];
    const int distance = (resolution == preset.resolution && scale == preset.scale)
                             ? -1
                             : std::abs(ResolutionHeight(preset) - height);
    if (distance < best) {
      best = distance;
      resolution_index_ = int(i);
    }
  }
}

void LauncherDialog::SetGame(const std::filesystem::path& path) {
  game_path_ = path;
  if (path.empty()) {
    game_check_ = {false, options_.picker_hint};
  } else if (check_game_) {
    game_check_ = check_game_(path);
  } else {
    game_check_ = {std::filesystem::exists(path), ""};
  }
}

void LauncherDialog::OpenPicker() {
  {
    std::lock_guard lock(picker_->mutex);
    if (picker_->open) {
      return;
    }
    picker_->open = true;
    picker_->done = false;
    picker_->error.clear();
  }
  static const SDL_DialogFileFilter kFilters[] = {
      {"Xbox 360 disc image (*.iso)", "iso"},
      {"All files", "*"},
  };
  auto* sdl_window = dynamic_cast<WindowSDL*>(window_);
  // SDL owns a copy of nothing: the userdata must outlive the dialog, hence a heap shared_ptr.
  auto* holder = new std::shared_ptr<PickerState>(picker_);
  std::string start;
  if (!game_path_.empty()) {
    start = (std::filesystem::is_directory(game_path_) ? game_path_ : game_path_.parent_path())
                .string();
  }
  SDL_ShowOpenFileDialog(
      [](void* userdata, const char* const* files, int) {
        auto* state_holder = static_cast<std::shared_ptr<PickerState>*>(userdata);
        auto& state = **state_holder;
        {
          std::lock_guard lock(state.mutex);
          if (!files) {
            state.error = SDL_GetError();
          } else if (files[0]) {
            state.result = std::filesystem::u8path(files[0]);
          }
          state.open = false;
          state.done = true;
        }
        delete state_holder;
      },
      holder, sdl_window ? sdl_window->sdl_window() : nullptr, kFilters, int(std::size(kFilters)),
      start.empty() ? nullptr : start.c_str(), false);
}

void LauncherDialog::PollPicker() {
  std::filesystem::path picked;
  std::string error;
  {
    std::lock_guard lock(picker_->mutex);
    if (!picker_->done) {
      return;
    }
    picker_->done = false;
    picked = std::move(picker_->result);
    picker_->result.clear();
    error = std::move(picker_->error);
  }
  // The modal picker swallows the release of whatever activated it.
  ImGuiIO& io = GetIO();
  io.AddMouseButtonEvent(0, false);
  io.AddKeyEvent(ImGuiKey_Enter, false);
  io.AddKeyEvent(ImGuiKey_Space, false);
  if (!picked.empty()) {
    SetGame(picked);
  } else if (!error.empty()) {
    REXLOG_WARN("Launcher: file picker unavailable: {}", error);
    game_check_ = {false,
                   "The file picker isn't available here. Put the .iso next to the game and "
                   "start it again."};
  }
}

void LauncherDialog::ApplyDisplay() {
  std::string monitor = std::to_string(display_index_ + 1);
  std::string mode = screen_mode_index_ == 2 ? "exclusive" : "borderless";
  std::string refresh = "0";
  const auto& rates = displays_[display_index_].refresh_rates;
  if (screen_mode_index_ == 2 && refresh_index_ > 0 && refresh_index_ <= int(rates.size())) {
    refresh = fmt::format("{:.3f}", rates[refresh_index_ - 1]);
  }
  std::string fullscreen = screen_mode_index_ != 0 ? "true" : "false";
  auto apply = [monitor, mode, refresh, fullscreen, window = window_]() {
    rex::cvar::SetFlagByName("monitor", monitor);
    rex::cvar::SetFlagByName("fullscreen_mode", mode);
    rex::cvar::SetFlagByName("fullscreen_refresh_rate", refresh);
    // The fullscreen change callback resizes the window.
    rex::cvar::SetFlagByName("fullscreen", fullscreen);
    if (window) {
      window->ApplyDisplaySettings();
    }
  };
  // Not from inside the UI drawer: moving and resizing the window reconnects the presenter.
  if (window_) {
    window_->app_context().CallInUIThreadDeferred(std::move(apply));
  } else {
    apply();
  }
}

// In-game: what the running game reads only at start goes straight to the settings file; the
// display settings (applied live already) are saved from their current values.
void LauncherDialog::SaveForNextStart() {
  if (options_.settings_path.empty()) {
    return;
  }
  rex::cvar::SaveConfigValues(options_.settings_path,
                              {"monitor", "fullscreen", "fullscreen_mode", "fullscreen_refresh_rate"});
  toml::table table;
  try {
    if (std::filesystem::exists(options_.settings_path)) {
      table = toml::parse_file(options_.settings_path.string());
    }
  } catch (const toml::parse_error& e) {
    REXLOG_WARN("Options: {} unreadable ({}), rewriting the settings it holds",
                options_.settings_path.string(), e.description());
  }
  const bool vsync = vsync_index_ == 1;
  const int scale = kResolutions[resolution_index_].scale;
  table.insert_or_assign("vulkan_allow_present_mode_immediate", !vsync);
  table.insert_or_assign("vulkan_allow_present_mode_mailbox", !vsync);
  table.insert_or_assign("resolution", std::string(kResolutions[resolution_index_].resolution));
  table.insert_or_assign("draw_resolution_scale_x", int64_t(scale));
  table.insert_or_assign("draw_resolution_scale_y", int64_t(scale));
  std::error_code ec;
  const auto temp = options_.settings_path.string() + ".tmp";
  {
    std::ofstream out(temp, std::ios::binary | std::ios::trunc);
    out << "# Auto-generated cvar configuration\n" << table << "\n";
    if (!out) {
      REXLOG_ERROR("Options: cannot write {}", temp);
      return;
    }
  }
  std::filesystem::rename(temp, options_.settings_path, ec);
  if (ec) {
    REXLOG_ERROR("Options: cannot replace {}: {}", options_.settings_path.string(), ec.message());
  }
}

void LauncherDialog::SaveAll() {
  if (options_.in_game) {
    SaveForNextStart();
    return;
  }
  const bool vsync = vsync_index_ == 1;
  const bool vsync_changed = rex::cvar::Query<bool>("vulkan_allow_present_mode_immediate") == vsync;
  rex::cvar::SetFlagByName("vulkan_allow_present_mode_immediate", vsync ? "false" : "true");
  rex::cvar::SetFlagByName("vulkan_allow_present_mode_mailbox", vsync ? "false" : "true");
  const auto& preset = kResolutions[resolution_index_];
  rex::cvar::SetFlagByName("resolution", preset.resolution);
  rex::cvar::SetFlagByName("draw_resolution_scale_x", std::to_string(preset.scale));
  rex::cvar::SetFlagByName("draw_resolution_scale_y", std::to_string(preset.scale));
  if (!game_path_.empty()) {
    rex::cvar::SetFlagByName("game_data_root", game_path_.string());
  }
  if (!options_.settings_path.empty()) {
    std::vector<std::string_view> names = {"monitor",
                                           "fullscreen",
                                           "fullscreen_mode",
                                           "fullscreen_refresh_rate",
                                           "vulkan_allow_present_mode_immediate",
                                           "vulkan_allow_present_mode_mailbox",
                                           "resolution",
                                           "draw_resolution_scale_x",
                                           "draw_resolution_scale_y"};
    if (!game_path_.empty()) {
      names.push_back("game_data_root");
    }
    rex::cvar::SaveConfigValues(options_.settings_path, names);
  }
  if (vsync_changed && window_) {
    Window* window = window_;
    window->app_context().CallInUIThreadDeferred([window]() { window->ReconnectPresenter(); });
  }
}

void LauncherDialog::PollGamepad(ImGuiIO& io) {
  if (!pad_opened_) {
    return;
  }
  // The first connected pad drives the menu.
  SDL_Gamepad* pad = nullptr;
  int count = 0;
  if (SDL_JoystickID* ids = SDL_GetGamepads(&count)) {
    if (count > 0) {
      pad = SDL_GetGamepadFromID(ids[0]);
      if (!pad) {
        pad = SDL_OpenGamepad(ids[0]);
      }
    }
    SDL_free(ids);
  }
  if (!pad) {
    return;
  }
  io.BackendFlags |= ImGuiBackendFlags_HasGamepad;
  auto button = [&](ImGuiKey key, SDL_GamepadButton b) {
    io.AddKeyEvent(key, SDL_GetGamepadButton(pad, b));
  };
  button(ImGuiKey_GamepadFaceDown, SDL_GAMEPAD_BUTTON_SOUTH);
  button(ImGuiKey_GamepadFaceRight, SDL_GAMEPAD_BUTTON_EAST);
  button(ImGuiKey_GamepadFaceLeft, SDL_GAMEPAD_BUTTON_WEST);
  button(ImGuiKey_GamepadFaceUp, SDL_GAMEPAD_BUTTON_NORTH);
  button(ImGuiKey_GamepadDpadUp, SDL_GAMEPAD_BUTTON_DPAD_UP);
  button(ImGuiKey_GamepadDpadDown, SDL_GAMEPAD_BUTTON_DPAD_DOWN);
  button(ImGuiKey_GamepadDpadLeft, SDL_GAMEPAD_BUTTON_DPAD_LEFT);
  button(ImGuiKey_GamepadDpadRight, SDL_GAMEPAD_BUTTON_DPAD_RIGHT);
  button(ImGuiKey_GamepadStart, SDL_GAMEPAD_BUTTON_START);
  const bool b_down = SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_EAST);
  if (options_.in_game && b_down && !pad_b_was_down_ &&
      !ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId)) {
    close_requested_ = true;
  }
  pad_b_was_down_ = b_down;
  auto axis = [&](ImGuiKey negative, ImGuiKey positive, SDL_GamepadAxis a) {
    const float v = SDL_GetGamepadAxis(pad, a) / 32767.0f;
    constexpr float kDead = 0.35f;
    const float n = v < -kDead ? std::min(1.0f, (-v - kDead) / (1.0f - kDead)) : 0.0f;
    const float p = v > kDead ? std::min(1.0f, (v - kDead) / (1.0f - kDead)) : 0.0f;
    io.AddKeyAnalogEvent(negative, n > 0.0f, n);
    io.AddKeyAnalogEvent(positive, p > 0.0f, p);
  };
  axis(ImGuiKey_GamepadLStickUp, ImGuiKey_GamepadLStickDown, SDL_GAMEPAD_AXIS_LEFTY);
  axis(ImGuiKey_GamepadLStickLeft, ImGuiKey_GamepadLStickRight, SDL_GAMEPAD_AXIS_LEFTX);
}

bool LauncherDialog::DrawCombo(const char* id, int& index, const std::vector<Choice>& choices,
                               bool enabled, float width) {
  bool changed = false;
  index = std::clamp(index, 0, std::max(0, int(choices.size()) - 1));
  ImGui::SetNextItemWidth(width);
  ImGui::BeginDisabled(!enabled || choices.size() < 2);
  const char* preview = choices.empty() ? "" : choices[index].label.c_str();
  // The list gets its own, tighter padding (the panel's would leave a gap and a scrollbar).
  const float s = ImGui::GetStyle().FramePadding.y / 9.0f;
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(6.0f * s, 6.0f * s));
  const bool open = ImGui::BeginCombo(id, preview, ImGuiComboFlags_HeightLarge);
  ImGui::PopStyleVar();
  if (open) {
    for (int i = 0; i < int(choices.size()); ++i) {
      const bool selected = i == index;
      if (ImGui::Selectable(choices[i].label.c_str(), selected)) {
        changed = i != index;
        index = i;
      }
      if (selected) {
        ImGui::SetItemDefaultFocus();
      }
      if (!choices[i].tooltip.empty() && ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s", choices[i].tooltip.c_str());
      }
    }
    ImGui::EndCombo();
  }
  ImGui::EndDisabled();
  return changed;
}

std::vector<LauncherDialog::Choice> LauncherDialog::RefreshChoices() const {
  const auto& display = displays_[display_index_];
  std::vector<Choice> choices;
  const std::string current =
      display.refresh_hz > 0.0f ? FormatHz(display.refresh_hz) : std::string("current");
  if (screen_mode_index_ != 2) {
    choices.push_back({fmt::format("Desktop ({})", current),
                       "Windowed and borderless use the desktop's refresh rate."});
    return choices;
  }
  choices.push_back({fmt::format("Current ({})", current), ""});
  for (float hz : display.refresh_rates) {
    choices.push_back({FormatHz(hz), ""});
  }
  return choices;
}

void LauncherDialog::OnDraw(ImGuiIO& io) {
  if (first_frame_) {
    saved_config_flags_ = io.ConfigFlags;
    saved_backend_flags_ = io.BackendFlags;
  }
  io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
  PollGamepad(io);
  PollPicker();

  const ImVec2 display = io.DisplaySize;
  // Laid out for 1080p; scaled with the window, kept readable on the Deck's 800p panel.
  const float s = std::clamp(display.y / 1080.0f, 0.75f, 3.0f);
  ImGuiDrawer* drawer = imgui_drawer();
  ImFont* font = drawer->ui_font();
  ImFont* font_semibold = drawer->ui_font_semibold() ? drawer->ui_font_semibold() : font;
  ImFont* font_bold = drawer->ui_font_bold() ? drawer->ui_font_bold() : font_semibold;

  // Backdrop.
  ImDrawList* bg = ImGui::GetBackgroundDrawList();
  // The launcher covers the screen; in the game, the game stays visible behind a dim scrim.
  const float backdrop_alpha = options_.in_game ? 0.62f : 1.0f;
  ImVec4 top = kBackdropTop, bottom = kBackdropBottom;
  top.w = bottom.w = backdrop_alpha;
  bg->AddRectFilledMultiColor(ImVec2(0, 0), display, ImGui::GetColorU32(top),
                              ImGui::GetColorU32(top), ImGui::GetColorU32(bottom),
                              ImGui::GetColorU32(bottom));
  bg->AddRectFilled(ImVec2(0, 0), ImVec2(display.x, 4.0f * s), ImGui::GetColorU32(kAccent));

  const float panel_w = std::min(display.x - 32.0f * s, 860.0f * s);
  ImGui::SetNextWindowPos(ImVec2(display.x * 0.5f, display.y * 0.5f), ImGuiCond_Always,
                          ImVec2(0.5f, 0.5f));
  ImGui::SetNextWindowSize(ImVec2(panel_w, 0.0f), ImGuiCond_Always);

  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(40.0f * s, 34.0f * s));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 14.0f * s);
  ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 8.0f * s);
  ImGui::PushStyleVar(ImGuiStyleVar_PopupRounding, 8.0f * s);
  ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(14.0f * s, 9.0f * s));
  ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(12.0f * s, 12.0f * s));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_PopupBorderSize, 1.0f);
  ImGui::PushStyleColor(ImGuiCol_WindowBg, kPanel);
  ImGui::PushStyleColor(ImGuiCol_PopupBg, ImVec4(0.10f, 0.115f, 0.16f, 0.99f));
  ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(1, 1, 1, 0.08f));
  ImGui::PushStyleColor(ImGuiCol_Text, kText);
  ImGui::PushStyleColor(ImGuiCol_TextDisabled, kTextDim);
  ImGui::PushStyleColor(ImGuiCol_FrameBg, kField);
  ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, kFieldHover);
  ImGui::PushStyleColor(ImGuiCol_FrameBgActive, kFieldHover);
  ImGui::PushStyleColor(ImGuiCol_Button, kField);
  ImGui::PushStyleColor(ImGuiCol_ButtonHovered, kFieldHover);
  ImGui::PushStyleColor(ImGuiCol_ButtonActive, kFieldHover);
  ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(kAccent.x, kAccent.y, kAccent.z, 0.30f));
  ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImVec4(kAccent.x, kAccent.y, kAccent.z, 0.45f));
  ImGui::PushStyleColor(ImGuiCol_HeaderActive, ImVec4(kAccent.x, kAccent.y, kAccent.z, 0.55f));
  ImGui::PushStyleColor(ImGuiCol_NavCursor, kAccent);
  ImGui::PushStyleColor(ImGuiCol_Separator, ImVec4(1, 1, 1, 0.07f));
  ImGui::PushStyleColor(ImGuiCol_ScrollbarBg, ImVec4(0, 0, 0, 0));
  ImGui::PushStyleColor(ImGuiCol_ScrollbarGrab, ImVec4(1, 1, 1, 0.18f));
  ImGui::PushStyleColor(ImGuiCol_ScrollbarGrabHovered, ImVec4(1, 1, 1, 0.28f));
  ImGui::PushStyleColor(ImGuiCol_ScrollbarGrabActive, kAccent);
  ImGui::PushFont(font, 19.0f * s);

  ImGui::Begin("##launcher", nullptr,
               ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                   ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_AlwaysAutoResize);

  // Heading.
  ImGui::PushFont(font_bold, 40.0f * s);
  ImGui::TextUnformatted(options_.title.c_str());
  ImGui::PopFont();
  ImGui::PushStyleColor(ImGuiCol_Text, kTextDim);
  ImGui::TextUnformatted(options_.subtitle.c_str());
  ImGui::PopStyleColor();
  ImGui::Dummy(ImVec2(0, 8.0f * s));

  const float content_w = ImGui::GetContentRegionAvail().x;
  const float label_w = 250.0f * s;
  // SameLine / SetCursorPosX positions count from the window edge, padding included.
  const float field_x = ImGui::GetCursorStartPos().x + label_w;
  const float field_w = content_w - label_w;

  auto section = [&](const char* name) {
    ImGui::Dummy(ImVec2(0, 6.0f * s));
    ImGui::PushFont(font_semibold, 14.0f * s);
    ImGui::PushStyleColor(ImGuiCol_Text, kAccent);
    ImGui::TextUnformatted(name);
    ImGui::PopStyleColor();
    ImGui::PopFont();
    ImGui::Separator();
  };
  auto row_label = [&](const char* label) {
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(label);
    ImGui::SameLine(field_x);
  };

  // Game (the launcher only: the running game keeps its disc).
  if (!options_.in_game) {
  section("GAME");
  row_label("Disc image");
  {
    const float button_w = 170.0f * s;
    std::string shown = game_path_.empty() ? std::string("Not selected")
                                           : game_path_.filename().string();
    if (shown.empty()) {
      shown = game_path_.string();
    }
    ImGui::PushStyleColor(ImGuiCol_Text, game_path_.empty() ? kTextDim : kText);
    ImGui::SetNextItemWidth(field_w - button_w - ImGui::GetStyle().ItemSpacing.x);
    ImGui::BeginDisabled();
    ImGui::InputText("##game", shown.data(), shown.size() + 1, ImGuiInputTextFlags_ReadOnly);
    ImGui::EndDisabled();
    ImGui::PopStyleColor();
    if (!game_path_.empty() && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
      ImGui::SetTooltip("%s", game_path_.string().c_str());
    }
    ImGui::SameLine();
    bool picker_open;
    {
      std::lock_guard lock(picker_->mutex);
      picker_open = picker_->open;
    }
    ImGui::BeginDisabled(picker_open);
    if (ImGui::Button(picker_open ? "Selecting..." : "Select ISO...", ImVec2(button_w, 0))) {
      OpenPicker();
    }
    ImGui::EndDisabled();
  }
  ImGui::SetCursorPosX(field_x);
  ImGui::PushStyleColor(ImGuiCol_Text, game_check_.ok ? kGood : (game_path_.empty() ? kTextDim : kBad));
  ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + field_w);
  ImGui::PushFont(nullptr, 16.0f * s);
  ImGui::TextUnformatted(game_check_.message.c_str());
  ImGui::PopFont();
  ImGui::PopTextWrapPos();
  ImGui::PopStyleColor();
  }

  // Display.
  section("DISPLAY");
  {
    std::vector<Choice> displays;
    for (size_t i = 0; i < displays_.size(); ++i) {
      const auto& d = displays_[i];
      std::string label = fmt::format("{}: {}", i + 1, d.name);
      if (d.width > 0) {
        label += fmt::format("  ({} x {})", d.width, d.height);
      }
      displays.push_back({std::move(label), ""});
    }
    row_label("Display");
    if (DrawCombo("##display", display_index_, displays, true, field_w)) {
      refresh_index_ = 0;
      ApplyDisplay();
    }

    std::vector<Choice> modes;
    for (const char* m : kScreenModes) {
      modes.push_back({m, ""});
    }
    modes[0].tooltip = "A window on the desktop. On Windows the desktop compositor can hold it below 60 FPS; fullscreen is smoother.";
    modes[1].tooltip = "Covers the screen without changing the display mode. Quick to switch.";
    modes[2].tooltip = "The game takes over the display; lets you pick the refresh rate.";
    row_label("Screen mode");
    if (DrawCombo("##screenmode", screen_mode_index_, modes, true, field_w)) {
      if (screen_mode_index_ != 2) {
        refresh_index_ = 0;
      }
      ApplyDisplay();
    }

    row_label("Refresh rate");
    if (DrawCombo("##refresh", refresh_index_, RefreshChoices(), screen_mode_index_ == 2,
                  field_w)) {
      ApplyDisplay();
    }

    row_label("Vsync");
    DrawCombo("##vsync", vsync_index_,
              {{"Off", "Lowest latency; the picture may tear."},
               {"On", "No tearing; slightly more latency."}},
              true, field_w);
  }

  // Graphics.
  section("GRAPHICS");
  {
    std::vector<Choice> resolutions;
    for (const auto& r : kResolutions) {
      resolutions.push_back({r.label, r.tooltip});
    }
    row_label("Resolution");
    DrawCombo("##resolution", resolution_index_, resolutions, true, field_w);

    row_label("Frame rate");
    DrawCombo("##framerate", frame_rate_index_,
              {{"60 FPS (locked, as on the console)", "The game's speed is tied to 60 FPS."}},
              true, field_w);
  }

  // Buttons.
  ImGui::Dummy(ImVec2(0, 16.0f * s));
  const float play_w = 220.0f * s;
  const float quit_w = (options_.in_game ? 170.0f : 140.0f) * s;
  const float button_h = 54.0f * s;
  ImGui::SetCursorPosX(ImGui::GetCursorPosX() + content_w - play_w - quit_w -
                       ImGui::GetStyle().ItemSpacing.x);
  if (ImGui::Button(options_.in_game ? "Quit game" : "Quit", ImVec2(quit_w, button_h)) &&
      !played_) {
    played_ = true;
    if (quit_) {
      quit_();
    }
  }
  ImGui::SameLine();
  const bool play_enabled = options_.in_game || game_check_.ok;
  ImGui::BeginDisabled(!play_enabled);
  ImGui::PushStyleColor(ImGuiCol_Button, kAccent);
  ImGui::PushStyleColor(ImGuiCol_ButtonHovered, kAccentHover);
  ImGui::PushStyleColor(ImGuiCol_ButtonActive, kAccentHover);
  ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.06f, 0.04f, 0.02f, 1.0f));
  ImGui::PushFont(font_bold, 22.0f * s);
  bool play =
      ImGui::Button(options_.in_game ? "BACK TO GAME" : "PLAY", ImVec2(play_w, button_h));
  if (const int autoplay_ms = REXCVAR_GET(launcher_autoplay_ms); autoplay_ms > 0) {
    if (first_frame_) {
      shown_time_ = ImGui::GetTime();
    } else if ((ImGui::GetTime() - shown_time_) * 1000.0 >= autoplay_ms) {
      play = true;
    }
  }
  if (first_frame_ && play_enabled) {
    ImGui::SetItemDefaultFocus();
    ImGui::SetKeyboardFocusHere(-1);
  }
  ImGui::PopFont();
  ImGui::PopStyleColor(4);
  ImGui::EndDisabled();

  // Footer.
  ImGui::Dummy(ImVec2(0, 4.0f * s));
  ImGui::PushFont(nullptr, 15.0f * s);
  ImGui::PushStyleColor(ImGuiCol_Text, kTextDim);
  ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + content_w);
  if (options_.in_game) {
    ImGui::TextUnformatted(
        "Display and screen mode change now. Resolution and vsync take effect the "
        "next time the game starts. The game keeps running while this menu is open. Close: B, Esc, "
        "F1 or Start + Back.");
  } else {
    ImGui::TextUnformatted(
        "Mouse, keyboard (arrows, Enter, Esc) or controller (D-pad, A, B). Settings are saved for "
        "next time. In the game: F1, Esc or Start + Back for these options.");
  }
  ImGui::PopTextWrapPos();
  ImGui::PopStyleColor();
  ImGui::PopFont();

  ImGui::End();
  ImGui::PopFont();
  ImGui::PopStyleColor(20);
  ImGui::PopStyleVar(8);

  first_frame_ = false;

  if (close_requested_) {
    close_requested_ = false;
    play = true;
  }
  if (play && play_enabled && !played_) {
    played_ = true;
    SaveAll();
    PlayCallback play_callback = play_;
    std::filesystem::path game = game_path_;
    if (play_callback) {
      play_callback(game);
    }
  }
}

}  // namespace rex::ui
