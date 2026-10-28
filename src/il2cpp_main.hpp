// Runs add-on work on the game's main thread, where Unity's API may be called.
//
// Unity calls MonoBehaviour.Update through the IL2CPP runtime, which reads the method's entry
// point from its MethodInfo at each call. The add-on points the Update of a few classes that
// live for the whole game (SteamManager first) at its own functions, which call the original and
// then run the queued work, at most once per frame. Only those pointers in the runtime's metadata
// change: no game code is written. Uninstall puts the originals back.
#pragma once

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <cstring>
#include <functional>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace il2cpp_main {

using UpdateFn = void (*)(void* self, const void* method);

struct Target {
    const char* image;
    const char* klass;    // class name, any namespace
    const char* method;   // a parameterless instance method Unity calls each frame
    void** slot = nullptr;
    UpdateFn original = nullptr;
    std::atomic<uint64_t> calls{0};
};

inline Target g_targets[] = {
    {"Assembly-CSharp.dll", "SteamManager", "Update"},
    {"Assembly-CSharp.dll", "UICamera", "Update"},
    {"Assembly-CSharp.dll", "CUIManager", "Update"},
    {"Assembly-CSharp.dll", "CriMonoBehaviourManager", "Update"},
    {"UnityEngine.UI.dll", "EventSystem", "Update"},
};
constexpr int kTargets = sizeof(g_targets) / sizeof(g_targets[0]);

struct State {
    std::atomic<uint64_t> calls{0};      // frames the work ran in
    std::atomic<uint64_t> frame{1};      // bumped by the add-on at each present
    uint64_t lastFrame = 0;              // main thread only
    std::atomic<DWORD> threadId{0};
    std::mutex mutex;
    std::vector<std::function<void()>> queue;
    std::vector<std::function<void()>> everyFrame;  // set before Install
};
inline State g_state;

inline void RunWork() {
    const uint64_t f = g_state.frame;
    if (f == g_state.lastFrame) return;  // once per frame, whichever hook comes first
    g_state.lastFrame = f;
    g_state.threadId = GetCurrentThreadId();
    ++g_state.calls;
    std::vector<std::function<void()>> work;
    {
        std::lock_guard<std::mutex> lock(g_state.mutex);
        work.swap(g_state.queue);
    }
    for (auto& w : g_state.everyFrame) w();
    for (auto& w : work) w();
}

template <int N>
void Hook(void* self, const void* method) {
    g_targets[N].original(self, method);
    ++g_targets[N].calls;
    RunWork();
}

template <int... N>
constexpr UpdateFn HookFor(int i, std::integer_sequence<int, N...>) {
    constexpr UpdateFn table[] = {&Hook<N>...};
    return table[i];
}

// Finds a class by image and name (any namespace) through the exported IL2CPP API.
inline void* FindClass(HMODULE mod, const char* image, const char* name) {
    auto domain_get = reinterpret_cast<void* (*)()>(GetProcAddress(mod, "il2cpp_domain_get"));
    auto get_assemblies = reinterpret_cast<void** (*)(void*, size_t*)>(GetProcAddress(mod, "il2cpp_domain_get_assemblies"));
    auto get_image = reinterpret_cast<void* (*)(void*)>(GetProcAddress(mod, "il2cpp_assembly_get_image"));
    auto image_name = reinterpret_cast<const char* (*)(void*)>(GetProcAddress(mod, "il2cpp_image_get_name"));
    auto class_count = reinterpret_cast<size_t (*)(void*)>(GetProcAddress(mod, "il2cpp_image_get_class_count"));
    auto get_class = reinterpret_cast<void* (*)(void*, size_t)>(GetProcAddress(mod, "il2cpp_image_get_class"));
    auto class_name = reinterpret_cast<const char* (*)(void*)>(GetProcAddress(mod, "il2cpp_class_get_name"));
    if (!domain_get || !get_assemblies || !get_image || !image_name || !class_count || !get_class || !class_name)
        return nullptr;
    size_t count = 0;
    void** assemblies = get_assemblies(domain_get(), &count);
    for (size_t i = 0; i < count; ++i) {
        void* img = get_image(assemblies[i]);
        const char* n = image_name(img);
        if (!n || strcmp(n, image)) continue;
        const size_t classes = class_count(img);
        for (size_t c = 0; c < classes; ++c) {
            void* k = get_class(img, c);
            const char* cn = k ? class_name(k) : nullptr;
            if (cn && !strcmp(cn, name)) return k;
        }
    }
    return nullptr;
}

// Call once the game has started. Returns a line for the log: which hooks went in.
inline std::string Install() {
    HMODULE mod = GetModuleHandleW(L"GameAssembly.dll");
    if (!mod) return "GameAssembly.dll not loaded";
    auto get_method = reinterpret_cast<void* (*)(void*, const char*, int)>(
        GetProcAddress(mod, "il2cpp_class_get_method_from_name"));
    if (!get_method) return "il2cpp_class_get_method_from_name missing";
    std::string result;
    for (int i = 0; i < kTargets; ++i) {
        Target& t = g_targets[i];
        if (t.slot) continue;
        void* klass = FindClass(mod, t.image, t.klass);
        void* method = klass ? get_method(klass, t.method, 0) : nullptr;
        auto slot = static_cast<void**>(method);  // MethodInfo::methodPointer is the first member
        result += std::string(result.empty() ? "" : ", ") + t.klass + "." + t.method;
        if (!slot || !*slot) {
            result += klass ? " (no method)" : " (no class)";
            continue;
        }
        t.original = reinterpret_cast<UpdateFn>(*slot);
        t.slot = slot;
        InterlockedExchangePointer(slot, reinterpret_cast<void*>(HookFor(i, std::make_integer_sequence<int, kTargets>{})));
    }
    return result;
}

inline void Uninstall() {
    for (Target& t : g_targets) {
        if (!t.slot) continue;
        InterlockedExchangePointer(t.slot, reinterpret_cast<void*>(t.original));
        t.slot = nullptr;
    }
}

// Which hooks have been called, for the log and the overlay.
inline std::string Report() {
    std::string r;
    for (const Target& t : g_targets) {
        if (!t.slot) continue;
        r += std::string(r.empty() ? "" : ", ") + t.klass + " " + std::to_string(t.calls.load());
    }
    return r.empty() ? "none installed" : r;
}

inline void Post(std::function<void()> f) {
    std::lock_guard<std::mutex> lock(g_state.mutex);
    g_state.queue.push_back(std::move(f));
}

// Unity's internal calls (icalls), resolved by name; nullptr if missing.
inline void* Icall(const char* name) {
    static auto resolve = reinterpret_cast<void* (*)(const char*)>(
        GetProcAddress(GetModuleHandleW(L"GameAssembly.dll"), "il2cpp_resolve_icall"));
    return resolve ? resolve(name) : nullptr;
}

}  // namespace il2cpp_main
