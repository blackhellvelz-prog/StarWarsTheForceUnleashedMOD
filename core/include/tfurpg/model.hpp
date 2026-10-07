// Content definitions loaded from data/*.ini: balance, attributes, skill trees, abilities, locations,
// quests and lightsaber crystals. Nothing here talks to the game.
#pragma once

#include <array>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "tfurpg/ini.hpp"

namespace tfurpg {

// ---------------------------------------------------------------- modifiers

// Additive bonuses from attributes, skills and crystals. Percentages are whole numbers (10 = +10%).
struct Modifiers {
    double maxHealthPct = 0;
    double maxForcePct = 0;
    double damageReductionPct = 0;
    double hpRegen = 0;        // health per second
    double forceRegen = 0;     // Force energy per second
    double xpBonusPct = 0;
    double killHealPct = 0;    // % of max health restored per kill
    double killForcePct = 0;   // % of max Force restored per kill
    double abilityCostPct = 0; // reduction of custom ability Force cost
    double cooldownPct = 0;    // reduction of custom ability cooldowns
    double damageBonusPct = 0; // needs CapDamageScale
    double lastStand = 0;      // > 0 enables "second wind"

    Modifiers& operator+=(const Modifiers& o);
    Modifiers scaled(double k) const;
    // Sets one field by its ini name ("maxHealthPct"). Returns false for an unknown key.
    bool add(const std::string& key, double value);
    // "maxHealthPct 4, hpRegen 0.5"
    static bool parse(const std::string& text, Modifiers& out, std::string* err = nullptr);
    std::string describe() const;  // Russian, for the UI
};

// ---------------------------------------------------------------- balance

enum class Attr { Might = 0, Vitality, Force, Agility, Count };
constexpr int kAttrCount = static_cast<int>(Attr::Count);
const char* attrName(Attr a);  // Russian display name
const char* attrKey(Attr a);   // ini key

struct Balance {
    int maxLevel = 50;
    double xpBase = 120;      // xp for level 1 -> 2
    double xpGrowth = 1.18;   // geometric growth per level
    int attrPointsPerLevel = 3;
    int skillPointsPerLevel = 1;
    double xpPerForcePoint = 1.0;
    double maxForcePointDelta = 4000;  // bigger jumps are save loads / menus, not kills
    double overlevelPenaltyPerLevel = 0.12;
    double minLocationXpFactor = 0.15;
    double underlevelBonusPerLevel = 0.05;
    double crystalDropChance = 0.04;
    int inventorySize = 24;
    double lastStandHealPct = 50;
    double lastStandThresholdPct = 12;
    double lastStandCooldown = 120;
    std::array<Modifiers, kAttrCount> perAttrPoint{};

    void load(const Ini& ini);
    int64_t xpToNext(int level) const;
};

// ---------------------------------------------------------------- skills

struct SkillNode {
    std::string id, name, tree, desc;
    int maxRank = 1;
    int reqLevel = 1;
    int cost = 1;  // skill points per rank
    std::vector<std::pair<std::string, int>> prereqs;  // node id, min rank
    Modifiers perRank;
    std::string grantsAbility;  // ability id unlocked at rank 1
};

// ---------------------------------------------------------------- abilities

enum class EffectType {
    Heal,           // heal:pctMaxHp
    RestoreForce,   // force:pctMaxForce
    Regen,          // regen:hpPerSec:forcePerSec:duration
    DamageReduce,   // dr:pct:duration
    Shield,         // shield:amount:duration
    TimeScale,      // timescale:scale:duration
    KillHeal,       // killheal:hpPct:forcePct:duration
    XpBoost,        // xpboost:pct:duration
    AreaDamage,     // aoe:radius:damage                      (CapAreaDamage)
    HealthToForce,  // hp2force:hpPctSpent:forcePctGained
    DamageBoost,    // dmgboost:pct:duration                  (CapDamageScale)
};

struct Effect {
    EffectType type = EffectType::Heal;
    double a = 0, b = 0, duration = 0;
    static bool parse(const std::string& text, Effect& out, std::string* err = nullptr);
    uint32_t requiredCaps() const;
};

struct AbilityDef {
    std::string id, name, desc;
    double forceCost = 0;
    double cooldown = 10;
    int reqLevel = 1;
    std::vector<Effect> effects;
    uint32_t requiredCaps() const;
};

// ---------------------------------------------------------------- world

struct Location {
    std::string id, name, planet, desc;
    std::vector<std::string> levelMatch;  // substrings of the engine's level name that mean "you are here"
    std::string travelLevel;              // level name handed to GameBridge::loadLevel
    std::vector<std::string> links;       // travel routes (bidirectional)
    int tier = 1;                         // recommended character level
    int reqLevel = 1;
    std::string reqQuest;
    bool hub = false;
    bool startUnlocked = false;
};

// ---------------------------------------------------------------- quests

enum class QuestKind { Kill, Visit, ReachLevel };

struct QuestDef {
    std::string id, name, desc;
    QuestKind kind = QuestKind::Kill;
    std::string location;  // Kill: where kills count (empty = anywhere); Visit: target
    int count = 1;         // Kill: kills; ReachLevel: level
    int reqLevel = 1;
    std::vector<std::string> requiresQuests;
    int64_t rewardXp = 0;
    int rewardSkillPoints = 0;
    int rewardAttrPoints = 0;
    std::string rewardCrystal;  // crystal base id, generated at the quest's tier
    bool repeatable = false;
};

// ---------------------------------------------------------------- crystals

enum class Rarity { Common = 0, Rare, Legendary };
const char* rarityName(Rarity r);

struct CrystalBase {
    std::string id, name, slot;  // slot: "color" or "focus"
    Modifiers mods;
    int minLevel = 1;
    double weight = 1;
};

struct Affix {
    std::string id, name, key;
    double min = 0, max = 0;
    double weight = 1;
};

struct Crystal {
    std::string base;
    std::string name;
    std::string slot;
    Rarity rarity = Rarity::Common;
    int itemLevel = 1;
    Modifiers mods;

    std::string serialize() const;
    static bool deserialize(const std::string& s, Crystal& out);
};

// ---------------------------------------------------------------- all content

struct Content {
    Balance balance;
    std::map<std::string, SkillNode> skills;
    std::vector<std::string> skillOrder;
    std::vector<std::string> trees;
    std::map<std::string, AbilityDef> abilities;
    std::vector<std::string> abilityOrder;
    std::map<std::string, Location> locations;
    std::vector<std::string> locationOrder;
    std::map<std::string, QuestDef> quests;
    std::vector<std::string> questOrder;
    std::map<std::string, CrystalBase> crystals;
    std::vector<Affix> affixes;

    // Loads balance.ini, skills.ini, abilities.ini, world.ini, quests.ini, crystals.ini from `dir`.
    // Cross-references (skill -> ability, quest -> location, links) are validated.
    bool load(const std::string& dir, std::string* err);
    bool validate(std::string* err) const;

    const Location* locationForLevel(const std::string& levelName) const;
};

}  // namespace tfurpg
