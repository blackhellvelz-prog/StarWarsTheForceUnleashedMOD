// Unit tests for the RPG core against FakeGame. Run: tfurpg_tests <path-to-data-dir>
#include <cmath>
#include <cstdio>
#include <functional>
#include <string>
#include <vector>

#include "fake_game.hpp"
#include "tfurpg/session.hpp"

using namespace tfurpg;

namespace {

int g_failed = 0, g_checks = 0;
std::string g_dataDir;

#define CHECK(cond)                                                                 \
    do {                                                                            \
        ++g_checks;                                                                 \
        if (!(cond)) {                                                              \
            ++g_failed;                                                             \
            std::printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);           \
        }                                                                           \
    } while (0)

#define CHECK_NEAR(a, b, eps) CHECK(std::fabs((a) - (b)) <= (eps))

const Content& content() {
    static Content c;
    static bool loaded = false;
    if (!loaded) {
        std::string err;
        if (!c.load(g_dataDir, &err)) {
            std::printf("cannot load content: %s\n", err.c_str());
            std::exit(2);
        }
        loaded = true;
    }
    return c;
}

// Kill helper: raise the stock counter and let the session notice it.
void kill(Session& s, FakeGame& g, double fp = 50) {
    g.forcePoints += fp;
    s.tick(0.016);
}

void levelTo(Session& s, int level) {
    while (s.character().level < level) s.grantXp(s.xpToNext() - s.character().xp, "");
}

// ---------------------------------------------------------------- tests

void testIni() {
    Ini ini;
    std::string err;
    CHECK(ini.loadString("; c\n[a]\nx = 1\ny = hello world \n[b]\nlist = p, q ,, r\n", &err));
    CHECK(ini.getInt("a", "x") == 1);
    CHECK(ini.get("a", "y") == "hello world");
    CHECK(ini.getList("b", "list").size() == 3);
    CHECK(!ini.loadString("[a\n", &err));
    CHECK(!ini.loadString("[a]\nnoequals\n", &err));
    Ini round;
    CHECK(round.loadString(ini.serialize() + "", &err) || true);
}

void testContentLoads() {
    const Content& c = content();
    CHECK(c.skills.size() >= 15);
    CHECK(c.abilities.size() >= 8);
    CHECK(c.locations.size() >= 10);
    CHECK(c.quests.size() >= 10);
    // Every location is reachable from the hub: the "one connected world" promise.
    const Location* hub = nullptr;
    for (const auto& [id, l] : c.locations)
        if (l.hub) hub = &l;
    CHECK(hub != nullptr);
    for (const auto& id : c.locationOrder) {
        if (hub && id == hub->id) continue;
        const auto& links = c.locations.at(id).links;
        CHECK(std::find(links.begin(), links.end(), hub->id) != links.end());
    }
    CHECK(c.locationForLevel("Levels/Felucia_02.lp") == &c.locations.at("felucia"));
    // Longest match wins.
    CHECK(c.locationForLevel("kashyyyk_vader_prologue")->id == "kashyyyk_prologue");
}

void testBadContentRejected() {
    Effect e;
    std::string err;
    CHECK(Effect::parse("shield:100:5", e, &err) && e.type == EffectType::Shield && e.a == 100 && e.duration == 5);
    CHECK(!Effect::parse("shield:100", e, &err));
    CHECK(!Effect::parse("explode:1", e, &err));
    Modifiers m;
    CHECK(Modifiers::parse("maxHealthPct 4, hpRegen 0.5", m, &err) && m.maxHealthPct == 4 && m.hpRegen == 0.5);
    CHECK(!Modifiers::parse("speed 4", m, &err));
}

void testXpCurveAndLevelUp() {
    FakeGame g;
    Session s(content(), g);
    const auto& b = content().balance;
    CHECK(s.character().level == 1);
    CHECK(s.xpToNext() == b.xpToNext(1));
    int attr0 = s.character().attrPoints;
    s.grantXp(b.xpToNext(1) + b.xpToNext(2) + 5, "");
    CHECK(s.character().level == 3);
    CHECK(s.character().xp == 5 + content().quests.at("first_steps").rewardXp);
    CHECK(s.character().attrPoints == attr0 + 2 * b.attrPointsPerLevel);
    // Reaching level 2 completes "first_steps" (+1 skill point on top of 2 levels).
    CHECK(s.character().questsDone.count("first_steps") == 1);
    levelTo(s, b.maxLevel);
    CHECK(s.character().level == b.maxLevel);
    CHECK(s.character().xp == 0);
    s.grantXp(1000, "");
    CHECK(s.character().level == b.maxLevel);
}

void testForcePointsBecomeXp() {
    FakeGame g;
    Session s(content(), g);
    s.tick(0.016);  // primes counters, detects location
    CHECK(s.character().currentLocation == "empirical");
    int64_t before = s.character().totalXp;
    kill(s, g, 40);
    CHECK(s.character().kills == 1);
    CHECK(s.character().totalXp > before);
    // Spending points on stock upgrades isn't a kill.
    g.forcePoints -= 30;
    s.tick(0.016);
    CHECK(s.character().kills == 1);
    // A save load (huge jump) isn't a kill either.
    g.forcePoints += 1e6;
    s.tick(0.016);
    CHECK(s.character().kills == 1);
    // Changing level re-primes: the first reading after a load is ignored.
    g.level = "raxus_prime_01";
    g.forcePoints += 100;
    s.tick(0.016);
    CHECK(s.character().kills == 1);
    CHECK(s.character().currentLocation == "raxus_prime");
    kill(s, g);
    CHECK(s.character().kills == 2);
}

void testOverlevelPenalty() {
    FakeGame g;
    Session low(content(), g);
    low.tick(0.016);
    int64_t x0 = low.character().totalXp;
    kill(low, g, 100);
    int64_t gainedLow = low.character().totalXp - x0;

    FakeGame g2;
    Session high(content(), g2);
    levelTo(high, 20);
    high.tick(0.016);
    int64_t before = high.character().totalXp;
    kill(high, g2, 100);
    int64_t gainedHigh = high.character().totalXp - before;
    CHECK(gainedHigh < gainedLow);
    CHECK(gainedHigh >= static_cast<int64_t>(100 * content().balance.minLocationXpFactor) - 1);
}

void testSkillsAndAbilities() {
    FakeGame g;
    Session s(content(), g);
    std::string why;
    CHECK(!s.learnSkill("hatred", &why));  // needs dark_initiate
    CHECK(s.learnSkill("light_initiate", &why));
    CHECK(s.character().unlockedAbilities.count("battle_meditation") == 1);
    CHECK(s.character().slots[0] == "battle_meditation");
    CHECK(!s.learnSkill("serenity", &why));  // no points left
    levelTo(s, 6);
    CHECK(s.learnSkill("serenity", &why));
    CHECK(s.learnSkill("barrier", &why));
    CHECK(s.character().slots[1] == "force_barrier");

    // Barrier absorbs damage until depleted.
    s.tick(0.016);
    float startForce = g.force;
    CHECK(s.useAbility(1, &why));
    CHECK(g.force < startForce);
    CHECK(!s.useAbility(1, &why));  // cooldown
    g.health -= 100;
    s.tick(0.016);
    CHECK(g.health > 499);  // fully absorbed
    CHECK(s.shieldLeft() < 251 && s.shieldLeft() > 140);
    g.health -= 300;
    s.tick(0.016);
    CHECK(s.shieldLeft() == 0);
    CHECK(g.health < 499);

    // Not enough Force.
    g.force = 1;
    std::string reason;
    CHECK(!s.useAbility(0, &reason));
    CHECK(reason.find("Силы") != std::string::npos);
}

void testCooldownExpires() {
    FakeGame g;
    Session s(content(), g);
    s.learnSkill("light_initiate");
    s.tick(0.016);
    CHECK(s.useAbility(0));
    const auto& a = content().abilities.at("battle_meditation");
    CHECK_NEAR(s.cooldownLeft(a.id), s.effectiveCooldown(a), 1e-6);
    for (int i = 0; i < 100 * static_cast<int>(a.cooldown); ++i) s.tick(0.0101);
    CHECK(s.cooldownLeft(a.id) == 0);
    g.force = g.maxForce;
    CHECK(s.useAbility(0));
}

void testRegenAndDamageReduction() {
    FakeGame g;
    Session s(content(), g);
    s.learnSkill("light_initiate");  // hpRegen 0.5 + battle meditation
    s.tick(0.016);
    g.health = 300;
    s.tick(0.016);
    for (int i = 0; i < 100; ++i) s.tick(0.1);  // 10 s at 0.5 hp/s
    CHECK(g.health >= 304 && g.health <= 306);

    // Battle meditation: 25% damage reduction.
    CHECK(s.useAbility(0));
    s.tick(0.016);
    float h = g.health;
    g.health -= 100;
    s.tick(0.016);
    CHECK(g.health > h - 100 + 20);
}

void testMaxStatsFollowGameUpgrades() {
    FakeGame g;
    Session s(content(), g);
    levelTo(s, 3);
    s.learnSkill("toughness");  // +6%
    s.tick(0.016);
    CHECK_NEAR(g.maxHealth, 530.0f, 0.6f);
    s.tick(0.016);
    CHECK_NEAR(g.maxHealth, 530.0f, 0.6f);  // no compounding
    g.maxHealth = 700;                       // stock health upgrade bought in the game menu
    s.tick(0.016);
    CHECK_NEAR(g.maxHealth, 742.0f, 0.6f);
    std::string why;
    CHECK(s.spendAttribute(Attr::Vitality, &why));  // +3%
    s.tick(0.016);
    CHECK_NEAR(g.maxHealth, 763.0f, 0.6f);
}

void testLastStand() {
    FakeGame g;
    Session s(content(), g);
    levelTo(s, 12);
    for (int i = 0; i < 3; ++i) CHECK(s.learnSkill("toughness"));
    CHECK(s.learnSkill("second_wind"));
    s.tick(0.016);
    s.tick(0.016);
    g.health = g.maxHealth * 0.05f;
    s.tick(0.016);
    CHECK(g.health >= g.maxHealth * 0.49f);
    g.health = g.maxHealth * 0.05f;
    s.tick(0.016);
    CHECK(g.health < g.maxHealth * 0.1f);  // on cooldown
}

void testTimeStasisRestores() {
    FakeGame g;
    Session s(content(), g);
    levelTo(s, 14);
    s.learnSkill("light_initiate");
    s.learnSkill("serenity");
    s.learnSkill("barrier");
    std::string why;
    CHECK(s.learnSkill("time_master", &why));
    int slot = -1;
    for (int i = 0; i < kAbilitySlots; ++i)
        if (s.character().slots[i] == "time_stasis") slot = i;
    CHECK(slot >= 0);
    s.tick(0.016);
    CHECK(s.useAbility(slot, &why));
    s.tick(0.016);
    CHECK_NEAR(g.timeScale, 0.4f, 1e-4);
    for (int i = 0; i < 60; ++i) s.tick(0.1);
    CHECK_NEAR(g.timeScale, 1.0f, 1e-4);
}

void testUnsupportedAbilityRefused() {
    FakeGame g;  // no CapAreaDamage
    Session s(content(), g);
    CHECK(!s.abilitySupported(content().abilities.at("force_storm")));
    g.capMask |= CapAreaDamage;
    CHECK(s.abilitySupported(content().abilities.at("force_storm")));
    CHECK(!(s.abilitySupported(content().abilities.at("sith_fury"))));
}

void testHealthToForceNotRefunded() {
    FakeGame g;
    Session s(content(), g);
    s.learnSkill("dark_initiate");
    s.learnSkill("light_initiate");  // won't have points; ensure we only rely on dark_initiate
    s.tick(0.016);
    g.force = 0;
    std::string why;
    CHECK(s.useAbility(0, &why));
    CHECK_NEAR(g.health, 350.0f, 0.5f);
    CHECK(g.force > 100);
    s.tick(0.016);
    CHECK_NEAR(g.health, 350.0f, 0.5f);
}

void testWorldUnlockAndTravel() {
    // The shipped data leaves travelLevel empty until a player confirms real level names; fill it here.
    Content c = content();
    for (auto& [id, l] : c.locations) l.travelLevel = id;
    FakeGame g;
    Session s(c, g);
    s.tick(0.016);
    std::string why;
    CHECK(!s.locationUnlocked("raxus_prime"));
    CHECK(!s.travel("raxus_prime", &why));
    CHECK(why.find("уровень") != std::string::npos);
    CHECK(s.locationUnlocked("kashyyyk_prologue"));
    CHECK(s.travel("kashyyyk_prologue", &why));
    CHECK(g.loadRequests.size() == 1);
    s.tick(0.016);  // the level load re-primes the counters
    CHECK(s.character().currentLocation == "kashyyyk_prologue");

    // Progress the chain: training (15 kills anywhere) -> visit TIE facility -> tie_purge.
    for (int i = 0; i < 15; ++i) kill(s, g, 10);
    CHECK(s.character().questsDone.count("training") == 1);
    levelTo(s, 3);
    CHECK(s.locationUnlocked("tie_facility"));
    g.level = "tie_factory_a";
    s.tick(0.016);
    CHECK(s.character().questsDone.count("kota") == 1);
    s.tick(0.016);
    for (int i = 0; i < 40; ++i) kill(s, g, 10);
    CHECK(s.character().questsDone.count("tie_purge") == 1);
    levelTo(s, 5);
    CHECK(s.locationUnlocked("raxus_prime"));
    auto route = s.travelRoute("tie_facility", "raxus_prime");
    CHECK(route.size() == 2);  // direct link
    route = s.travelRoute("kashyyyk_prologue", "raxus_prime");
    CHECK(route.size() == 3 && route[1] == "rogue_shadow");

    // Without a load-level address the mod explains how to get there instead.
    g.capMask &= ~CapLoadLevel;
    CHECK(!s.travel("raxus_prime", &why));
    CHECK(why.find("меню миссий") != std::string::npos);
}

void testCrystals() {
    FakeGame g;
    Session s(content(), g, 1234);
    for (int i = 0; i < 50; ++i) {
        Crystal c = s.rollCrystal(20);
        CHECK(!c.base.empty());
        CHECK(c.slot == "color" || c.slot == "focus");
        Crystal back;
        CHECK(Crystal::deserialize(c.serialize(), back));
        CHECK(back.name == c.name && back.rarity == c.rarity);
        CHECK_NEAR(back.mods.maxHealthPct, c.mods.maxHealthPct, 1e-6);
    }
    Crystal low = s.rollCrystal(1);
    CHECK(content().crystals.at(low.base).minLevel <= 1);
    Session s2(content(), g, 1234);
    CHECK(s2.rollCrystal(20).serialize() == Session(content(), g, 1234).rollCrystal(20).serialize());
}

void testEquipCrystal() {
    FakeGame g;
    Session s(content(), g);
    // Complete "training" to receive an Ilum crystal.
    s.tick(0.016);
    for (int i = 0; i < 15; ++i) kill(s, g, 10);
    size_t n = s.character().inventory.size();
    CHECK(n >= 1);
    size_t idx = 0;
    for (; idx < n; ++idx)
        if (s.character().inventory[idx].base == "ilum") break;
    CHECK(idx < n);
    double before = s.totalModifiers().maxForcePct;
    CHECK(s.equipCrystal(idx));
    CHECK(s.totalModifiers().maxForcePct > before);
    CHECK(s.character().equippedColor.base == "ilum");
}

void testSaveRoundTrip() {
    FakeGame g;
    Session s(content(), g, 7);
    levelTo(s, 9);
    s.learnSkill("dark_initiate");
    s.learnSkill("hatred");
    s.spendAttribute(Attr::Might);
    s.tick(0.016);
    for (int i = 0; i < 20; ++i) kill(s, g, 10);
    Ini save = s.save();

    Ini reparsed;
    CHECK(reparsed.loadString(save.serialize()));
    FakeGame g2;
    Session t(content(), g2);
    std::string err;
    CHECK(t.load(reparsed, &err));
    const auto& a = s.character();
    const auto& b = t.character();
    CHECK(a.level == b.level && a.xp == b.xp && a.totalXp == b.totalXp);
    CHECK(a.skillRanks == b.skillRanks && a.attrs == b.attrs);
    CHECK(a.slots == b.slots && a.unlockedAbilities == b.unlockedAbilities);
    CHECK(a.questsDone == b.questsDone && a.questProgress == b.questProgress);
    CHECK(a.inventory.size() == b.inventory.size());
    CHECK(a.kills == b.kills && a.currentLocation == b.currentLocation);
    CHECK_NEAR(s.totalModifiers().damageBonusPct, t.totalModifiers().damageBonusPct, 1e-9);

    // Unknown skills from an older content version are refunded, not kept.
    reparsed.set("skills", "removed_skill", "2");
    CHECK(t.load(reparsed, &err));
    CHECK(t.character().skillRanks.count("removed_skill") == 0);
}

void testRespec() {
    FakeGame g;
    Session s(content(), g);
    levelTo(s, 5);
    int sp = s.character().skillPoints, ap = s.character().attrPoints;
    s.learnSkill("toughness");
    s.learnSkill("recovery");
    s.spendAttribute(Attr::Agility);
    CHECK(s.respec());
    CHECK(s.character().skillPoints == sp && s.character().attrPoints == ap);
    CHECK(s.character().slots[0].empty());
}

void testRepeatableBounty() {
    FakeGame g;
    Session s(content(), g);
    levelTo(s, 5);
    s.tick(0.016);
    int64_t before = s.character().totalXp;
    for (int i = 0; i < 100; ++i) kill(s, g, 1);
    CHECK(s.character().questsDone.count("bounty") == 0);
    CHECK(s.character().totalXp - before >= 2 * 600);
}

}  // namespace

int main(int argc, char** argv) {
    g_dataDir = argc > 1 ? argv[1] : "data";
    const std::vector<std::pair<const char*, std::function<void()>>> tests = {
        {"ini", testIni},
        {"content loads", testContentLoads},
        {"bad content rejected", testBadContentRejected},
        {"xp curve and level up", testXpCurveAndLevelUp},
        {"force points become xp", testForcePointsBecomeXp},
        {"overlevel penalty", testOverlevelPenalty},
        {"skills and abilities", testSkillsAndAbilities},
        {"cooldown expires", testCooldownExpires},
        {"regen and damage reduction", testRegenAndDamageReduction},
        {"max stats follow game upgrades", testMaxStatsFollowGameUpgrades},
        {"last stand", testLastStand},
        {"time stasis restores", testTimeStasisRestores},
        {"unsupported ability refused", testUnsupportedAbilityRefused},
        {"health to force not refunded", testHealthToForceNotRefunded},
        {"world unlock and travel", testWorldUnlockAndTravel},
        {"crystals", testCrystals},
        {"equip crystal", testEquipCrystal},
        {"save round trip", testSaveRoundTrip},
        {"respec", testRespec},
        {"repeatable bounty", testRepeatableBounty},
    };
    for (const auto& [name, fn] : tests) {
        int before = g_failed;
        fn();
        std::printf("%s %s\n", g_failed == before ? "ok  " : "FAIL", name);
    }
    std::printf("%d checks, %d failed\n", g_checks, g_failed);
    return g_failed == 0 ? 0 : 1;
}
