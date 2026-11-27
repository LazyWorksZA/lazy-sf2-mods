#pragma once
// Pure text edits of MAINSCREEN.INI and MenuSystem.ini for the multiplayer lobby (no game calls, unit tested).
// See lazy-sf2-recon: recon-notes §28.
#include <string>

namespace LazyWorks::SF2Patch::hooks {

/// <summary>The screen the stock (hidden) Multiplayer button goes to; the lobby is defined under this name.</summary>
inline constexpr wchar_t kLobbyScreen[] = L"NetworkConnectScreen";

/// <summary>Keys answered by the text hook (see menu_hooks.cpp). String IDs are cut at about 31 characters.</summary>
inline constexpr wchar_t kLobbyStatusKey[] = L"TXT_LAZY_LOBBY_STATUS";
inline constexpr wchar_t kLobbyJoinInfoKey[] = L"TXT_LAZY_LOBBY_JOIN_INFO";

/// <summary>MenuItemNNN numbers of the lobby screen's items (the menu code addresses them by number).</summary>
namespace lobby_item {
inline constexpr int kBack = 1;  // also what Esc reports (menu-code click idiom)
inline constexpr int kHost = 2;
inline constexpr int kJoin = 3;
inline constexpr int kCallsign = 7;
inline constexpr int kAddress = 9;
inline constexpr int kStatus = 10;
inline constexpr int kPlayers = 12;
}  // namespace lobby_item

/// <summary>Characters the Address field holds (TextBufferSize minus the terminator).</summary>
inline constexpr int kLobbyAddressChars = 63;

/// <summary>
/// Shows the stock Multiplayer button: makes [MultiplayerHotspot] active and enabled and moves it into the gap between
/// Campaign and Pilot Record in Pilot Record's smaller style. Returns false and leaves `text` untouched if the section
/// is missing or does not go to the lobby screen.
/// </summary>
bool splice_main_screen_button(std::wstring& text);

/// <summary>
/// Appends the lobby screen ([NetworkConnectScreen] and its items) to MenuSystem.ini text, with `menu_code_dll` (an
/// absolute path) as its MenuCodeDLL. Returns false and leaves `text` untouched if the screen is already defined or the
/// path is not usable in an ini value.
/// </summary>
bool splice_lobby_screen(std::wstring& text, const std::wstring& menu_code_dll);

}  // namespace LazyWorks::SF2Patch::hooks
