#include "memory.hpp"

#include <windows.h>

#include <cstdlib>

#include "tfurpg/ini.hpp"

namespace tfuplugin {

using tfurpg::split;
using tfurpg::trim;

namespace {

bool parseInt(const std::string& s, long long& out) {
    std::string t = trim(s);
    bool neg = false;
    if (!t.empty() && (t[0] == '-' || t[0] == '+')) {
        neg = t[0] == '-';
        t = t.substr(1);
    }
    if (t.empty()) return false;
    char* end = nullptr;
    // Cheat Engine shows offsets in hex without a prefix, so hex is the default.
    out = std::strtoll(t.c_str(), &end, 16);
    if (*end != '\0') return false;
    if (neg) out = -out;
    return true;
}

}  // namespace

bool parseAddressSpec(const std::string& text, AddressSpec& out, std::string* err) {
    out = AddressSpec{};
    out.source = trim(text);
    if (out.source.empty()) return true;  // feature disabled
    auto parts = split(out.source, ',');
    std::string base = parts[0];
    for (size_t i = 1; i < parts.size(); ++i) {
        long long v = 0;
        if (!parseInt(parts[i], v)) {
            if (err) *err = "bad offset '" + parts[i] + "'";
            return false;
        }
        out.chain.push_back(static_cast<int32_t>(v));
    }
    if (base.compare(0, 4, "aob:") == 0) {
        out.kind = AddressSpec::Kind::Aob;
        std::string pat = base.substr(4);
        size_t at = pat.find('@');
        if (at != std::string::npos) {
            out.aobOperand = std::atoi(pat.substr(at + 1).c_str());
            pat = pat.substr(0, at);
        }
        for (const auto& byte : split(pat, ' ')) {
            if (byte == "??" || byte == "?") {
                out.pattern.push_back(-1);
                continue;
            }
            char* end = nullptr;
            long v = std::strtol(byte.c_str(), &end, 16);
            if (*end != '\0' || v < 0 || v > 255) {
                if (err) *err = "bad pattern byte '" + byte + "'";
                return false;
            }
            out.pattern.push_back(static_cast<int>(v));
        }
        if (out.pattern.size() < 4) {
            if (err) *err = "pattern too short";
            return false;
        }
        return true;
    }
    size_t plus = base.find('+');
    if (plus != std::string::npos) {
        out.kind = AddressSpec::Kind::ModuleOffset;
        out.module = trim(base.substr(0, plus));
        long long v = 0;
        std::string off = trim(base.substr(plus + 1));
        if (off.compare(0, 2, "0x") == 0 || off.compare(0, 2, "0X") == 0) off = off.substr(2);
        if (!parseInt(off, v)) {
            if (err) *err = "bad module offset '" + off + "'";
            return false;
        }
        out.offset = static_cast<uintptr_t>(v);
        return true;
    }
    long long v = 0;
    std::string abs = base;
    if (abs.compare(0, 2, "0x") == 0 || abs.compare(0, 2, "0X") == 0) abs = abs.substr(2);
    if (!parseInt(abs, v)) {
        if (err) *err = "bad address '" + base + "'";
        return false;
    }
    out.kind = AddressSpec::Kind::Absolute;
    out.offset = static_cast<uintptr_t>(v);
    return true;
}

bool Memory::readBytes(uintptr_t addr, void* dst, size_t n) const {
    if (addr < 0x10000) return false;
    SIZE_T got = 0;
    // ReadProcessMemory on our own process fails cleanly on unmapped pages instead of faulting the game.
    return ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<LPCVOID>(addr), dst, n, &got) && got == n;
}

bool Memory::writeBytes(uintptr_t addr, const void* src, size_t n) const {
    if (addr < 0x10000) return false;
    SIZE_T put = 0;
    return WriteProcessMemory(GetCurrentProcess(), reinterpret_cast<LPVOID>(addr), src, n, &put) && put == n;
}

bool Memory::readCString(uintptr_t addr, std::string& out, size_t max) const {
    out.clear();
    char buf[16];
    while (out.size() < max) {
        if (!readBytes(addr + out.size(), buf, sizeof buf)) {
            // Near a page end: fall back to byte reads.
            char c;
            if (!readBytes(addr + out.size(), &c, 1)) return !out.empty();
            if (!c) return true;
            out.push_back(c);
            continue;
        }
        for (char c : buf) {
            if (!c) return true;
            out.push_back(c);
        }
    }
    return true;
}

bool Memory::resolveBase(const AddressSpec& spec, uintptr_t& out) {
    for (const auto& c : cache_)
        if (c.key == spec.source) {
            out = c.base;
            return c.ok;
        }
    bool ok = false;
    uintptr_t base = 0;
    switch (spec.kind) {
        case AddressSpec::Kind::None: break;
        case AddressSpec::Kind::Absolute:
            base = spec.offset;
            ok = true;
            break;
        case AddressSpec::Kind::ModuleOffset: {
            HMODULE m = GetModuleHandleA(spec.module.c_str());
            if (m) {
                base = reinterpret_cast<uintptr_t>(m) + spec.offset;
                ok = true;
            }
            break;
        }
        case AddressSpec::Kind::Aob: {
            auto* mod = reinterpret_cast<uint8_t*>(GetModuleHandleA(nullptr));
            auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(mod);
            auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(mod + dos->e_lfanew);
            auto* sec = IMAGE_FIRST_SECTION(nt);
            const size_t n = spec.pattern.size();
            for (int s = 0; s < nt->FileHeader.NumberOfSections && !ok; ++s, ++sec) {
                if (!(sec->Characteristics & IMAGE_SCN_MEM_EXECUTE)) continue;
                const uint8_t* start = mod + sec->VirtualAddress;
                size_t size = sec->Misc.VirtualSize;
                for (size_t i = 0; i + n <= size; ++i) {
                    size_t j = 0;
                    while (j < n && (spec.pattern[j] < 0 || start[i + j] == spec.pattern[j])) ++j;
                    if (j != n) continue;
                    uintptr_t match = reinterpret_cast<uintptr_t>(start + i);
                    if (spec.aobOperand < 0) {
                        base = match;
                        ok = true;
                    } else {
                        uint32_t v = 0;
                        ok = read(match + spec.aobOperand, v);
                        base = v;
                    }
                    break;
                }
            }
            break;
        }
    }
    cache_.push_back({spec.source, base, ok});
    out = base;
    return ok;
}

bool Memory::resolve(const AddressSpec& spec, uintptr_t& out) {
    uintptr_t p = 0;
    if (!resolveBase(spec, p)) return false;
    for (int32_t off : spec.chain) {
        uint32_t next = 0;
        if (!read(p, next) || next == 0) return false;
        p = static_cast<uintptr_t>(next) + static_cast<uintptr_t>(static_cast<intptr_t>(off));
    }
    out = p;
    return true;
}

}  // namespace tfuplugin
