#include "capture/screenshot_region_overlay.h"

#include "config/config_types.h"
#include "core/deferred_call.h"
#include "core/key_modifiers.h"
#include "core/keybind_matcher.h"
#include "core/log.h"
#include "core/ui_phase.h"
#include "render/animation/animation_manager.h"
#include "render/core/color.h"
#include "render/core/render_styles.h"
#include "render/render_context.h"
#include "render/scene/input_area.h"
#include "render/scene/input_dispatcher.h"
#include "render/scene/node.h"
#include "ui/controls/box.h"
#include "ui/controls/image.h"
#include "ui/controls/label.h"
#include "ui/palette.h"
#include "ui/style.h"
#include "wayland/layer_surface.h"
#include "wayland/wayland_connection.h"
#include "wayland/wayland_seat.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <linux/input-event-codes.h>
#include <memory>

namespace capture {
  namespace {

    constexpr Logger kLog("screenshot-region");
    constexpr float kDimensionFontSize = 14.0f;
    constexpr float kDimensionCursorOffsetX = 12.0f;
    constexpr float kDimensionCursorOffsetY = 14.0f;
    constexpr float kDimensionPaddingX = 6.0f;
    constexpr float kDimensionPaddingY = 4.0f;

    [[nodiscard]] const WaylandOutput* findOutput(const WaylandConnection& wayland, wl_output* output) {
      for (const auto& entry : wayland.outputs()) {
        if (entry.output == output) {
          return &entry;
        }
      }
      return nullptr;
    }

    [[nodiscard]] LayerShellKeyboard overlayKeyboardMode() { return LayerShellKeyboard::Exclusive; }

    [[nodiscard]] const ScreencopyImage*
    frozenImageForOutput(const std::vector<FrozenScreenshot>& screenshots, wl_output* output) {
      for (const auto& entry : screenshots) {
        if (entry.output == output) {
          return &entry.image;
        }
      }
      return nullptr;
    }

  } // namespace

  struct ScreenshotRegionOverlay::Instance {
    wl_output* output = nullptr;
    std::unique_ptr<LayerSurface> surface;
    std::unique_ptr<Node> sceneRoot;
    InputArea* input = nullptr;
    Image* backdrop = nullptr;
    Box* dim = nullptr;
    Box* selection = nullptr;
    Box* dimensionsBadge = nullptr;
    Label* dimensionsLabel = nullptr;
    AnimationManager animations;
    InputDispatcher inputDispatcher;
    bool pointerInside = false;
  };

  ScreenshotRegionOverlay::ScreenshotRegionOverlay() = default;

  ScreenshotRegionOverlay::~ScreenshotRegionOverlay() = default;

  void ScreenshotRegionOverlay::initialize(WaylandConnection& wayland, RenderContext* renderContext) {
    m_wayland = &wayland;
    m_renderContext = renderContext;
  }

  void ScreenshotRegionOverlay::setCompleteCallback(CompleteCallback callback) { m_onComplete = std::move(callback); }

  void ScreenshotRegionOverlay::setFrozenScreenshots(std::vector<FrozenScreenshot> screenshots) {
    m_frozenScreenshots = std::move(screenshots);
  }

  void ScreenshotRegionOverlay::begin(bool freezeScreen) {
    if (m_wayland == nullptr || m_renderContext == nullptr) {
      return;
    }
    destroySurfaces();
    m_freezeScreen = freezeScreen;
    m_active = true;
    m_dragging = false;
    ensureSurfaces();
    for (auto& inst : m_instances) {
      if (inst->surface != nullptr) {
        inst->surface->requestLayout();
        inst->surface->requestRedraw();
      }
    }
  }

  void ScreenshotRegionOverlay::cancel() {
    m_active = false;
    m_dragging = false;
    m_freezeScreen = false;
    m_frozenScreenshots.clear();
    destroySurfaces();
  }

  void ScreenshotRegionOverlay::cancelSelection() {
    if (!m_active) {
      return;
    }
    DeferredCall::callLater([this]() {
      if (!m_active) {
        return;
      }
      cancel();
      if (m_onComplete) {
        m_onComplete(std::nullopt, nullptr);
      }
    });
  }

  void ScreenshotRegionOverlay::onOutputChange() {
    if (!m_active) {
      return;
    }
    if (!m_instances.empty() && !surfacesMatchOutputs()) {
      destroySurfaces();
      ensureSurfaces();
    }
  }

  bool ScreenshotRegionOverlay::surfacesMatchOutputs() const {
    if (m_wayland == nullptr) {
      return m_instances.empty();
    }
    const auto& outputs = m_wayland->outputs();
    if (m_instances.size() != outputs.size()) {
      return false;
    }
    for (std::size_t i = 0; i < outputs.size(); ++i) {
      if (m_instances[i] == nullptr || m_instances[i]->output != outputs[i].output) {
        return false;
      }
    }
    return true;
  }

  void ScreenshotRegionOverlay::ensureSurfaces() {
    if (m_wayland == nullptr || m_renderContext == nullptr || !m_active) {
      return;
    }
    if (!m_instances.empty() && surfacesMatchOutputs()) {
      return;
    }
    destroySurfaces();

    for (const auto& output : m_wayland->outputs()) {
      if (output.output == nullptr || output.logicalWidth <= 0 || output.logicalHeight <= 0) {
        continue;
      }

      auto inst = std::make_unique<Instance>();
      inst->output = output.output;

      auto config = LayerSurfaceConfig{
          .nameSpace = "noctalia-screenshot-region",
          .layer = LayerShellLayer::Overlay,
          .anchor = LayerShellAnchor::Top | LayerShellAnchor::Bottom | LayerShellAnchor::Left | LayerShellAnchor::Right,
          .width = 0,
          .height = 0,
          .exclusiveZone = -1,
          .keyboard = overlayKeyboardMode(),
          .defaultWidth = static_cast<std::uint32_t>(output.logicalWidth),
          .defaultHeight = static_cast<std::uint32_t>(output.logicalHeight),
      };

      inst->surface = std::make_unique<LayerSurface>(*m_wayland, std::move(config));
      auto* instPtr = inst.get();
      inst->surface->setRenderContext(m_renderContext);
      inst->surface->setAnimationManager(&inst->animations);
      inst->surface->setConfigureCallback([instPtr](std::uint32_t /*width*/, std::uint32_t /*height*/) {
        instPtr->surface->requestLayout();
      });
      inst->surface->setPrepareFrameCallback([this, instPtr](bool needsUpdate, bool needsLayout) {
        prepareFrame(*instPtr, needsUpdate, needsLayout);
      });

      if (!inst->surface->initialize(output.output)) {
        kLog.warn("failed to initialize screenshot region overlay on {}", output.connectorName);
        continue;
      }

      m_instances.push_back(std::move(inst));
    }
  }

  void ScreenshotRegionOverlay::destroySurfaces() {
    for (auto& inst : m_instances) {
      if (inst != nullptr) {
        if (inst->backdrop != nullptr && m_renderContext != nullptr) {
          inst->backdrop->clear(*m_renderContext);
        }
        inst->inputDispatcher.setSceneRoot(nullptr);
        inst->animations.cancelAll();
      }
    }
    m_instances.clear();
    m_dragging = false;
  }

  void ScreenshotRegionOverlay::prepareFrame(Instance& inst, bool /*needsUpdate*/, bool /*needsLayout*/) {
    if (m_renderContext == nullptr || inst.surface == nullptr) {
      return;
    }

    const auto width = inst.surface->width();
    const auto height = inst.surface->height();
    if (width == 0 || height == 0) {
      return;
    }

    m_renderContext->makeCurrent(inst.surface->renderTarget());

    const bool needsSceneBuild = inst.sceneRoot == nullptr
        || static_cast<std::uint32_t>(std::round(inst.sceneRoot->width())) != width
        || static_cast<std::uint32_t>(std::round(inst.sceneRoot->height())) != height;
    if (!needsSceneBuild) {
      updateSelectionVisuals();
      return;
    }

    UiPhaseScope layoutPhase(UiPhase::Layout);

    const float w = static_cast<float>(width);
    const float h = static_cast<float>(height);

    inst.sceneRoot = std::make_unique<Node>();
    inst.sceneRoot->setSize(w, h);

    auto input = std::make_unique<InputArea>();
    input->setAcceptedButtons(InputArea::buttonMask(BTN_LEFT));
    input->setCursorShape(1); // pointer - wp cursor shape set in dispatcher from bar usually

    input->setOnPress([this, output = inst.output](const InputArea::PointerData& data) {
      if (!data.pressed || data.button != BTN_LEFT) {
        return;
      }
      const auto* out = findOutput(*m_wayland, output);
      if (out == nullptr) {
        return;
      }
      m_dragging = true;
      m_startGlobalX = static_cast<double>(out->logicalX) + static_cast<double>(data.localX);
      m_startGlobalY = static_cast<double>(out->logicalY) + static_cast<double>(data.localY);
      m_currentGlobalX = m_startGlobalX;
      m_currentGlobalY = m_startGlobalY;
      updateSelectionVisuals();
      for (auto& instance : m_instances) {
        if (instance->surface != nullptr) {
          instance->surface->requestRedraw();
        }
      }
    });

    input->setOnMotion([this, output = inst.output](const InputArea::PointerData& data) {
      if (!m_dragging) {
        return;
      }
      const auto* out = findOutput(*m_wayland, output);
      if (out == nullptr) {
        return;
      }
      m_currentGlobalX = static_cast<double>(out->logicalX) + static_cast<double>(data.localX);
      m_currentGlobalY = static_cast<double>(out->logicalY) + static_cast<double>(data.localY);
      updateSelectionVisuals();
      for (auto& instance : m_instances) {
        if (instance->surface != nullptr) {
          instance->surface->requestRedraw();
        }
      }
    });

    input->setOnClick([this](const InputArea::PointerData& data) {
      if (data.pressed || data.button != BTN_LEFT) {
        return;
      }
      if (!m_dragging) {
        return;
      }
      m_dragging = false;
      // completeSelection() tears down surfaces; defer past InputDispatcher::pointerButton.
      DeferredCall::callLater([this]() { completeSelection(); });
    });

    input->setOnKeyDown([this](const InputArea::KeyData& key) {
      if (!key.pressed) {
        return;
      }
      if (KeybindMatcher::matches(KeybindAction::Cancel, key.sym, key.modifiers)) {
        cancelSelection();
      }
    });
    input->setFocusable(true);

    const auto* frozen = m_freezeScreen ? frozenImageForOutput(m_frozenScreenshots, inst.output) : nullptr;
    if (frozen != nullptr) {
      auto backdrop = std::make_unique<Image>();
      backdrop->setFit(ImageFit::Stretch);
      backdrop->setPosition(0.0f, 0.0f);
      backdrop->setSize(w, h);
      if (!backdrop->setSourceRaw(
              *m_renderContext, frozen->rgba.data(), frozen->rgba.size(), frozen->width, frozen->height,
              frozen->width * 4, PixmapFormat::RGBA, false
          )) {
        kLog.warn("failed to upload frozen screenshot backdrop");
      }
      inst.backdrop = static_cast<Image*>(input->addChild(std::move(backdrop)));
    }

    auto dim = std::make_unique<Box>();
    dim->setFill(colorSpecFromRole(ColorRole::Surface));
    dim->setOpacity(0.45f);
    dim->setPosition(0.0f, 0.0f);
    dim->setSize(w, h);
    inst.dim = static_cast<Box*>(input->addChild(std::move(dim)));

    auto selection = std::make_unique<Box>();
    Color fill = colorForRole(ColorRole::Primary);
    fill.a = 0.28f;
    Color border = colorForRole(ColorRole::Primary);
    border.a = 1.0f;
    selection->setFill(fixedColorSpec(fill));
    selection->setBorder(fixedColorSpec(border), 2.0f);
    selection->setVisible(false);

    auto dimensionsBadge = std::make_unique<Box>();
    Color badgeFill = colorForRole(ColorRole::Surface);
    badgeFill.a = 0.94f;
    dimensionsBadge->setFill(fixedColorSpec(badgeFill));
    dimensionsBadge->setBorder(fixedColorSpec(border), 1.0f);
    dimensionsBadge->setRadius(Style::radiusSm);
    dimensionsBadge->setVisible(false);

    auto dimensionsLabel = std::make_unique<Label>();
    dimensionsLabel->setFontSize(kDimensionFontSize);
    dimensionsLabel->setFontWeight(FontWeight::Bold);
    dimensionsLabel->setColor(border);

    inst.dimensionsLabel = static_cast<Label*>(dimensionsBadge->addChild(std::move(dimensionsLabel)));
    inst.selection = static_cast<Box*>(input->addChild(std::move(selection)));
    inst.dimensionsBadge = static_cast<Box*>(input->addChild(std::move(dimensionsBadge)));
    inst.input = input.get();
    inst.sceneRoot->addChild(std::move(input));
    inst.surface->setSceneRoot(inst.sceneRoot.get());
    inst.inputDispatcher.setSceneRoot(inst.sceneRoot.get());
    inst.inputDispatcher.setCursorShapeCallback([this](std::uint32_t serial, std::uint32_t shape) {
      if (m_wayland != nullptr) {
        m_wayland->setCursorShape(serial, shape);
      }
    });
    if (inst.input != nullptr) {
      inst.inputDispatcher.setFocus(inst.input);
    }

    updateSelectionVisuals();
  }

  bool ScreenshotRegionOverlay::onPointerEvent(const PointerEvent& event) {
    if (!m_active) {
      return false;
    }

    Instance* target = nullptr;
    if (event.surface != nullptr) {
      for (auto& inst : m_instances) {
        if (inst != nullptr && inst->surface != nullptr && inst->surface->wlSurface() == event.surface) {
          target = inst.get();
          break;
        }
      }
    }

    if (target == nullptr) {
      for (auto& inst : m_instances) {
        if (inst != nullptr && inst->pointerInside) {
          target = inst.get();
          break;
        }
      }
    }

    if (target == nullptr) {
      return false;
    }

    const bool onTarget =
        event.surface != nullptr && target->surface != nullptr && event.surface == target->surface->wlSurface();

    switch (event.type) {
    case PointerEvent::Type::Enter:
      if (onTarget) {
        target->pointerInside = true;
        target->inputDispatcher.pointerEnter(static_cast<float>(event.sx), static_cast<float>(event.sy), event.serial);
      }
      return onTarget;
    case PointerEvent::Type::Leave:
      if (onTarget || target->pointerInside) {
        target->pointerInside = false;
        target->inputDispatcher.pointerLeave();
      }
      return onTarget || target->pointerInside;
    case PointerEvent::Type::Motion:
      if (onTarget) {
        target->pointerInside = true;
      }
      if (onTarget || target->pointerInside) {
        target->inputDispatcher.pointerMotion(static_cast<float>(event.sx), static_cast<float>(event.sy), 0);
        return true;
      }
      return false;
    case PointerEvent::Type::Button: {
      if (onTarget) {
        target->pointerInside = true;
      }
      if (!onTarget && !target->pointerInside) {
        return false;
      }
      const bool pressed = (event.state == 1);
      return target->inputDispatcher.pointerButton(
          static_cast<float>(event.sx), static_cast<float>(event.sy), event.button, pressed
      );
    }
    case PointerEvent::Type::Axis:
      if (onTarget || target->pointerInside) {
        return target->inputDispatcher.pointerAxis(
            static_cast<float>(event.sx), static_cast<float>(event.sy), event.axis, event.axisSource, event.axisValue,
            event.axisDiscrete, event.axisValue120, event.axisLines
        );
      }
      return false;
    }

    return false;
  }

  bool ScreenshotRegionOverlay::onKeyboardEvent(const KeyboardEvent& event) {
    if (!m_active || !event.pressed || m_wayland == nullptr) {
      return false;
    }

    wl_surface* const kbSurface = m_wayland->lastKeyboardSurface();
    bool onOverlay = false;
    for (const auto& inst : m_instances) {
      if (inst != nullptr && inst->surface != nullptr && inst->surface->wlSurface() == kbSurface) {
        onOverlay = true;
        break;
      }
    }
    if (!onOverlay) {
      return false;
    }

    if (!KeybindMatcher::matches(KeybindAction::Cancel, event.sym, event.modifiers)) {
      return false;
    }
    cancelSelection();
    return true;
  }

  void ScreenshotRegionOverlay::updateSelectionVisuals() {
    if (!m_dragging) {
      for (auto& inst : m_instances) {
        if (inst->selection != nullptr) {
          inst->selection->setVisible(false);
        }
        if (inst->dimensionsBadge != nullptr) {
          inst->dimensionsBadge->setVisible(false);
        }
      }
      return;
    }

    const int globalX0 = static_cast<int>(std::floor(std::min(m_startGlobalX, m_currentGlobalX)));
    const int globalY0 = static_cast<int>(std::floor(std::min(m_startGlobalY, m_currentGlobalY)));
    const int globalX1 = static_cast<int>(std::ceil(std::max(m_startGlobalX, m_currentGlobalX)));
    const int globalY1 = static_cast<int>(std::ceil(std::max(m_startGlobalY, m_currentGlobalY)));
    const int selectionWidth = globalX1 - globalX0;
    const int selectionHeight = globalY1 - globalY0;
    const int cursorGlobalX = static_cast<int>(std::lround(m_currentGlobalX));
    const int cursorGlobalY = static_cast<int>(std::lround(m_currentGlobalY));

    char dimensionText[32];
    std::snprintf(dimensionText, sizeof(dimensionText), "%dx%d", selectionWidth, selectionHeight);

    for (auto& inst : m_instances) {
      if (inst->selection == nullptr || inst->surface == nullptr) {
        continue;
      }
      const auto* out = findOutput(*m_wayland, inst->output);
      if (out == nullptr) {
        inst->selection->setVisible(false);
        if (inst->dimensionsBadge != nullptr) {
          inst->dimensionsBadge->setVisible(false);
        }
        continue;
      }

      const int outLeft = out->logicalX;
      const int outTop = out->logicalY;
      const int outRight = out->logicalX + out->logicalWidth;
      const int outBottom = out->logicalY + out->logicalHeight;

      const int ix0 = std::max(globalX0, outLeft);
      const int iy0 = std::max(globalY0, outTop);
      const int ix1 = std::min(globalX1, outRight);
      const int iy1 = std::min(globalY1, outBottom);
      if (ix1 <= ix0 || iy1 <= iy0) {
        inst->selection->setVisible(false);
        if (inst->dimensionsBadge != nullptr) {
          inst->dimensionsBadge->setVisible(false);
        }
        continue;
      }

      inst->selection->setVisible(true);
      inst->selection->setPosition(static_cast<float>(ix0 - outLeft), static_cast<float>(iy0 - outTop));
      inst->selection->setSize(static_cast<float>(ix1 - ix0), static_cast<float>(iy1 - iy0));

      if (inst->dimensionsBadge != nullptr && inst->dimensionsLabel != nullptr && m_renderContext != nullptr) {
        const bool cursorOnOutput = cursorGlobalX >= outLeft
            && cursorGlobalX < outRight
            && cursorGlobalY >= outTop
            && cursorGlobalY < outBottom;
        if (cursorOnOutput) {
          inst->dimensionsLabel->setText(dimensionText);
          inst->dimensionsLabel->measure(*m_renderContext);
          const float badgeWidth = inst->dimensionsLabel->width() + (kDimensionPaddingX * 2.0f);
          const float badgeHeight = inst->dimensionsLabel->height() + (kDimensionPaddingY * 2.0f);
          inst->dimensionsBadge->setSize(badgeWidth, badgeHeight);

          const float surfaceW = static_cast<float>(inst->surface->width());
          const float surfaceH = static_cast<float>(inst->surface->height());
          float badgeX = static_cast<float>(cursorGlobalX - outLeft) + kDimensionCursorOffsetX;
          float badgeY = static_cast<float>(cursorGlobalY - outTop) + kDimensionCursorOffsetY;
          const float maxX = std::max(0.0f, surfaceW - badgeWidth);
          const float maxY = std::max(0.0f, surfaceH - badgeHeight);
          badgeX = std::clamp(badgeX, 0.0f, maxX);
          badgeY = std::clamp(badgeY, 0.0f, maxY);

          inst->dimensionsBadge->setPosition(badgeX, badgeY);
          inst->dimensionsLabel->setPosition(kDimensionPaddingX, kDimensionPaddingY);
          inst->dimensionsBadge->setVisible(true);
        } else {
          inst->dimensionsBadge->setVisible(false);
        }
      }
    }
  }

  void ScreenshotRegionOverlay::completeSelection() {
    m_dragging = false;
    const int globalX0 = static_cast<int>(std::floor(std::min(m_startGlobalX, m_currentGlobalX)));
    const int globalY0 = static_cast<int>(std::floor(std::min(m_startGlobalY, m_currentGlobalY)));
    const int globalX1 = static_cast<int>(std::ceil(std::max(m_startGlobalX, m_currentGlobalX)));
    const int globalY1 = static_cast<int>(std::ceil(std::max(m_startGlobalY, m_currentGlobalY)));
    const int width = globalX1 - globalX0;
    const int height = globalY1 - globalY0;

    m_active = false;
    destroySurfaces();

    if (width < 2 || height < 2) {
      if (m_onComplete) {
        m_onComplete(std::nullopt, nullptr);
      }
      return;
    }

    const double centerX = (m_startGlobalX + m_currentGlobalX) * 0.5;
    const double centerY = (m_startGlobalY + m_currentGlobalY) * 0.5;
    wl_output* chosenOutput = nullptr;
    for (const auto& out : m_wayland->outputs()) {
      if (out.output == nullptr) {
        continue;
      }
      if (centerX >= out.logicalX
          && centerX < out.logicalX + out.logicalWidth
          && centerY >= out.logicalY
          && centerY < out.logicalY + out.logicalHeight) {
        chosenOutput = out.output;
        break;
      }
    }
    if (chosenOutput == nullptr) {
      if (m_onComplete) {
        m_onComplete(std::nullopt, nullptr);
      }
      return;
    }

    const auto* out = findOutput(*m_wayland, chosenOutput);
    if (out == nullptr) {
      if (m_onComplete) {
        m_onComplete(std::nullopt, nullptr);
      }
      return;
    }

    const int ix0 = std::max(globalX0, out->logicalX);
    const int iy0 = std::max(globalY0, out->logicalY);
    const int ix1 = std::min(globalX1, out->logicalX + out->logicalWidth);
    const int iy1 = std::min(globalY1, out->logicalY + out->logicalHeight);
    if (ix1 <= ix0 || iy1 <= iy0) {
      if (m_onComplete) {
        m_onComplete(std::nullopt, nullptr);
      }
      return;
    }

    LogicalRect region{
        .x = ix0 - out->logicalX,
        .y = iy0 - out->logicalY,
        .width = ix1 - ix0,
        .height = iy1 - iy0,
    };
    if (m_onComplete) {
      m_onComplete(region, chosenOutput);
    }
  }

} // namespace capture
