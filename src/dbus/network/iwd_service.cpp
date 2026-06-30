#include "dbus/network/iwd_service.h"

#include "core/log.h"
#include "core/process.h"
#include "dbus/system_bus.h"
#include "system/rfkill_helper.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <map>
#include <optional>
#include <sdbus-c++/IProxy.h>
#include <sdbus-c++/Types.h>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace {

  constexpr Logger kLog("iwd");

  const sdbus::ServiceName kIwdBusName{"net.connman.iwd"};
  const sdbus::ObjectPath kRootPath{"/"};
  constexpr auto kDeviceInterface = "net.connman.iwd.Device";
  constexpr auto kStationInterface = "net.connman.iwd.Station";
  constexpr auto kNetworkInterface = "net.connman.iwd.Network";
  constexpr auto kKnownNetworkInterface = "net.connman.iwd.KnownNetwork";
  constexpr auto kObjectManagerInterface = "org.freedesktop.DBus.ObjectManager";

  using VariantMap = std::map<std::string, sdbus::Variant>;
  using ObjectInterfaces = std::map<std::string, VariantMap>;
  using ManagedObjects = std::map<sdbus::ObjectPath, ObjectInterfaces>;
  using OrderedNetwork = std::tuple<sdbus::ObjectPath, std::int16_t>;

  template <typename T> std::optional<T> variantGet(const sdbus::Variant& value) {
    try {
      return value.get<T>();
    } catch (const sdbus::Error&) {
      return std::nullopt;
    }
  }

  constexpr auto kPropertiesInterface = "org.freedesktop.DBus.Properties";

  std::uint8_t signalToPercent(std::int16_t dBm) {
    if (dBm <= -100) {
      return 0;
    }
    if (dBm >= -50) {
      return 100;
    }
    return static_cast<std::uint8_t>(2 * (dBm + 100));
  }

  std::uint8_t strengthFromValue(std::int16_t value) {
    if (value >= 0 && value <= 100) {
      return static_cast<std::uint8_t>(value);
    }
    return signalToPercent(value);
  }

  std::optional<std::string> stringProp(const VariantMap& props, std::string_view name) {
    const auto it = props.find(std::string{name});
    if (it == props.end()) {
      return std::nullopt;
    }
    return variantGet<std::string>(it->second);
  }

  std::optional<bool> boolProp(const VariantMap& props, std::string_view name) {
    const auto it = props.find(std::string{name});
    if (it == props.end()) {
      return std::nullopt;
    }
    return variantGet<bool>(it->second);
  }

  std::optional<sdbus::ObjectPath> objectPathProp(const VariantMap& props, std::string_view name) {
    const auto it = props.find(std::string{name});
    if (it == props.end()) {
      return std::nullopt;
    }
    return variantGet<sdbus::ObjectPath>(it->second);
  }

  bool isRootPath(const sdbus::ObjectPath& path) { return std::string(path) == "/"; }

} // namespace

IwdService::IwdService(SystemBus& bus) : m_bus(bus) {
  if (!bus.nameHasOwner("net.connman.iwd")) {
    throw sdbus::Error(
        sdbus::Error::Name{"org.freedesktop.DBus.Error.ServiceUnknown"},
        "The name net.connman.iwd was not provided by any .service files"
    );
  }

  m_iwd = sdbus::createProxy(m_bus.connection(), kIwdBusName, kRootPath);
  m_iwd->uponSignal("InterfacesAdded")
      .onInterface(kObjectManagerInterface)
      .call([this](const sdbus::ObjectPath&, const ObjectInterfaces&) { refresh(); });
  m_iwd->uponSignal("InterfacesRemoved")
      .onInterface(kObjectManagerInterface)
      .call([this](const sdbus::ObjectPath&, const std::vector<std::string>&) { refresh(); });

  refresh();
}

IwdService::~IwdService() = default;

void IwdService::setChangeCallback(ChangeCallback callback) { m_changeCallback = std::move(callback); }

void IwdService::subscribeObject(const std::string& path, const ObjectInterfaces& interfaces) {
  if (m_objectProxies.contains(path)) {
    return;
  }
  if (!interfaces.contains(kDeviceInterface) && !interfaces.contains(kStationInterface) && !interfaces.contains(kNetworkInterface)
      && !interfaces.contains(kKnownNetworkInterface)) {
    return;
  }

  try {
    auto proxy = sdbus::createProxy(m_bus.connection(), kIwdBusName, sdbus::ObjectPath{path});
    proxy->uponSignal("PropertiesChanged")
        .onInterface(kPropertiesInterface)
        .call([this](const std::string& interfaceName, const VariantMap&, const std::vector<std::string>&) {
          if (interfaceName == kDeviceInterface || interfaceName == kStationInterface || interfaceName == kNetworkInterface
              || interfaceName == kKnownNetworkInterface) {
            refresh();
          }
        });
    m_objectProxies.emplace(path, std::move(proxy));
  } catch (const sdbus::Error& e) {
    kLog.debug("failed to subscribe IWD object {}: {}", path, e.what());
  }
}

void IwdService::refresh() {
  if (m_refreshInFlight) {
    m_refreshQueued = true;
    return;
  }
  m_refreshInFlight = true;

  ManagedObjects objects;
  try {
    m_iwd->callMethod("GetManagedObjects").onInterface(kObjectManagerInterface).storeResultsTo(objects);
  } catch (const sdbus::Error& e) {
    kLog.warn("GetManagedObjects failed: {}", e.what());
    m_refreshInFlight = false;
    return;
  }

  NetworkState next;
  std::vector<AccessPointInfo> aps;
  std::vector<std::string> liveObjects;
  m_knownNetworks.clear();
  m_deviceNames.clear();

  for (const auto& [path, interfaces] : objects) {
    const std::string pathString = std::string(path);
    subscribeObject(pathString, interfaces);
    liveObjects.push_back(pathString);

    if (const auto it = interfaces.find(kDeviceInterface); it != interfaces.end()) {
      const auto name = stringProp(it->second, "Name").value_or("");
      const auto powered = boolProp(it->second, "Powered").value_or(true);
      next.wirelessEnabled = next.wirelessEnabled || powered;
      if (!name.empty()) {
        m_deviceNames[std::string(path)] = name;
      }
    }
    if (const auto it = interfaces.find(kKnownNetworkInterface); it != interfaces.end()) {
      if (auto name = stringProp(it->second, "Name"); name.has_value() && !name->empty()) {
        m_knownNetworks[*name] = std::string(path);
      }
    }
  }

  for (auto it = m_objectProxies.begin(); it != m_objectProxies.end();) {
    if (std::ranges::find(liveObjects, it->first) == liveObjects.end()) {
      it = m_objectProxies.erase(it);
    } else {
      ++it;
    }
  }

  for (const auto& [stationPath, interfaces] : objects) {
    const auto stationIt = interfaces.find(kStationInterface);
    if (stationIt == interfaces.end()) {
      continue;
    }

    auto stationProxy = sdbus::createProxy(m_bus.connection(), kIwdBusName, stationPath);
    const auto state = stringProp(stationIt->second, "State").value_or("");
    const bool connected = state == "connected";
    const bool connecting = state == "connecting" || state == "associating" || state == "authenticating";
    next.scanning = next.scanning || boolProp(stationIt->second, "Scanning").value_or(false);

    std::string connectedNetworkPath;
    if (auto connectedNetwork = objectPathProp(stationIt->second, "ConnectedNetwork");
        connectedNetwork.has_value() && !isRootPath(*connectedNetwork)) {
      connectedNetworkPath = std::string(*connectedNetwork);
    }

    if (connected || connecting) {
      next.kind = NetworkConnectivity::Wireless;
      next.connected = connected;
      next.resolving = connecting;
      next.interfaceName = m_deviceNames[std::string(stationPath)];
    }

    std::vector<OrderedNetwork> orderedNetworks;
    try {
      stationProxy->callMethod("GetOrderedNetworks").onInterface(kStationInterface).storeResultsTo(orderedNetworks);
    } catch (const sdbus::Error& e) {
      kLog.debug("GetOrderedNetworks failed on {}: {}", std::string(stationPath), e.what());
    }

    for (const auto& [networkPath, signal] : orderedNetworks) {
      const auto objectIt = objects.find(networkPath);
      if (objectIt == objects.end()) {
        continue;
      }
      const auto networkIt = objectIt->second.find(kNetworkInterface);
      if (networkIt == objectIt->second.end()) {
        continue;
      }

      const auto ssid = stringProp(networkIt->second, "Name").value_or("");
      if (ssid.empty()) {
        continue;
      }
      const auto type = stringProp(networkIt->second, "Type").value_or("");
      const bool active = boolProp(networkIt->second, "Connected").value_or(false)
          || (!connectedNetworkPath.empty() && connectedNetworkPath == std::string(networkPath));
      const std::uint8_t strength = strengthFromValue(signal);

      auto existing = std::ranges::find(aps, ssid, &AccessPointInfo::ssid);
      if (existing != aps.end()) {
        existing->strength = std::max(existing->strength, strength);
        existing->active = existing->active || active;
        existing->secured = existing->secured || type != "open";
      } else {
        AccessPointInfo ap;
        ap.path = std::string(networkPath);
        ap.devicePath = std::string(stationPath);
        ap.ssid = ssid;
        ap.strength = strength;
        ap.secured = type != "open";
        ap.active = active;
        aps.push_back(std::move(ap));
      }

      if (active) {
        next.ssid = ssid;
        next.signalStrength = std::max(next.signalStrength, strength);
      }
    }
  }

  std::ranges::sort(aps, [](const AccessPointInfo& a, const AccessPointInfo& b) {
    if (a.active != b.active) {
      return a.active > b.active;
    }
    return a.strength > b.strength;
  });

  m_accessPoints = std::move(aps);
  emitChangedIfNeeded(std::move(next));

  m_refreshInFlight = false;
  if (m_refreshQueued) {
    m_refreshQueued = false;
    refresh();
  }
}

void IwdService::requestScan() {
  for (const auto& [stationPath, ifname] : m_deviceNames) {
    (void)ifname;
    try {
      auto station = sdbus::createProxy(m_bus.connection(), kIwdBusName, sdbus::ObjectPath{stationPath});
      station->callMethod("Scan").onInterface(kStationInterface);
    } catch (const sdbus::Error& e) {
      kLog.debug("Scan failed on {}: {}", stationPath, e.what());
    }
  }
  refresh();
}

bool IwdService::activateAccessPoint(const AccessPointInfo& ap) {
  try {
    auto network = sdbus::createProxy(m_bus.connection(), kIwdBusName, sdbus::ObjectPath{ap.path});
    network->callMethod("Connect").onInterface(kNetworkInterface);
    refresh();
    return true;
  } catch (const sdbus::Error& e) {
    kLog.warn("Connect failed for {}: {}", ap.ssid, e.what());
    return false;
  }
}

bool IwdService::activateAccessPoint(const AccessPointInfo& ap, const std::string& psk) {
  if (psk.empty()) {
    return activateAccessPoint(ap);
  }

  const auto ifnameIt = m_deviceNames.find(ap.devicePath);
  if (ifnameIt != m_deviceNames.end() && process::commandExists("iwctl")) {
    const auto result = process::runSyncWithTimeout(
        {"iwctl", "--passphrase", psk.c_str(), "station", ifnameIt->second.c_str(), "connect", ap.ssid.c_str()},
        std::chrono::seconds{20}
    );
    if (result) {
      refresh();
      return true;
    }
    kLog.warn("iwctl connect failed for {}: {}", ap.ssid, result.err.empty() ? result.out : result.err);
  }

  return activateAccessPoint(ap);
}

void IwdService::setWirelessEnabled(bool enabled) {
  const bool softBlocked = !enabled;
  bool rfkillDone = false;
  for (const auto& [stationPath, ifname] : m_deviceNames) {
    (void)stationPath;
    if (ifname.empty()) {
      continue;
    }
    const RfkillSwitchResult result = setRfkillSoftBlockedForNetInterface(ifname, softBlocked);
    if (result.hardBlocked) {
      kLog.warn("setWirelessEnabled: rfkill hard block on {}", ifname);
      break;
    }
    if (result.success) {
      rfkillDone = true;
      break;
    }
  }

  if (!rfkillDone) {
    const RfkillSwitchResult fallback = setRfkillSoftBlocked(RfkillDeviceType::Wlan, softBlocked);
    if (fallback.hardBlocked) {
      kLog.warn("setWirelessEnabled: wlan rfkill hard block is active");
    } else if (fallback.success) {
      rfkillDone = true;
    } else if (!fallback.detail.empty()) {
      kLog.debug("setWirelessEnabled: wlan rfkill fallback: {}", fallback.detail);
    }
  }

  for (const auto& [stationPath, ifname] : m_deviceNames) {
    (void)ifname;
    try {
      auto device = sdbus::createProxy(m_bus.connection(), kIwdBusName, sdbus::ObjectPath{stationPath});
      device->setProperty("Powered").onInterface(kDeviceInterface).toValue(enabled);
    } catch (const sdbus::Error& e) {
      kLog.debug("setting IWD Powered failed on {}: {}", stationPath, e.what());
    }
  }

  refresh();
}

void IwdService::disconnect() {
  for (const auto& [stationPath, ifname] : m_deviceNames) {
    (void)ifname;
    try {
      auto station = sdbus::createProxy(m_bus.connection(), kIwdBusName, sdbus::ObjectPath{stationPath});
      station->callMethod("Disconnect").onInterface(kStationInterface);
    } catch (const sdbus::Error& e) {
      kLog.debug("Disconnect failed on {}: {}", stationPath, e.what());
    }
  }
  refresh();
}

void IwdService::forgetSsid(const std::string& ssid) {
  const auto it = m_knownNetworks.find(ssid);
  if (it == m_knownNetworks.end()) {
    return;
  }
  try {
    auto knownNetwork = sdbus::createProxy(m_bus.connection(), kIwdBusName, sdbus::ObjectPath{it->second});
    knownNetwork->callMethod("Forget").onInterface(kKnownNetworkInterface);
    m_knownNetworks.erase(it);
    refresh();
  } catch (const sdbus::Error& e) {
    kLog.warn("Forget failed for {}: {}", ssid, e.what());
  }
}

bool IwdService::hasSavedConnection(const std::string& ssid) const { return m_knownNetworks.contains(ssid); }

void IwdService::emitChangedIfNeeded(NetworkState next) {
  const bool firstSnapshot = !m_hasStateSnapshot;
  const bool stateChanged = next != m_state;
  m_state = std::move(next);
  m_hasStateSnapshot = true;
  if ((firstSnapshot || stateChanged) && m_changeCallback) {
    m_changeCallback(m_state, NetworkChangeOrigin::External);
  }
}
