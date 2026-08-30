#include "infrastructure/windows/device_manager.hpp"

#include <Functiondiscoverykeys_devpkey.h>
#include <Propvarutil.h>

#include <algorithm>
#include <cwctype>
#include <sstream>
#include <stdexcept>

namespace echonull {
namespace {

using Microsoft::WRL::ComPtr;

void check_hr(const HRESULT result, const char* action) {
  if (FAILED(result)) {
    std::ostringstream message;
    message << action << " failed (HRESULT 0x" << std::hex << static_cast<unsigned long>(result) << ')';
    throw std::runtime_error(message.str());
  }
}

std::wstring lower(std::wstring value) {
  std::transform(value.begin(), value.end(), value.begin(),
                 [](const wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
  return value;
}

ComPtr<IMMDeviceEnumerator> create_enumerator() {
  ComPtr<IMMDeviceEnumerator> enumerator;
  check_hr(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                            IID_PPV_ARGS(&enumerator)),
           "CoCreateInstance(MMDeviceEnumerator)");
  return enumerator;
}

}  // namespace

std::wstring DeviceManager::endpoint_id(IMMDevice* device) {
  LPWSTR raw = nullptr;
  check_hr(device->GetId(&raw), "IMMDevice::GetId");
  std::wstring id(raw);
  CoTaskMemFree(raw);
  return id;
}

std::wstring DeviceManager::friendly_name(IMMDevice* device) {
  ComPtr<IPropertyStore> properties;
  check_hr(device->OpenPropertyStore(STGM_READ, &properties), "IMMDevice::OpenPropertyStore");
  PROPVARIANT value;
  PropVariantInit(&value);
  check_hr(properties->GetValue(PKEY_Device_FriendlyName, &value), "IPropertyStore::GetValue");
  const std::wstring name = value.vt == VT_LPWSTR && value.pwszVal != nullptr ? value.pwszVal : L"(unnamed)";
  PropVariantClear(&value);
  return name;
}

std::vector<DeviceInfo> DeviceManager::list(const EDataFlow flow) {
  const auto enumerator = create_enumerator();
  ComPtr<IMMDeviceCollection> collection;
  check_hr(enumerator->EnumAudioEndpoints(flow, DEVICE_STATE_ACTIVE, &collection),
           "IMMDeviceEnumerator::EnumAudioEndpoints");
  ComPtr<IMMDevice> default_device;
  std::wstring default_id;
  if (SUCCEEDED(enumerator->GetDefaultAudioEndpoint(flow, eConsole, &default_device))) {
    default_id = endpoint_id(default_device.Get());
  }

  UINT count = 0;
  check_hr(collection->GetCount(&count), "IMMDeviceCollection::GetCount");
  std::vector<DeviceInfo> devices;
  devices.reserve(count);
  for (UINT index = 0; index < count; ++index) {
    ComPtr<IMMDevice> device;
    check_hr(collection->Item(index, &device), "IMMDeviceCollection::Item");
    auto id = endpoint_id(device.Get());
    const bool is_default = id == default_id;
    devices.push_back(DeviceInfo{flow, std::move(id), friendly_name(device.Get()), is_default});
  }
  return devices;
}

ComPtr<IMMDevice> DeviceManager::resolve(const EDataFlow flow,
                                         const std::wstring& selector,
                                         DeviceInfo* resolved) {
  const auto enumerator = create_enumerator();
  ComPtr<IMMDevice> device;
  if (selector.empty() || lower(selector) == L"default") {
    check_hr(enumerator->GetDefaultAudioEndpoint(flow, eConsole, &device),
             "IMMDeviceEnumerator::GetDefaultAudioEndpoint");
  } else if (SUCCEEDED(enumerator->GetDevice(selector.c_str(), &device))) {
    // Exact endpoint ID.
  } else {
    const auto selector_lower = lower(selector);
    const auto devices = list(flow);
    std::vector<DeviceInfo> matches;
    for (const auto& candidate : devices) {
      if (lower(candidate.name).find(selector_lower) != std::wstring::npos) {
        matches.push_back(candidate);
      }
    }
    if (matches.empty()) {
      throw std::runtime_error("no active audio endpoint matches the selector");
    }
    if (matches.size() != 1) {
      throw std::runtime_error("audio endpoint selector is ambiguous; use an endpoint ID");
    }
    check_hr(enumerator->GetDevice(matches.front().id.c_str(), &device),
             "IMMDeviceEnumerator::GetDevice");
  }

  if (resolved != nullptr) {
    resolved->flow = flow;
    resolved->id = endpoint_id(device.Get());
    resolved->name = friendly_name(device.Get());
    resolved->is_default = selector.empty() || lower(selector) == L"default";
  }
  return device;
}

std::vector<AudioEndpoint> WasapiDeviceCatalog::list(const AudioFlow flow) const {
  const EDataFlow native_flow = flow == AudioFlow::capture ? eCapture : eRender;
  const auto native_devices = DeviceManager::list(native_flow);
  std::vector<AudioEndpoint> endpoints;
  endpoints.reserve(native_devices.size());
  for (const auto& device : native_devices) {
    endpoints.push_back(AudioEndpoint{flow, device.id, device.name, device.is_default});
  }
  return endpoints;
}

}  // namespace echonull
