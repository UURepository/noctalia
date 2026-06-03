#pragma once

#include "config/config_service.h"
#include "shell/bar/widget.h"
#include "ui/signal.h"

#include <cstdint>
#include <string>

class Glyph;
class Image;
struct wl_output;

class LauncherWidget : public Widget {
public:
  LauncherWidget(ConfigService& config, wl_output* output, std::string barGlyphId, std::string logoPath = "");

  void create() override;

private:
  void doLayout(Renderer& renderer, float containerWidth, float containerHeight) override;
  void doUpdate(Renderer& renderer) override;
  void refreshCustomImageTint();
  ConfigService& m_config;
  std::string m_barGlyphId;
  std::string m_logoPath;
  Glyph* m_glyph = nullptr;
  Image* m_image = nullptr;
  Signal<>::ScopedConnection m_appIconColorizeConn;
};
