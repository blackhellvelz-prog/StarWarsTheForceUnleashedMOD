// In-memory stand-in for the game process, shared by the tests and the simulator.
#pragma once

#include <string>
#include <vector>

#include "tfurpg/game_bridge.hpp"

namespace tfurpg {

class FakeGame : public GameBridge {
public:
    uint32_t capMask = CapHealth | CapMaxHealth | CapForce | CapMaxForce | CapForcePoints | CapLevelName |
                       CapTimeScale | CapLoadLevel;
    float health = 500, maxHealth = 500, force = 200, maxForce = 200;
    double forcePoints = 0;
    std::string level = "empirical_01";
    float timeScale = 1.0f, damageScale = 1.0f;
    std::vector<std::string> loadRequests;
    int areaDamageCalls = 0;

    uint32_t caps() const override { return capMask; }
    bool getHealth(float& v) override { return has(CapHealth) && (v = health, true); }
    bool setHealth(float v) override { return has(CapHealth) && (health = v, true); }
    bool getMaxHealth(float& v) override { return has(CapMaxHealth) && (v = maxHealth, true); }
    bool setMaxHealth(float v) override { return has(CapMaxHealth) && (maxHealth = v, true); }
    bool getForce(float& v) override { return has(CapForce) && (v = force, true); }
    bool setForce(float v) override { return has(CapForce) && (force = v, true); }
    bool getMaxForce(float& v) override { return has(CapMaxForce) && (v = maxForce, true); }
    bool setMaxForce(float v) override { return has(CapMaxForce) && (maxForce = v, true); }
    bool getForcePoints(double& v) override { return has(CapForcePoints) && (v = forcePoints, true); }
    bool getLevelName(std::string& v) override { return has(CapLevelName) && (v = level, true); }
    bool getTimeScale(float& v) override { return has(CapTimeScale) && (v = timeScale, true); }
    bool setTimeScale(float v) override { return has(CapTimeScale) && (timeScale = v, true); }
    bool loadLevel(const std::string& n) override {
        if (!has(CapLoadLevel)) return false;
        loadRequests.push_back(n);
        level = n;
        return true;
    }
    bool areaDamage(float, float) override { return has(CapAreaDamage) && (++areaDamageCalls, true); }
    bool setDamageScale(float v) override { return has(CapDamageScale) && (damageScale = v, true); }

private:
    bool has(uint32_t c) const { return (capMask & c) == c; }
};

}  // namespace tfurpg
