// GameBridge backed by the addresses configured in tfu_rpg.ini [addresses].
#pragma once

#include <functional>
#include <string>

#include "memory.hpp"
#include "tfurpg/game_bridge.hpp"
#include "tfurpg/ini.hpp"

namespace tfuplugin {

class WinBridge : public tfurpg::GameBridge {
public:
    // Parses [addresses]; every malformed entry is reported through `log` and left disabled.
    void configure(const tfurpg::Ini& cfg, const std::function<void(const std::string&)>& log);
    uint32_t caps() const override { return caps_; }
    // One line per configured address with its current value, for verifying a CE table in game.
    std::string dump();

    bool getHealth(float& v) override { return readF(health_, v); }
    bool setHealth(float v) override { return writeF(health_, v); }
    bool getMaxHealth(float& v) override { return readF(maxHealth_, v); }
    bool setMaxHealth(float v) override { return writeF(maxHealth_, v); }
    bool getForce(float& v) override { return readF(force_, v); }
    bool setForce(float v) override { return writeF(force_, v); }
    bool getMaxForce(float& v) override { return readF(maxForce_, v); }
    bool setMaxForce(float v) override { return writeF(maxForce_, v); }
    bool getForcePoints(double& v) override;
    bool getLevelName(std::string& v) override;
    bool getTimeScale(float& v) override { return readF(timeScale_, v); }
    bool setTimeScale(float v) override { return writeF(timeScale_, v); }
    bool loadLevel(const std::string& name) override;

private:
    bool readF(const AddressSpec& s, float& v);
    bool writeF(const AddressSpec& s, float v);

    Memory mem_;
    uint32_t caps_ = 0;
    AddressSpec health_, maxHealth_, force_, maxForce_, forcePoints_, levelName_, timeScale_, loadLevel_, loadLevelThis_;
    bool fpIsFloat_ = false;
    bool levelNameIsPointer_ = false;
    std::string loadConv_ = "cdecl";
};

}  // namespace tfuplugin
