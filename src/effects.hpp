// Post-processing options: the game's Post-Processing Stack v1 profile (ambient occlusion,
// screen-space reflections, anti-aliasing) and the legacy SunShafts effect. Main thread only.
//
// The game switches profiles per scene. Every value the add-on writes goes through a Slot that
// remembers the game's own value: a value that differs from what the add-on last wrote was set by
// the game and becomes the new original, and switching an option off writes the original back.
// The objects written to are kept alive with GC handles, so their addresses stay valid.
#pragma once

#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <type_traits>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "il2cpp_main.hpp"

namespace effects {

struct Api {
    bool loaded = false, ok = false;
    void* (*field_from_name)(void* klass, const char* name) = nullptr;
    size_t (*field_get_offset)(void* field) = nullptr;
    void* (*field_get_type)(void* field) = nullptr;
    void* (*class_from_type)(void* type) = nullptr;
    uint32_t (*gchandle_new)(void* obj, bool pinned) = nullptr;
    void (*gchandle_free)(uint32_t handle) = nullptr;
    void* (*type_get_object)(void* type) = nullptr;
    void* (*class_get_type)(void* klass) = nullptr;
    void* (*object_get_class)(void* obj) = nullptr;
    void* (*find)(void* type) = nullptr;  // UnityEngine.Object.FindObjectsOfType(Type)

    bool Load() {
        if (loaded) return ok;
        loaded = true;
        HMODULE m = GetModuleHandleW(L"GameAssembly.dll");
        if (!m) return false;
        auto get = [&](auto& f, const char* name) {
            f = reinterpret_cast<std::remove_reference_t<decltype(f)>>(GetProcAddress(m, name));
            return f != nullptr;
        };
        ok = get(field_from_name, "il2cpp_class_get_field_from_name") && get(field_get_offset, "il2cpp_field_get_offset") &&
             get(field_get_type, "il2cpp_field_get_type") && get(class_from_type, "il2cpp_class_from_type") &&
             get(gchandle_new, "il2cpp_gchandle_new") && get(gchandle_free, "il2cpp_gchandle_free") &&
             get(type_get_object, "il2cpp_type_get_object") && get(class_get_type, "il2cpp_class_get_type") &&
             get(object_get_class, "il2cpp_object_get_class");
        find = reinterpret_cast<void* (*)(void*)>(il2cpp_main::Icall("UnityEngine.Object::FindObjectsOfType(System.Type)"));
        ok = ok && find;
        return ok;
    }
};
inline Api g_api;

// Offset of a dotted field path ("m_Settings.reflection.reflectionQuality") in an object of
// class klass, or -1. Fields of embedded structs are 16 bytes (the object header) closer.
inline int PathOffset(void* klass, const char* path) {
    int offset = 0;
    bool inStruct = false;
    std::string rest = path;
    while (klass) {
        const size_t dot = rest.find('.');
        const std::string name = rest.substr(0, dot);
        void* field = g_api.field_from_name(klass, name.c_str());
        if (!field) return -1;
        offset += static_cast<int>(g_api.field_get_offset(field)) - (inStruct ? 16 : 0);
        if (dot == std::string::npos) return offset;
        klass = g_api.class_from_type(g_api.field_get_type(field));
        inStruct = true;
        rest = rest.substr(dot + 1);
    }
    return -1;
}

// One written value (1 or 4 bytes) in one object.
struct Slot {
    uint32_t handle = 0;  // keeps the object alive
    int original = 0;
    int lastSet = INT32_MIN;
};
inline std::map<std::pair<void*, int>, Slot> g_slots;

inline int Read(const char* p, int size) {
    return size == 1 ? *reinterpret_cast<const uint8_t*>(p) : *reinterpret_cast<const int32_t*>(p);
}
inline void Write(char* p, int size, int v) {
    if (size == 1) *reinterpret_cast<uint8_t*>(p) = static_cast<uint8_t>(v);
    else *reinterpret_cast<int32_t*>(p) = v;
}

// Writes want while active, the game's own value otherwise.
inline void Apply(void* obj, int offset, int size, bool active, int want) {
    if (!obj || offset < 0) return;
    char* p = static_cast<char*>(obj) + offset;
    auto [it, fresh] = g_slots.try_emplace({obj, offset});
    Slot& s = it->second;
    if (fresh) s.handle = g_api.gchandle_new(obj, false);
    const int v = Read(p, size);
    if (v != s.lastSet) s.original = v;  // the game's value
    const int target = active ? want : s.original;
    if (v != target) Write(p, size, target);
    s.lastSet = target;
}

// Field offsets, resolved once from the first live objects.
struct Offsets {
    bool resolved = false;
    int profile = -1;                                         // PostProcessingBehaviour.profile
    int ao = -1, ssr = -1, aa = -1;                           // PostProcessingProfile models
    int enabled = -1;                                         // PostProcessingModel.m_Enabled
    int aoSamples = -1, aoDownsampling = -1;                  // AmbientOcclusionModel
    int ssrQuality = -1;                                      // ScreenSpaceReflectionModel
    int aaMethod = -1;                                        // AntialiasingModel
    int shaftsResolution = -1;                                // SunShafts.resolution
};
inline Offsets g_off;

// Live objects, refreshed every 30 frames (held by GC handles in between).
struct Live {
    void* behaviourClass = nullptr;
    void* shaftsClass = nullptr;
    std::vector<std::pair<uint32_t, void*>> behaviours, shafts;
    int countdown = 0;
};
inline Live g_live;

inline void Refresh(void* klass, std::vector<std::pair<uint32_t, void*>>& list) {
    for (auto& [h, o] : list) g_api.gchandle_free(h);
    list.clear();
    if (!klass) return;
    const char* arr = static_cast<const char*>(g_api.find(g_api.type_get_object(g_api.class_get_type(klass))));
    if (!arr) return;
    const uintptr_t n = *reinterpret_cast<const uintptr_t*>(arr + 0x18);  // Il2CppArray::max_length
    for (uintptr_t i = 0; i < n && i < 16; ++i) {
        void* o = *reinterpret_cast<void* const*>(arr + 0x20 + i * sizeof(void*));
        if (o) list.emplace_back(g_api.gchandle_new(o, false), o);
    }
}

struct Wanted {
    bool on;          // F8
    int ao;           // 0 game, 1 better where the game uses it, 2 everywhere
    int ssr;          // 0 game, 1 full resolution where the game uses it (never turned on)
    bool taa;         // TAA instead of the game's FXAA
    bool shaftsHigh;  // full-resolution sun shafts
};

// Called each frame on the main thread. Returns false if the classes aren't there.
inline bool Update(const Wanted& w, std::string* status) {
    if (!g_api.Load()) {
        *status = "IL2CPP functions missing";
        return false;
    }
    if (!g_live.behaviourClass) {
        HMODULE m = GetModuleHandleW(L"GameAssembly.dll");
        g_live.behaviourClass = il2cpp_main::FindClass(m, "Assembly-CSharp.dll", "PostProcessingBehaviour");
        g_live.shaftsClass = il2cpp_main::FindClass(m, "Assembly-CSharp.dll", "SunShafts");
        if (!g_live.behaviourClass) {
            *status = "PostProcessingBehaviour not found";
            return false;
        }
    }
    if (--g_live.countdown <= 0) {
        g_live.countdown = 30;
        Refresh(g_live.behaviourClass, g_live.behaviours);
        Refresh(g_live.shaftsClass, g_live.shafts);
    }

    int profiles = 0;
    char now[128] = "";  // the first profile's values after applying, read back
    for (auto& [h, b] : g_live.behaviours) {
        if (!g_off.resolved) {
            g_off.resolved = true;
            void* bk = g_api.object_get_class(b);
            g_off.profile = PathOffset(bk, "profile");
        }
        if (g_off.profile < 0) break;
        void* profile = *reinterpret_cast<void**>(static_cast<char*>(b) + g_off.profile);
        if (!profile) continue;
        ++profiles;
        if (g_off.ao < 0 && g_off.ssr < 0) {
            void* pk = g_api.object_get_class(profile);
            g_off.ao = PathOffset(pk, "ambientOcclusion");
            g_off.ssr = PathOffset(pk, "screenSpaceReflection");
            g_off.aa = PathOffset(pk, "antialiasing");
        }
        auto model = [&](int off) -> void* {
            return off < 0 ? nullptr : *reinterpret_cast<void**>(static_cast<char*>(profile) + off);
        };
        void* ao = model(g_off.ao);
        void* ssr = model(g_off.ssr);
        void* aa = model(g_off.aa);
        if (ao && g_off.aoSamples < 0) {
            void* k = g_api.object_get_class(ao);
            g_off.enabled = PathOffset(k, "m_Enabled");
            g_off.aoSamples = PathOffset(k, "m_Settings.sampleCount");
            g_off.aoDownsampling = PathOffset(k, "m_Settings.downsampling");
        }
        if (ssr && g_off.ssrQuality < 0) g_off.ssrQuality = PathOffset(g_api.object_get_class(ssr), "m_Settings.reflection.reflectionQuality");
        if (aa && g_off.aaMethod < 0) g_off.aaMethod = PathOffset(g_api.object_get_class(aa), "m_Settings.method");

        const bool aoOn = w.on && w.ao > 0, ssrOn = w.on && w.ssr > 0;
        Apply(ao, g_off.enabled, 1, w.on && w.ao == 2, 1);
        Apply(ao, g_off.aoSamples, 4, aoOn, 16);          // SampleCount.High
        Apply(ao, g_off.aoDownsampling, 1, aoOn, 0);      // full resolution
        Apply(ssr, g_off.ssrQuality, 4, ssrOn, 0);        // SSRResolution.High
        Apply(aa, g_off.aaMethod, 4, w.on && w.taa, 1);   // Method.Taa
        if (!now[0] && ao && ssr && aa) {
            auto rd = [](void* o, int off, int size) { return off < 0 ? -1 : Read(static_cast<char*>(o) + off, size); };
            snprintf(now, sizeof now, "AO %s %d samples%s, SSR %s %s, AA %s",
                     rd(ao, g_off.enabled, 1) ? "on" : "off", rd(ao, g_off.aoSamples, 4),
                     rd(ao, g_off.aoDownsampling, 1) ? " half-res" : " full-res", rd(ssr, g_off.enabled, 1) ? "on" : "off",
                     rd(ssr, g_off.ssrQuality, 4) == 0 ? "high" : "low", rd(aa, g_off.aaMethod, 4) == 1 ? "TAA" : "FXAA");
        }
    }
    for (auto& [h, s] : g_live.shafts) {
        if (g_off.shaftsResolution < 0) g_off.shaftsResolution = PathOffset(g_api.object_get_class(s), "resolution");
        Apply(s, g_off.shaftsResolution, 4, w.on && w.shaftsHigh, 2);  // SunShaftsResolution.High
    }
    int shafts = -1;
    if (!g_live.shafts.empty() && g_off.shaftsResolution >= 0)
        shafts = Read(static_cast<char*>(g_live.shafts[0].second) + g_off.shaftsResolution, 4);
    char b[256];
    snprintf(b, sizeof b, "%d profile%s: %s; sun shafts %s", profiles, profiles == 1 ? "" : "s", now[0] ? now : "-",
             shafts < 0 ? "none" : shafts == 2 ? "high" : shafts == 1 ? "normal" : "low");
    *status = b;
    return true;
}

}  // namespace effects
