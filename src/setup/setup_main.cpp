#include <Windows.h>
#include <CommCtrl.h>
#include <TlHelp32.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "infrastructure/bundle_format.hpp"
#include "infrastructure/equalizer_apo_config.hpp"

#pragma comment(linker, "\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

namespace {

constexpr wchar_t kPluginName[] = L"EchoNullPlugin.dll";
constexpr std::size_t kCopyBufferSize = 4U * 1024U * 1024U;

std::runtime_error windows_error(const char* message) {
  return std::runtime_error(std::string(message) + " (Windows error " +
                            std::to_string(GetLastError()) + ")");
}

std::wstring widen(const std::string& value) {
  if (value.empty()) return {};
  const int length = MultiByteToWideChar(CP_UTF8, 0, value.data(),
                                         static_cast<int>(value.size()),
                                         nullptr, 0);
  if (length <= 0) return L"Unknown error";
  std::wstring output(static_cast<std::size_t>(length), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()),
                      output.data(), length);
  return output;
}

std::filesystem::path module_path() {
  std::wstring value(32'768, L'\0');
  const DWORD length = GetModuleFileNameW(nullptr, value.data(),
                                          static_cast<DWORD>(value.size()));
  if (length == 0 || length >= value.size()) {
    throw windows_error("Cannot locate EchoNullSetup.exe");
  }
  value.resize(length);
  return value;
}

std::wstring registry_string(HKEY root, const wchar_t* path,
                             const wchar_t* name) {
  HKEY key = nullptr;
  if (RegOpenKeyExW(root, path, 0, KEY_QUERY_VALUE | KEY_WOW64_64KEY, &key) !=
      ERROR_SUCCESS) {
    return {};
  }
  DWORD type = 0;
  DWORD bytes = 0;
  if (RegQueryValueExW(key, name, nullptr, &type, nullptr, &bytes) !=
          ERROR_SUCCESS ||
      (type != REG_SZ && type != REG_EXPAND_SZ)) {
    RegCloseKey(key);
    return {};
  }
  std::wstring value(bytes / sizeof(wchar_t), L'\0');
  const LONG result = RegQueryValueExW(
      key, name, nullptr, &type, reinterpret_cast<BYTE*>(value.data()), &bytes);
  RegCloseKey(key);
  if (result != ERROR_SUCCESS) return {};
  while (!value.empty() && value.back() == L'\0') value.pop_back();
  return value;
}

struct EqualizerApoPaths {
  std::filesystem::path config;
  std::filesystem::path plugin;
  std::filesystem::path editor;
};

EqualizerApoPaths equalizer_apo_paths() {
  const auto config_directory = registry_string(
      HKEY_LOCAL_MACHINE, L"SOFTWARE\\EqualizerAPO", L"ConfigPath");
  if (config_directory.empty()) {
    throw std::runtime_error(
        "Equalizer APO is not installed. Install it before EchoNull.");
  }
  const auto root = std::filesystem::path(config_directory).parent_path();
  EqualizerApoPaths paths{
      std::filesystem::path(config_directory) / L"config.txt",
      root / L"VSTPlugins" / kPluginName, root / L"Editor.exe"};
  if (!std::filesystem::is_regular_file(paths.config)) {
    throw std::runtime_error("Equalizer APO config.txt was not found.");
  }
  return paths;
}

std::string read_text(const std::filesystem::path& path) {
  std::ifstream input(path, std::ios::binary);
  if (!input) throw std::runtime_error("Cannot read Equalizer APO config.txt.");
  return std::string(std::istreambuf_iterator<char>(input),
                     std::istreambuf_iterator<char>());
}

void write_text_atomic(const std::filesystem::path& path,
                       const std::string& content) {
  const auto temporary = path.wstring() + L".echonull.tmp";
  std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
  if (!output) throw std::runtime_error("Cannot update Equalizer APO config.txt.");
  output.write(content.data(), static_cast<std::streamsize>(content.size()));
  output.close();
  if (!output) {
    DeleteFileW(temporary.c_str());
    throw std::runtime_error("Cannot finish the Equalizer APO configuration.");
  }
  if (!MoveFileExW(temporary.c_str(), path.c_str(),
                   MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
    DeleteFileW(temporary.c_str());
    throw windows_error("Cannot replace Equalizer APO config.txt");
  }
}

void install_configuration(const std::filesystem::path& path) {
  const auto backup = path.wstring() + L".echonull-backup";
  if (!CopyFileW(path.c_str(), backup.c_str(), TRUE) &&
      GetLastError() != ERROR_FILE_EXISTS) {
    throw windows_error("Cannot back up Equalizer APO config.txt");
  }
  write_text_atomic(path, echonull::install_echonull_capture_scope(read_text(path)));
}

void remove_configuration(const std::filesystem::path& path) {
  write_text_atomic(path, echonull::remove_echonull_capture_scope(read_text(path)));
}

class AudioServiceGuard {
 public:
  AudioServiceGuard() {
    manager_ = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (manager_ == nullptr) throw windows_error("Cannot open Service Manager");
    service_ = OpenServiceW(manager_, L"Audiosrv",
                            SERVICE_QUERY_STATUS | SERVICE_STOP | SERVICE_START);
    if (service_ == nullptr) throw windows_error("Cannot open Windows Audio");
    SERVICE_STATUS_PROCESS status{};
    DWORD bytes = 0;
    if (!QueryServiceStatusEx(service_, SC_STATUS_PROCESS_INFO,
                              reinterpret_cast<BYTE*>(&status), sizeof(status),
                              &bytes)) {
      throw windows_error("Cannot query Windows Audio");
    }
    was_running_ = status.dwCurrentState != SERVICE_STOPPED;
    if (was_running_ && status.dwCurrentState != SERVICE_STOP_PENDING) {
      SERVICE_STATUS ignored{};
      if (!ControlService(service_, SERVICE_CONTROL_STOP, &ignored) &&
          GetLastError() != ERROR_SERVICE_NOT_ACTIVE) {
        throw windows_error("Cannot stop Windows Audio");
      }
    }
    wait_for(SERVICE_STOPPED);
  }

  ~AudioServiceGuard() {
    if (was_running_ && service_ != nullptr) {
      if (!StartServiceW(service_, 0, nullptr) &&
          GetLastError() != ERROR_SERVICE_ALREADY_RUNNING) {
        // The installer reports file/configuration failures. Windows can restore
        // its audio service independently if this best-effort restart fails.
      }
      wait_for(SERVICE_RUNNING);
    }
    if (service_ != nullptr) CloseServiceHandle(service_);
    if (manager_ != nullptr) CloseServiceHandle(manager_);
  }

  AudioServiceGuard(const AudioServiceGuard&) = delete;
  AudioServiceGuard& operator=(const AudioServiceGuard&) = delete;

 private:
  void wait_for(const DWORD target) const noexcept {
    if (service_ == nullptr) return;
    for (int attempt = 0; attempt < 100; ++attempt) {
      SERVICE_STATUS_PROCESS status{};
      DWORD bytes = 0;
      if (!QueryServiceStatusEx(service_, SC_STATUS_PROCESS_INFO,
                                reinterpret_cast<BYTE*>(&status), sizeof(status),
                                &bytes) ||
          status.dwCurrentState == target) {
        return;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
  }

  SC_HANDLE manager_ = nullptr;
  SC_HANDLE service_ = nullptr;
  bool was_running_ = false;
};

std::vector<DWORD> matching_processes(const std::filesystem::path& executable) {
  std::vector<DWORD> matches;
  const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
  if (snapshot == INVALID_HANDLE_VALUE) return matches;
  PROCESSENTRY32W entry{sizeof(entry)};
  if (Process32FirstW(snapshot, &entry)) do {
    const HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION,
                                       FALSE, entry.th32ProcessID);
    if (process == nullptr) continue;
    std::wstring path(32'768, L'\0');
    DWORD length = static_cast<DWORD>(path.size());
    if (QueryFullProcessImageNameW(process, 0, path.data(), &length)) {
      path.resize(length);
      if (_wcsicmp(path.c_str(), executable.c_str()) == 0) {
        matches.push_back(entry.th32ProcessID);
      }
    }
    CloseHandle(process);
  } while (Process32NextW(snapshot, &entry));
  CloseHandle(snapshot);
  return matches;
}

void close_editor(const std::filesystem::path& editor) {
  const auto processes = matching_processes(editor);
  if (processes.empty()) return;
  EnumWindows(
      [](const HWND window, const LPARAM parameter) -> BOOL {
        DWORD process = 0;
        GetWindowThreadProcessId(window, &process);
        const auto* ids = reinterpret_cast<const std::vector<DWORD>*>(parameter);
        if (std::ranges::find(*ids, process) != ids->end()) {
          PostMessageW(window, WM_CLOSE, 0, 0);
        }
        return TRUE;
      },
      reinterpret_cast<LPARAM>(&processes));
  std::this_thread::sleep_for(std::chrono::milliseconds(800));
  for (const DWORD id : processes) {
    const HANDLE process = OpenProcess(SYNCHRONIZE | PROCESS_TERMINATE, FALSE, id);
    if (process == nullptr) continue;
    if (WaitForSingleObject(process, 0) == WAIT_TIMEOUT) TerminateProcess(process, 0);
    CloseHandle(process);
  }
}

void terminate_audiodg() {
  const auto system = std::filesystem::path(
      registry_string(HKEY_LOCAL_MACHINE,
                      L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion",
                      L"SystemRoot"));
  const auto expected = system.empty() ? std::filesystem::path{}
                                       : system / L"System32" / L"audiodg.exe";
  if (expected.empty()) return;
  for (const DWORD id : matching_processes(expected)) {
    const HANDLE process = OpenProcess(PROCESS_TERMINATE, FALSE, id);
    if (process != nullptr) {
      TerminateProcess(process, 0);
      CloseHandle(process);
    }
  }
}

struct EmbeddedAsset {
  std::uint64_t offset = 0;
  std::uint64_t size = 0;
};

EmbeddedAsset plugin_asset(std::ifstream& input, const std::uint64_t file_size) {
  echonull::bundle::Footer footer{};
  if (file_size < sizeof(footer)) {
    throw std::runtime_error("EchoNullSetup.exe has no embedded plug-in.");
  }
  input.seekg(static_cast<std::streamoff>(file_size - sizeof(footer)));
  input.read(reinterpret_cast<char*>(&footer), sizeof(footer));
  const auto footer_offset = file_size - sizeof(footer);
  if (!input || footer.magic != echonull::bundle::kMagic ||
      footer.version != echonull::bundle::kVersion ||
      footer.index_offset + footer.index_size != footer_offset ||
      footer.entry_count == 0 || footer.entry_count > 16) {
    throw std::runtime_error("EchoNullSetup.exe payload is corrupt.");
  }
  input.seekg(static_cast<std::streamoff>(footer.index_offset));
  for (std::uint32_t index = 0; index < footer.entry_count; ++index) {
    echonull::bundle::Entry entry{};
    input.read(reinterpret_cast<char*>(&entry), sizeof(entry));
    if (!input || entry.name_size == 0 || entry.name_size > 255 ||
        entry.offset < footer.original_size ||
        entry.offset + entry.size > footer.index_offset) {
      throw std::runtime_error("EchoNullSetup.exe payload index is corrupt.");
    }
    std::string name(entry.name_size, '\0');
    input.read(name.data(), static_cast<std::streamsize>(name.size()));
    if (_stricmp(name.c_str(), "EchoNullPlugin.dll") == 0) {
      return {entry.offset, entry.size};
    }
  }
  throw std::runtime_error("EchoNullPlugin.dll is missing from the setup package.");
}

void extract_plugin(const std::filesystem::path& setup,
                    const std::filesystem::path& destination) {
  std::ifstream input(setup, std::ios::binary | std::ios::ate);
  if (!input) throw std::runtime_error("Cannot open EchoNullSetup.exe.");
  const auto size = static_cast<std::uint64_t>(input.tellg());
  const auto asset = plugin_asset(input, size);
  std::filesystem::create_directories(destination.parent_path());
  const auto temporary = destination.wstring() + L".setup.tmp";
  std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
  if (!output) throw std::runtime_error("Cannot create EchoNullPlugin.dll.");
  input.clear();
  input.seekg(static_cast<std::streamoff>(asset.offset));
  std::vector<char> buffer(kCopyBufferSize);
  std::uint64_t remaining = asset.size;
  while (remaining != 0) {
    const auto count = static_cast<std::streamsize>(
        std::min<std::uint64_t>(remaining, buffer.size()));
    input.read(buffer.data(), count);
    if (input.gcount() != count) {
      output.close();
      DeleteFileW(temporary.c_str());
      throw std::runtime_error("The embedded EchoNullPlugin.dll is truncated.");
    }
    output.write(buffer.data(), count);
    if (!output) {
      output.close();
      DeleteFileW(temporary.c_str());
      throw std::runtime_error("Cannot write EchoNullPlugin.dll.");
    }
    remaining -= static_cast<std::uint64_t>(count);
  }
  output.close();
  if (!MoveFileExW(temporary.c_str(), destination.c_str(),
                   MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
    DeleteFileW(temporary.c_str());
    throw windows_error("Cannot install EchoNullPlugin.dll");
  }
}

int choose_action(const bool installed) {
  const TASKDIALOG_BUTTON buttons[] = {
      {100, installed ? L"Repair / Update EchoNull"
                      : L"Install EchoNull"},
      {101, L"Remove EchoNull"},
  };
  TASKDIALOGCONFIG config{sizeof(config)};
  config.hInstance = GetModuleHandleW(nullptr);
  config.dwFlags = TDF_USE_COMMAND_LINKS | TDF_ALLOW_DIALOG_CANCELLATION;
  config.pszWindowTitle = L"EchoNull Setup";
  config.pszMainIcon = TD_INFORMATION_ICON;
  config.pszMainInstruction = installed ? L"EchoNull is already installed"
                                        : L"Install EchoNull";
  config.pszContent =
      L"Setup installs the self-contained plug-in and automatically scopes it "
      L"to Equalizer APO capture processing. No virtual microphone is created.\n\n"
      L"Windows Audio restarts briefly during installation.";
  config.pButtons = buttons;
  config.cButtons = installed ? 2U : 1U;
  config.nDefaultButton = 100;
  config.nDefaultRadioButton = 0;
  int selected = IDCANCEL;
  if (FAILED(TaskDialogIndirect(&config, &selected, nullptr, nullptr))) {
    return MessageBoxW(nullptr, config.pszContent, L"EchoNull Setup",
                       MB_OKCANCEL | MB_ICONINFORMATION) == IDOK
               ? 100
               : IDCANCEL;
  }
  return selected;
}

void show_result(const wchar_t* instruction, const wchar_t* content) {
  TASKDIALOGCONFIG config{sizeof(config)};
  config.pszWindowTitle = L"EchoNull Setup";
  config.pszMainIcon = TD_INFORMATION_ICON;
  config.pszMainInstruction = instruction;
  config.pszContent = content;
  config.dwCommonButtons = TDCBF_OK_BUTTON;
  TaskDialogIndirect(&config, nullptr, nullptr, nullptr);
}

}  // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
  SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
  try {
    const auto paths = equalizer_apo_paths();
    const int action = choose_action(std::filesystem::is_regular_file(paths.plugin));
    if (action == IDCANCEL) return 0;
    close_editor(paths.editor);
    {
      AudioServiceGuard audio;
      terminate_audiodg();
      if (action == 100) {
        extract_plugin(module_path(), paths.plugin);
        install_configuration(paths.config);
      } else if (action == 101) {
        remove_configuration(paths.config);
        if (!DeleteFileW(paths.plugin.c_str()) &&
            GetLastError() != ERROR_FILE_NOT_FOUND &&
            !MoveFileExW(paths.plugin.c_str(), nullptr,
                         MOVEFILE_DELAY_UNTIL_REBOOT)) {
          throw windows_error("Cannot remove EchoNullPlugin.dll");
        }
      }
    }
    if (action == 100) {
      show_result(L"EchoNull is ready",
                  L"Open Equalizer APO Configuration Editor, open the EchoNull "
                  L"panel, and select a playback reference. Capture scoping is "
                  L"already configured.");
    } else {
      show_result(L"EchoNull was removed",
                  L"The setup-owned capture configuration and plug-in were removed.");
    }
    return 0;
  } catch (const std::exception& error) {
    const auto message = widen(error.what());
    MessageBoxW(nullptr, message.c_str(), L"EchoNull Setup",
                MB_OK | MB_ICONERROR);
    return 1;
  }
}
