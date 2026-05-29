#include "capture/screenshot_service.h"

#include "capture/png_writer.h"
#include "capture/screenshot_region_overlay.h"
#include "compositors/compositor_detect.h"
#include "compositors/compositor_platform.h"
#include "compositors/niri/niri_runtime.h"
#include "config/config_service.h"
#include "config/config_types.h"
#include "core/deferred_call.h"
#include "core/keybind_matcher.h"
#include "core/log.h"
#include "ipc/ipc_service.h"
#include "notification/notification.h"
#include "notification/notification_manager.h"
#include "render/render_context.h"
#include "shell/panel/panel_manager.h"
#include "shell/settings/widget_settings_registry.h"
#include "util/file_utils.h"
#include "util/string_utils.h"
#include "wayland/clipboard_service.h"
#include "wayland/wayland_connection.h"
#include "wayland/wayland_seat.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fcntl.h>
#include <filesystem>
#include <json.hpp>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <unordered_map>
#include <wayland-client.h>

namespace {

  constexpr Logger kLog("screenshot");

  [[nodiscard]] std::string defaultFilenamePattern() { return "screenshot_%Y%m%d_%H%M%S"; }

  [[nodiscard]] std::string formatFilenameStem(std::string_view pattern, const std::string& labelBase, int suffix) {
    const auto now = std::chrono::system_clock::now();
    const std::time_t t = std::chrono::system_clock::to_time_t(now);
    std::tm local{};
    localtime_r(&t, &local);

    const std::string resolvedPattern = pattern.empty() ? defaultFilenamePattern() : std::string(pattern);
    std::string stem(64, '\0');
    std::size_t written = 0;
    while (written == 0) {
      written = std::strftime(stem.data(), stem.size(), resolvedPattern.c_str(), &local);
      if (written == 0) {
        if (stem.size() >= 4096) {
          stem = "screenshot";
          break;
        }
        stem.resize(stem.size() * 2);
      } else {
        stem.resize(written);
      }
    }

    if (suffix > 0) {
      stem += '-';
      stem += std::to_string(suffix);
    }
    if (labelBase != "screenshot") {
      stem += '-';
      stem += labelBase;
    }
    return stem;
  }

  [[nodiscard]] wl_output* outputByConnector(WaylandConnection& wayland, std::string_view connector) {
    for (const auto& entry : wayland.outputs()) {
      if (entry.connectorName == connector) {
        return entry.output;
      }
    }
    return nullptr;
  }

  [[nodiscard]] std::optional<std::pair<double, double>> jsonPair(const nlohmann::json& value) {
    if (!value.is_array() || value.size() < 2) {
      return std::nullopt;
    }
    return std::pair{value[0].get<double>(), value[1].get<double>()};
  }

  [[nodiscard]] bool hasAnyOutput(const ScreenshotService::OutputOptions& options) {
    return options.saveToFile || options.copyToClipboard || (options.pipeToCommand && !options.pipeCommand.empty());
  }

  [[nodiscard]] const WaylandOutput* findOutput(const WaylandConnection& wayland, wl_output* output) {
    for (const auto& entry : wayland.outputs()) {
      if (entry.output == output) {
        return &entry;
      }
    }
    return nullptr;
  }

  [[nodiscard]] std::optional<ScreencopyImage>
  cropFrozenRegion(const ScreencopyImage& source, int logicalOutputWidth, int logicalOutputHeight, LogicalRect region) {
    if (logicalOutputWidth <= 0 || logicalOutputHeight <= 0 || region.width <= 0 || region.height <= 0) {
      return std::nullopt;
    }

    const double scaleX = static_cast<double>(source.width) / static_cast<double>(logicalOutputWidth);
    const double scaleY = static_cast<double>(source.height) / static_cast<double>(logicalOutputHeight);

    LogicalRect clipped = region;
    clipped.x = std::clamp(region.x, 0, logicalOutputWidth);
    clipped.y = std::clamp(region.y, 0, logicalOutputHeight);
    clipped.width = std::clamp(region.width, 0, logicalOutputWidth - clipped.x);
    clipped.height = std::clamp(region.height, 0, logicalOutputHeight - clipped.y);
    if (clipped.width <= 0 || clipped.height <= 0) {
      return std::nullopt;
    }

    const int srcX0 = std::clamp(static_cast<int>(std::floor(clipped.x * scaleX)), 0, source.width);
    const int srcY0 = std::clamp(static_cast<int>(std::floor(clipped.y * scaleY)), 0, source.height);
    const int srcX1 = std::clamp(static_cast<int>(std::ceil((clipped.x + clipped.width) * scaleX)), 0, source.width);
    const int srcY1 = std::clamp(static_cast<int>(std::ceil((clipped.y + clipped.height) * scaleY)), 0, source.height);
    const int outWidth = srcX1 - srcX0;
    const int outHeight = srcY1 - srcY0;
    if (outWidth <= 0 || outHeight <= 0) {
      return std::nullopt;
    }

    ScreencopyImage cropped;
    cropped.width = outWidth;
    cropped.height = outHeight;
    cropped.rgba.resize(static_cast<std::size_t>(outWidth) * static_cast<std::size_t>(outHeight) * 4U);

    for (int y = 0; y < outHeight; ++y) {
      const int srcY = srcY0 + y;
      const auto* srcRow = source.rgba.data()
          + (static_cast<std::size_t>(srcY) * static_cast<std::size_t>(source.width) + static_cast<std::size_t>(srcX0))
              * 4U;
      auto* dstRow = cropped.rgba.data() + static_cast<std::size_t>(y) * static_cast<std::size_t>(outWidth) * 4U;
      std::memcpy(dstRow, srcRow, static_cast<std::size_t>(outWidth) * 4U);
    }

    return cropped;
  }

  [[nodiscard]] capture::FrozenScreenshot*
  findFrozenScreenshot(std::vector<capture::FrozenScreenshot>& screenshots, wl_output* output) {
    for (auto& entry : screenshots) {
      if (entry.output == output) {
        return &entry;
      }
    }
    return nullptr;
  }

  [[nodiscard]] bool captureOutputBlocking(
      ScreencopyCapture& capture, WaylandConnection& wayland, wl_output* output, ScreencopyImage& out,
      std::string& error
  ) {
    error.clear();
    bool finished = false;
    capture.capture(output, std::nullopt, false, [&](std::optional<ScreencopyImage> image, const std::string& err) {
      finished = true;
      if (!err.empty() || !image.has_value()) {
        error = err.empty() ? "screencopy capture failed" : err;
        return;
      }
      out = std::move(*image);
    });

    if (!error.empty()) {
      return false;
    }

    while (!finished && capture.busy()) {
      if (wl_display_roundtrip(wayland.display()) < 0) {
        error = "Wayland roundtrip failed";
        return false;
      }
    }

    if (!error.empty() || !finished) {
      if (error.empty()) {
        error = "screencopy capture failed";
      }
      return false;
    }

    if (out.width <= 0 || out.height <= 0 || out.rgba.empty()) {
      error = "screencopy capture returned an empty frame";
      return false;
    }

    return true;
  }

  [[nodiscard]] bool isScreenshotWidget(const Config& cfg, std::string_view name) {
    return settings::widgetTypeForReference(cfg, name) == "screenshot";
  }

  [[nodiscard]] std::string resolveDefaultScreenshotWidgetName(const Config& cfg) {
    const auto scanSection = [&](const std::vector<std::string>& names) -> std::optional<std::string> {
      for (const auto& name : names) {
        if (isScreenshotWidget(cfg, name)) {
          return name;
        }
      }
      return std::nullopt;
    };

    for (const auto& bar : cfg.bars) {
      if (auto name = scanSection(bar.startWidgets)) {
        return *name;
      }
      if (auto name = scanSection(bar.centerWidgets)) {
        return *name;
      }
      if (auto name = scanSection(bar.endWidgets)) {
        return *name;
      }
    }

    std::vector<std::string> configured;
    configured.reserve(cfg.widgets.size());
    for (const auto& [name, widget] : cfg.widgets) {
      (void)widget;
      if (isScreenshotWidget(cfg, name)) {
        configured.push_back(name);
      }
    }
    if (configured.empty()) {
      return "screenshot";
    }
    std::sort(configured.begin(), configured.end());
    return configured.front();
  }

  void attachStdioToDevNull() {
    const int devnull = ::open("/dev/null", O_RDWR);
    if (devnull >= 0) {
      ::dup2(devnull, STDOUT_FILENO);
      ::dup2(devnull, STDERR_FILENO);
      if (devnull > STDERR_FILENO) {
        ::close(devnull);
      }
    }
  }

  bool writeAll(int fd, const std::uint8_t* data, std::size_t size) {
    std::size_t offset = 0;
    while (offset < size) {
      const ssize_t written = ::write(fd, data + offset, size - offset);
      if (written < 0) {
        if (errno == EINTR) {
          continue;
        }
        return false;
      }
      if (written == 0) {
        return false;
      }
      offset += static_cast<std::size_t>(written);
    }
    return true;
  }

  void pipePngToCommandAsync(std::string command, std::vector<std::uint8_t> png) {
    if (command.empty() || png.empty()) {
      return;
    }

    std::thread([command = std::move(command), png = std::move(png)]() {
      int stdinPipe[2] = {-1, -1};
      if (::pipe(stdinPipe) != 0) {
        kLog.warn("screenshot pipe: failed to create stdin pipe");
        return;
      }

      const pid_t child = ::fork();
      if (child < 0) {
        kLog.warn("screenshot pipe: fork failed");
        ::close(stdinPipe[0]);
        ::close(stdinPipe[1]);
        return;
      }

      if (child == 0) {
        ::close(stdinPipe[1]);
        if (::dup2(stdinPipe[0], STDIN_FILENO) < 0) {
          ::_exit(126);
        }
        ::close(stdinPipe[0]);
        attachStdioToDevNull();
        const char* argv[] = {"/bin/sh", "-lc", command.c_str(), nullptr};
        ::execv("/bin/sh", const_cast<char* const*>(argv));
        ::_exit(127);
      }

      ::close(stdinPipe[0]);
      const bool wrote = writeAll(stdinPipe[1], png.data(), png.size());
      ::close(stdinPipe[1]);
      if (!wrote) {
        kLog.warn("screenshot pipe: failed to write PNG to command stdin");
      }

      int status = 0;
      while (::waitpid(child, &status, 0) < 0 && errno == EINTR) {
      }
      if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        kLog.warn("screenshot pipe: command exited with status {}", status);
      }
    }).detach();
  }

} // namespace

ScreenshotService::ScreenshotService(
    WaylandConnection& wayland, CompositorPlatform& platform, NotificationManager& notifications,
    ClipboardService* clipboard
)
    : m_wayland(wayland), m_platform(platform), m_notifications(notifications), m_clipboard(clipboard),
      m_capture(wayland) {}

ScreenshotService::~ScreenshotService() = default;

bool ScreenshotService::available() const noexcept { return m_capture.available(); }

void ScreenshotService::onOutputChange() {
  if (m_regionOverlay != nullptr) {
    m_regionOverlay->onOutputChange();
  }
}

bool ScreenshotService::onPointerEvent(const PointerEvent& event) {
  if (m_regionOverlay == nullptr || !m_regionOverlay->isActive()) {
    return false;
  }
  return m_regionOverlay->onPointerEvent(event);
}

bool ScreenshotService::onKeyboardEvent(const KeyboardEvent& event) {
  if (!event.pressed) {
    return false;
  }
  const bool regionActive = m_regionOverlay != nullptr && m_regionOverlay->isActive();
  if (!m_freezeCaptureActive && !regionActive) {
    return false;
  }
  if (regionActive && m_regionOverlay->onKeyboardEvent(event)) {
    return true;
  }
  if (!KeybindMatcher::matches(KeybindAction::Cancel, event.sym, event.modifiers)) {
    return false;
  }
  cancelRegionCapture();
  return true;
}

ScreenshotService::OutputOptions ScreenshotService::outputOptionsFromWidget(const WidgetConfig& widget) {
  OutputOptions options;
  options.saveToFile = widget.getBool("save_to_file", true);
  options.copyToClipboard = widget.getBool("copy_to_clipboard", false);
  options.pipeCommand = widget.getString("pipe_command", "");
  options.pipeToCommand = widget.getBool("pipe_to_command", false);
  options.freezeScreen = widget.getBool("freeze_screen", false);
  if (!options.pipeToCommand && !options.pipeCommand.empty()) {
    options.pipeToCommand = true;
  }
  options.directory = widget.getString("directory", "");
  options.filenamePattern = widget.getString("filename_pattern", "");
  return options;
}

ScreenshotService::OutputOptions
ScreenshotService::outputOptionsFromConfig(const Config& config, std::string_view widgetName) {
  std::string error;
  if (auto options = tryOutputOptionsFromConfig(config, widgetName, error)) {
    return *options;
  }
  WidgetConfig defaults;
  defaults.type = "screenshot";
  return outputOptionsFromWidget(defaults);
}

std::optional<ScreenshotService::OutputOptions> ScreenshotService::tryOutputOptionsFromConfig(
    const Config& config, std::string_view widgetNameArg, std::string& error
) {
  error.clear();
  const std::string widgetName = StringUtils::trim(widgetNameArg);
  const std::string resolvedName = widgetName.empty() ? resolveDefaultScreenshotWidgetName(config) : widgetName;

  if (!widgetName.empty() && !isScreenshotWidget(config, resolvedName)) {
    error = "widget '" + resolvedName + "' is not a screenshot widget";
    return std::nullopt;
  }

  if (const auto it = config.widgets.find(resolvedName); it != config.widgets.end()) {
    return outputOptionsFromWidget(it->second);
  }

  WidgetConfig defaults;
  defaults.type = "screenshot";
  return outputOptionsFromWidget(defaults);
}

void ScreenshotService::registerIpc(IpcService& ipc, const ConfigService& configService) {
  const auto runWithOptions =
      [this, &configService](const std::string& args, std::string& error) -> std::optional<OutputOptions> {
    return tryOutputOptionsFromConfig(configService.config(), args, error);
  };

  ipc.registerHandler(
      "screenshot-region",
      [this, runWithOptions](const std::string& args) -> std::string {
        if (!available()) {
          return "error: screen capture is not available on this compositor\n";
        }
        std::string error;
        const auto options = runWithOptions(args, error);
        if (!options.has_value()) {
          return "error: " + error + "\n";
        }
        auto* renderContext = PanelManager::instance().renderContext();
        if (renderContext == nullptr) {
          return "error: render context unavailable\n";
        }
        beginRegionCapture(*renderContext, *options);
        return "ok\n";
      },
      "screenshot-region [widget-name]", "Start an interactive region screenshot"
  );

  ipc.registerHandler(
      "screenshot-fullscreen",
      [this, runWithOptions](const std::string& args) -> std::string {
        if (!available()) {
          return "error: screen capture is not available on this compositor\n";
        }
        std::string error;
        const auto options = runWithOptions(args, error);
        if (!options.has_value()) {
          return "error: " + error + "\n";
        }
        captureFullscreen(*options);
        return "ok\n";
      },
      "screenshot-fullscreen [widget-name]", "Capture all outputs fullscreen"
  );
}

void ScreenshotService::captureFullscreen(const OutputOptions& options) {
  if (!available()) {
    notifyError("Screen capture is not available on this compositor");
    return;
  }
  if (!hasAnyOutput(options)) {
    notifyError("No screenshot output enabled");
    return;
  }
  captureAllOutputs(options);
}

void ScreenshotService::beginRegionCapture(RenderContext& renderContext, const OutputOptions& options) {
  if (!available()) {
    notifyError("Screen capture is not available on this compositor");
    return;
  }
  if (!hasAnyOutput(options)) {
    notifyError("No screenshot output enabled");
    return;
  }
  if (m_regionOverlay != nullptr && m_regionOverlay->isActive()) {
    m_regionOverlay->cancel();
  }
  if (m_freezeCaptureActive) {
    abortFreezeCapture("Screenshot cancelled");
  }

  m_regionOutputOptions = options;
  m_regionRenderContext = &renderContext;

  if (options.freezeScreen) {
    DeferredCall::callLater([this]() { beginFreezeCapture(); });
    return;
  }

  startRegionOverlay(renderContext);
}

void ScreenshotService::ensureRegionOverlay() {
  if (m_regionRenderContext == nullptr) {
    return;
  }
  if (m_regionOverlay == nullptr) {
    m_regionOverlay = std::make_unique<capture::ScreenshotRegionOverlay>();
  }
  m_regionOverlay->initialize(m_wayland, m_regionRenderContext);
  m_regionOverlay->setCompleteCallback([this](std::optional<LogicalRect> region, wl_output* output) {
    if (!region.has_value() || output == nullptr) {
      m_frozenScreenshots.clear();
      return;
    }
    if (m_regionOutputOptions.freezeScreen && !m_frozenScreenshots.empty()) {
      deliverFrozenRegion(*region, output, m_regionOutputOptions);
      return;
    }
    captureOutput(output, region, "region", m_regionOutputOptions);
  });
}

void ScreenshotService::startRegionOverlay(RenderContext& renderContext) {
  m_regionRenderContext = &renderContext;
  ensureRegionOverlay();
  m_regionOverlay->setFrozenScreenshots({});
  m_regionOverlay->begin(false);
}

void ScreenshotService::beginFreezeCapture() {
  if (m_regionRenderContext == nullptr) {
    notifyError("Render context unavailable");
    return;
  }

  m_frozenScreenshots.clear();
  m_pendingFreezeOutputs.clear();
  for (const auto& output : m_wayland.outputs()) {
    if (output.output != nullptr && output.logicalWidth > 0 && output.logicalHeight > 0) {
      m_pendingFreezeOutputs.push_back(output.output);
    }
  }
  if (m_pendingFreezeOutputs.empty()) {
    notifyError("No outputs available");
    return;
  }

  m_freezeCaptureActive = true;

  while (!m_pendingFreezeOutputs.empty()) {
    if (!m_freezeCaptureActive) {
      m_frozenScreenshots.clear();
      return;
    }

    wl_output* output = m_pendingFreezeOutputs.front();
    m_pendingFreezeOutputs.erase(m_pendingFreezeOutputs.begin());

    if (m_capture.busy()) {
      m_capture.cancelInFlight();
    }

    ScreencopyImage image;
    std::string error;
    if (!captureOutputBlocking(m_capture, m_wayland, output, image, error)) {
      if (!m_freezeCaptureActive) {
        m_frozenScreenshots.clear();
        return;
      }
      abortFreezeCapture(error.empty() ? "Failed to freeze screen" : error);
      return;
    }
    if (!m_freezeCaptureActive) {
      m_frozenScreenshots.clear();
      return;
    }
    m_frozenScreenshots.push_back(capture::FrozenScreenshot{.output = output, .image = std::move(image)});
  }

  m_freezeCaptureActive = false;
  finishFreezeCapture();
}

void ScreenshotService::finishFreezeCapture() {
  if (m_regionRenderContext == nullptr) {
    notifyError("Render context unavailable");
    m_frozenScreenshots.clear();
    return;
  }
  if (m_frozenScreenshots.empty()) {
    notifyError("Failed to freeze screen");
    return;
  }

  ensureRegionOverlay();
  m_regionOverlay->setFrozenScreenshots(m_frozenScreenshots);
  m_regionOverlay->begin(true);
}

void ScreenshotService::abortFreezeCapture(const std::string& message) {
  m_freezeCaptureActive = false;
  m_pendingFreezeOutputs.clear();
  m_frozenScreenshots.clear();
  m_capture.cancelInFlight();
  if (!message.empty()) {
    notifyError(message);
  }
}

void ScreenshotService::cancelRegionCapture() {
  if (m_freezeCaptureActive) {
    abortFreezeCapture({});
    return;
  }
  if (m_regionOverlay != nullptr && m_regionOverlay->isActive()) {
    m_regionOverlay->cancelSelection();
  }
}

void ScreenshotService::deliverFrozenRegion(LogicalRect region, wl_output* output, const OutputOptions& options) {
  auto* frozen = findFrozenScreenshot(m_frozenScreenshots, output);
  const auto* out = findOutput(m_wayland, output);
  if (frozen == nullptr || out == nullptr) {
    notifyError("Failed to crop frozen screenshot");
    m_frozenScreenshots.clear();
    return;
  }

  auto cropped = cropFrozenRegion(frozen->image, out->logicalWidth, out->logicalHeight, region);
  m_frozenScreenshots.clear();
  if (!cropped.has_value()) {
    notifyError("Failed to crop frozen screenshot");
    return;
  }

  const std::optional<std::filesystem::path> destPath =
      options.saveToFile ? std::optional(makeScreenshotPath(options, "region")) : std::nullopt;
  onCaptureComplete(std::move(*cropped), {}, options, destPath);
}

void ScreenshotService::captureWindow(const std::string& windowId, const OutputOptions& options) {
  if (!available()) {
    notifyError("Screen capture is not available on this compositor");
    return;
  }
  if (!hasAnyOutput(options)) {
    notifyError("No screenshot output enabled");
    return;
  }
  const auto target = resolveWindowTarget(windowId);
  if (!target.has_value() || target->output == nullptr) {
    notifyError("Could not resolve window geometry for capture");
    return;
  }
  captureOutput(target->output, target->region, "window", options);
}

std::vector<ScreenshotService::WindowTarget> ScreenshotService::windowTargets() const {
  std::vector<WindowTarget> targets;
  const auto assignments = m_platform.workspaceWindowAssignments(nullptr);
  for (const auto& assignment : assignments) {
    if (assignment.windowId.empty()) {
      continue;
    }
    if (auto resolved = resolveWindowTarget(assignment.windowId); resolved.has_value()) {
      targets.push_back(*resolved);
    }
  }
  return targets;
}

void ScreenshotService::captureOutput(
    wl_output* output, std::optional<LogicalRect> region, const std::string& labelBase, const OutputOptions& options,
    int pathSuffix
) {
  if (output == nullptr) {
    notifyError("No output for capture");
    return;
  }

  PendingCapture pending{
      .output = output,
      .region = region,
      .outputOptions = options,
      .destPath = options.saveToFile ? std::optional(makeScreenshotPath(options, labelBase, pathSuffix)) : std::nullopt,
  };
  if (m_capture.busy()) {
    m_captureQueue.push_back(std::move(pending));
    return;
  }

  m_capture.capture(
      pending.output, pending.region, true,
      [this, options = pending.outputOptions,
       destPath = pending.destPath](std::optional<ScreencopyImage> image, const std::string& error) {
        onCaptureComplete(std::move(image), error, std::move(options), std::move(destPath));
      }
  );
}

void ScreenshotService::startNextQueuedCapture() {
  if (m_captureQueue.empty() || m_capture.busy()) {
    return;
  }
  PendingCapture pending = std::move(m_captureQueue.front());
  m_captureQueue.erase(m_captureQueue.begin());
  m_capture.capture(
      pending.output, pending.region, true,
      [this, options = pending.outputOptions,
       destPath = pending.destPath](std::optional<ScreencopyImage> image, const std::string& error) {
        onCaptureComplete(std::move(image), error, std::move(options), std::move(destPath));
      }
  );
}

void ScreenshotService::captureAllOutputs(const OutputOptions& options) {
  const auto& outputs = m_wayland.outputs();
  if (outputs.empty()) {
    notifyError("No outputs available");
    return;
  }

  if (outputs.size() == 1) {
    captureOutput(outputs.front().output, std::nullopt, "screenshot", options);
    return;
  }

  std::size_t index = 0;
  for (const auto& output : outputs) {
    if (output.output == nullptr) {
      continue;
    }
    ++index;
    captureOutput(output.output, std::nullopt, "screenshot", options, static_cast<int>(index));
  }
}

void ScreenshotService::onCaptureComplete(
    std::optional<ScreencopyImage> image, const std::string& error, OutputOptions options,
    std::optional<std::filesystem::path> destPath
) {
  if (!error.empty() || !image.has_value()) {
    kLog.warn("screenshot failed: {}", error.empty() ? "empty frame" : error);
    notifyError(error.empty() ? "Screenshot failed" : error);
    startNextQueuedCapture();
    return;
  }

  std::string encodeError;
  std::vector<std::uint8_t> png = capture::encodePng(image->rgba.data(), image->width, image->height, &encodeError);
  if (png.empty()) {
    kLog.warn("screenshot encode failed: {}", encodeError);
    notifyError(encodeError.empty() ? "Failed to encode screenshot" : encodeError);
    startNextQueuedCapture();
    return;
  }

  bool delivered = false;
  std::string failureMessage;

  if (options.saveToFile && destPath.has_value()) {
    std::error_code ec;
    std::filesystem::create_directories(destPath->parent_path(), ec);
    std::string writeError;
    if (!capture::writePng(*destPath, image->rgba.data(), image->width, image->height, &writeError)) {
      kLog.warn("screenshot write failed: {}", writeError);
      failureMessage = writeError.empty() ? "Failed to save screenshot" : writeError;
    } else {
      notifySaved(*destPath);
      delivered = true;
    }
  }

  if (options.copyToClipboard) {
    if (m_clipboard == nullptr || !m_clipboard->isAvailable()) {
      kLog.warn("screenshot clipboard copy skipped: clipboard unavailable");
      if (failureMessage.empty()) {
        failureMessage = "Clipboard is not available";
      }
    } else if (m_clipboard->copyImagePng(png)) {
      delivered = true;
    } else {
      kLog.warn("screenshot clipboard copy failed");
      if (failureMessage.empty()) {
        failureMessage = "Failed to copy screenshot to clipboard";
      }
    }
  }

  if (options.pipeToCommand && !options.pipeCommand.empty()) {
    pipePngToCommandAsync(options.pipeCommand, png);
    delivered = true;
  }

  if (!delivered) {
    notifyError(failureMessage.empty() ? "No screenshot output enabled" : failureMessage);
  }

  startNextQueuedCapture();
}

std::filesystem::path ScreenshotService::defaultPicturesDirectory() const {
  if (const char* home = std::getenv("HOME"); home != nullptr && home[0] != '\0') {
    return std::filesystem::path(home) / "Pictures";
  }
  return std::filesystem::path("/tmp");
}

std::filesystem::path ScreenshotService::outputDirectory(const OutputOptions& options) const {
  if (options.directory.empty()) {
    return defaultPicturesDirectory();
  }
  return FileUtils::expandUserPath(options.directory);
}

std::filesystem::path
ScreenshotService::makeScreenshotPath(const OutputOptions& options, const std::string& labelBase, int suffix) const {
  const auto dir = outputDirectory(options);
  const std::string stem = formatFilenameStem(options.filenamePattern, labelBase, suffix);
  return dir / (stem + ".png");
}

void ScreenshotService::notifySaved(const std::filesystem::path& path) {
  m_notifications.addInternal("Noctalia", "Screenshot saved", path.string());
}

void ScreenshotService::notifyError(const std::string& message) {
  m_notifications.addInternal("Noctalia", "Screenshot failed", message, Urgency::Critical);
}

std::optional<ScreenshotService::WindowTarget>
ScreenshotService::resolveWindowTarget(const std::string& windowId) const {
  WindowTarget target;
  target.windowId = windowId;

  if (compositors::isNiri() && m_platform.niriRuntime().available()) {
    const auto response = m_platform.niriRuntime().requestJson("\"Windows\"\n");
    if (!response.has_value() || !response->contains("Windows")) {
      return std::nullopt;
    }

    std::unordered_map<std::uint64_t, std::string> workspaceOutputs;
    if (const auto workspaces = m_platform.niriRuntime().requestJson("\"Workspaces\"\n");
        workspaces.has_value() && workspaces->contains("Workspaces")) {
      for (const auto& ws : (*workspaces)["Workspaces"]) {
        if (!ws.is_object()) {
          continue;
        }
        if (const auto id = ws.value("id", 0ULL); id != 0) {
          workspaceOutputs.emplace(id, ws.value("output", std::string{}));
        }
      }
    }

    for (const auto& entry : (*response)["Windows"]) {
      const nlohmann::json* windowJson = nullptr;
      std::uint64_t id = 0;
      if (entry.is_array() && entry.size() >= 2) {
        id = entry[0].get<std::uint64_t>();
        windowJson = &entry[1];
      } else if (entry.is_object()) {
        windowJson = &entry;
        id = entry.value("id", 0ULL);
      }
      if (windowJson == nullptr || std::to_string(id) != windowId) {
        continue;
      }

      target.title = windowJson->value("title", std::string{"Window"});
      if (const auto workspaceId = windowJson->value("workspace_id", 0ULL); workspaceId != 0) {
        const auto it = workspaceOutputs.find(workspaceId);
        if (it != workspaceOutputs.end()) {
          target.output = outputByConnector(m_wayland, it->second);
        }
      }

      const auto& layout = (*windowJson)["layout"];
      double x = 0.0;
      double y = 0.0;
      double w = 0.0;
      double h = 0.0;
      if (const auto pos = jsonPair(layout["tile_pos_in_workspace_view"]); pos.has_value()) {
        x = pos->first;
        y = pos->second;
      }
      if (const auto size = jsonPair(layout["tile_size"]); size.has_value()) {
        w = size->first;
        h = size->second;
      }
      target.region = LogicalRect{
          .x = static_cast<int>(std::floor(x)),
          .y = static_cast<int>(std::floor(y)),
          .width = std::max(1, static_cast<int>(std::round(w))),
          .height = std::max(1, static_cast<int>(std::round(h))),
      };
      if (target.output != nullptr && target.region.width > 0 && target.region.height > 0) {
        return target;
      }
      return std::nullopt;
    }
  }

  const auto assignments = m_platform.workspaceWindowAssignments(nullptr);
  const auto it = std::find_if(assignments.begin(), assignments.end(), [&](const WorkspaceWindowAssignment& entry) {
    return entry.windowId == windowId;
  });
  if (it == assignments.end()) {
    return std::nullopt;
  }

  target.title = it->title;
  for (const auto& output : m_wayland.outputs()) {
    if (output.output != nullptr) {
      target.output = output.output;
      break;
    }
  }
  target.region = LogicalRect{
      .x = it->x,
      .y = it->y,
      .width = std::max(1, 320),
      .height = std::max(1, 240),
  };
  return target;
}
