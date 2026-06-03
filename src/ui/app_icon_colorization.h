#pragma once

#include "config/config_types.h"
#include "render/core/color.h"
#include "ui/signal.h"

#include <cstdint>
#include <optional>

struct ShellConfig;
struct WidgetBarCapsuleSpec;

struct AppIconColorizationStyle {
  Color tint = rgba(1.0f, 1.0f, 1.0f, 1.0f);
  bool monochrome = false;
};

struct ShellAppIconColorizationSettings {
  bool enabled = false;
  std::optional<ColorSpec> color;

  bool operator==(const ShellAppIconColorizationSettings&) const = default;
};

// App-icon bake: desktop/tray/taskbar/dock bitmap icons only (CPU desaturate + tint).
// Bar launcher/control-center custom_image uses Image::setForegroundTint (alpha-mask recolor in shader).

[[nodiscard]] ShellAppIconColorizationSettings shellAppIconColorizationSettings(const ShellConfig& shell) noexcept;
[[nodiscard]] std::optional<ColorSpec> effectiveShellAppIconColorizationTint(const ShellConfig& shell) noexcept;
[[nodiscard]] std::optional<ColorSpec>
effectiveShellCustomBarImageTint(const ShellConfig& shell, const WidgetBarCapsuleSpec& capsule) noexcept;
[[nodiscard]] AppIconColorizationStyle resolveAppIconColorization(const ColorSpec& tint) noexcept;
void bakeAppIconForColorization(std::uint8_t* rgba, int width, int height, const Color& tint) noexcept;

[[nodiscard]] Signal<>& shellAppIconColorizationChanged() noexcept;
void notifyShellAppIconColorizationChanged() noexcept;
