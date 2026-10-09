#include "ui.hpp"

#include <algorithm>
#include <cstdio>

namespace tfuplugin {

using namespace tfurpg;

namespace {

const char* kTabNames[Ui::kTabCount] = {"Персонаж", "Навыки", "Способности", "Карта", "Задания", "Кристаллы"};
constexpr double kToastSeconds = 6.0;

std::string num(double v, int decimals = 0) {
    char buf[48];
    std::snprintf(buf, sizeof buf, "%.*f", decimals, v);
    return buf;
}

std::string treeName(const std::string& id) {
    if (id == "dark") return "ТЁМНАЯ СТОРОНА";
    if (id == "light") return "СВЕТЛАЯ СТОРОНА";
    if (id == "blade") return "МАСТЕР КЛИНКА";
    if (id == "survival") return "ВЫЖИВАНИЕ";
    return id;
}

uint32_t treeColor(const std::string& id) {
    if (id == "dark") return color::kDark;
    if (id == "light") return color::kLight;
    return color::kGold;
}

}  // namespace

void Ui::setOpen(bool v) {
    open_ = v;
    confirmRespec_ = false;
    status_.clear();
}

void Ui::report(bool ok, const std::string& reason) {
    status_ = ok ? "Готово" : reason;
    statusColor_ = ok ? color::kGood : color::kBad;
}

void Ui::onKey(Key k) {
    if (k == Key::Menu) {
        setOpen(!open_);
        return;
    }
    int slot = k == Key::Slot1 ? 0 : k == Key::Slot2 ? 1 : k == Key::Slot3 ? 2 : k == Key::Slot4 ? 3 : -1;
    if (!open_) {
        if (slot >= 0) {
            std::string why;
            if (!s_.useAbility(slot, &why)) toasts_.push_back({why, now_});
        }
        return;
    }
    auto rs = rows();
    std::vector<int> selectable;
    for (int i = 0; i < static_cast<int>(rs.size()); ++i)
        if (rs[i].selectable) selectable.push_back(i);
    switch (k) {
        case Key::Left:
            tab_ = (tab_ + kTabCount - 1) % kTabCount;
            sel_ = 0;
            confirmRespec_ = false;
            break;
        case Key::Right:
            tab_ = (tab_ + 1) % kTabCount;
            sel_ = 0;
            confirmRespec_ = false;
            break;
        case Key::Up:
            if (!selectable.empty()) sel_ = (sel_ + static_cast<int>(selectable.size()) - 1) % static_cast<int>(selectable.size());
            break;
        case Key::Down:
            if (!selectable.empty()) sel_ = (sel_ + 1) % static_cast<int>(selectable.size());
            break;
        case Key::Enter:
            if (sel_ < static_cast<int>(selectable.size()) && rs[selectable[sel_]].act) rs[selectable[sel_]].act();
            break;
        case Key::Back:
            setOpen(false);
            break;
        default:
            if (slot >= 0 && sel_ < static_cast<int>(selectable.size()) && rs[selectable[sel_]].bind)
                rs[selectable[sel_]].bind(slot);
            break;
    }
}

void Ui::update(double now) {
    now_ = now;
    for (auto& m : s_.messages()) toasts_.push_back({m, now});
    s_.messages().clear();
    while (!toasts_.empty() && (now - toasts_.front().at > kToastSeconds || toasts_.size() > 6)) toasts_.pop_front();
}

std::vector<UiLine> Ui::hud(double now) const {
    const Character& c = s_.character();
    std::vector<UiLine> out;
    std::string xp = c.level >= s_.content().balance.maxLevel
                         ? "МАКС."
                         : std::to_string(c.xp) + "/" + std::to_string(s_.xpToNext());
    std::string head = "Ур. " + std::to_string(c.level) + "   Опыт " + xp;
    if (c.attrPoints || c.skillPoints)
        head += "   [F10: очки " + std::to_string(c.attrPoints) + "/" + std::to_string(c.skillPoints) + "]";
    out.push_back({head, color::kGold});
    std::string slots;
    for (int i = 0; i < kAbilitySlots; ++i) {
        const std::string& id = c.slots[i];
        if (id.empty()) continue;
        const AbilityDef& a = s_.content().abilities.at(id);
        double cd = s_.cooldownLeft(id);
        slots += "F" + std::to_string(i + 1) + " " + a.name + (cd > 0 ? " (" + num(cd) + ")" : "") + "   ";
    }
    if (!slots.empty()) out.push_back({slots, color::kText});
    if (s_.shieldLeft() > 0) out.push_back({"Барьер: " + num(s_.shieldLeft()), color::kLight});
    for (const auto& t : toasts_)
        if (now - t.at <= kToastSeconds) out.push_back({t.text, color::kSelect});
    return out;
}

std::vector<UiLine> Ui::panel() {
    std::vector<UiLine> out;
    std::string tabs;
    for (int i = 0; i < kTabCount; ++i) tabs += (i == tab_ ? "[" + std::string(kTabNames[i]) + "]" : " " + std::string(kTabNames[i]) + " ") + " ";
    out.push_back({"STAR WARS: THE FORCE UNLEASHED — RPG", color::kGold});
    out.push_back({tabs, color::kSelect});
    out.push_back({"", color::kText});
    auto rs = rows();
    int selIdx = -1, n = 0;
    for (int i = 0; i < static_cast<int>(rs.size()); ++i)
        if (rs[i].selectable && n++ == sel_) selIdx = i;
    if (selIdx < 0) {
        sel_ = 0;
        for (int i = 0; i < static_cast<int>(rs.size()); ++i)
            if (rs[i].selectable) {
                selIdx = i;
                break;
            }
    }
    for (int i = 0; i < static_cast<int>(rs.size()); ++i)
        out.push_back({(i == selIdx ? "> " : "  ") + rs[i].text, i == selIdx ? color::kSelect : rs[i].color});
    out.push_back({"", color::kText});
    if (selIdx >= 0 && !rs[selIdx].detail.empty()) out.push_back({rs[selIdx].detail, color::kDim});
    if (!status_.empty()) out.push_back({status_, statusColor_});
    out.push_back({"←/→ вкладки  ↑/↓ выбор  Enter действие  F1-F4 назначить слот  F10 закрыть", color::kDim});
    return out;
}

std::vector<Ui::Row> Ui::rows() {
    std::vector<Row> out;
    switch (tab_) {
        case 0: rowsCharacter(out); break;
        case 1: rowsSkills(out); break;
        case 2: rowsAbilities(out); break;
        case 3: rowsMap(out); break;
        case 4: rowsQuests(out); break;
        case 5: rowsCrystals(out); break;
    }
    return out;
}

void Ui::rowsCharacter(std::vector<Row>& out) {
    const Character& c = s_.character();
    const auto& b = s_.content().balance;
    out.push_back({"Уровень " + std::to_string(c.level) + " из " + std::to_string(b.maxLevel) + "   всего опыта " +
                       std::to_string(c.totalXp) + "   убийств " + std::to_string(c.kills),
                   color::kGold});
    out.push_back({"Очки характеристик: " + std::to_string(c.attrPoints) + "   очки навыков: " + std::to_string(c.skillPoints),
                   c.attrPoints ? color::kGood : color::kText});
    for (int i = 0; i < kAttrCount; ++i) {
        Attr a = static_cast<Attr>(i);
        Row r;
        r.text = std::string(attrName(a)) + ": " + std::to_string(c.attrs[i]) + (c.attrPoints ? "   [+]" : "");
        r.selectable = true;
        r.detail = "За очко: " + b.perAttrPoint[i].describe();
        r.act = [this, a] {
            std::string why;
            report(s_.spendAttribute(a, &why), why);
        };
        out.push_back(r);
    }
    out.push_back({"Итоговые бонусы:", color::kGold});
    std::string all = s_.totalModifiers().describe();
    out.push_back({all.empty() ? "нет" : all, color::kText});
    Row respec;
    respec.text = confirmRespec_ ? "Сбросить навыки и характеристики? Enter ещё раз — подтвердить" : "Сбросить навыки и характеристики";
    respec.color = confirmRespec_ ? color::kBad : color::kDim;
    respec.selectable = true;
    respec.act = [this] {
        if (!confirmRespec_) {
            confirmRespec_ = true;
            return;
        }
        confirmRespec_ = false;
        std::string why;
        report(s_.respec(&why), why);
    };
    out.push_back(respec);
}

void Ui::rowsSkills(std::vector<Row>& out) {
    const Character& c = s_.character();
    out.push_back({"Очки навыков: " + std::to_string(c.skillPoints), c.skillPoints ? color::kGood : color::kText});
    for (const auto& tree : s_.content().trees) {
        out.push_back({treeName(tree), treeColor(tree)});
        for (const auto& id : s_.content().skillOrder) {
            const SkillNode& n = s_.content().skills.at(id);
            if (n.tree != tree) continue;
            auto it = c.skillRanks.find(id);
            int rank = it == c.skillRanks.end() ? 0 : it->second;
            std::string why;
            bool can = s_.canLearnSkill(id, &why);
            Row r;
            r.text = "  " + n.name + "  " + std::to_string(rank) + "/" + std::to_string(n.maxRank) +
                     (n.reqLevel > 1 ? "  (ур. " + std::to_string(n.reqLevel) + ")" : "");
            r.color = rank >= n.maxRank ? color::kGold : can ? color::kGood : color::kDim;
            r.selectable = true;
            r.detail = n.desc + (n.perRank.describe().empty() ? "" : "  За ранг: " + n.perRank.describe()) +
                       (can || rank >= n.maxRank ? "" : "  — " + why);
            r.act = [this, id] {
                std::string w;
                report(s_.learnSkill(id, &w), w);
            };
            out.push_back(r);
        }
    }
}

void Ui::rowsAbilities(std::vector<Row>& out) {
    const Character& c = s_.character();
    std::string bound;
    for (int i = 0; i < kAbilitySlots; ++i)
        bound += "F" + std::to_string(i + 1) + ": " + (c.slots[i].empty() ? "—" : s_.content().abilities.at(c.slots[i]).name) + "   ";
    out.push_back({bound, color::kGold});
    for (const auto& id : s_.content().abilityOrder) {
        const AbilityDef& a = s_.content().abilities.at(id);
        bool learned = c.unlockedAbilities.count(id) > 0;
        bool supported = s_.abilitySupported(a);
        Row r;
        r.text = a.name + "   Сила " + num(s_.effectiveCost(a)) + ", перезарядка " + num(s_.effectiveCooldown(a)) + " с" +
                 (learned ? "" : "   (не изучена)") + (supported ? "" : "   [нет адресов]");
        r.color = !learned ? color::kDim : supported ? color::kText : color::kBad;
        r.selectable = true;
        r.detail = a.desc + (learned ? "  Нажмите F1-F4, чтобы назначить в слот." : "  Открывается навыком.");
        if (learned)
            r.bind = [this, id](int slot) {
                std::string why;
                report(s_.bindAbility(slot, id, &why), why);
            };
        out.push_back(r);
    }
}

void Ui::rowsMap(std::vector<Row>& out) {
    const Character& c = s_.character();
    const auto& content = s_.content();
    auto here = content.locations.find(c.currentLocation);
    out.push_back({"Вы здесь: " + (here == content.locations.end() ? std::string("неизвестно") : here->second.name), color::kGold});
    for (const auto& id : content.locationOrder) {
        const Location& l = content.locations.at(id);
        bool open = s_.locationUnlocked(id);
        bool visited = c.visitedLocations.count(id) > 0;
        Row r;
        r.text = l.name + " — " + l.planet + "   ур. " + std::to_string(l.tier) + (id == c.currentLocation ? "   ◄ вы здесь" : "") +
                 (open ? (visited ? "" : "   новое!") : "   закрыто");
        r.color = !open ? color::kDim : id == c.currentLocation ? color::kGold : visited ? color::kText : color::kGood;
        r.selectable = true;
        std::string req;
        if (!open) {
            req = "  Нужно: уровень " + std::to_string(l.reqLevel);
            if (!l.reqQuest.empty()) req += ", задание «" + content.quests.at(l.reqQuest).name + "»";
        }
        std::string links;
        for (const auto& ln : l.links) links += (links.empty() ? "" : ", ") + content.locations.at(ln).name;
        r.detail = l.desc + req + "  Маршруты: " + links;
        r.act = [this, id] {
            std::string why;
            bool ok = s_.travel(id, &why);
            report(ok, why);
            if (ok) setOpen(false);
        };
        out.push_back(r);
    }
}

void Ui::rowsQuests(std::vector<Row>& out) {
    const Character& c = s_.character();
    const auto& content = s_.content();
    out.push_back({"Активные", color::kGold});
    for (const auto& id : content.questOrder) {
        const QuestDef& q = content.quests.at(id);
        if (!s_.questAvailable(q)) continue;
        Row r;
        auto it = c.questProgress.find(id);
        int p = it == c.questProgress.end() ? 0 : it->second;
        std::string progress = q.kind == QuestKind::Kill ? "   " + std::to_string(p) + "/" + std::to_string(q.count)
                               : q.kind == QuestKind::ReachLevel ? "   ур. " + std::to_string(c.level) + "/" + std::to_string(q.count)
                                                                 : "";
        r.text = "  " + q.name + progress;
        r.selectable = true;
        std::string reward = "Награда: " + std::to_string(q.rewardXp) + " опыта";
        if (q.rewardSkillPoints) reward += ", " + std::to_string(q.rewardSkillPoints) + " очк. навыков";
        if (q.rewardAttrPoints) reward += ", " + std::to_string(q.rewardAttrPoints) + " очк. характеристик";
        if (!q.rewardCrystal.empty()) reward += ", " + content.crystals.at(q.rewardCrystal).name;
        r.detail = q.desc + "  " + reward;
        out.push_back(r);
    }
    out.push_back({"Выполнено: " + std::to_string(c.questsDone.size()), color::kDim});
}

void Ui::rowsCrystals(std::vector<Row>& out) {
    const Character& c = s_.character();
    auto equipped = [](const Crystal& cr) { return cr.base.empty() ? std::string("—") : cr.name; };
    out.push_back({"Цвет: " + equipped(c.equippedColor), color::kGold});
    if (!c.equippedColor.base.empty()) out.push_back({"   " + c.equippedColor.mods.describe(), color::kDim});
    out.push_back({"Фокус: " + equipped(c.equippedFocus), color::kGold});
    if (!c.equippedFocus.base.empty()) out.push_back({"   " + c.equippedFocus.mods.describe(), color::kDim});
    out.push_back({"Инвентарь " + std::to_string(c.inventory.size()) + "/" + std::to_string(s_.content().balance.inventorySize), color::kText});
    for (size_t i = 0; i < c.inventory.size(); ++i) {
        const Crystal& cr = c.inventory[i];
        Row r;
        r.text = std::string(cr.slot == "color" ? "[цвет] " : "[фокус] ") + cr.name + "  (" + rarityName(cr.rarity) +
                 ", ур. " + std::to_string(cr.itemLevel) + ")";
        r.color = cr.rarity == Rarity::Legendary ? color::kGold : cr.rarity == Rarity::Rare ? color::kLight : color::kText;
        r.selectable = true;
        r.detail = cr.mods.describe() + "  Enter — вставить в меч.";
        r.act = [this, i] {
            std::string why;
            report(s_.equipCrystal(i, &why), why);
        };
        out.push_back(r);
    }
}

}  // namespace tfuplugin
