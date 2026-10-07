// The only way the RPG layer touches the game. The Windows plugin implements it with memory reads/writes
// resolved from tfu_rpg.ini; tests and the simulator implement it in plain C++.
// Every call may fail (address not configured, pointer chain broken during a load screen): callers must
// treat `false` as "not available right now" and never as an error worth crashing over.
#pragma once

#include <cstdint>
#include <string>

namespace tfurpg {

enum Cap : uint32_t {
    CapHealth = 1u << 0,       // read/write current player health
    CapMaxHealth = 1u << 1,    // read/write player max health
    CapForce = 1u << 2,        // read/write current Force energy
    CapMaxForce = 1u << 3,     // read/write max Force energy
    CapForcePoints = 1u << 4,  // read the stock "Force Points" counter (our XP source)
    CapLevelName = 1u << 5,    // read the name of the loaded level
    CapTimeScale = 1u << 6,    // read/write the global time scale
    CapLoadLevel = 1u << 7,    // ask the engine to load a level by name
    CapAreaDamage = 1u << 8,   // damage enemies around the player (needs a reversed engine function)
    CapDamageScale = 1u << 9,  // scale the player's outgoing damage (needs a reversed hook)
};

class GameBridge {
public:
    virtual ~GameBridge() = default;
    virtual uint32_t caps() const = 0;

    virtual bool getHealth(float&) { return false; }
    virtual bool setHealth(float) { return false; }
    virtual bool getMaxHealth(float&) { return false; }
    virtual bool setMaxHealth(float) { return false; }
    virtual bool getForce(float&) { return false; }
    virtual bool setForce(float) { return false; }
    virtual bool getMaxForce(float&) { return false; }
    virtual bool setMaxForce(float) { return false; }
    virtual bool getForcePoints(double&) { return false; }
    virtual bool getLevelName(std::string&) { return false; }
    virtual bool getTimeScale(float&) { return false; }
    virtual bool setTimeScale(float) { return false; }
    virtual bool loadLevel(const std::string&) { return false; }
    virtual bool areaDamage(float /*radius*/, float /*damage*/) { return false; }
    virtual bool setDamageScale(float) { return false; }
};

}  // namespace tfurpg
