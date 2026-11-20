// Installed fonts on Windows: DirectWrite's system font collection, which includes the fonts installed for the current
// user only (Windows 10 1809+). The names are DirectWrite's family names, the ones Chromium (WebView2) matches CSS
// font-family against; monospace is IDWriteFont1::IsMonospacedFont of the family's regular face.
#include "platform/ui/Fonts.h"

#include "platform/detail/WinApi.h"
#include "platform/win/sys/WinText.h"

#include <dwrite_1.h>
#include <wrl/client.h>

#include <algorithm>
#include <set>

namespace st::ui::fonts {

namespace {

using Microsoft::WRL::ComPtr;

/** The family's English name if it has one (as CSS sees it in this app), else its first name. */
std::wstring familyName(IDWriteFontFamily* family) {
    ComPtr<IDWriteLocalizedStrings> names;
    if (FAILED(family->GetFamilyNames(&names)) || names->GetCount() == 0) return {};
    UINT32 index = 0;
    BOOL exists = FALSE;
    if (FAILED(names->FindLocaleName(L"en-us", &index, &exists)) || !exists) index = 0;
    UINT32 length = 0;
    if (FAILED(names->GetStringLength(index, &length))) return {};
    std::wstring name(length + 1, L'\0');
    if (FAILED(names->GetString(index, name.data(), length + 1))) return {};
    name.resize(length);
    return name;
}

bool isMonospace(IDWriteFontFamily* family) {
    ComPtr<IDWriteFont> font;
    if (FAILED(family->GetFirstMatchingFont(DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STRETCH_NORMAL, DWRITE_FONT_STYLE_NORMAL,
                                            &font)))
        return false;
    ComPtr<IDWriteFont1> font1;
    return SUCCEEDED(font.As(&font1)) && font1->IsMonospacedFont();
}

}  // namespace

std::vector<FontFamily> installedFamilies() {
    ComPtr<IDWriteFactory> factory;
    if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                   reinterpret_cast<IUnknown**>(factory.GetAddressOf()))))
        return {};
    ComPtr<IDWriteFontCollection> collection;
    if (FAILED(factory->GetSystemFontCollection(&collection, TRUE))) return {};

    std::vector<FontFamily> out;
    std::set<std::string> seen;
    const UINT32 count = collection->GetFontFamilyCount();
    for (UINT32 i = 0; i < count; ++i) {
        ComPtr<IDWriteFontFamily> family;
        if (FAILED(collection->GetFontFamily(i, &family))) continue;
        const std::wstring wide = familyName(family.Get());
        if (wide.empty() || wide[0] == L'@') continue;  // "@MS Gothic": vertical-writing variants, not for a terminal
        std::string name = narrow(wide);
        if (!seen.insert(name).second) continue;
        out.push_back({std::move(name), isMonospace(family.Get())});
    }
    std::sort(out.begin(), out.end(), [](const FontFamily& a, const FontFamily& b) { return a.name < b.name; });
    return out;
}

}  // namespace st::ui::fonts
