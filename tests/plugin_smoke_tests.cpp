#include <Windows.h>

#include <cmath>
#include <cstdint>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <vector>

#include <vst.h>

namespace {

intptr_t VST_FUNCTION_INTERFACE host_callback(vst_effect_t*, const int32_t opcode,
                                               int32_t, int64_t, const char*, float) {
  if (opcode == VST_HOST_OPCODE_VST_VERSION) return VST_VERSION_2_4_0_0;
  if (opcode == VST_HOST_OPCODE_04) return 0;
  return 0;
}

void require(const bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}

}  // namespace

int wmain(const int argc, wchar_t** argv) {
  try {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    require(argc == 2 || argc == 3, "plugin path argument is missing");
    HMODULE module = LoadLibraryW(argv[1]);
    require(module != nullptr, "EchoNullPlugin.dll could not be loaded");
    const auto main_entry = reinterpret_cast<vst_effect_t* (*)(vst_host_callback_t)>(
        GetProcAddress(module, "VSTPluginMain"));
    require(main_entry != nullptr, "VSTPluginMain export is missing");
    if (argc == 3 && _wcsicmp(argv[2], L"--runtime") == 0) {
      const auto runtime_self_test = reinterpret_cast<int (*)()>(
          GetProcAddress(module, "EchoNullRuntimeSelfTest"));
      require(runtime_self_test != nullptr, "runtime self-test export is missing");
      require(runtime_self_test() == 0,
              "packaged NvAFX runtime or AEC model self-test failed");
    }
    vst_effect_t* effect = main_entry(&host_callback);
    require(effect != nullptr && effect->magic_number == VST_MAGICNUMBER,
            "VST-compatible effect initialization failed");
    require(effect->num_inputs == 2 && effect->num_outputs == 2,
            "plugin channel contract changed");
    require(effect->num_params == 5, "plugin parameter contract changed");
    require((effect->flags & VST_EFFECT_FLAG_EDITOR) != 0,
            "embedded editor flag is missing");
    require((effect->flags & VST_EFFECT_FLAG_CHUNKS) != 0,
            "VST chunk persistence flag is missing");

    effect->set_parameter(effect, 1, 0.0F);
    void* chunk_pointer = nullptr;
    const auto chunk_size = effect->control(
        effect, VST_EFFECT_OPCODE_GET_CHUNK_DATA, 1, 0, &chunk_pointer, 0.0F);
    require(chunk_size > 0 && chunk_pointer != nullptr,
            "plug-in state chunk could not be saved");
    const auto* chunk_begin = static_cast<const std::uint8_t*>(chunk_pointer);
    std::vector<std::uint8_t> chunk(
        chunk_begin, chunk_begin + static_cast<std::size_t>(chunk_size));
    effect->set_parameter(effect, 1, 1.0F);
    require(effect->control(effect, VST_EFFECT_OPCODE_SET_CHUNK_DATA, 1,
                            static_cast<intptr_t>(chunk.size()), chunk.data(),
                            0.0F) == 1 &&
                effect->get_parameter(effect, 1) < 0.5F,
            "plug-in state chunk could not be restored");

    vst_rect_t* editor_rect = nullptr;
    require(effect->control(effect, VST_EFFECT_OPCODE_EDITOR_GET_RECT, 0, 0,
                            &editor_rect, 0.0F) == 1 &&
                editor_rect != nullptr && editor_rect->right >= 700 &&
                editor_rect->bottom >= 580,
            "embedded editor dimensions are invalid");
    HWND editor_parent = CreateWindowExW(
        0, L"STATIC", L"EchoNull smoke host", WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, editor_rect->right + 20,
        editor_rect->bottom + 50, nullptr, nullptr,
        GetModuleHandleW(nullptr), nullptr);
    require(editor_parent != nullptr, "editor smoke-test host could not be created");
    require(effect->control(effect, VST_EFFECT_OPCODE_EDITOR_OPEN, 0, 0,
                            editor_parent, 0.0F) == 1,
            "embedded editor could not be opened");
    effect->control(effect, VST_EFFECT_OPCODE_EDITOR_KEEP_ALIVE, 0, 0, nullptr,
                    0.0F);
    wchar_t preview[2]{};
    if (GetEnvironmentVariableW(L"ECHONULL_SHOW_EDITOR", preview,
                                static_cast<DWORD>(std::size(preview))) != 0) {
      ShowWindow(editor_parent, SW_SHOW);
      UpdateWindow(editor_parent);
      const ULONGLONG deadline = GetTickCount64() + 60'000;
      while (GetTickCount64() < deadline) {
        MSG message{};
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
          TranslateMessage(&message);
          DispatchMessageW(&message);
        }
        effect->control(effect, VST_EFFECT_OPCODE_EDITOR_KEEP_ALIVE, 0, 0,
                        nullptr, 0.0F);
        Sleep(16);
      }
    }
    effect->control(effect, VST_EFFECT_OPCODE_EDITOR_CLOSE, 0, 0, nullptr, 0.0F);
    DestroyWindow(editor_parent);

    effect->control(effect, VST_EFFECT_OPCODE_INITIALIZE, 0, 0, nullptr, 0.0F);
    effect->control(effect, VST_EFFECT_OPCODE_SET_SAMPLE_RATE, 0, 0, nullptr, 96'000.0F);
    effect->control(effect, VST_EFFECT_OPCODE_SET_BLOCK_SIZE, 0, 960, nullptr, 0.0F);
    effect->control(effect, VST_EFFECT_OPCODE_SUSPEND_RESUME, 0, 1, nullptr, 0.0F);
    effect->control(effect, VST_EFFECT_OPCODE_PROCESS_BEGIN, 0, 0, nullptr, 0.0F);

    std::vector<float> left(960, 0.25F);
    std::vector<float> right(960, -0.125F);
    std::vector<float> output_left(960, 0.0F);
    std::vector<float> output_right(960, 0.0F);
    const float* inputs[2] = {left.data(), right.data()};
    float* outputs[2] = {output_left.data(), output_right.data()};
    effect->process_float(effect, inputs, outputs, 960);
    require(output_left == left && output_right == right,
            "disabled AEC is not bit-transparent");

    effect->control(effect, VST_EFFECT_OPCODE_PROCESS_END, 0, 0, nullptr, 0.0F);
    effect->control(effect, VST_EFFECT_OPCODE_SUSPEND_RESUME, 0, 0, nullptr, 0.0F);
    effect->control(effect, VST_EFFECT_OPCODE_DESTROY, 0, 0, nullptr, 0.0F);
    FreeLibrary(module);
    std::cout << "EchoNull plugin smoke test passed.\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "Plugin smoke test failure: " << error.what() << '\n';
    return 1;
  }
}
