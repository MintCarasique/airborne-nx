// Scripted virtual Xbox 360 gamepad. Syntax of the `input_script` cvar:
//   <t>[-<t2>]:<button>[+<button>...][,...]   (times in seconds)
// Time zero is the game's first GetDeviceState poll. A single time is a press
// held for 0.25 s.

#include "input_script.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <rex/cvar.h>
#include <rex/input/input.h>
#include <rex/input/input_driver.h>
#include <rex/input/input_system.h>
#include <rex/logging.h>

REXCVAR_DEFINE_STRING(input_script, "", "Input",
                      "Scripted gamepad steps, e.g. \"20:start,26.5-27.5:down,30:a+x\"");

namespace {

using namespace rex::input;
using rex::X_RESULT;
using rex::X_STATUS;

constexpr DeviceId kScriptDevice = static_cast<DeviceId>(0x53435250);
constexpr double kDefaultHold = 0.25;

struct Step {
  double start = 0;
  double end = 0;
  uint16_t buttons = 0;
  uint8_t left_trigger = 0;
  uint8_t right_trigger = 0;
  std::string text;  // for logging
  bool logged = false;
};

std::string Trim(const std::string& s) {
  size_t b = s.find_first_not_of(" \t\r\n");
  if (b == std::string::npos) return {};
  size_t e = s.find_last_not_of(" \t\r\n");
  return s.substr(b, e - b + 1);
}

bool ParseTime(const std::string& s, double* out) {
  std::string t = Trim(s);
  if (t.empty()) return false;
  char* end = nullptr;
  double v = std::strtod(t.c_str(), &end);
  if (end != t.c_str() + t.size() || v < 0) return false;
  *out = v;
  return true;
}

// Returns false for an unknown button name.
bool ApplyButton(const std::string& name, Step& step) {
  struct Entry {
    const char* name;
    uint16_t bit;
  };
  static const Entry kButtons[] = {
      {"a", X_INPUT_GAMEPAD_A},
      {"b", X_INPUT_GAMEPAD_B},
      {"x", X_INPUT_GAMEPAD_X},
      {"y", X_INPUT_GAMEPAD_Y},
      {"lb", X_INPUT_GAMEPAD_LEFT_SHOULDER},
      {"rb", X_INPUT_GAMEPAD_RIGHT_SHOULDER},
      {"back", X_INPUT_GAMEPAD_BACK},
      {"start", X_INPUT_GAMEPAD_START},
      {"up", X_INPUT_GAMEPAD_DPAD_UP},
      {"down", X_INPUT_GAMEPAD_DPAD_DOWN},
      {"left", X_INPUT_GAMEPAD_DPAD_LEFT},
      {"right", X_INPUT_GAMEPAD_DPAD_RIGHT},
      {"ls", X_INPUT_GAMEPAD_LEFT_THUMB},
      {"rs", X_INPUT_GAMEPAD_RIGHT_THUMB},
  };
  if (name == "lt") {
    step.left_trigger = 255;
    return true;
  }
  if (name == "rt") {
    step.right_trigger = 255;
    return true;
  }
  for (const auto& e : kButtons) {
    if (name == e.name) {
      step.buttons |= e.bit;
      return true;
    }
  }
  return false;
}

bool ParseStep(const std::string& text, Step* out) {
  size_t colon = text.find(':');
  if (colon == std::string::npos) return false;
  std::string times = text.substr(0, colon);
  std::string buttons = text.substr(colon + 1);

  Step step;
  size_t dash = times.find('-');
  if (dash == std::string::npos) {
    if (!ParseTime(times, &step.start)) return false;
    step.end = step.start + kDefaultHold;
  } else {
    if (!ParseTime(times.substr(0, dash), &step.start) ||
        !ParseTime(times.substr(dash + 1), &step.end) || step.end <= step.start) {
      return false;
    }
  }

  size_t pos = 0;
  while (pos <= buttons.size()) {
    size_t plus = buttons.find('+', pos);
    if (plus == std::string::npos) plus = buttons.size();
    std::string name = Trim(buttons.substr(pos, plus - pos));
    std::transform(name.begin(), name.end(), name.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (!ApplyButton(name, step)) return false;
    pos = plus + 1;
  }
  step.text = Trim(buttons);
  *out = std::move(step);
  return true;
}

class ScriptInputDriver : public InputDriver {
 public:
  explicit ScriptInputDriver(const std::string& script) : InputDriver(nullptr, 0) {
    size_t pos = 0;
    while (pos <= script.size()) {
      size_t comma = script.find(',', pos);
      if (comma == std::string::npos) comma = script.size();
      std::string text = Trim(script.substr(pos, comma - pos));
      pos = comma + 1;
      if (text.empty()) continue;
      Step step;
      if (ParseStep(text, &step)) {
        steps_.push_back(std::move(step));
      } else {
        REXLOG_WARN("[input_script] bad step '{}', skipped", text);
      }
    }
    REXLOG_INFO("[input_script] {} step(s) loaded", steps_.size());
  }

  X_STATUS Setup() override { return X_STATUS_SUCCESS; }

  void EnumerateDevices(std::vector<DeviceInfo>& out) override {
    DeviceInfo info;
    info.id = kScriptDevice;
    info.name = "Scripted gamepad";
    info.synthetic = true;
    out.push_back(info);
  }

  X_RESULT GetDeviceCapabilities(DeviceId id, uint32_t, X_INPUT_CAPABILITIES* out_caps) override {
    if (id != kScriptDevice) return X_ERROR_DEVICE_NOT_CONNECTED;
    if (out_caps) {
      std::memset(out_caps, 0, sizeof(*out_caps));
      out_caps->type = 0x01;      // XINPUT_DEVTYPE_GAMEPAD
      out_caps->sub_type = 0x01;  // XINPUT_DEVSUBTYPE_GAMEPAD
      out_caps->gamepad.buttons = 0xFFFF;
      out_caps->gamepad.left_trigger = 0xFF;
      out_caps->gamepad.right_trigger = 0xFF;
      out_caps->gamepad.thumb_lx = static_cast<int16_t>(0x7FFF);
      out_caps->gamepad.thumb_ly = static_cast<int16_t>(0x7FFF);
      out_caps->gamepad.thumb_rx = static_cast<int16_t>(0x7FFF);
      out_caps->gamepad.thumb_ry = static_cast<int16_t>(0x7FFF);
      out_caps->vibration.left_motor_speed = 0xFFFF;
      out_caps->vibration.right_motor_speed = 0xFFFF;
    }
    return X_ERROR_SUCCESS;
  }

  X_RESULT GetDeviceState(DeviceId id, X_INPUT_STATE* out_state) override {
    if (id != kScriptDevice) return X_ERROR_DEVICE_NOT_CONNECTED;
    if (!out_state) return X_ERROR_SUCCESS;

    std::lock_guard<std::mutex> lock(mutex_);
    auto now = std::chrono::steady_clock::now();
    if (!started_) {
      started_ = true;
      t0_ = now;
    }
    double t = std::chrono::duration<double>(now - t0_).count();

    uint16_t buttons = 0;
    uint8_t lt = 0, rt = 0;
    for (auto& s : steps_) {
      if (t < s.start || t >= s.end) continue;
      buttons |= s.buttons;
      lt = std::max(lt, s.left_trigger);
      rt = std::max(rt, s.right_trigger);
      if (!s.logged) {
        s.logged = true;
        REXLOG_INFO("[input_script] t={:.1f} s: {}", t, s.text);
      }
    }

    if (!have_last_ || buttons != last_buttons_ || lt != last_lt_ || rt != last_rt_) {
      ++packet_;
      have_last_ = true;
      last_buttons_ = buttons;
      last_lt_ = lt;
      last_rt_ = rt;
    }

    std::memset(out_state, 0, sizeof(*out_state));
    out_state->packet_number = packet_;
    out_state->gamepad.buttons = buttons;
    out_state->gamepad.left_trigger = lt;
    out_state->gamepad.right_trigger = rt;
    return X_ERROR_SUCCESS;
  }

  X_RESULT SetDeviceVibration(DeviceId id, X_INPUT_VIBRATION*) override {
    return id == kScriptDevice ? X_ERROR_SUCCESS : X_ERROR_DEVICE_NOT_CONNECTED;
  }

  X_RESULT GetDeviceKeystroke(DeviceId id, uint32_t, X_INPUT_KEYSTROKE*) override {
    return id == kScriptDevice ? X_ERROR_EMPTY : X_ERROR_DEVICE_NOT_CONNECTED;
  }

 private:
  std::mutex mutex_;
  std::vector<Step> steps_;
  bool started_ = false;
  std::chrono::steady_clock::time_point t0_;
  bool have_last_ = false;
  uint16_t last_buttons_ = 0;
  uint8_t last_lt_ = 0, last_rt_ = 0;
  uint32_t packet_ = 0;
};

}  // namespace

void InstallInputScript(rex::RuntimeConfig& config) {
  std::string script = REXCVAR_GET(input_script);
  if (script.empty()) return;

  auto original = std::move(config.input_factory);
  config.input_factory = [original = std::move(original), script](bool tool_mode) {
    std::unique_ptr<rex::system::IInputSystem> system;
    if (original) system = original(tool_mode);
    if (!tool_mode && system) {
      static_cast<InputSystem*>(system.get())
          ->AddDriver(std::make_unique<ScriptInputDriver>(script));
    }
    return system;
  };
}
