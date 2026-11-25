#pragma once
// Pure text edit of OptionsScreen.ini for the "Other" Options page (no game calls, unit tested).
#include <string>

namespace lazysf2::hooks {

/// <summary>
/// Tab label positions (centred text): Gameplay, Graphics, Sound, Controls, Other. The underline bars under the four
/// stock tabs are part of the background image, so the stock tabs keep their x (597, 711, 824, 938) and move up a
/// little; "Other" sits on a second row under Gameplay, at the stock tab height. Tune these live.
/// </summary>
inline constexpr int kTabX[5] = {597, 711, 824, 938, 597};
inline constexpr int kTabY[5] = {94, 94, 94, 94, 120};

/// <summary>Keys answered by the text hook (see menu_hooks.cpp).</summary>
inline constexpr wchar_t kOtherTabKey[] = L"TXT_LAZY_OTHER_OPTIONS";
inline constexpr wchar_t kOtherTabInfoKey[] = L"TXT_LAZY_OTHER_OPTIONS_INFO";
inline constexpr wchar_t kTacviewLabelKey[] = L"TXT_LAZY_TACVIEW_RECORDING";
inline constexpr wchar_t kTacviewInfoKey[] = L"TXT_LAZY_TACVIEW_INFO";

/// <summary>
/// Activates the Other tab, re-spaces the tabs, sizes the Other page like the others and adds the
/// Tacview Recording row. Returns the new list's control ID (the text is the next ID), or 0 and
/// leaves `text` untouched if the file is not as expected or already carries the row.
/// </summary>
int splice_other_page(std::wstring& text);

}  // namespace lazysf2::hooks
