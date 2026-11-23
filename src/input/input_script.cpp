/**
 * @file        input/input_script.cpp
 * @brief       Timed controller input played back from a text file (see input_script.h).
 */
#include <rex/input/input_script.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <sstream>

#include <rex/cvar.h>
#include <rex/logging.h>

namespace rex::input {

namespace {

std::string Upper(std::string s) {
  for (auto& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
  return s;
}

bool ParseButtons(const std::string& spec, uint16_t& out) {
  static const struct {
    const char* name;
    uint16_t mask;
  } kNames[] = {
      {"A", X_INPUT_GAMEPAD_A},
      {"B", X_INPUT_GAMEPAD_B},
      {"X", X_INPUT_GAMEPAD_X},
      {"Y", X_INPUT_GAMEPAD_Y},
      {"START", X_INPUT_GAMEPAD_START},
      {"BACK", X_INPUT_GAMEPAD_BACK},
      {"LB", X_INPUT_GAMEPAD_LEFT_SHOULDER},
      {"RB", X_INPUT_GAMEPAD_RIGHT_SHOULDER},
      {"LS", X_INPUT_GAMEPAD_LEFT_THUMB},
      {"RS", X_INPUT_GAMEPAD_RIGHT_THUMB},
      {"UP", X_INPUT_GAMEPAD_DPAD_UP},
      {"DOWN", X_INPUT_GAMEPAD_DPAD_DOWN},
      {"LEFT", X_INPUT_GAMEPAD_DPAD_LEFT},
      {"RIGHT", X_INPUT_GAMEPAD_DPAD_RIGHT},
      {"GUIDE", X_INPUT_GAMEPAD_GUIDE},
  };
  out = 0;
  std::stringstream ss(Upper(spec));
  std::string part;
  while (std::getline(ss, part, '+')) {
    auto it = std::find_if(std::begin(kNames), std::end(kNames),
                           [&](const auto& n) { return part == n.name; });
    if (it == std::end(kNames)) return false;
    out |= it->mask;
  }
  return out != 0;
}

int16_t AxisValue(float v) {
  v = std::clamp(v, -1.0f, 1.0f);
  return static_cast<int16_t>(std::lround(v * 32767.0f));
}

}  // namespace

int64_t InputScript::NowMs() const {
  using namespace std::chrono;
  return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count() - epoch_ms_;
}

std::unique_ptr<InputScript> InputScript::Load(const std::filesystem::path& path) {
  std::ifstream file(path);
  if (!file) {
    REXSYS_ERROR("input_script: cannot open {}", path.string());
    return nullptr;
  }
  std::unique_ptr<InputScript> script(new InputScript());
  int64_t time = 0;
  uint32_t user = 0;
  std::string line;
  int line_no = 0;
  int errors = 0;
  while (std::getline(file, line)) {
    line_no++;
    if (auto hash = line.find('#'); hash != std::string::npos) line.resize(hash);
    std::stringstream ss(line);
    std::string time_tok, cmd;
    if (!(ss >> time_tok)) continue;
    auto bad = [&](const char* why) {
      REXSYS_ERROR("input_script: {}:{}: {}", path.filename().string(), line_no, why);
      errors++;
    };
    char* end = nullptr;
    const bool relative = time_tok[0] == '+';
    const long long t = std::strtoll(time_tok.c_str() + (relative ? 1 : 0), &end, 10);
    if (*end != '\0' || t < 0) {
      bad("time must be a number of ms, or +ms");
      continue;
    }
    time = relative ? time + t : t;
    if (!(ss >> cmd)) {
      bad("missing command");
      continue;
    }
    cmd = Upper(cmd);
    Event ev;
    ev.start_ms = time;
    ev.user = user;
    ev.line = line_no;
    if (cmd == "PRESS" || cmd == "HOLD") {
      std::string spec;
      int64_t ms = cmd == "PRESS" ? 120 : -1;
      ss >> spec;
      ss >> ms;
      if (!ParseButtons(spec, ev.buttons)) {
        bad("unknown button");
        continue;
      }
      if (ms <= 0) {
        bad("hold needs a duration in ms");
        continue;
      }
      ev.kind = Kind::kButtons;
      ev.end_ms = time + ms;
    } else if (cmd == "STICK" || cmd == "TRIGGER") {
      std::string side;
      int64_t ms = 0;
      ss >> side;
      side = Upper(side);
      if (side != "L" && side != "R") {
        bad("side must be L or R");
        continue;
      }
      ev.right = side == "R";
      if (cmd == "STICK") {
        ev.kind = Kind::kStick;
        if (!(ss >> ev.x >> ev.y >> ms)) {
          bad("stick needs x y ms");
          continue;
        }
      } else {
        ev.kind = Kind::kTrigger;
        if (!(ss >> ev.x >> ms)) {
          bad("trigger needs value ms");
          continue;
        }
      }
      if (ms <= 0) {
        bad("duration must be > 0");
        continue;
      }
      ev.end_ms = time + ms;
    } else if (cmd == "USER") {
      if (!(ss >> user) || user > 3) {
        bad("user must be 0-3");
        user = 0;
      }
      continue;
    } else if (cmd == "LOG") {
      ev.kind = Kind::kLog;
      std::getline(ss, ev.text);
      ev.text.erase(0, ev.text.find_first_not_of(' '));
      ev.end_ms = time;
    } else if (cmd == "SET") {
      ev.kind = Kind::kSet;
      if (!(ss >> ev.text >> ev.value)) {
        bad("set needs a name and a value");
        continue;
      }
      ev.end_ms = time;
    } else if (cmd == "QUIT") {
      ev.kind = Kind::kQuit;
      ev.end_ms = time;
    } else {
      bad("unknown command");
      continue;
    }
    script->events_.push_back(std::move(ev));
  }
  if (script->events_.empty()) {
    REXSYS_ERROR("input_script: {} has no events", path.string());
    return nullptr;
  }
  using namespace std::chrono;
  script->epoch_ms_ =
      duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
  REXSYS_INFO("input_script: loaded {} events from {} ({} errors); last at {} ms",
              script->events_.size(), path.string(), errors,
              std::max_element(script->events_.begin(), script->events_.end(),
                               [](const Event& a, const Event& b) { return a.end_ms < b.end_ms; })
                  ->end_ms);
  return script;
}

bool InputScript::HasUser(uint32_t user_index) const {
  return std::any_of(events_.begin(), events_.end(), [&](const Event& e) {
    return e.user == user_index &&
           (e.kind == Kind::kButtons || e.kind == Kind::kStick || e.kind == Kind::kTrigger);
  });
}

uint32_t InputScript::Apply(uint32_t user_index, X_INPUT_GAMEPAD& pad) {
  std::lock_guard<std::mutex> lock(mutex_);
  const int64_t now = NowMs();
  uint16_t buttons = 0;
  bool lstick = false, rstick = false, ltrig = false, rtrig = false;
  int16_t lx = 0, ly = 0, rx = 0, ry = 0;
  uint8_t lt = 0, rt = 0;

  for (auto& ev : events_) {
    if (ev.start_ms > now) continue;
    // Log and quit fire once, whatever user is polling; the others apply to their user.
    if (!ev.started) {
      ev.started = true;
      switch (ev.kind) {
        case Kind::kLog:
          REXSYS_INFO("input_script: t={} ms (line {}): {}", now, ev.line, ev.text);
          break;
        case Kind::kSet: {
          const bool ok = rex::cvar::SetFlagByName(ev.text, ev.value);
          REXSYS_INFO("input_script: t={} ms (line {}): set {} = {}{}", now, ev.line, ev.text,
                      ev.value, ok ? "" : " (FAILED: unknown setting or bad value)");
          break;
        }
        case Kind::kQuit:
          REXSYS_INFO("input_script: t={} ms (line {}): quit", now, ev.line);
          rex::FlushLogging();
          std::_Exit(0);
        default:
          REXSYS_INFO("input_script: t={} ms (line {}): user {} {} for {} ms", now, ev.line,
                      ev.user,
                      ev.kind == Kind::kButtons ? "buttons" : ev.kind == Kind::kStick ? "stick"
                                                                                       : "trigger",
                      ev.end_ms - ev.start_ms);
      }
    }
    if (ev.user != user_index || now >= ev.end_ms) continue;
    switch (ev.kind) {
      case Kind::kButtons:
        buttons |= ev.buttons;
        break;
      case Kind::kStick:
        (ev.right ? rstick : lstick) = true;
        (ev.right ? rx : lx) = AxisValue(ev.x);
        (ev.right ? ry : ly) = AxisValue(ev.y);
        break;
      case Kind::kTrigger:
        (ev.right ? rtrig : ltrig) = true;
        (ev.right ? rt : lt) =
            static_cast<uint8_t>(std::lround(std::clamp(ev.x, 0.0f, 1.0f) * 255.0f));
        break;
      default:
        break;
    }
  }

  pad.buttons = static_cast<uint16_t>(static_cast<uint16_t>(pad.buttons) | buttons);
  if (lstick) {
    pad.thumb_lx = lx;
    pad.thumb_ly = ly;
  }
  if (rstick) {
    pad.thumb_rx = rx;
    pad.thumb_ry = ry;
  }
  if (ltrig) pad.left_trigger = lt;
  if (rtrig) pad.right_trigger = rt;

  if (user_index >= 4) return 0;
  // Count changes of the scripted part, so the packet number moves when it does.
  const uint64_t signature = uint64_t(buttons) | uint64_t(uint16_t(lx)) << 16 |
                             uint64_t(uint16_t(ly)) << 32 | uint64_t(uint16_t(rx)) << 48;
  const uint64_t signature2 = uint64_t(uint16_t(ry)) | uint64_t(lt) << 16 | uint64_t(rt) << 24 |
                              uint64_t(lstick) << 32 | uint64_t(rstick) << 33 |
                              uint64_t(ltrig) << 34 | uint64_t(rtrig) << 35;
  const uint64_t combined = signature * 0x9E3779B97F4A7C15ull ^ signature2;
  auto& state = users_[user_index];
  if (combined != state.last_signature) {
    state.last_signature = combined;
    state.changes++;
  }
  return state.changes;
}

}  // namespace rex::input
