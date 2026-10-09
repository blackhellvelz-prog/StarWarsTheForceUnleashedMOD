// Address specs from tfu_rpg.ini and crash-safe access to the game's memory.
//
// Spec grammar (Cheat Engine pointer notation):
//   <base>[, off1, off2, ...]
//   base = module+0xOFFSET        e.g. SWTFU.exe+0x00A1B2C4   (static address inside a module)
//        | 0xABSOLUTE             only for non-relocated images; prefer module+offset
//        | aob:<bytes>@<k>        scan the main module for a byte pattern (?? = wildcard); the 4 bytes at
//                                 match+k are an absolute address embedded in the instruction
//        | aob:<bytes>            (functions) the match itself is the address
//   Each offset dereferences: p = [p] + off (exactly like a CE pointer chain).
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace tfuplugin {

struct AddressSpec {
    enum class Kind { None, ModuleOffset, Absolute, Aob };
    Kind kind = Kind::None;
    std::string module;
    uintptr_t offset = 0;
    std::vector<int> pattern;  // -1 = wildcard
    int aobOperand = -1;       // -1: the match is the address
    std::vector<int32_t> chain;
    std::string source;        // the original text, for logs

    bool empty() const { return kind == Kind::None; }
};

bool parseAddressSpec(const std::string& text, AddressSpec& out, std::string* err);

class Memory {
public:
    // Resolves the base once (module lookup / AOB scans are slow) and the pointer chain on every call.
    bool resolve(const AddressSpec& spec, uintptr_t& out);
    bool readBytes(uintptr_t addr, void* dst, size_t n) const;
    bool writeBytes(uintptr_t addr, const void* src, size_t n) const;
    template <typename T>
    bool read(uintptr_t addr, T& v) const { return readBytes(addr, &v, sizeof v); }
    template <typename T>
    bool write(uintptr_t addr, const T& v) const { return writeBytes(addr, &v, sizeof v); }
    bool readCString(uintptr_t addr, std::string& out, size_t max = 128) const;

private:
    bool resolveBase(const AddressSpec& spec, uintptr_t& out);
    struct Cached {
        std::string key;
        uintptr_t base;
        bool ok;
    };
    std::vector<Cached> cache_;
};

}  // namespace tfuplugin
