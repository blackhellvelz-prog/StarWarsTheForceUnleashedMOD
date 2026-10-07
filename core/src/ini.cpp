#include "tfurpg/ini.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <sstream>

namespace tfurpg {

std::string trim(const std::string& s) {
    size_t b = 0, e = s.size();
    while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
    return s.substr(b, e - b);
}

std::vector<std::string> split(const std::string& s, char sep) {
    std::vector<std::string> out;
    std::string cur;
    std::istringstream in(s);
    while (std::getline(in, cur, sep)) {
        cur = trim(cur);
        if (!cur.empty()) out.push_back(cur);
    }
    return out;
}

std::string toLower(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

bool Ini::loadFile(const std::string& path, std::string* err) {
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        if (err) *err = "cannot open " + path;
        return false;
    }
    std::stringstream ss;
    ss << f.rdbuf();
    return loadString(ss.str(), err);
}

bool Ini::loadString(const std::string& text, std::string* err) {
    data_.clear();
    std::istringstream in(text);
    std::string line;
    std::string section;
    int lineNo = 0;
    while (std::getline(in, line)) {
        ++lineNo;
        if (lineNo == 1 && line.size() >= 3 && static_cast<unsigned char>(line[0]) == 0xEF &&
            static_cast<unsigned char>(line[1]) == 0xBB && static_cast<unsigned char>(line[2]) == 0xBF)
            line = line.substr(3);  // UTF-8 BOM
        std::string t = trim(line);
        if (t.empty() || t[0] == ';' || t[0] == '#') continue;
        if (t.front() == '[') {
            if (t.back() != ']') {
                if (err) *err = "line " + std::to_string(lineNo) + ": unterminated section";
                return false;
            }
            section = trim(t.substr(1, t.size() - 2));
            if (!findSection(section)) data_.push_back({section, {}});
            continue;
        }
        size_t eq = t.find('=');
        if (eq == std::string::npos) {
            if (err) *err = "line " + std::to_string(lineNo) + ": expected key = value";
            return false;
        }
        set(section, trim(t.substr(0, eq)), trim(t.substr(eq + 1)));
    }
    return true;
}

bool Ini::saveFile(const std::string& path) const {
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) return false;
    f << serialize();
    return static_cast<bool>(f);
}

std::string Ini::serialize() const {
    std::ostringstream out;
    for (const auto& s : data_) {
        if (!s.name.empty()) out << '[' << s.name << "]\n";
        for (const auto& e : s.entries) out << e.first << " = " << e.second << '\n';
        out << '\n';
    }
    return out.str();
}

Ini::Section* Ini::findSection(const std::string& name) {
    for (auto& s : data_)
        if (s.name == name) return &s;
    return nullptr;
}

const Ini::Section* Ini::findSection(const std::string& name) const {
    for (const auto& s : data_)
        if (s.name == name) return &s;
    return nullptr;
}

bool Ini::hasSection(const std::string& section) const { return findSection(section) != nullptr; }

bool Ini::has(const std::string& section, const std::string& key) const {
    const Section* s = findSection(section);
    if (!s) return false;
    return std::any_of(s->entries.begin(), s->entries.end(), [&](const Entry& e) { return e.first == key; });
}

std::string Ini::get(const std::string& section, const std::string& key, const std::string& def) const {
    const Section* s = findSection(section);
    if (!s) return def;
    for (const auto& e : s->entries)
        if (e.first == key) return e.second;
    return def;
}

double Ini::getNum(const std::string& section, const std::string& key, double def) const {
    std::string v = get(section, key);
    if (v.empty()) return def;
    char* end = nullptr;
    double d = std::strtod(v.c_str(), &end);
    return end == v.c_str() ? def : d;
}

int Ini::getInt(const std::string& section, const std::string& key, int def) const {
    return static_cast<int>(getNum(section, key, def));
}

bool Ini::getBool(const std::string& section, const std::string& key, bool def) const {
    std::string v = toLower(get(section, key));
    if (v.empty()) return def;
    return v == "1" || v == "true" || v == "yes" || v == "on";
}

std::vector<std::string> Ini::getList(const std::string& section, const std::string& key, char sep) const {
    return split(get(section, key), sep);
}

std::vector<std::string> Ini::sections() const {
    std::vector<std::string> out;
    for (const auto& s : data_) out.push_back(s.name);
    return out;
}

std::vector<std::string> Ini::sectionsWithPrefix(const std::string& prefix) const {
    std::vector<std::string> out;
    for (const auto& s : data_)
        if (s.name.compare(0, prefix.size(), prefix) == 0) out.push_back(s.name);
    return out;
}

const std::vector<Ini::Entry>& Ini::entries(const std::string& section) const {
    static const std::vector<Entry> empty;
    const Section* s = findSection(section);
    return s ? s->entries : empty;
}

void Ini::set(const std::string& section, const std::string& key, const std::string& value) {
    Section* s = findSection(section);
    if (!s) {
        data_.push_back({section, {}});
        s = &data_.back();
    }
    for (auto& e : s->entries)
        if (e.first == key) {
            e.second = value;
            return;
        }
    s->entries.emplace_back(key, value);
}

}  // namespace tfurpg
