// Installed fonts (platform/ui/Fonts.h), the same contract on every OS: a sorted list without duplicates, with the
// OS's own terminal font marked monospace and its standard text font not (Windows: Consolas / Arial; macOS: Menlo /
// Helvetica).
#include "platform/ui/Fonts.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <set>
#include <string>

using namespace st::ui::fonts;

TEST_CASE("fonts: the installed families, with the monospace ones marked", "[platform][fonts]") {
    const auto list = installedFamilies();
    REQUIRE_FALSE(list.empty());

    std::set<std::string> names;
    for (const auto& f : list) {
        CHECK_FALSE(f.name.empty());
        CHECK(f.name[0] != '@');
        CHECK(names.insert(f.name).second);  // each family once
    }
    CHECK(std::is_sorted(list.begin(), list.end(), [](const FontFamily& a, const FontFamily& b) { return a.name < b.name; }));

    auto find = [&](const std::string& name) {
        return std::find_if(list.begin(), list.end(), [&](const FontFamily& f) { return f.name == name; });
    };
    // One of each pair comes with the OS.
    const auto mono = find("Consolas") != list.end() ? find("Consolas") : find("Menlo");
    const auto text = find("Arial") != list.end() ? find("Arial") : find("Helvetica");
    REQUIRE(mono != list.end());
    REQUIRE(text != list.end());
    CHECK(mono->monospace);
    CHECK_FALSE(text->monospace);
}
