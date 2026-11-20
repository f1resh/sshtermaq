// Installed fonts on macOS: NSFontManager's families (WKWebView has no Local Font Access API). A family is
// monospace when its first member (the regular face, as NSFontManager orders them) has the fixed-pitch trait.
#import <AppKit/AppKit.h>

#include "platform/ui/Fonts.h"

#include <algorithm>

namespace st::ui::fonts {

std::vector<FontFamily> installedFamilies() {
    std::vector<FontFamily> out;
    NSFontManager* manager = NSFontManager.sharedFontManager;
    for (NSString* family in manager.availableFontFamilies) {
        if ([family hasPrefix:@"."]) continue;  // ".SF NS..." are private system fonts
        bool monospace = false;
        // Each member: @[PostScript name, style name, weight (NSNumber), traits (NSNumber)].
        NSArray* members = [manager availableMembersOfFontFamily:family];
        if (members.count > 0) {
            NSArray* first = members[0];
            if (first.count > 3 && [first[3] isKindOfClass:NSNumber.class])
                monospace = ([first[3] unsignedIntegerValue] & NSFixedPitchFontMask) != 0;
        }
        out.push_back({family.UTF8String, monospace});
    }
    std::sort(out.begin(), out.end(), [](const FontFamily& a, const FontFamily& b) { return a.name < b.name; });
    return out;
}

}  // namespace st::ui::fonts
