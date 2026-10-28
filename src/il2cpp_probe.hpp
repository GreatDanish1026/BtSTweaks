// Read-only probe of the game's IL2CPP classes: writes the fields, enum values, methods and the
// current values of singletons for the classes that hold graphics settings. It runs on its own
// thread, attached to the IL2CPP runtime for the duration, and calls no game code (only the
// runtime's reflection API, exported by GameAssembly.dll).
#pragma once

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <type_traits>

namespace il2cpp_probe {

using Fn = void*;
struct Api {
    HMODULE mod = nullptr;
    void* (*domain_get)();
    void** (*domain_get_assemblies)(void* domain, size_t* count);
    void* (*assembly_get_image)(void* assembly);
    const char* (*image_get_name)(void* image);
    size_t (*image_get_class_count)(void* image);
    void* (*image_get_class)(void* image, size_t index);
    const char* (*class_get_name)(void* klass);
    const char* (*class_get_namespace)(void* klass);
    void* (*class_get_parent)(void* klass);
    void* (*class_get_fields)(void* klass, void** iter);
    void* (*class_get_methods)(void* klass, void** iter);
    void* (*class_get_nested_types)(void* klass, void** iter);
    bool (*class_is_enum)(void* klass);
    int32_t (*class_instance_size)(void* klass);
    void* (*class_get_static_field_data)(void* klass);
    void* (*class_from_type)(void* type);
    const char* (*field_get_name)(void* field);
    void* (*field_get_type)(void* field);
    size_t (*field_get_offset)(void* field);
    bool (*field_is_literal)(void* field);
    void (*field_static_get_value)(void* field, void* value);
    int (*type_get_type)(void* type);
    char* (*type_get_name)(void* type);
    uint32_t (*type_get_attrs)(void* type);
    const char* (*method_get_name)(void* method);
    uint32_t (*method_get_param_count)(void* method);
    void* (*method_get_param)(void* method, uint32_t index);
    uint32_t (*method_get_flags)(void* method, uint32_t* iflags);
    void (*free)(void* p);
    void* (*thread_attach)(void* domain);
    void* (*type_get_object)(void* type);
    void* (*class_get_type)(void* klass);
    void* (*object_get_class)(void* obj);
    void (*thread_detach)(void* thread);

    bool Load() {
        mod = GetModuleHandleW(L"GameAssembly.dll");
        if (!mod) return false;
        bool ok = true;
        auto get = [&](auto& f, const char* name) {
            f = reinterpret_cast<std::remove_reference_t<decltype(f)>>(GetProcAddress(mod, name));
            if (!f) ok = false;
        };
        get(domain_get, "il2cpp_domain_get");
        get(domain_get_assemblies, "il2cpp_domain_get_assemblies");
        get(assembly_get_image, "il2cpp_assembly_get_image");
        get(image_get_name, "il2cpp_image_get_name");
        get(image_get_class_count, "il2cpp_image_get_class_count");
        get(image_get_class, "il2cpp_image_get_class");
        get(class_get_name, "il2cpp_class_get_name");
        get(class_get_namespace, "il2cpp_class_get_namespace");
        get(class_get_parent, "il2cpp_class_get_parent");
        get(class_get_fields, "il2cpp_class_get_fields");
        get(class_get_methods, "il2cpp_class_get_methods");
        get(class_get_nested_types, "il2cpp_class_get_nested_types");
        get(class_is_enum, "il2cpp_class_is_enum");
        get(class_instance_size, "il2cpp_class_instance_size");
        get(class_get_static_field_data, "il2cpp_class_get_static_field_data");
        get(class_from_type, "il2cpp_class_from_type");
        get(field_get_name, "il2cpp_field_get_name");
        get(field_get_type, "il2cpp_field_get_type");
        get(field_get_offset, "il2cpp_field_get_offset");
        get(field_is_literal, "il2cpp_field_is_literal");
        get(field_static_get_value, "il2cpp_field_static_get_value");
        get(type_get_type, "il2cpp_type_get_type");
        get(type_get_name, "il2cpp_type_get_name");
        get(type_get_attrs, "il2cpp_type_get_attrs");
        get(method_get_name, "il2cpp_method_get_name");
        get(method_get_param_count, "il2cpp_method_get_param_count");
        get(method_get_param, "il2cpp_method_get_param");
        get(method_get_flags, "il2cpp_method_get_flags");
        get(free, "il2cpp_free");
        get(thread_attach, "il2cpp_thread_attach");
        get(thread_detach, "il2cpp_thread_detach");
        get(type_get_object, "il2cpp_type_get_object");
        get(class_get_type, "il2cpp_class_get_type");
        get(object_get_class, "il2cpp_object_get_class");
        return ok;
    }
};

// IL2CPP type codes (Il2CppTypeEnum).
enum : int { kBool = 2, kChar = 3, kI1 = 4, kU1 = 5, kI2 = 6, kU2 = 7, kI4 = 8, kU4 = 9, kI8 = 10, kU8 = 11,
             kR4 = 12, kR8 = 13, kString = 14, kValueType = 17, kClass = 18, kGenericInst = 21, kObject = 28 };
constexpr uint32_t kFieldStatic = 0x10;  // FIELD_ATTRIBUTE_STATIC

inline bool SafeRead(const void* p, void* out, size_t n) {
    __try {
        memcpy(out, p, n);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// Classes whose name contains one of these are written out (with their nested types).
inline bool Wanted(const char* name, const char* ns) {
    static const char* const keys[] = {
        "RenderSettings", "Quality", "SettingsManager", "AdvancedSetting", "BasicSetting", "GraphicsSettings",
        "GraphicSettings", "Dof34", "SSRSettings", "SunShafts", "TiltShift", "PostProcessing", "CameraUpscaler",
        "IdolPrefs", "PrefData", "ScreenSiz", "DawnSettings", "SettingsUI", "LODParameters", "GlobalFog",
        "CameraMotionBlur", "Upscal", "Shadow", "Reflection", "Render_", "SettingOption", "Preset"};
    if (!name) return false;
    if (ns && (!strncmp(ns, "System", 6) || !strncmp(ns, "Mono", 4) || !strncmp(ns, "Newtonsoft", 10) ||
               !strncmp(ns, "Steamworks", 10)))
        return false;
    for (const char* k : keys)
        if (strstr(name, k)) return true;
    return false;
}

class Probe {
public:
    explicit Probe(FILE* f) : f_(f) {}

    void Run() {
        if (!api_.Load()) {
            fprintf(f_, "IL2CPP API not found in GameAssembly.dll\n");
            return;
        }
        base_ = reinterpret_cast<uintptr_t>(api_.mod);
        void* domain = api_.domain_get();
        void* thread = api_.thread_attach(domain);
        size_t count = 0;
        void** assemblies = api_.domain_get_assemblies(domain, &count);
        fprintf(f_, "GameAssembly.dll at %016llx, %zu assemblies\n", static_cast<unsigned long long>(base_), count);
        for (size_t a = 0; a < count; ++a) {
            void* image = api_.assembly_get_image(assemblies[a]);
            const char* iname = api_.image_get_name(image);
            const size_t n = api_.image_get_class_count(image);
            fprintf(f_, "\n## image %s (%zu classes)\n", iname ? iname : "?", n);
            if (iname && (!strncmp(iname, "mscorlib", 8) || !strncmp(iname, "System", 6) || !strncmp(iname, "Mono.", 5)))
                continue;
            for (size_t i = 0; i < n; ++i) {
                void* k = api_.image_get_class(image, i);
                if (k && Wanted(api_.class_get_name(k), api_.class_get_namespace(k))) DumpClass(k, 0);
            }
        }
        fflush(f_);
        api_.thread_detach(thread);
    }

    // On the main thread: the live objects of the given classes, with their settings. Fields of
    // struct type are expanded, and references to *Model / *Profile objects are followed.
    void RunLive(const char* const* classes, size_t n, void (*extra)(FILE*)) {
        if (!api_.Load()) return;
        base_ = reinterpret_cast<uintptr_t>(api_.mod);
        auto resolve = reinterpret_cast<void* (*)(const char*)>(GetProcAddress(api_.mod, "il2cpp_resolve_icall"));
        auto find = reinterpret_cast<void* (*)(void*)>(
            resolve ? resolve("UnityEngine.Object::FindObjectsOfType(System.Type)") : nullptr);
        if (extra) extra(f_);
        if (!find) {
            fprintf(f_, "Object.FindObjectsOfType(Type) not found\n");
            return;
        }
        size_t count = 0;
        void** assemblies = api_.domain_get_assemblies(api_.domain_get(), &count);
        for (size_t c = 0; c < n; ++c) {
            for (size_t a = 0; a < count; ++a) {
                void* image = api_.assembly_get_image(assemblies[a]);
                const size_t classCount = api_.image_get_class_count(image);
                for (size_t i = 0; i < classCount; ++i) {
                    void* k = api_.image_get_class(image, i);
                    const char* kn = k ? api_.class_get_name(k) : nullptr;
                    if (!kn || strcmp(kn, classes[c])) continue;
                    void* arr = find(api_.type_get_object(api_.class_get_type(k)));
                    uintptr_t len = 0;
                    if (arr) SafeRead(static_cast<char*>(arr) + 0x18, &len, sizeof len);  // Il2CppArray::max_length
                    fprintf(f_, "\n# %s.%s: %zu live\n", api_.class_get_namespace(k), kn, static_cast<size_t>(len));
                    for (uintptr_t e = 0; e < len && e < 8; ++e) {
                        void* obj = nullptr;
                        SafeRead(static_cast<char*>(arr) + 0x20 + e * sizeof(void*), &obj, sizeof obj);
                        if (!obj) continue;
                        fprintf(f_, "  [%zu]\n", static_cast<size_t>(e));
                        DumpObject(api_.object_get_class(obj), static_cast<const char*>(obj), 4, 2, false);
                    }
                }
            }
        }
        fflush(f_);
    }

private:
    // Fields of an object (or of a struct at p when inStruct: offsets include the 16-byte object
    // header, which an embedded struct doesn't have).
    void DumpObject(void* klass, const char* p, int indent, int depth, bool inStruct) {
        if (indent > 24) return;
        for (void* k = klass; k; k = api_.class_get_parent(k)) {
            const char* kn = api_.class_get_name(k);
            if (kn && (!strcmp(kn, "Object") || !strcmp(kn, "MonoBehaviour") || !strcmp(kn, "Behaviour") ||
                       !strcmp(kn, "Component") || !strcmp(kn, "ScriptableObject") || !strcmp(kn, "ValueType")))
                break;
            void* it = nullptr;
            while (void* fld = api_.class_get_fields(k, &it)) {
                void* type = api_.field_get_type(fld);
                if (api_.type_get_attrs(type) & kFieldStatic) continue;
                const char* at = p + api_.field_get_offset(fld) - (inStruct ? 16 : 0);
                const char* name = api_.field_get_name(fld);
                const int t = api_.type_get_type(type);
                void* fk = api_.class_from_type(type);
                if (t == kValueType && fk && !api_.class_is_enum(fk)) {
                    fprintf(f_, "%*s%s:\n", indent, "", name);
                    DumpObject(fk, at, indent + 2, depth, true);
                    continue;
                }
                const std::string tn = TypeName(type);
                const size_t tl = tn.size();
                const bool follow = t == kClass && depth > 0 &&
                    ((tl > 5 && !tn.compare(tl - 5, 5, "Model")) || (tl > 7 && !tn.compare(tl - 7, 7, "Profile")));
                if (follow) {
                    void* obj = nullptr;
                    SafeRead(at, &obj, sizeof obj);
                    fprintf(f_, "%*s%s (%s)%s\n", indent, "", name, tn.c_str(), obj ? ":" : " = null");
                    if (obj) DumpObject(api_.object_get_class(obj), static_cast<const char*>(obj), indent + 2, depth - 1, false);
                    continue;
                }
                fprintf(f_, "%*s%s = %s\n", indent, "", name, Value(type, at).c_str());
            }
        }
    }

    std::string TypeName(void* type) {
        char* s = api_.type_get_name(type);
        std::string r = s ? s : "?";
        if (s) api_.free(s);
        return r;
    }

    // A field value at p, as text, for simple types; empty otherwise.
    std::string Value(void* type, const void* p) {
        char b[96] = "";
        int t = api_.type_get_type(type);
        if (t == kValueType) {
            void* k = api_.class_from_type(type);
            if (k && api_.class_is_enum(k)) t = kI4;  // nearly every enum is int
            else return "";
        }
        union { uint8_t u8; int32_t i4; uint32_t u4; int64_t i8; float r4; double r8; void* ptr; } v = {};
        size_t n = t == kBool || t == kI1 || t == kU1 ? 1 : t == kI2 || t == kU2 || t == kChar ? 2
                 : t == kI8 || t == kU8 || t == kR8 ? 8 : t == kI4 || t == kU4 || t == kR4 ? 4
                 : (t == kClass || t == kString || t == kObject || t == kGenericInst) ? sizeof(void*) : 0;
        if (!n || !SafeRead(p, &v, n)) return "";
        switch (t) {
        case kBool: snprintf(b, sizeof b, "%s", v.u8 ? "true" : "false"); break;
        case kI1: case kU1: snprintf(b, sizeof b, "%u", v.u8); break;
        case kI4: snprintf(b, sizeof b, "%d", v.i4); break;
        case kU4: snprintf(b, sizeof b, "%u", v.u4); break;
        case kI8: case kU8: snprintf(b, sizeof b, "%lld", static_cast<long long>(v.i8)); break;
        case kR4: snprintf(b, sizeof b, "%.6g", v.r4); break;
        case kR8: snprintf(b, sizeof b, "%.9g", v.r8); break;
        case kString: {
            // Il2CppString: header (2 pointers), int32 length, UTF-16 chars.
            int32_t len = 0;
            if (v.ptr && SafeRead(static_cast<char*>(v.ptr) + 16, &len, 4) && len >= 0 && len < 40) {
                wchar_t w[41] = {};
                SafeRead(static_cast<char*>(v.ptr) + 20, w, len * 2);
                snprintf(b, sizeof b, "\"%ls\"", w);
            } else {
                snprintf(b, sizeof b, v.ptr ? "(string)" : "null");
            }
            break;
        }
        default: snprintf(b, sizeof b, v.ptr ? "%p" : "null", v.ptr); break;
        }
        return b;
    }

    void DumpInstance(void* klass, const void* obj, int indent) {
        for (void* k = klass; k; k = api_.class_get_parent(k)) {
            const char* kn = api_.class_get_name(k);
            if (kn && (!strcmp(kn, "Object") || !strcmp(kn, "MonoBehaviour") || !strcmp(kn, "Behaviour") ||
                       !strcmp(kn, "Component")))
                break;
            void* it = nullptr;
            while (void* fld = api_.class_get_fields(k, &it)) {
                void* type = api_.field_get_type(fld);
                if (api_.type_get_attrs(type) & kFieldStatic) continue;
                const std::string v = Value(type, static_cast<const char*>(obj) + api_.field_get_offset(fld));
                fprintf(f_, "%*s  = %s.%s = %s\n", indent, "", kn ? kn : "?", api_.field_get_name(fld), v.c_str());
            }
        }
    }

    void DumpClass(void* k, int indent) {
        if (indent > 8) return;
        const char* ns = api_.class_get_namespace(k);
        void* parent = api_.class_get_parent(k);
        fprintf(f_, "\n%*sclass %s%s%s : %s, size %d%s\n", indent, "", ns && *ns ? ns : "", ns && *ns ? "." : "",
                api_.class_get_name(k), parent ? api_.class_get_name(parent) : "-", api_.class_instance_size(k),
                api_.class_is_enum(k) ? " (enum)" : "");
        char* statics = static_cast<char*>(api_.class_get_static_field_data(k));
        void* it = nullptr;
        while (void* fld = api_.class_get_fields(k, &it)) {
            void* type = api_.field_get_type(fld);
            const bool isStatic = (api_.type_get_attrs(type) & kFieldStatic) != 0;
            const std::string tn = TypeName(type);
            std::string v;
            if (api_.field_is_literal(fld)) {
                int64_t c = 0;
                api_.field_static_get_value(fld, &c);
                char b[32];
                snprintf(b, sizeof b, "%lld", static_cast<long long>(static_cast<int32_t>(c)));
                v = b;
            } else if (isStatic && statics) {
                v = Value(type, statics + api_.field_get_offset(fld));
            }
            fprintf(f_, "%*s  %s0x%03zx %s %s%s%s\n", indent, "", isStatic ? "static " : "", api_.field_get_offset(fld),
                    tn.c_str(), api_.field_get_name(fld), v.empty() ? "" : " = ", v.c_str());
            // A static field holding an instance of a class: show that instance's fields.
            if (isStatic && statics && !api_.field_is_literal(fld) && api_.type_get_type(type) == kClass) {
                void* obj = nullptr;
                if (SafeRead(statics + api_.field_get_offset(fld), &obj, sizeof obj) && obj) {
                    void* ok = nullptr;
                    if (SafeRead(obj, &ok, sizeof ok) && ok) DumpInstance(ok, obj, indent + 2);
                }
            }
        }
        if (!api_.class_is_enum(k)) {
            it = nullptr;
            while (void* m = api_.class_get_methods(k, &it)) {
                uint32_t iflags = 0;
                const uint32_t flags = api_.method_get_flags(m, &iflags);
                uintptr_t ptr = 0;
                SafeRead(m, &ptr, sizeof ptr);  // MethodInfo::methodPointer
                std::string params;
                const uint32_t pc = api_.method_get_param_count(m);
                for (uint32_t p = 0; p < pc; ++p) params += (p ? ", " : "") + TypeName(api_.method_get_param(m, p));
                fprintf(f_, "%*s  %s%s(%s) rva %llx\n", indent, "", flags & 0x10 ? "static " : "", api_.method_get_name(m),
                        params.c_str(), ptr ? static_cast<unsigned long long>(ptr - base_) : 0ull);
            }
        }
        it = nullptr;
        while (void* nk = api_.class_get_nested_types(k, &it)) DumpClass(nk, indent + 2);
    }

    FILE* f_;
    Api api_;
    uintptr_t base_ = 0;
};

// Starts the probe on its own thread; `done` is set when the file is written.
inline void Start(const std::wstring& path, std::atomic<int>* state) {
    int expected = 0;
    if (!state->compare_exchange_strong(expected, 1)) return;  // 0 idle, 1 running, 2 done, 3 failed
    std::thread([path, state] {
        FILE* f = _wfopen(path.c_str(), L"w");
        if (!f) {
            *state = 3;
            return;
        }
        Probe(f).Run();
        fclose(f);
        *state = 2;
    }).detach();
}

// On the main thread: writes the live objects of these classes to path.
inline bool RunLive(const std::wstring& path, const char* const* classes, size_t n, void (*extra)(FILE*)) {
    FILE* f = _wfopen(path.c_str(), L"w");
    if (!f) return false;
    Probe(f).RunLive(classes, n, extra);
    fclose(f);
    return true;
}

}  // namespace il2cpp_probe
