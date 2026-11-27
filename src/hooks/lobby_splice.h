#pragma once
// Pure text edits of MAINSCREEN.INI and MenuSystem.ini for the multiplayer lobby (no game calls, unit tested).
// See lazy-sf2-recon: recon-notes §28.
#include <string>

namespace LazyWorks::SF2Patch::hooks {

/// <summary>The screen the stock (hidden) Multiplayer button goes to: the lobby's connect screen.</summary>
inline constexpr wchar_t kLobbyScreen[] = L"NetworkConnectScreen";
/// <summary>The lobby's screens while hosting and while joined (same menu code, see lobby_screen.cpp).</summary>
inline constexpr wchar_t kLobbyHostScreen[] = L"LazyLobbyHostScreen";
inline constexpr wchar_t kLobbyJoinScreen[] = L"LazyLobbyJoinScreen";

/// <summary>Our English strings, answered by the text hook (menu_hooks.cpp). Keys stay under ~31 characters.</summary>
struct LobbyText {
    const wchar_t* key;
    const wchar_t* text;
};
inline constexpr LobbyText kLobbyTexts[] = {
    {L"TXT_LAZY_LOBBY_STATUS", L"Not connected."},
    {L"TXT_LAZY_LOBBY_JOIN_INFO", L"Join the game at the address above."},
    {L"TXT_LAZY_LOBBY_NET_HINT1", L"Internet play: the host opens UDP port 7777,"},
    {L"TXT_LAZY_LOBBY_NET_HINT2", L"or both players use Tailscale."},
    {L"TXT_LAZY_LOBBY_CLOSE", L"Close session"},
    {L"TXT_LAZY_LOBBY_CLOSE_INFO", L"Close the session and return to the Multiplayer screen."},
    {L"TXT_LAZY_LOBBY_LAUNCH", L"Launch"},
    {L"TXT_LAZY_LOBBY_LAUNCH_INFO", L"Start the mission for every ready pilot."},
    {L"TXT_LAZY_LOBBY_SETUP", L"Set up mission..."},
    {L"TXT_LAZY_LOBBY_SETUP_INFO", L"Choose the mission on the Single Mission screen."},
    {L"TXT_LAZY_LOBBY_LEAVE", L"Leave"},
    {L"TXT_LAZY_LOBBY_LEAVE_INFO", L"Leave the session and return to the Multiplayer screen."},
    {L"TXT_LAZY_LOBBY_READY", L"Ready"},
    {L"TXT_LAZY_LOBBY_READY_INFO", L"Tell the host you are ready to fly."},
    {L"TXT_LAZY_LOBBY_BRIEFING", L"Briefing..."},
    {L"TXT_LAZY_LOBBY_BRIEFING_INFO", L"Read the host's mission in the Hangar."},
    {L"TXT_LAZY_LOBBY_MISSION", L"Mission"},
    {L"TXT_LAZY_LOBBY_HOST_MISSION", L"Host's mission"},
    {L"TXT_LAZY_LOBBY_PILOTS", L"Pilots"},
    {L"TXT_LAZY_LOBBY_SEAT", L"Seat"},
    {L"TXT_LAZY_LOBBY_COL_STATUS", L"Status"},
};

/// <summary>MenuItemNNN numbers of the connect screen's items (the menu code addresses them by number).</summary>
namespace lobby_item {
inline constexpr int kBack = 1;  // also what Esc reports (menu-code click idiom)
inline constexpr int kHost = 2;
inline constexpr int kJoin = 3;
inline constexpr int kCallsign = 7;
inline constexpr int kAddress = 9;  // the lobby's only EDIT field: identifies the connect screen
inline constexpr int kStatus = 11;
inline constexpr int kCount = 13;
}  // namespace lobby_item

/// <summary>Item numbers shared by the host and join screens.</summary>
namespace session_item {
inline constexpr int kLeave = 1;          // Close session / Leave; also Esc
inline constexpr int kAction = 2;         // Launch / Ready
inline constexpr int kMissionButton = 3;  // Set up mission... / Briefing...
inline constexpr int kHeader = 5;
inline constexpr int kSummary = 7;        // mission summary lines (TEXTAREA)
inline constexpr int kSeats = 12;         // pilot list columns (TEXTAREAs)
inline constexpr int kCallsigns = 13;
inline constexpr int kStates = 14;
inline constexpr int kStatus = 15;
inline constexpr int kYouFly = 16;  // join screen only: identifies it
inline constexpr int kHostCount = 15;
inline constexpr int kJoinCount = 16;
}  // namespace session_item

/// <summary>Characters the Address field holds (TextBufferSize minus the terminator).</summary>
inline constexpr int kLobbyAddressChars = 63;

/// <summary>
/// Shows the stock Multiplayer button: makes [MultiplayerHotspot] active and enabled and moves it into the gap between
/// Campaign and Pilot Record in Pilot Record's smaller style. Returns false and leaves `text` untouched if the section
/// is missing or does not go to the lobby screen.
/// </summary>
bool splice_main_screen_button(std::wstring& text);

/// <summary>
/// Appends the lobby's three screens (connect, host, join) and their items to MenuSystem.ini text, with
/// `menu_code_dll` (an absolute path) as their MenuCodeDLL. Returns false and leaves `text` untouched if a screen is
/// already defined or the path is not usable in an ini value.
/// </summary>
bool splice_lobby_screen(std::wstring& text, const std::wstring& menu_code_dll);

}  // namespace LazyWorks::SF2Patch::hooks
