#pragma once
// Installed fonts, for the terminal font picker (bridge call app:listFonts).
#include <string>
#include <vector>

namespace st::ui::fonts {

struct FontFamily {
    /** The family name as CSS font-family takes it ("Cascadia Mono", "JetBrains Mono"). */
    std::string name;
    /** Fixed-pitch: every character has the same width (what a terminal needs). */
    bool monospace = false;
};

/**
 * The font families installed on this computer, sorted by name, each once: fonts installed for all users and for the
 * current user only. Windows: DirectWrite's system font collection; macOS: NSFontManager. Empty if the system cannot
 * list them.
 */
std::vector<FontFamily> installedFamilies();

}  // namespace st::ui::fonts
