#pragma once
// Pure text edit of OptionsScreen.ini for the "Other" Options page (no game calls, unit tested).
#include <string>

namespace lazysf2::hooks {

// X of the five visible tab labels (centred text, y = 120): Gameplay, Graphics, Sound, Controls, Other.
// Tune these live; the stock tabs sit at 597, 711, 824 and 938 with the Network tab hidden.
inline constexpr int kTabX[5] = {560, 660, 760, 860, 960};
inline constexpr int kTabY = 120;

// Keys answered by the text hook (see menu_hooks.cpp).
inline constexpr wchar_t kOtherTabKey[] = L"TXT_LAZY_OTHER_OPTIONS";
inline constexpr wchar_t kOtherTabInfoKey[] = L"TXT_LAZY_OTHER_OPTIONS_INFO";
inline constexpr wchar_t kTacviewLabelKey[] = L"TXT_LAZY_TACVIEW_RECORDING";
inline constexpr wchar_t kTacviewInfoKey[] = L"TXT_LAZY_TACVIEW_INFO";

// Activates the Other tab, re-spaces the tabs, sizes the Other page like the others and adds the
// Tacview Recording row. Returns the new list's control ID (the text is the next ID), or 0 and
// leaves `text` untouched if the file is not as expected or already carries the row.
int splice_other_page(std::wstring& text);

}  // namespace lazysf2::hooks
