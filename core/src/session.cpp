#include "tfurpg/session.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <queue>

namespace tfurpg {

namespace {

std::string fmtNum(double v, int decimals = 0) {
    char buf[64];
    std::snprintf(buf, sizeof buf, "%.*f", decimals, v);
    return buf;
}

bool fail(std::string* reason, const std::string& msg) {
    if (reason) *reason = msg;
    return false;
}

std::string joinSet(const std::set<std::string>& s) {
    std::string out;
    for (const auto& v : s) out += (out.empty() ? "" : ",") + v;
    return out;
}

}  // namespace

Session::Session(const Content& content, GameBridge& bridge, uint32_t seed)
    : content_(content), bridge_(bridge), rng_(seed) {
    newCharacter();
}

// ---------------------------------------------------------------- lifecycle

void Session::newCharacter() {
    ch_ = Character{};
    ch_.attrPoints = content_.balance.attrPointsPerLevel;
    ch_.skillPoints = content_.balance.skillPointsPerLevel;
    for (const auto& id : content_.locationOrder)
        if (content_.locations.at(id).startUnlocked) ch_.unlockedLocations.insert(id);
    buffs_.clear();
    cooldowns_.clear();
    announceUnlocks(/*silent=*/true);
}

Ini Session::save() const {
    Ini s;
    s.set("character", "version", "1");
    s.set("character", "level", std::to_string(ch_.level));
    s.set("character", "xp", std::to_string(ch_.xp));
    s.set("character", "totalXp", std::to_string(ch_.totalXp));
    s.set("character", "attrPoints", std::to_string(ch_.attrPoints));
    s.set("character", "skillPoints", std::to_string(ch_.skillPoints));
    s.set("character", "kills", std::to_string(ch_.kills));
    s.set("character", "location", ch_.currentLocation);
    for (int i = 0; i < kAttrCount; ++i) s.set("attributes", attrKey(static_cast<Attr>(i)), std::to_string(ch_.attrs[i]));
    for (const auto& [id, r] : ch_.skillRanks) s.set("skills", id, std::to_string(r));
    for (int i = 0; i < kAbilitySlots; ++i) s.set("slots", "slot" + std::to_string(i + 1), ch_.slots[i]);
    s.set("locations", "unlocked", joinSet(ch_.unlockedLocations));
    s.set("locations", "visited", joinSet(ch_.visitedLocations));
    s.set("quests", "done", joinSet(ch_.questsDone));
    for (const auto& [id, p] : ch_.questProgress) s.set("questProgress", id, std::to_string(p));
    s.set("inventory", "count", std::to_string(ch_.inventory.size()));
    for (size_t i = 0; i < ch_.inventory.size(); ++i) s.set("inventory", "item" + std::to_string(i), ch_.inventory[i].serialize());
    s.set("equipped", "color", ch_.equippedColor.base.empty() ? "" : ch_.equippedColor.serialize());
    s.set("equipped", "focus", ch_.equippedFocus.base.empty() ? "" : ch_.equippedFocus.serialize());
    return s;
}

bool Session::load(const Ini& s, std::string* err) {
    if (s.get("character", "version") != "1") return fail(err, "unknown save version");
    newCharacter();
    const auto& b = content_.balance;
    ch_.level = std::clamp(s.getInt("character", "level", 1), 1, b.maxLevel);
    ch_.xp = static_cast<int64_t>(s.getNum("character", "xp", 0));
    ch_.totalXp = static_cast<int64_t>(s.getNum("character", "totalXp", 0));
    ch_.attrPoints = s.getInt("character", "attrPoints", 0);
    ch_.skillPoints = s.getInt("character", "skillPoints", 0);
    ch_.kills = static_cast<int64_t>(s.getNum("character", "kills", 0));
    ch_.currentLocation = s.get("character", "location");
    if (!content_.locations.count(ch_.currentLocation)) ch_.currentLocation.clear();
    for (int i = 0; i < kAttrCount; ++i) ch_.attrs[i] = std::max(0, s.getInt("attributes", attrKey(static_cast<Attr>(i)), 0));
    // Unknown ids are dropped (content changed between versions) and their points refunded.
    for (const auto& [id, val] : s.entries("skills")) {
        int r = std::atoi(val.c_str());
        auto it = content_.skills.find(id);
        if (it == content_.skills.end()) continue;
        int kept = std::clamp(r, 0, it->second.maxRank);
        if (kept > 0) ch_.skillRanks[id] = kept;
        ch_.skillPoints += (r - kept) * it->second.cost;
    }
    refreshAbilityUnlocks();
    for (int i = 0; i < kAbilitySlots; ++i) {
        std::string id = s.get("slots", "slot" + std::to_string(i + 1));
        if (ch_.unlockedAbilities.count(id)) ch_.slots[i] = id;
    }
    for (const auto& id : s.getList("locations", "unlocked"))
        if (content_.locations.count(id)) ch_.unlockedLocations.insert(id);
    for (const auto& id : s.getList("locations", "visited"))
        if (content_.locations.count(id)) ch_.visitedLocations.insert(id);
    for (const auto& id : s.getList("quests", "done"))
        if (content_.quests.count(id)) ch_.questsDone.insert(id);
    for (const auto& [id, val] : s.entries("questProgress"))
        if (content_.quests.count(id)) ch_.questProgress[id] = std::atoi(val.c_str());
    int count = s.getInt("inventory", "count", 0);
    for (int i = 0; i < count; ++i) {
        Crystal c;
        if (Crystal::deserialize(s.get("inventory", "item" + std::to_string(i)), c)) ch_.inventory.push_back(c);
    }
    Crystal c;
    if (Crystal::deserialize(s.get("equipped", "color"), c)) ch_.equippedColor = c;
    if (Crystal::deserialize(s.get("equipped", "focus"), c)) ch_.equippedFocus = c;
    announceUnlocks(/*silent=*/true);
    return true;
}

// ---------------------------------------------------------------- per frame

void Session::tick(double dt) {
    for (auto& [id, cd] : cooldowns_) cd = std::max(0.0, cd - dt);
    lastStandCooldown_ = std::max(0.0, lastStandCooldown_ - dt);
    pollLocation();
    pollForcePoints();
    applyMaxStats();
    protectHealth(dt);
    applyRegen(dt);
    updateBuffs(dt);
}

void Session::pollLocation() {
    if (!(bridge_.caps() & CapLevelName)) return;
    std::string name;
    if (!bridge_.getLevelName(name) || name.empty() || name == lastLevelName_) return;
    lastLevelName_ = name;
    // A level load reloads the player: start the counters over instead of reading the jump as kills/damage.
    fpPrimed_ = false;
    lastHealth_ = -1;
    const Location* loc = content_.locationForLevel(name);
    if (!loc || loc->id == ch_.currentLocation) return;
    ch_.currentLocation = loc->id;
    ch_.unlockedLocations.insert(loc->id);
    if (ch_.visitedLocations.insert(loc->id).second)
        post("Новая локация: " + loc->name + " (рекомендуемый уровень " + std::to_string(loc->tier) + ")");
    else
        post("Локация: " + loc->name);
    questsOnVisit(loc->id);
    announceUnlocks(false);
}

void Session::pollForcePoints() {
    if (!(bridge_.caps() & CapForcePoints)) return;
    double fp = 0;
    if (!bridge_.getForcePoints(fp)) return;
    if (!fpPrimed_) {
        lastForcePoints_ = fp;
        fpPrimed_ = true;
        return;
    }
    double delta = fp - lastForcePoints_;
    lastForcePoints_ = fp;
    // Negative: points were spent on stock upgrades. Huge: a save was loaded. Neither is a kill.
    if (delta <= 0 || delta > content_.balance.maxForcePointDelta) return;
    onKill(delta);
}

void Session::applyMaxStats() {
    const Modifiers m = totalModifiers();
    const uint32_t caps = bridge_.caps();
    float cur = 0;
    if ((caps & CapMaxHealth) && bridge_.getMaxHealth(cur) && cur > 0) {
        // If the value isn't the one we wrote, the game changed it (stock upgrade, new level): new base.
        if (writtenMaxHealth_ < 0 || std::fabs(cur - writtenMaxHealth_) > 0.5f) baseMaxHealth_ = cur;
        float target = baseMaxHealth_ * static_cast<float>(1.0 + m.maxHealthPct / 100.0);
        if (std::fabs(target - cur) > 0.5f) bridge_.setMaxHealth(target);
        writtenMaxHealth_ = target;
    }
    if ((caps & CapMaxForce) && bridge_.getMaxForce(cur) && cur > 0) {
        if (writtenMaxForce_ < 0 || std::fabs(cur - writtenMaxForce_) > 0.5f) baseMaxForce_ = cur;
        float target = baseMaxForce_ * static_cast<float>(1.0 + m.maxForcePct / 100.0);
        if (std::fabs(target - cur) > 0.5f) bridge_.setMaxForce(target);
        writtenMaxForce_ = target;
    }
}

void Session::protectHealth(double) {
    if (!(bridge_.caps() & CapHealth)) return;
    float h = 0;
    if (!bridge_.getHealth(h)) return;
    if (lastHealth_ < 0) {
        lastHealth_ = h;
        return;
    }
    float before = h;
    if (h < lastHealth_ && h > 0) {
        double dmg = lastHealth_ - h;
        double refund = 0;
        for (auto& b : buffs_) {
            if (b.type != EffectType::Shield || b.a <= 0) continue;
            double absorb = std::min(b.a, dmg - refund);
            b.a -= absorb;
            refund += absorb;
        }
        double dr = std::min(75.0, totalModifiers().damageReductionPct + buffSum(EffectType::DamageReduce));
        if (dr > 0) refund += (dmg - refund) * dr / 100.0;
        h += static_cast<float>(refund);
    }
    float maxH = 0;
    if (totalModifiers().lastStand > 0 && lastStandCooldown_ <= 0 && h > 0 && bridge_.getMaxHealth(maxH) && maxH > 0 &&
        h <= maxH * content_.balance.lastStandThresholdPct / 100.0) {
        h = static_cast<float>(maxH * content_.balance.lastStandHealPct / 100.0);
        lastStandCooldown_ = content_.balance.lastStandCooldown;
        post("Второе дыхание! Сила не даёт вам пасть.");
    }
    if (h != before) bridge_.setHealth(h);
    lastHealth_ = h;
}

void Session::applyRegen(double dt) {
    const Modifiers m = totalModifiers();
    regenHpAcc_ += (m.hpRegen + buffSum(EffectType::Regen)) * dt;
    regenFpAcc_ += (m.forceRegen + buffSum(EffectType::Regen, true)) * dt;
    // Write in whole points to keep memory traffic low.
    if (regenHpAcc_ >= 1.0) {
        float h = 0, maxH = 0;
        if (bridge_.getHealth(h) && bridge_.getMaxHealth(maxH) && h > 0 && h < maxH) {
            h = std::min(maxH, h + static_cast<float>(regenHpAcc_));
            bridge_.setHealth(h);
            lastHealth_ = h;
        }
        regenHpAcc_ = 0;
    }
    if (regenFpAcc_ >= 1.0) {
        float f = 0, maxF = 0;
        if (bridge_.getForce(f) && bridge_.getMaxForce(maxF) && f < maxF)
            bridge_.setForce(std::min(maxF, f + static_cast<float>(regenFpAcc_)));
        regenFpAcc_ = 0;
    }
}

void Session::updateBuffs(double dt) {
    for (auto& b : buffs_) b.remaining -= dt;
    buffs_.erase(std::remove_if(buffs_.begin(), buffs_.end(),
                                [](const Buff& b) { return b.remaining <= 0 || (b.type == EffectType::Shield && b.a <= 0); }),
                 buffs_.end());

    if (bridge_.caps() & CapTimeScale) {
        double scale = 1e9;
        for (const auto& b : buffs_)
            if (b.type == EffectType::TimeScale) scale = std::min(scale, b.a);
        if (scale < 1e9) {
            if (!timeScaleActive_) {
                float normal = 1.0f;
                if (bridge_.getTimeScale(normal) && normal > 0) normalTimeScale_ = normal;
                timeScaleActive_ = true;
            }
            bridge_.setTimeScale(static_cast<float>(scale) * normalTimeScale_);
        } else if (timeScaleActive_) {
            bridge_.setTimeScale(normalTimeScale_);
            timeScaleActive_ = false;
        }
    }
    if (bridge_.caps() & CapDamageScale)
        bridge_.setDamageScale(static_cast<float>(1.0 + (totalModifiers().damageBonusPct + buffSum(EffectType::DamageBoost)) / 100.0));
}

double Session::buffSum(EffectType t, bool useB) const {
    double s = 0;
    for (const auto& b : buffs_)
        if (b.type == t) s += useB ? b.b : b.a;
    return s;
}

double Session::shieldLeft() const { return buffSum(EffectType::Shield); }

// ---------------------------------------------------------------- kills, xp, loot

double Session::locationXpFactor() const {
    auto it = content_.locations.find(ch_.currentLocation);
    if (it == content_.locations.end()) return 1.0;
    const auto& b = content_.balance;
    int diff = ch_.level - it->second.tier;
    if (diff > 0) return std::max(b.minLocationXpFactor, 1.0 - diff * b.overlevelPenaltyPerLevel);
    return std::min(2.0, 1.0 - diff * b.underlevelBonusPerLevel);
}

void Session::onKill(double forcePoints) {
    ++ch_.kills;
    const Modifiers m = totalModifiers();
    double bonus = 1.0 + (m.xpBonusPct + buffSum(EffectType::XpBoost)) / 100.0;
    double xp = forcePoints * content_.balance.xpPerForcePoint * locationXpFactor() * bonus;
    grantXp(static_cast<int64_t>(std::llround(std::max(1.0, xp))), "");

    double hp = m.killHealPct + buffSum(EffectType::KillHeal);
    double fp = m.killForcePct + buffSum(EffectType::KillHeal, true);
    if (hp > 0) healPct(hp);
    if (fp > 0) forcePct(fp);

    questsOnKill();
    std::uniform_real_distribution<double> roll(0, 1);
    if (roll(rng_) < content_.balance.crystalDropChance) addCrystal(rollCrystal(ch_.level));
}

void Session::grantXp(int64_t amount, const std::string& why) {
    const auto& b = content_.balance;
    if (amount <= 0 || ch_.level >= b.maxLevel) return;
    ch_.xp += amount;
    ch_.totalXp += amount;
    if (!why.empty()) post("+" + std::to_string(amount) + " опыта: " + why);
    bool leveled = false;
    while (ch_.level < b.maxLevel && ch_.xp >= b.xpToNext(ch_.level)) {
        ch_.xp -= b.xpToNext(ch_.level);
        ++ch_.level;
        ch_.attrPoints += b.attrPointsPerLevel;
        ch_.skillPoints += b.skillPointsPerLevel;
        leveled = true;
        post("УРОВЕНЬ " + std::to_string(ch_.level) + "! +" + std::to_string(b.attrPointsPerLevel) +
             " очка характеристик, +" + std::to_string(b.skillPointsPerLevel) + " очко навыков");
    }
    if (ch_.level >= b.maxLevel) ch_.xp = 0;
    if (leveled) {
        questsOnLevel();
        announceUnlocks(false);
    }
}

Crystal Session::rollCrystal(int itemLevel, const std::string& forcedBase) {
    Crystal c;
    const CrystalBase* base = nullptr;
    if (!forcedBase.empty()) {
        auto it = content_.crystals.find(forcedBase);
        if (it != content_.crystals.end()) base = &it->second;
    } else {
        double total = 0;
        for (const auto& [id, cb] : content_.crystals)
            if (cb.minLevel <= itemLevel) total += cb.weight;
        std::uniform_real_distribution<double> pick(0, total);
        double r = pick(rng_);
        for (const auto& [id, cb] : content_.crystals) {
            if (cb.minLevel > itemLevel) continue;
            base = &cb;
            if ((r -= cb.weight) <= 0) break;
        }
    }
    if (!base) return c;

    std::uniform_real_distribution<double> u(0, 1);
    double rr = u(rng_);
    c.rarity = rr < 0.05 ? Rarity::Legendary : rr < 0.30 ? Rarity::Rare : Rarity::Common;
    c.base = base->id;
    c.slot = base->slot;
    c.itemLevel = itemLevel;
    c.mods = base->mods;
    std::string affixNames;
    int count = static_cast<int>(c.rarity) + 1;
    double scale = (1.0 + itemLevel * 0.03) * (c.rarity == Rarity::Legendary ? 1.5 : 1.0);
    std::vector<const Affix*> pool;
    for (const auto& a : content_.affixes) pool.push_back(&a);
    for (int i = 0; i < count && !pool.empty(); ++i) {
        double total = 0;
        for (auto* a : pool) total += a->weight;
        double r = u(rng_) * total;
        size_t idx = 0;
        for (; idx + 1 < pool.size(); ++idx)
            if ((r -= pool[idx]->weight) <= 0) break;
        const Affix* a = pool[idx];
        pool.erase(pool.begin() + static_cast<long>(idx));
        double v = std::round((a->min + (a->max - a->min) * u(rng_)) * scale * 10.0) / 10.0;
        c.mods.add(a->key, v);
        affixNames += (affixNames.empty() ? "" : ", ") + a->name;
    }
    c.name = base->name + (affixNames.empty() ? "" : " [" + affixNames + "]");
    return c;
}

void Session::addCrystal(const Crystal& c) {
    if (c.base.empty()) return;
    if (static_cast<int>(ch_.inventory.size()) >= content_.balance.inventorySize) {
        // Salvage instead of silently losing the drop.
        int64_t xp = 15LL * c.itemLevel * (static_cast<int>(c.rarity) + 1);
        post("Инвентарь полон: кристалл распылён в " + std::to_string(xp) + " опыта");
        grantXp(xp, "");
        return;
    }
    ch_.inventory.push_back(c);
    post(std::string("Найден кристалл (") + rarityName(c.rarity) + "): " + c.name);
}

// ---------------------------------------------------------------- abilities & effects

bool Session::abilitySupported(const AbilityDef& a) const {
    uint32_t need = a.requiredCaps();
    return (bridge_.caps() & need) == need;
}

double Session::effectiveCooldown(const AbilityDef& a) const {
    return a.cooldown * std::max(0.3, 1.0 - totalModifiers().cooldownPct / 100.0);
}

double Session::effectiveCost(const AbilityDef& a) const {
    return a.forceCost * std::max(0.25, 1.0 - totalModifiers().abilityCostPct / 100.0);
}

double Session::cooldownLeft(const std::string& id) const {
    auto it = cooldowns_.find(id);
    return it == cooldowns_.end() ? 0.0 : it->second;
}

bool Session::useAbility(int slot, std::string* reason) {
    if (slot < 0 || slot >= kAbilitySlots) return fail(reason, "Нет такого слота");
    const std::string& id = ch_.slots[slot];
    if (id.empty()) return fail(reason, "Слот " + std::to_string(slot + 1) + " пуст");
    const AbilityDef& a = content_.abilities.at(id);
    if (!abilitySupported(a)) return fail(reason, a.name + ": не настроены адреса памяти для этой способности (см. tfu_rpg.ini)");
    if (ch_.level < a.reqLevel) return fail(reason, a.name + ": нужен уровень " + std::to_string(a.reqLevel));
    double cd = cooldownLeft(id);
    if (cd > 0) return fail(reason, a.name + ": перезарядка " + fmtNum(cd, 1) + " с");
    double cost = effectiveCost(a);
    if (cost > 0) {
        float f = 0;
        if (!bridge_.getForce(f)) return fail(reason, "Не удаётся прочитать энергию Силы");
        if (f < cost) return fail(reason, a.name + ": недостаточно Силы (" + fmtNum(cost) + ")");
        bridge_.setForce(f - static_cast<float>(cost));
    }
    for (const auto& e : a.effects) applyEffect(e, a.id);
    cooldowns_[id] = effectiveCooldown(a);
    post(a.name + "!");
    return true;
}

void Session::healPct(double pct) {
    float h = 0, maxH = 0;
    if (!bridge_.getHealth(h) || !bridge_.getMaxHealth(maxH) || h <= 0 || maxH <= 0) return;
    h = std::min(maxH, h + static_cast<float>(maxH * pct / 100.0));
    bridge_.setHealth(h);
    lastHealth_ = h;
}

void Session::forcePct(double pct) {
    float f = 0, maxF = 0;
    if (!bridge_.getForce(f) || !bridge_.getMaxForce(maxF) || maxF <= 0) return;
    bridge_.setForce(std::min(maxF, f + static_cast<float>(maxF * pct / 100.0)));
}

void Session::applyEffect(const Effect& e, const std::string& source) {
    switch (e.type) {
        case EffectType::Heal: healPct(e.a); break;
        case EffectType::RestoreForce: forcePct(e.a); break;
        case EffectType::AreaDamage: {
            double bonus = 1.0 + totalModifiers().damageBonusPct / 100.0;
            bridge_.areaDamage(static_cast<float>(e.a), static_cast<float>(e.b * bonus));
            break;
        }
        case EffectType::HealthToForce: {
            float h = 0;
            if (!bridge_.getHealth(h) || h <= 1) break;
            float spent = std::min(h - 1.0f, static_cast<float>(h * e.a / 100.0));
            h -= spent;
            bridge_.setHealth(h);
            lastHealth_ = h;  // self-inflicted: damage reduction must not refund it
            forcePct(e.b);
            break;
        }
        default:
            buffs_.push_back(Buff{e.type, e.a, e.b, e.duration, source});
            break;
    }
}

// ---------------------------------------------------------------- character building

Modifiers Session::totalModifiers() const {
    Modifiers m;
    for (int i = 0; i < kAttrCount; ++i) m += content_.balance.perAttrPoint[i].scaled(ch_.attrs[i]);
    for (const auto& [id, rank] : ch_.skillRanks) m += content_.skills.at(id).perRank.scaled(rank);
    if (!ch_.equippedColor.base.empty()) m += ch_.equippedColor.mods;
    if (!ch_.equippedFocus.base.empty()) m += ch_.equippedFocus.mods;
    return m;
}

bool Session::spendAttribute(Attr a, std::string* reason) {
    int i = static_cast<int>(a);
    if (i < 0 || i >= kAttrCount) return fail(reason, "Нет такой характеристики");
    if (ch_.attrPoints <= 0) return fail(reason, "Нет свободных очков характеристик");
    --ch_.attrPoints;
    ++ch_.attrs[i];
    return true;
}

bool Session::canLearnSkill(const std::string& id, std::string* reason) const {
    auto it = content_.skills.find(id);
    if (it == content_.skills.end()) return fail(reason, "Нет такого навыка");
    const SkillNode& n = it->second;
    auto rk = ch_.skillRanks.find(id);
    int rank = rk == ch_.skillRanks.end() ? 0 : rk->second;
    if (rank >= n.maxRank) return fail(reason, n.name + ": максимальный ранг");
    if (ch_.level < n.reqLevel) return fail(reason, n.name + ": нужен уровень " + std::to_string(n.reqLevel));
    for (const auto& [req, minRank] : n.prereqs) {
        auto r = ch_.skillRanks.find(req);
        if (r == ch_.skillRanks.end() || r->second < minRank)
            return fail(reason, n.name + ": сначала " + content_.skills.at(req).name + " (ранг " + std::to_string(minRank) + ")");
    }
    if (ch_.skillPoints < n.cost) return fail(reason, n.name + ": нужно очков навыков: " + std::to_string(n.cost));
    return true;
}

bool Session::learnSkill(const std::string& id, std::string* reason) {
    if (!canLearnSkill(id, reason)) return false;
    const SkillNode& n = content_.skills.at(id);
    ch_.skillPoints -= n.cost;
    int rank = ++ch_.skillRanks[id];
    post("Навык: " + n.name + " (ранг " + std::to_string(rank) + "/" + std::to_string(n.maxRank) + ")");
    if (rank == 1 && !n.grantsAbility.empty() && ch_.unlockedAbilities.insert(n.grantsAbility).second) {
        post("Новая способность: " + content_.abilities.at(n.grantsAbility).name);
        for (auto& s : ch_.slots)
            if (s.empty()) {
                s = n.grantsAbility;
                break;
            }
    }
    return true;
}

void Session::refreshAbilityUnlocks() {
    ch_.unlockedAbilities.clear();
    for (const auto& [id, rank] : ch_.skillRanks) {
        const auto& g = content_.skills.at(id).grantsAbility;
        if (rank > 0 && !g.empty()) ch_.unlockedAbilities.insert(g);
    }
}

bool Session::bindAbility(int slot, const std::string& abilityId, std::string* reason) {
    if (slot < 0 || slot >= kAbilitySlots) return fail(reason, "Нет такого слота");
    if (!abilityId.empty() && !ch_.unlockedAbilities.count(abilityId)) return fail(reason, "Способность не изучена");
    for (auto& s : ch_.slots)
        if (s == abilityId) s = ch_.slots[slot];  // swap if already bound elsewhere
    ch_.slots[slot] = abilityId;
    return true;
}

bool Session::equipCrystal(size_t idx, std::string* reason) {
    if (idx >= ch_.inventory.size()) return fail(reason, "Нет такого предмета");
    Crystal c = ch_.inventory[idx];
    Crystal& slot = c.slot == "color" ? ch_.equippedColor : ch_.equippedFocus;
    if (slot.base.empty())
        ch_.inventory.erase(ch_.inventory.begin() + static_cast<long>(idx));
    else
        ch_.inventory[idx] = slot;
    slot = c;
    post("Вставлен кристалл: " + c.name);
    return true;
}

bool Session::respec(std::string* reason) {
    if (ch_.skillRanks.empty() && std::all_of(ch_.attrs.begin(), ch_.attrs.end(), [](int v) { return v == 0; }))
        return fail(reason, "Нечего сбрасывать");
    for (const auto& [id, rank] : ch_.skillRanks) ch_.skillPoints += rank * content_.skills.at(id).cost;
    for (auto& a : ch_.attrs) {
        ch_.attrPoints += a;
        a = 0;
    }
    ch_.skillRanks.clear();
    ch_.unlockedAbilities.clear();
    ch_.slots = {};
    buffs_.clear();
    post("Навыки и характеристики сброшены");
    return true;
}

// ---------------------------------------------------------------- world

bool Session::locationUnlocked(const std::string& id) const {
    auto it = content_.locations.find(id);
    if (it == content_.locations.end()) return false;
    const Location& l = it->second;
    if (l.startUnlocked || ch_.unlockedLocations.count(id)) return true;
    return ch_.level >= l.reqLevel && (l.reqQuest.empty() || ch_.questsDone.count(l.reqQuest));
}

void Session::announceUnlocks(bool silent) {
    for (const auto& id : content_.locationOrder) {
        if (!locationUnlocked(id) || announced_.count(id)) continue;
        announced_.insert(id);
        if (!silent) post("Открыт маршрут: " + content_.locations.at(id).name);
    }
}

std::vector<std::string> Session::travelRoute(const std::string& fromId, const std::string& to) const {
    std::string from = fromId;
    if (from.empty() || !content_.locations.count(from))
        for (const auto& [id, l] : content_.locations)
            if (l.hub) from = id;
    if (!locationUnlocked(to) || !content_.locations.count(from)) return {};
    std::map<std::string, std::string> prev;
    std::queue<std::string> q;
    q.push(from);
    prev[from] = "";
    while (!q.empty()) {
        std::string cur = q.front();
        q.pop();
        if (cur == to) break;
        for (const auto& next : content_.locations.at(cur).links) {
            if (prev.count(next) || (!locationUnlocked(next) && next != to)) continue;
            prev[next] = cur;
            q.push(next);
        }
    }
    if (!prev.count(to)) return {};
    std::vector<std::string> path;
    for (std::string c = to; !c.empty(); c = prev[c]) path.push_back(c);
    std::reverse(path.begin(), path.end());
    return path;
}

bool Session::travel(const std::string& id, std::string* reason) {
    auto it = content_.locations.find(id);
    if (it == content_.locations.end()) return fail(reason, "Нет такой локации");
    const Location& l = it->second;
    if (!locationUnlocked(id)) {
        std::string why = "Маршрут закрыт: нужен уровень " + std::to_string(l.reqLevel);
        if (!l.reqQuest.empty()) why += " и задание «" + content_.quests.at(l.reqQuest).name + "»";
        return fail(reason, why);
    }
    if (id == ch_.currentLocation) return fail(reason, "Вы уже здесь");
    auto route = travelRoute(ch_.currentLocation, id);
    if (route.empty()) return fail(reason, "Нет маршрута до " + l.name);
    std::string path;
    for (const auto& r : route) path += (path.empty() ? "" : " → ") + content_.locations.at(r).name;
    if (l.travelLevel.empty() || !(bridge_.caps() & CapLoadLevel))
        return fail(reason, "Автоперелёт не настроен. Маршрут: " + path + ". Выберите «" + l.name + "» в меню миссий.");
    if (!bridge_.loadLevel(l.travelLevel)) return fail(reason, "Игра отклонила загрузку уровня " + l.travelLevel);
    post("Курс проложен: " + path);
    return true;
}

// ---------------------------------------------------------------- quests

bool Session::questAvailable(const QuestDef& q) const {
    if (!q.repeatable && ch_.questsDone.count(q.id)) return false;
    if (ch_.level < q.reqLevel) return false;
    for (const auto& r : q.requiresQuests)
        if (!ch_.questsDone.count(r)) return false;
    return true;
}

void Session::questsOnKill() {
    for (const auto& id : content_.questOrder) {
        const QuestDef& q = content_.quests.at(id);
        if (q.kind != QuestKind::Kill || !questAvailable(q)) continue;
        if (!q.location.empty() && q.location != ch_.currentLocation) continue;
        if (++ch_.questProgress[id] >= q.count) completeQuest(q);
    }
}

void Session::questsOnVisit(const std::string& loc) {
    for (const auto& id : content_.questOrder) {
        const QuestDef& q = content_.quests.at(id);
        if (q.kind == QuestKind::Visit && q.location == loc && questAvailable(q)) completeQuest(q);
    }
}

void Session::questsOnLevel() {
    for (const auto& id : content_.questOrder) {
        const QuestDef& q = content_.quests.at(id);
        if (q.kind == QuestKind::ReachLevel && ch_.level >= q.count && questAvailable(q)) completeQuest(q);
    }
}

void Session::completeQuest(const QuestDef& q) {
    if (q.repeatable)
        ch_.questProgress[q.id] = 0;
    else {
        ch_.questsDone.insert(q.id);
        ch_.questProgress.erase(q.id);
    }
    post("Задание выполнено: " + q.name);
    ch_.attrPoints += q.rewardAttrPoints;
    ch_.skillPoints += q.rewardSkillPoints;
    if (q.rewardSkillPoints > 0) post("+" + std::to_string(q.rewardSkillPoints) + " очк. навыков");
    if (!q.rewardCrystal.empty()) {
        int lvl = ch_.level;
        auto it = content_.locations.find(q.location);
        if (it != content_.locations.end()) lvl = std::max(lvl, it->second.tier);
        addCrystal(rollCrystal(lvl, q.rewardCrystal));
    }
    grantXp(q.rewardXp, q.name);
    announceUnlocks(false);
}

void Session::post(const std::string& msg) {
    messages_.push_back(msg);
    while (messages_.size() > 64) messages_.pop_front();
}

}  // namespace tfurpg
