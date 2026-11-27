#pragma once
// Pure text edits of MAINSCREEN.INI and MenuSystem.ini for the multiplayer lobby (no game calls, unit tested).
// See lazy-sf2-recon: recon-notes §28.
#include <string>

#include "net/world_proto.h"

namespace LazyWorks::SF2Patch::hooks {

/// <summary>The screen the stock (hidden) Multiplayer button goes to: the lobby's connect screen.</summary>
inline constexpr wchar_t kLobbyScreen[] = L"NetworkConnectScreen";
/// <summary>The lobby's screens while hosting and while joined (same menu code, see lobby_screen.cpp).</summary>
inline constexpr wchar_t kLobbyHostScreen[] = L"LazyLobbyHostScreen";
inline constexpr wchar_t kLobbyJoinScreen[] = L"LazyLobbyJoinScreen";

/// <summary>The initial text of items the menu code fills: a TEXT item created empty gets no text node, and one set later
/// is created hidden (StaticText SetText copies the old node's visibility), so these start as one space.</summary>
inline constexpr wchar_t kLobbyBlankKey[] = L"TXT_LAZY_LOBBY_BLANK";

/// <summary>Our English strings, answered by the text hook (menu_hooks.cpp). Keys stay under ~31 characters.</summary>
struct LobbyText {
    const wchar_t* key;
    const wchar_t* text;
};
inline constexpr LobbyText kLobbyTexts[] = {
    {kLobbyBlankKey, L" "},
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
    {L"TXT_LAZY_LOBBY_PASSWORD", L"Session password"},
    {L"TXT_LAZY_LOBBY_PASS_HINT", L"Required. The host sets it; players type the same."},
    {L"TXT_LAZY_LOBBY_DONE", L"Done"},
    {L"TXT_LAZY_LOBBY_DONE_INFO", L"Back to the lobby: the players get this mission."},
    {L"TXT_LAZY_LOBBY_HANGAR_READY", L"Ready: back to the lobby."},
    {L"TXT_LAZY_LOBBY_ROW_INFO", L"Click a free seat to take it."},
    {L"TXT_LAZY_LOBBY_SEAT_CALLSIGN", L"Seat and callsign"},
};

/// <summary>MenuItemNNN numbers of the connect screen's items (the menu code addresses them by number).</summary>
namespace lobby_item {
inline constexpr int kBack = 1;  // also what Esc reports (menu-code click idiom)
inline constexpr int kHost = 2;
inline constexpr int kJoin = 3;
inline constexpr int kCallsign = 7;
inline constexpr int kAddress = 9;  // an EDIT field: only the connect screen has one, which identifies it
inline constexpr int kStatus = 11;
inline constexpr int kPassword = 15;  // EDIT; not masked (the menu system's EDIT has no mask option)
inline constexpr int kCount = 16;
}  // namespace lobby_item

/// <summary>Item numbers shared by the host and join screens (the menu code tells them apart by the session's role).</summary>
namespace session_item {
inline constexpr int kLeave = 1;          // Close session / Leave; also Esc
inline constexpr int kAction = 2;         // Launch / Ready
inline constexpr int kMissionButton = 3;  // Set up mission... / Briefing...
inline constexpr int kHeader = 5;
inline constexpr int kSummary = 7;        // three lines: 7, 8, 9
inline constexpr int kStatus = 14;
inline constexpr int kYouFly = 15;
inline constexpr int kRowFirst = 16;      // kLobbyRows rows: a TEXTBUTTON ("#n  Callsign"; a click takes a free seat), a TEXT
inline constexpr int row_button(int r) { return kRowFirst + 2 * r; }
inline constexpr int row_status(int r) { return kRowFirst + 2 * r + 1; }
/// <summary>The row whose button is item, or -1.</summary>
inline constexpr int row_of_button(int item) {
    return item >= kRowFirst && item < kRowFirst + 2 * net::kLobbyRows && (item - kRowFirst) % 2 == 0 ? (item - kRowFirst) / 2 : -1;
}
inline constexpr int kCount = kRowFirst + 2 * net::kLobbyRows - 1;
}  // namespace session_item

/// <summary>Characters the Address and Session password fields hold (TextBufferSize minus the terminator).</summary>
inline constexpr int kLobbyAddressChars = 63;
inline constexpr int kLobbyPasswordChars = 31;

/// <summary>
/// Shows the stock Multiplayer button: makes [MultiplayerHotspot] active and enabled, in the large style one step below
/// Campaign, and moves Pilot Record and Options down to keep the spacing even (EXIT stays). Returns false and leaves
/// `text` untouched if the button's section is missing or does not go to the lobby screen.
/// </summary>
bool splice_main_screen_button(std::wstring& text);

/// <summary>
/// Appends the lobby's three screens (connect, host, join) and their items to MenuSystem.ini text, with
/// `menu_code_dll` (an absolute path) as their MenuCodeDLL. Returns false and leaves `text` untouched if a screen is
/// already defined or the path is not usable in an ini value.
/// </summary>
bool splice_lobby_screen(std::wstring& text, const std::wstring& menu_code_dll);

/// <summary>
/// SINGLEMISSIONSCREEN.INI while hosting a lobby session (L5): Cancel goes back to the host screen, and the Mission
/// Editor button is hidden. False (text untouched) if the file is not as expected.
/// </summary>
bool splice_single_mission_for_lobby(std::wstring& text);

/// <summary>
/// HANGARSCREEN.INI in a lobby session (L5, L6): Fly reads Done (host) or Ready (client), the client's Loadout button
/// is hidden, and a hidden button going back to the lobby screen is added as the next MenuItem. Returns that item's
/// number (the Hangar hook posts it instead of Fly), or 0 and leaves the text untouched if the file is not as expected.
/// </summary>
int splice_hangar_for_lobby(std::wstring& text, bool host);
/// <summary>DEBRIEFSCREEN.INI in a lobby session (L8): Exit (the Accept button) goes back to the host or join screen,
/// also through a hidden [LazyLobbyReturn] item (the screen's code sets Exit's destination itself); Refly and the
/// Mission Editor are moved off the screen. Returns the hidden item's number, or 0 (text untouched) if the file is not
/// as expected.</summary>
int splice_debrief_for_lobby(std::wstring& text, bool host);
/// <summary>The MenuItem number under which [screen] lists item, or 0.</summary>
int screen_item_number(const std::wstring& text, const wchar_t* screen, const wchar_t* item);

}  // namespace LazyWorks::SF2Patch::hooks
