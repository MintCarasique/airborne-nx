// airborne - ReXGlue Recompiled Project
//
// Customize your app by overriding virtual hooks from rex::ReXApp.

#pragma once

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

  // Install the scripted gamepad when the input_script cvar is set.
  void OnPreSetup(rex::RuntimeConfig& config) override { InstallInputScript(config); }

  // Schedule RenderDoc captures when renderdoc_capture_at is set.
  void OnPostSetup() override { StartRenderDocTrigger(); }

  // Override virtual hooks for customization:
  // void OnPostInitLogging() override {}
  // void OnLoadXexImage(std::string& xex_image) override {}
  // void OnPostLoadXexImage() override {}
  // void OnPostSetup() override {}
  // void OnCreateDialogs(rex::ui::ImGuiDrawer* drawer) override {}
  // std::unique_ptr<rex::ui::ImGuiDialog> CreateAchievementsOverlay() override;
  // std::unique_ptr<rex::ui::AchievementNotificationDialog>
  // CreateAchievementNotificationDialog() override;
  // void OnShutdown() override {}
  // void OnConfigurePaths(rex::PathConfig& paths) override {}
};
