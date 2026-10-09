// In-game RPG menu and HUD as plain text lines. No Windows or D3D here: the overlay draws the lines,
// and tests drive the menu with keys on any platform.
#pragma once

#include <cstdint>
#include <deque>
#include <functional>
#include <string>
#include <vector>

#include "tfurpg/session.hpp"

namespace tfuplugin {

enum class Key { Menu, Left, Right, Up, Down, Enter, Back, Slot1, Slot2, Slot3, Slot4 };

namespace color {
constexpr uint32_t kText = 0xFFE8E8E8;
constexpr uint32_t kDim = 0xFF8A8A8A;
constexpr uint32_t kGold = 0xFFFFC84A;
constexpr uint32_t kGood = 0xFF7CFC7C;
constexpr uint32_t kBad = 0xFFFF6A5A;
constexpr uint32_t kSelect = 0xFF6AD8FF;
constexpr uint32_t kDark = 0xFFFF5050;
constexpr uint32_t kLight = 0xFF70B8FF;
}  // namespace color

struct UiLine {
    std::string text;
    uint32_t color = color::kText;
};

class Ui {
public:
    explicit Ui(tfurpg::Session& s) : s_(s) {}

    bool isOpen() const { return open_; }
    void setOpen(bool v);
    // Ability keys (Slot1-4) cast when the menu is closed; inside the Abilities tab they bind instead.
    void onKey(Key k);
    // Moves new session messages into timed toasts.
    void update(double now);

    std::vector<UiLine> hud(double now) const;
    std::vector<UiLine> panel();
    int tab() const { return tab_; }
    static constexpr int kTabCount = 6;

private:
    struct Row {
        std::string text;
        uint32_t color = color::kText;
        bool selectable = false;
        std::string detail;
        std::function<void()> act;
        std::function<void(int)> bind;
    };
    std::vector<Row> rows();
    void rowsCharacter(std::vector<Row>& out);
    void rowsSkills(std::vector<Row>& out);
    void rowsAbilities(std::vector<Row>& out);
    void rowsMap(std::vector<Row>& out);
    void rowsQuests(std::vector<Row>& out);
    void rowsCrystals(std::vector<Row>& out);
    void report(bool ok, const std::string& reason);

    tfurpg::Session& s_;
    bool open_ = false;
    int tab_ = 0;
    int sel_ = 0;
    bool confirmRespec_ = false;
    std::string status_;
    uint32_t statusColor_ = color::kText;
    struct Toast {
        std::string text;
        double at;
    };
    std::deque<Toast> toasts_;
    double now_ = 0;
};

}  // namespace tfuplugin
