// Minimal ordered INI reader/writer used for both content data and save files.
// Format: [section], key = value, comments start with ';' or '#'. UTF-8 text.
#pragma once

#include <string>
#include <utility>
#include <vector>

namespace tfurpg {

std::string trim(const std::string& s);
std::vector<std::string> split(const std::string& s, char sep);  // trims parts, drops empties
std::string toLower(std::string s);

class Ini {
public:
    using Entry = std::pair<std::string, std::string>;

    bool loadFile(const std::string& path, std::string* err = nullptr);
    bool loadString(const std::string& text, std::string* err = nullptr);
    bool saveFile(const std::string& path) const;
    std::string serialize() const;

    bool hasSection(const std::string& section) const;
    bool has(const std::string& section, const std::string& key) const;
    std::string get(const std::string& section, const std::string& key, const std::string& def = "") const;
    double getNum(const std::string& section, const std::string& key, double def = 0.0) const;
    int getInt(const std::string& section, const std::string& key, int def = 0) const;
    bool getBool(const std::string& section, const std::string& key, bool def = false) const;
    std::vector<std::string> getList(const std::string& section, const std::string& key, char sep = ',') const;

    // Section names in file order.
    std::vector<std::string> sections() const;
    std::vector<std::string> sectionsWithPrefix(const std::string& prefix) const;
    const std::vector<Entry>& entries(const std::string& section) const;

    void set(const std::string& section, const std::string& key, const std::string& value);
    void clear() { data_.clear(); }

private:
    struct Section {
        std::string name;
        std::vector<Entry> entries;
    };
    Section* findSection(const std::string& name);
    const Section* findSection(const std::string& name) const;
    std::vector<Section> data_;
};

}  // namespace tfurpg
