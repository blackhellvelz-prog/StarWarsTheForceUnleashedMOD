// Headless playthrough of the RPG layer against a fake game: walks the location chain, fights, levels,
// spends points and prints what the player would see. Useful for balancing data/*.ini without the game.
//   tfurpg_sim [data-dir] [kills-per-location]
#include <cstdio>
#include <cstdlib>
#include <random>
#include <string>

#include "../tests/fake_game.hpp"
#include "tfurpg/session.hpp"

using namespace tfurpg;

namespace {

void flush(Session& s) {
    for (const auto& m : s.messages()) std::printf("    %s\n", m.c_str());
    s.messages().clear();
}

// Simple build: buy every skill we can (in data order), alternate attributes.
void spendPoints(Session& s) {
    bool any = true;
    while (any) {
        any = false;
        for (const auto& id : s.content().skillOrder)
            if (s.learnSkill(id)) any = true;
    }
    int i = 0;
    while (s.spendAttribute(static_cast<Attr>(i++ % kAttrCount))) {
    }
}

}  // namespace

int main(int argc, char** argv) {
    std::string dir = argc > 1 ? argv[1] : "data";
    int killsPerLocation = argc > 2 ? std::atoi(argv[2]) : 160;
    Content content;
    std::string err;
    if (!content.load(dir, &err)) {
        std::printf("content error: %s\n", err.c_str());
        return 1;
    }
    FakeGame game;
    game.capMask |= CapAreaDamage | CapDamageScale;
    Session s(content, game, 42);
    std::mt19937 rng(1);
    std::uniform_int_distribution<int> fpRoll(20, 90);

    for (const auto& locId : content.locationOrder) {
        const Location& loc = content.locations.at(locId);
        if (loc.hub) continue;
        // Grind on the previous location until this one opens, like a player would.
        int guard = 0;
        while (!s.locationUnlocked(locId) && guard++ < 5000) {
            game.forcePoints += fpRoll(rng);
            s.tick(0.05);
        }
        std::string why;
        if (!s.travel(locId, &why)) std::printf("  travel: %s\n", why.c_str());
        game.level = loc.levelMatch.empty() ? loc.id : loc.levelMatch.front();
        s.tick(0.05);
        s.tick(0.05);
        std::printf("== %s (tier %d) — персонаж %d ур.\n", loc.name.c_str(), loc.tier, s.character().level);
        flush(s);
        for (int k = 0; k < killsPerLocation; ++k) {
            game.forcePoints += fpRoll(rng);
            game.health -= 15;
            s.tick(0.05);
            for (int slot = 0; slot < kAbilitySlots; ++slot)
                if (game.force > game.maxForce * 0.5f) s.useAbility(slot);
            game.force = std::min(game.maxForce, game.force + 5);
            if (game.health < game.maxHealth * 0.3f) game.health = game.maxHealth;  // stock health pickup
            spendPoints(s);
        }
        flush(s);
    }
    const auto& c = s.character();
    std::printf("\nИтог: уровень %d, опыт %lld, убийств %lld, заданий %zu, кристаллов %zu\n", c.level,
                static_cast<long long>(c.totalXp), static_cast<long long>(c.kills), c.questsDone.size(),
                c.inventory.size());
    std::printf("Бонусы: %s\n", s.totalModifiers().describe().c_str());
    return 0;
}
