#pragma once

#include "config/config_types.h"
#include "render/animation/animation_manager.h"
#include "system/desktop_entry.h"
#include "system/desktop_entry_launch.h"

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

class Box;
class CompositorPlatform;
class ConfigService;
class Flex;
class Glyph;
class IconResolver;
class Image;
class InputArea;
class Label;
class RenderContext;
struct wl_output;
struct wl_surface;
struct zwlr_foreign_toplevel_handle_v1;

namespace shell::dock {

  struct DockInstance;

  struct DockItemView {
    DesktopEntry entry;
    std::string idLower;
    std::string startupWmClassLower;
    InputArea* area = nullptr;
    Box* background = nullptr;
    std::array<Box*, 3> dotIndicators{};
    Box* badge = nullptr;
    Label* badgeLabel = nullptr;
    Image* iconImage = nullptr;
    Glyph* iconGlyph = nullptr;
    bool hovered = false;
    bool running = false;
    bool active = false;
    float visualScale = -1.0f;
    float visualOpacity = -1.0f;
    AnimationManager::Id scaleAnimId = 0;
    AnimationManager::Id opacityAnimId = 0;
    std::size_t instanceCount = 0;
  };

  struct DockItemModelDependencies {
    CompositorPlatform& platform;
    ConfigService& config;
    std::unordered_map<std::string, zwlr_foreign_toplevel_handle_v1*>& lastActiveHandleByAppIdLower;
    const std::vector<DesktopEntry>& pinnedEntries;
    std::uint64_t modelSerial = 0;
  };

  struct DockItemSceneDependencies {
    DockItemModelDependencies model;
    RenderContext& renderContext;
    IconResolver& iconResolver;
  };

  struct DockItemCallbacks {
    std::function<void()> pruneCachedToplevelHandles;
    std::function<desktop_entry_launch::LaunchOptions(wl_surface*)> launchOptions;
    std::function<void(DockInstance&)> toggleLauncher;
    std::function<void(DockInstance&, DockItemView&)> openItemMenu;
  };

  [[nodiscard]] std::string currentActiveEntryIdLower(const CompositorPlatform& platform);
  [[nodiscard]] wl_output* dockFilterOutput(const DockConfig& cfg, wl_output* instanceOutput);
  [[nodiscard]] bool refreshPinnedAppsIfNeeded(
      const DockConfig& cfg, std::vector<std::string>& lastPinnedConfig, std::vector<DesktopEntry>& pinnedEntries,
      std::uint64_t& modelSerial, std::uint64_t& entriesVersion
  );
  [[nodiscard]] bool matchesActiveApp(const DockItemView& item, std::string_view activeAppIdLower);
  [[nodiscard]] bool matchesRunningApp(const DockItemView& item, const std::vector<std::string>& runningLower);
  [[nodiscard]] bool syncInstanceModel(DockInstance& instance, DockItemModelDependencies deps);

  [[nodiscard]] std::string_view dockLauncherIconGlyph(const DockConfig& cfg);
  [[nodiscard]] std::unique_ptr<Flex> makeDockItemRow(const DockConfig& cfg, bool vertical);
  void rebuildItems(DockInstance& instance, DockItemSceneDependencies deps, const DockItemCallbacks& callbacks);
  void updateVisuals(DockInstance& instance, DockItemSceneDependencies deps);

} // namespace shell::dock
