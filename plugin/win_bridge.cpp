#include "win_bridge.hpp"

#include <windows.h>

#include <cmath>
#include <cstdio>

namespace tfuplugin {

using namespace tfurpg;

void WinBridge::configure(const Ini& cfg, const std::function<void(const std::string&)>& log) {
    struct Item {
        const char* key;
        AddressSpec* spec;
        uint32_t cap;
    };
    Item items[] = {
        {"Health", &health_, CapHealth},          {"MaxHealth", &maxHealth_, CapMaxHealth},
        {"Force", &force_, CapForce},             {"MaxForce", &maxForce_, CapMaxForce},
        {"ForcePoints", &forcePoints_, CapForcePoints}, {"LevelName", &levelName_, CapLevelName},
        {"TimeScale", &timeScale_, CapTimeScale}, {"LoadLevel", &loadLevel_, CapLoadLevel},
    };
    caps_ = 0;
    for (auto& it : items) {
        std::string err;
        if (!parseAddressSpec(cfg.get("addresses", it.key), *it.spec, &err)) {
            log(std::string("[addresses] ") + it.key + ": " + err + " -> отключено");
            *it.spec = AddressSpec{};
            continue;
        }
        if (it.spec->empty()) {
            log(std::string("[addresses] ") + it.key + ": не задан");
            continue;
        }
        uintptr_t a = 0;
        bool ok = mem_.resolve(*it.spec, a);
        char buf[64];
        std::snprintf(buf, sizeof buf, "0x%08lX", static_cast<unsigned long>(a));
        // A broken chain at startup is normal (no level loaded yet); only a missing base is fatal.
        log(std::string("[addresses] ") + it.key + " = " + it.spec->source + (ok ? std::string(" -> ") + buf : " -> пока не разрешается"));
        caps_ |= it.cap;
    }
    std::string thisErr;
    if (!parseAddressSpec(cfg.get("addresses", "LoadLevelThis"), loadLevelThis_, &thisErr))
        log("[addresses] LoadLevelThis: " + thisErr);
    fpIsFloat_ = toLower(cfg.get("addresses", "ForcePointsType", "int32")) == "float";
    levelNameIsPointer_ = cfg.getBool("addresses", "LevelNameIsPointer", false);
    loadConv_ = toLower(cfg.get("addresses", "LoadLevelConvention", "cdecl"));
    if ((caps_ & CapLoadLevel) && loadConv_ == "thiscall" && loadLevelThis_.empty()) {
        log("[addresses] LoadLevel: thiscall требует LoadLevelThis -> отключено");
        caps_ &= ~CapLoadLevel;
    }
}

bool WinBridge::readF(const AddressSpec& s, float& v) {
    uintptr_t a = 0;
    return !s.empty() && mem_.resolve(s, a) && mem_.read(a, v) && std::isfinite(v);
}

bool WinBridge::writeF(const AddressSpec& s, float v) {
    uintptr_t a = 0;
    return !s.empty() && std::isfinite(v) && mem_.resolve(s, a) && mem_.write(a, v);
}

bool WinBridge::getForcePoints(double& v) {
    uintptr_t a = 0;
    if (forcePoints_.empty() || !mem_.resolve(forcePoints_, a)) return false;
    if (fpIsFloat_) {
        float f = 0;
        if (!mem_.read(a, f) || !std::isfinite(f)) return false;
        v = f;
    } else {
        int32_t i = 0;
        if (!mem_.read(a, i)) return false;
        v = i;
    }
    return true;
}

bool WinBridge::getLevelName(std::string& v) {
    uintptr_t a = 0;
    if (levelName_.empty() || !mem_.resolve(levelName_, a)) return false;
    if (levelNameIsPointer_) {
        uint32_t p = 0;
        if (!mem_.read(a, p)) return false;
        a = p;
    }
    if (!mem_.readCString(a, v)) return false;
    // Reject garbage read mid-load: names are short printable ASCII.
    for (char c : v)
        if (static_cast<unsigned char>(c) < 0x20 || static_cast<unsigned char>(c) > 0x7E) return false;
    return !v.empty();
}

bool WinBridge::loadLevel(const std::string& name) {
    uintptr_t fn = 0;
    if (loadLevel_.empty() || !mem_.resolve(loadLevel_, fn)) return false;
    const char* arg = name.c_str();
    // Called from the render-thread hook, i.e. the game's main thread.
    if (loadConv_ == "stdcall") {
        reinterpret_cast<void(__stdcall*)(const char*)>(fn)(arg);
    } else if (loadConv_ == "thiscall") {
        uintptr_t self = 0;
        if (!mem_.resolve(loadLevelThis_, self)) return false;
        uint32_t obj = 0;
        if (!mem_.read(self, obj) || !obj) return false;
        // __fastcall(ecx, edx, ...) is the portable way to call a __thiscall member from C++.
        reinterpret_cast<void(__fastcall*)(void*, void*, const char*)>(fn)(reinterpret_cast<void*>(obj), nullptr, arg);
    } else {
        reinterpret_cast<void(__cdecl*)(const char*)>(fn)(arg);
    }
    return true;
}

std::string WinBridge::dump() {
    std::string out;
    char buf[160];
    float f = 0;
    double d = 0;
    std::string s;
    auto line = [&](const char* name, bool ok, const char* fmt, double v) {
        if (ok)
            std::snprintf(buf, sizeof buf, fmt, name, v);
        else
            std::snprintf(buf, sizeof buf, "%s = <нет>", name);
        out += buf;
        out += '\n';
    };
    line("Health", getHealth(f), "%s = %.2f", f);
    line("MaxHealth", getMaxHealth(f), "%s = %.2f", f);
    line("Force", getForce(f), "%s = %.2f", f);
    line("MaxForce", getMaxForce(f), "%s = %.2f", f);
    line("ForcePoints", getForcePoints(d), "%s = %.0f", d);
    line("TimeScale", getTimeScale(f), "%s = %.3f", f);
    out += "LevelName = " + (getLevelName(s) ? s : std::string("<нет>")) + '\n';
    return out;
}

}  // namespace tfuplugin
