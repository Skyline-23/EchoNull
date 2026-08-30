#pragma once

#include <Audioclient.h>
#include <Mmdeviceapi.h>
#include <wrl/client.h>

#include <string>
#include <vector>

#include "application/ports.hpp"

namespace echonull {

struct DeviceInfo {
  EDataFlow flow = eAll;
  std::wstring id;
  std::wstring name;
  bool is_default = false;
};

class DeviceManager {
 public:
  static std::vector<DeviceInfo> list(EDataFlow flow);
  static Microsoft::WRL::ComPtr<IMMDevice> resolve(EDataFlow flow,
                                                   const std::wstring& selector,
                                                   DeviceInfo* resolved = nullptr);
  static std::wstring endpoint_id(IMMDevice* device);
  static std::wstring friendly_name(IMMDevice* device);
};

class WasapiDeviceCatalog final : public IDeviceCatalog {
 public:
  [[nodiscard]] std::vector<AudioEndpoint> list(AudioFlow flow) const override;
};

}  // namespace echonull
