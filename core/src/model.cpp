#include "tfurpg/model.hpp"

#include "tfurpg/game_bridge.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <set>
#include <sstream>

namespace tfurpg {

// ---------------------------------------------------------------- modifiers

namespace {

struct ModField {
    const char* key;
    double Modifiers::*field;
    const char* label;  // printf format, Russian
};

const ModField kModFields[] = {
    {"maxHealthPct", &Modifiers::maxHealthPct, "+%g%% к здоровью"},
    {"maxForcePct", &Modifiers::maxForcePct, "+%g%% к энергии Силы"},
    {"damageReductionPct", &Modifiers::damageReductionPct, "-%g%% получаемого урона"},
    {"hpRegen", &Modifiers::hpRegen, "+%g здоровья/сек"},
    {"forceRegen", &Modifiers::forceRegen, "+%g Силы/сек"},
    {"xpBonusPct", &Modifiers::xpBonusPct, "+%g%% опыта"},
    {"killHealPct", &Modifiers::killHealPct, "%g%% здоровья за убийство"},
    {"killForcePct", &Modifiers::killForcePct, "%g%% Силы за убийство"},
    {"abilityCostPct", &Modifiers::abilityCostPct, "-%g%% стоимости способностей"},
    {"cooldownPct", &Modifiers::cooldownPct, "-%g%% перезарядки"},
    {"damageBonusPct", &Modifiers::damageBonusPct, "+%g%% урона"},
    {"lastStand", &Modifiers::lastStand, "Второе дыхание"},
};

std::string fmt(const char* f, double v) {
    char buf[128];
    std::snprintf(buf, sizeof buf, f, v);
    return buf;
}

bool parseDouble(const std::string& s, double& out) {
    char* end = nullptr;
    out = std::strtod(s.c_str(), &end);
    return end != s.c_str() && *end == '\0';
}

}  // namespace

Modifiers& Modifiers::operator+=(const Modifiers& o) {
    for (const auto& f : kModFields) this->*(f.field) += o.*(f.field);
    return *this;
}

Modifiers Modifiers::scaled(double k) const {
    Modifiers m = *this;
    for (const auto& f : kModFields) m.*(f.field) *= k;
    return m;
}

bool Modifiers::add(const std::string& key, double value) {
    for (const auto& f : kModFields)
        if (key == f.key) {
            this->*(f.field) += value;
            return true;
        }
    return false;
}

bool Modifiers::parse(const std::string& text, Modifiers& out, std::string* err) {
    for (const auto& part : split(text, ',')) {
        auto words = split(part, ' ');
        double v = 0;
        if (words.size() != 2 || !parseDouble(words[1], v)) {
            if (err) *err = "bad modifier '" + part + "' (expected 'key value')";
            return false;
        }
        if (!out.add(words[0], v)) {
            if (err) *err = "unknown modifier '" + words[0] + "'";
            return false;
        }
    }
    return true;
}

std::string Modifiers::describe() const {
    std::string out;
    for (const auto& f : kModFields) {
        double v = this->*(f.field);
        if (std::fabs(v) < 1e-9) continue;
        if (!out.empty()) out += ", ";
        out += std::string(f.key) == "lastStand" ? f.label : fmt(f.label, std::round(v * 100) / 100);
    }
    return out;
}

// ---------------------------------------------------------------- balance

const char* attrName(Attr a) {
    switch (a) {
        case Attr::Might: return "Мощь";
        case Attr::Vitality: return "Стойкость";
        case Attr::Force: return "Связь с Силой";
        case Attr::Agility: return "Ловкость";
        default: return "?";
    }
}

const char* attrKey(Attr a) {
    switch (a) {
        case Attr::Might: return "might";
        case Attr::Vitality: return "vitality";
        case Attr::Force: return "force";
        case Attr::Agility: return "agility";
        default: return "?";
    }
}

void Balance::load(const Ini& ini) {
    const std::string s = "balance";
    maxLevel = ini.getInt(s, "maxLevel", maxLevel);
    xpBase = ini.getNum(s, "xpBase", xpBase);
    xpGrowth = ini.getNum(s, "xpGrowth", xpGrowth);
    attrPointsPerLevel = ini.getInt(s, "attrPointsPerLevel", attrPointsPerLevel);
    skillPointsPerLevel = ini.getInt(s, "skillPointsPerLevel", skillPointsPerLevel);
    xpPerForcePoint = ini.getNum(s, "xpPerForcePoint", xpPerForcePoint);
    maxForcePointDelta = ini.getNum(s, "maxForcePointDelta", maxForcePointDelta);
    overlevelPenaltyPerLevel = ini.getNum(s, "overlevelPenaltyPerLevel", overlevelPenaltyPerLevel);
    minLocationXpFactor = ini.getNum(s, "minLocationXpFactor", minLocationXpFactor);
    underlevelBonusPerLevel = ini.getNum(s, "underlevelBonusPerLevel", underlevelBonusPerLevel);
    crystalDropChance = ini.getNum(s, "crystalDropChance", crystalDropChance);
    inventorySize = ini.getInt(s, "inventorySize", inventorySize);
    lastStandHealPct = ini.getNum(s, "lastStandHealPct", lastStandHealPct);
    lastStandThresholdPct = ini.getNum(s, "lastStandThresholdPct", lastStandThresholdPct);
    lastStandCooldown = ini.getNum(s, "lastStandCooldown", lastStandCooldown);
    for (int i = 0; i < kAttrCount; ++i) {
        perAttrPoint[i] = Modifiers{};
        Modifiers::parse(ini.get("attributes", attrKey(static_cast<Attr>(i))), perAttrPoint[i]);
    }
}

int64_t Balance::xpToNext(int level) const {
    if (level >= maxLevel) return 0;
    return static_cast<int64_t>(std::llround(xpBase * std::pow(xpGrowth, level - 1)));
}

// ---------------------------------------------------------------- effects

namespace {

struct EffectSpec {
    const char* name;
    EffectType type;
    int args;  // a, b, duration in that order; how many are mandatory
    bool hasB;
    bool hasDuration;
};

const EffectSpec kEffects[] = {
    {"heal", EffectType::Heal, 1, false, false},
    {"force", EffectType::RestoreForce, 1, false, false},
    {"regen", EffectType::Regen, 3, true, true},
    {"dr", EffectType::DamageReduce, 2, false, true},
    {"shield", EffectType::Shield, 2, false, true},
    {"timescale", EffectType::TimeScale, 2, false, true},
    {"killheal", EffectType::KillHeal, 3, true, true},
    {"xpboost", EffectType::XpBoost, 2, false, true},
    {"aoe", EffectType::AreaDamage, 2, true, false},
    {"hp2force", EffectType::HealthToForce, 2, true, false},
    {"dmgboost", EffectType::DamageBoost, 2, false, true},
};

}  // namespace

bool Effect::parse(const std::string& text, Effect& out, std::string* err) {
    auto parts = split(text, ':');
    if (parts.empty()) {
        if (err) *err = "empty effect";
        return false;
    }
    for (const auto& spec : kEffects) {
        if (parts[0] != spec.name) continue;
        if (static_cast<int>(parts.size()) - 1 != spec.args) {
            if (err) *err = "effect '" + text + "' needs " + std::to_string(spec.args) + " values";
            return false;
        }
        std::vector<double> v;
        for (size_t i = 1; i < parts.size(); ++i) {
            double d = 0;
            if (!parseDouble(parts[i], d)) {
                if (err) *err = "effect '" + text + "': '" + parts[i] + "' is not a number";
                return false;
            }
            v.push_back(d);
        }
        out = Effect{};
        out.type = spec.type;
        size_t i = 0;
        out.a = v[i++];
        if (spec.hasB) out.b = v[i++];
        if (spec.hasDuration) out.duration = v[i++];
        return true;
    }
    if (err) *err = "unknown effect '" + parts[0] + "'";
    return false;
}

uint32_t Effect::requiredCaps() const {
    switch (type) {
        case EffectType::Heal: return CapHealth | CapMaxHealth;
        case EffectType::RestoreForce: return CapForce | CapMaxForce;
        case EffectType::Regen: return (a > 0 ? CapHealth | CapMaxHealth : 0u) | (b > 0 ? CapForce | CapMaxForce : 0u);
        case EffectType::DamageReduce:
        case EffectType::Shield: return CapHealth;
        case EffectType::TimeScale: return CapTimeScale;
        case EffectType::KillHeal: return CapForcePoints | CapHealth | CapMaxHealth;
        case EffectType::XpBoost: return CapForcePoints;
        case EffectType::AreaDamage: return CapAreaDamage;
        case EffectType::HealthToForce: return CapHealth | CapMaxHealth | CapForce | CapMaxForce;
        case EffectType::DamageBoost: return CapDamageScale;
    }
    return 0;
}

uint32_t AbilityDef::requiredCaps() const {
    uint32_t c = forceCost > 0 ? (CapForce | 0u) : 0u;
    for (const auto& e : effects) c |= e.requiredCaps();
    return c;
}

// ---------------------------------------------------------------- crystals

const char* rarityName(Rarity r) {
    switch (r) {
        case Rarity::Common: return "Обычный";
        case Rarity::Rare: return "Редкий";
        case Rarity::Legendary: return "Легендарный";
    }
    return "?";
}

// base|rarity|level|slot|key=val;key=val|name   (name last: it may contain anything but newlines)
std::string Crystal::serialize() const {
    std::ostringstream o;
    o << base << '|' << static_cast<int>(rarity) << '|' << itemLevel << '|' << slot << '|';
    bool first = true;
    for (const auto& f : kModFields) {
        double v = mods.*(f.field);
        if (std::fabs(v) < 1e-9) continue;
        if (!first) o << ';';
        first = false;
        o << f.key << '=' << v;
    }
    o << '|' << name;
    return o.str();
}

bool Crystal::deserialize(const std::string& s, Crystal& out) {
    std::vector<std::string> parts;
    size_t start = 0;
    for (int i = 0; i < 5; ++i) {
        size_t bar = s.find('|', start);
        if (bar == std::string::npos) return false;
        parts.push_back(s.substr(start, bar - start));
        start = bar + 1;
    }
    parts.push_back(s.substr(start));
    out = Crystal{};
    out.base = parts[0];
    int r = std::atoi(parts[1].c_str());
    if (r < 0 || r > 2) return false;
    out.rarity = static_cast<Rarity>(r);
    out.itemLevel = std::atoi(parts[2].c_str());
    out.slot = parts[3];
    for (const auto& kv : split(parts[4], ';')) {
        size_t eq = kv.find('=');
        double v = 0;
        if (eq == std::string::npos || !parseDouble(kv.substr(eq + 1), v) || !out.mods.add(kv.substr(0, eq), v))
            return false;
    }
    out.name = parts[5];
    return !out.base.empty();
}

// ---------------------------------------------------------------- content loading

namespace {

std::string idOf(const std::string& section, const std::string& prefix) { return section.substr(prefix.size()); }

bool loadIni(const std::string& dir, const char* file, Ini& ini, std::string* err) {
    std::string e;
    if (!ini.loadFile(dir + "/" + file, &e)) {
        if (err) *err = std::string(file) + ": " + e;
        return false;
    }
    return true;
}

bool fail(std::string* err, const std::string& msg) {
    if (err) *err = msg;
    return false;
}

}  // namespace

bool Content::load(const std::string& dir, std::string* err) {
    *this = Content{};
    Ini balanceIni, skillIni, abilityIni, worldIni, questIni, crystalIni;
    if (!loadIni(dir, "balance.ini", balanceIni, err) || !loadIni(dir, "skills.ini", skillIni, err) ||
        !loadIni(dir, "abilities.ini", abilityIni, err) || !loadIni(dir, "world.ini", worldIni, err) ||
        !loadIni(dir, "quests.ini", questIni, err) || !loadIni(dir, "crystals.ini", crystalIni, err))
        return false;

    balance.load(balanceIni);
    for (int i = 0; i < kAttrCount; ++i) {
        Modifiers m;
        std::string e;
        if (!Modifiers::parse(balanceIni.get("attributes", attrKey(static_cast<Attr>(i))), m, &e))
            return fail(err, std::string("balance.ini [attributes] ") + attrKey(static_cast<Attr>(i)) + ": " + e);
    }

    trees = skillIni.getList("trees", "order");
    for (const auto& sec : skillIni.sectionsWithPrefix("skill.")) {
        SkillNode n;
        n.id = idOf(sec, "skill.");
        n.name = skillIni.get(sec, "name", n.id);
        n.tree = skillIni.get(sec, "tree");
        n.desc = skillIni.get(sec, "desc");
        n.maxRank = skillIni.getInt(sec, "maxRank", 1);
        n.reqLevel = skillIni.getInt(sec, "reqLevel", 1);
        n.cost = skillIni.getInt(sec, "cost", 1);
        n.grantsAbility = skillIni.get(sec, "grantsAbility");
        for (const auto& r : skillIni.getList(sec, "requires")) {
            size_t colon = r.find(':');
            if (colon == std::string::npos)
                n.prereqs.emplace_back(r, 1);
            else
                n.prereqs.emplace_back(trim(r.substr(0, colon)), std::atoi(r.substr(colon + 1).c_str()));
        }
        std::string e;
        if (!Modifiers::parse(skillIni.get(sec, "perRank"), n.perRank, &e))
            return fail(err, "skills.ini [" + sec + "] perRank: " + e);
        skillOrder.push_back(n.id);
        skills[n.id] = n;
    }

    for (const auto& sec : abilityIni.sectionsWithPrefix("ability.")) {
        AbilityDef a;
        a.id = idOf(sec, "ability.");
        a.name = abilityIni.get(sec, "name", a.id);
        a.desc = abilityIni.get(sec, "desc");
        a.forceCost = abilityIni.getNum(sec, "forceCost", 0);
        a.cooldown = abilityIni.getNum(sec, "cooldown", 10);
        a.reqLevel = abilityIni.getInt(sec, "reqLevel", 1);
        for (const auto& t : abilityIni.getList(sec, "effects", ';')) {
            Effect e;
            std::string msg;
            if (!Effect::parse(t, e, &msg)) return fail(err, "abilities.ini [" + sec + "]: " + msg);
            a.effects.push_back(e);
        }
        if (a.effects.empty()) return fail(err, "abilities.ini [" + sec + "]: no effects");
        abilityOrder.push_back(a.id);
        abilities[a.id] = a;
    }

    for (const auto& sec : worldIni.sectionsWithPrefix("location.")) {
        Location l;
        l.id = idOf(sec, "location.");
        l.name = worldIni.get(sec, "name", l.id);
        l.planet = worldIni.get(sec, "planet");
        l.desc = worldIni.get(sec, "desc");
        l.levelMatch = worldIni.getList(sec, "levelMatch");
        l.travelLevel = worldIni.get(sec, "travelLevel");
        l.links = worldIni.getList(sec, "links");
        l.tier = worldIni.getInt(sec, "tier", 1);
        l.reqLevel = worldIni.getInt(sec, "reqLevel", 1);
        l.reqQuest = worldIni.get(sec, "reqQuest");
        l.hub = worldIni.getBool(sec, "hub");
        l.startUnlocked = worldIni.getBool(sec, "startUnlocked");
        locationOrder.push_back(l.id);
        locations[l.id] = l;
    }
    // Links are bidirectional: mirror them so data files only need to list each route once.
    for (const auto& id : locationOrder)
        for (const auto& other : locations[id].links) {
            auto it = locations.find(other);
            if (it == locations.end()) return fail(err, "world.ini: " + id + " links to unknown " + other);
            auto& back = it->second.links;
            if (std::find(back.begin(), back.end(), id) == back.end()) back.push_back(id);
        }

    for (const auto& sec : questIni.sectionsWithPrefix("quest.")) {
        QuestDef q;
        q.id = idOf(sec, "quest.");
        q.name = questIni.get(sec, "name", q.id);
        q.desc = questIni.get(sec, "desc");
        std::string kind = questIni.get(sec, "kind", "kill");
        if (kind == "kill")
            q.kind = QuestKind::Kill;
        else if (kind == "visit")
            q.kind = QuestKind::Visit;
        else if (kind == "level")
            q.kind = QuestKind::ReachLevel;
        else
            return fail(err, "quests.ini [" + sec + "]: unknown kind " + kind);
        q.location = questIni.get(sec, "location");
        q.count = questIni.getInt(sec, "count", 1);
        q.reqLevel = questIni.getInt(sec, "reqLevel", 1);
        q.requiresQuests = questIni.getList(sec, "requires");
        q.rewardXp = static_cast<int64_t>(questIni.getNum(sec, "rewardXp", 0));
        q.rewardSkillPoints = questIni.getInt(sec, "rewardSkillPoints", 0);
        q.rewardAttrPoints = questIni.getInt(sec, "rewardAttrPoints", 0);
        q.rewardCrystal = questIni.get(sec, "rewardCrystal");
        q.repeatable = questIni.getBool(sec, "repeatable");
        questOrder.push_back(q.id);
        quests[q.id] = q;
    }

    for (const auto& sec : crystalIni.sectionsWithPrefix("crystal.")) {
        CrystalBase c;
        c.id = idOf(sec, "crystal.");
        c.name = crystalIni.get(sec, "name", c.id);
        c.slot = crystalIni.get(sec, "slot", "focus");
        c.minLevel = crystalIni.getInt(sec, "minLevel", 1);
        c.weight = crystalIni.getNum(sec, "weight", 1);
        std::string e;
        if (!Modifiers::parse(crystalIni.get(sec, "mods"), c.mods, &e))
            return fail(err, "crystals.ini [" + sec + "]: " + e);
        crystals[c.id] = c;
    }
    for (const auto& sec : crystalIni.sectionsWithPrefix("affix.")) {
        Affix a;
        a.id = idOf(sec, "affix.");
        a.name = crystalIni.get(sec, "name", a.id);
        a.key = crystalIni.get(sec, "key");
        a.min = crystalIni.getNum(sec, "min");
        a.max = crystalIni.getNum(sec, "max");
        a.weight = crystalIni.getNum(sec, "weight", 1);
        Modifiers probe;
        if (!probe.add(a.key, 1)) return fail(err, "crystals.ini [" + sec + "]: unknown key " + a.key);
        affixes.push_back(a);
    }

    return validate(err);
}

bool Content::validate(std::string* err) const {
    if (balance.maxLevel < 2 || balance.xpBase <= 0 || balance.xpGrowth < 1)
        return fail(err, "balance.ini: maxLevel >= 2, xpBase > 0 and xpGrowth >= 1 required");
    std::set<std::string> treeSet(trees.begin(), trees.end());
    for (const auto& [id, n] : skills) {
        if (!treeSet.count(n.tree)) return fail(err, "skill " + id + ": unknown tree '" + n.tree + "'");
        if (n.maxRank < 1 || n.cost < 0) return fail(err, "skill " + id + ": bad maxRank/cost");
        for (const auto& [req, rank] : n.prereqs) {
            auto it = skills.find(req);
            if (it == skills.end()) return fail(err, "skill " + id + ": requires unknown " + req);
            if (rank < 1 || rank > it->second.maxRank) return fail(err, "skill " + id + ": bad rank for " + req);
        }
        if (!n.grantsAbility.empty() && !abilities.count(n.grantsAbility))
            return fail(err, "skill " + id + ": grants unknown ability " + n.grantsAbility);
    }
    int hubs = 0, starts = 0;
    for (const auto& [id, l] : locations) {
        hubs += l.hub;
        starts += l.startUnlocked;
        if (!l.reqQuest.empty() && !quests.count(l.reqQuest))
            return fail(err, "location " + id + ": unknown reqQuest " + l.reqQuest);
    }
    if (hubs != 1) return fail(err, "world.ini: exactly one location must have hub = true");
    if (starts < 1) return fail(err, "world.ini: at least one location must have startUnlocked = true");
    for (const auto& [id, q] : quests) {
        if (!q.location.empty() && !locations.count(q.location))
            return fail(err, "quest " + id + ": unknown location " + q.location);
        if (q.kind == QuestKind::Visit && q.location.empty()) return fail(err, "quest " + id + ": visit needs location");
        for (const auto& r : q.requiresQuests)
            if (!quests.count(r)) return fail(err, "quest " + id + ": requires unknown " + r);
        if (!q.rewardCrystal.empty() && !crystals.count(q.rewardCrystal))
            return fail(err, "quest " + id + ": unknown rewardCrystal " + q.rewardCrystal);
    }
    for (const auto& [id, c] : crystals)
        if (c.slot != "color" && c.slot != "focus") return fail(err, "crystal " + id + ": slot must be color or focus");
    return true;
}

const Location* Content::locationForLevel(const std::string& levelName) const {
    const std::string lv = toLower(levelName);
    const Location* best = nullptr;
    size_t bestLen = 0;
    // Longest match wins so "kashyyyk_imperial" beats "kashyyyk".
    for (const auto& id : locationOrder) {
        const Location& l = locations.at(id);
        for (const auto& m : l.levelMatch) {
            std::string ml = toLower(m);
            if (ml.size() > bestLen && lv.find(ml) != std::string::npos) {
                best = &l;
                bestLen = ml.size();
            }
        }
    }
    return best;
}

}  // namespace tfurpg
