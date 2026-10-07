// The running RPG layer: character state plus the per-frame logic that turns game memory changes into
// XP, quest progress and loot, and turns abilities/passives into memory writes through a GameBridge.
#pragma once

#include <array>
#include <cstdint>
#include <deque>
#include <map>
#include <random>
#include <set>
#include <string>
#include <vector>

#include "tfurpg/game_bridge.hpp"
#include "tfurpg/model.hpp"

namespace tfurpg {

constexpr int kAbilitySlots = 4;

struct Character {
    int level = 1;
    int64_t xp = 0;  // progress inside the current level
    int64_t totalXp = 0;
    int attrPoints = 0;
    int skillPoints = 0;
    std::array<int, kAttrCount> attrs{};
    std::map<std::string, int> skillRanks;
    std::set<std::string> unlockedAbilities;
    std::array<std::string, kAbilitySlots> slots;
    std::set<std::string> unlockedLocations;
    std::set<std::string> visitedLocations;
    std::map<std::string, int> questProgress;
    std::set<std::string> questsDone;
    std::vector<Crystal> inventory;
    Crystal equippedColor, equippedFocus;  // empty base = nothing equipped
    int64_t kills = 0;
    std::string currentLocation;
};

struct Buff {
    EffectType type;
    double a = 0, b = 0;
    double remaining = 0;
    std::string source;
};

class Session {
public:
    Session(const Content& content, GameBridge& bridge, uint32_t seed = 0x5EED);

    void newCharacter();
    bool load(const Ini& save, std::string* err = nullptr);
    Ini save() const;

    // Call every frame (or at least ~10x per second). dt in real seconds.
    void tick(double dt);

    // Player actions. On failure `reason` explains why in Russian.
    bool useAbility(int slot, std::string* reason = nullptr);
    bool spendAttribute(Attr a, std::string* reason = nullptr);
    bool learnSkill(const std::string& id, std::string* reason = nullptr);
    bool canLearnSkill(const std::string& id, std::string* reason = nullptr) const;
    bool bindAbility(int slot, const std::string& abilityId, std::string* reason = nullptr);
    bool equipCrystal(size_t inventoryIndex, std::string* reason = nullptr);
    bool travel(const std::string& locationId, std::string* reason = nullptr);
    bool respec(std::string* reason = nullptr);

    // Queries for UI.
    const Character& character() const { return ch_; }
    const Content& content() const { return content_; }
    Modifiers totalModifiers() const;
    int64_t xpToNext() const { return content_.balance.xpToNext(ch_.level); }
    double cooldownLeft(const std::string& abilityId) const;
    double effectiveCooldown(const AbilityDef& a) const;
    double effectiveCost(const AbilityDef& a) const;
    bool abilitySupported(const AbilityDef& a) const;
    bool locationUnlocked(const std::string& id) const;
    std::vector<std::string> travelRoute(const std::string& from, const std::string& to) const;
    bool questAvailable(const QuestDef& q) const;
    const std::vector<Buff>& buffs() const { return buffs_; }
    double shieldLeft() const;

    // Messages for the HUD/log, newest last. UI pops them.
    std::deque<std::string>& messages() { return messages_; }

    // Exposed for the simulator and tests: what one kill worth `forcePoints` stock points does.
    void onKill(double forcePoints);
    void grantXp(int64_t amount, const std::string& why);
    Crystal rollCrystal(int itemLevel, const std::string& forcedBase = "");

private:
    void post(const std::string& msg);
    void pollForcePoints();
    void pollLocation();
    void applyMaxStats();
    void protectHealth(double dt);
    void applyRegen(double dt);
    void updateBuffs(double dt);
    void applyEffect(const Effect& e, const std::string& source);
    void questsOnKill();
    void questsOnVisit(const std::string& loc);
    void questsOnLevel();
    void completeQuest(const QuestDef& q);
    void refreshAbilityUnlocks();
    double locationXpFactor() const;
    double buffSum(EffectType t, bool useB = false) const;
    void addCrystal(const Crystal& c);
    void healPct(double pct);
    void forcePct(double pct);
    void announceUnlocks(bool silent);

    const Content& content_;
    GameBridge& bridge_;
    Character ch_;
    std::mt19937 rng_;
    std::deque<std::string> messages_;
    std::vector<Buff> buffs_;
    std::map<std::string, double> cooldowns_;

    // Memory-tracking state. "base" values are what the game itself set; "written" is what we last wrote.
    double lastForcePoints_ = -1;
    bool fpPrimed_ = false;
    float baseMaxHealth_ = -1, writtenMaxHealth_ = -1;
    float baseMaxForce_ = -1, writtenMaxForce_ = -1;
    float lastHealth_ = -1;
    float normalTimeScale_ = 1.0f;
    bool timeScaleActive_ = false;
    double lastStandCooldown_ = 0;
    double regenHpAcc_ = 0, regenFpAcc_ = 0;
    std::string lastLevelName_;
    std::set<std::string> announced_;  // unlocked locations already reported to the player
};

}  // namespace tfurpg
