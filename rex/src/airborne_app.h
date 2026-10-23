// airborne - ReXGlue Recompiled Project
//
// Customize your app by overriding virtual hooks from rex::ReXApp.

#pragma once

#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/logging.h>
#include <rex/platform.h>
#include <rex/rex_app.h>

#include "input_script.h"
#include "renderdoc_trigger.h"

class AirborneApp : public rex::ReXApp {
 public:
  using rex::ReXApp::ReXApp;

  static std::unique_ptr<rex::ui::WindowedApp> Create(
      rex::ui::WindowedAppContext& ctx) {
    return std::unique_ptr<AirborneApp>(new AirborneApp(ctx, "airborne",
        PPCImageConfig));
  }

  // The Switch has no command line: without --game_data_root, use the game
  // folder next to the executable (sdmc:/switch/airborne-nx/game on the SD,
  // see AGENTS.md). The command line still takes precedence. This runs before
  // airborne.toml is read, and game_data_root cannot be set from the toml.
  void OnConfigurePaths(rex::PathConfig& paths) override {
    if (!paths.game_data_root.empty()) {
      return;
    }
    const auto game = rex::filesystem::GetExecutableFolder() / "game";
    if (std::filesystem::exists(game / "default.xex")) {
      paths.game_data_root = game;
    }
  }

  // Defaults that must hold without airborne.toml (the toml, read before this,
  // and the command line still win):
  //   gpu_plugin     without it the SDK silently runs with no GPU emulation.
  //   user_language  3 = German: this dump's English localization is a broken
  //                  Russian one (see docs/ASSESSMENT.md).
  //   mnk_mode       keyboard as the pad, PC only: on the Switch the synthetic
  //                  device would shadow the real controllers.
  void OnPostInitLogging() override {
    SetIfUnset("gpu_plugin", "xenos");
    SetIfUnset("user_language", "3");
#if !REX_PLATFORM_SWITCH
    SetIfUnset("mnk_mode", "true");
#endif
  }

  // Install the scripted gamepad when the input_script cvar is set.
  void OnPreSetup(rex::RuntimeConfig& config) override { InstallInputScript(config); }

  // Schedule RenderDoc captures when renderdoc_capture_at is set.
  void OnPostSetup() override { StartRenderDocTrigger(); }

  // Override virtual hooks for customization:
  // void OnLoadXexImage(std::string& xex_image) override {}
  // void OnPostLoadXexImage() override {}
  // void OnCreateDialogs(rex::ui::ImGuiDrawer* drawer) override {}
  // std::unique_ptr<rex::ui::ImGuiDialog> CreateAchievementsOverlay() override;
  // std::unique_ptr<rex::ui::AchievementNotificationDialog>
  // CreateAchievementNotificationDialog() override;
  // void OnShutdown() override {}

 private:
  static void SetIfUnset(const char* name, const char* value) {
    if (rex::cvar::GetFlagInfo(name) == nullptr || rex::cvar::HasNonDefaultValue(name)) {
      return;
    }
    if (!rex::cvar::SetFlagByName(name, value)) {
      REXLOG_WARN("[airborne] cannot set default {} = {}", name, value);
    }
  }
};
