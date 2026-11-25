#pragma once
// Pure text edit of OptionsScreen.ini for the Tacview Recording row on the Gameplay page (no game calls, unit tested).
#include <string>

namespace lazysf2::hooks {

/// <summary>Keys answered by the text hook (see menu_hooks.cpp).</summary>
inline constexpr wchar_t kTacviewLabelKey[] = L"TXT_LAZY_TACVIEW_RECORDING";
inline constexpr wchar_t kTacviewInfoKey[] = L"TXT_LAZY_TACVIEW_INFO";

/// <summary>
/// Adds the Tacview Recording row at the bottom of [GameplayOptionsSubMenu], as the next free MenuItem IDs. Returns
/// the new list's control ID (the text is the next ID), or 0 and leaves `text` untouched if the file is not as
/// expected or already carries the row.
/// </summary>
int splice_gameplay_row(std::wstring& text);

}  // namespace lazysf2::hooks
