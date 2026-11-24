# Option trace: the Graphics Options aspect-ratio list

This is the Phase 3 deliverable. It traces one existing graphics option end to end, so that Phase 4 can add a display-mode control the same way.

- Addresses are RVAs (offsets from the module base). `OptionsScreen.dll` is linked at `0x10000000`.
- Menu files were extracted with `tools/catx.py` to a folder outside the repo. Nothing of the game's data is reproduced here beyond the key and section names needed.

## Why AspectRatio, not LensFlare

LensFlare was the obvious first pick, because it's a three-value list with its own text keys. But in the final build its menu entries (`LensFlareList`, `LensFlareText`) are `MenuItemType=INACTIVE`: the code still fills the list, but the control is never shown. **AspectRatio** is an active `LIST` control with four values, so it's the template to copy.

## 1. Where the menu screen is defined

`Menu\MenuData.cat` contains the entry `OPTIONSSCREEN.INI`, a UTF-16 LE ini with 123 sections.

- **`[GraphicsOptionsSubMenu]`** (`MenuItemType=SUBMENU`) lists its children as `MenuItem001` … `MenuItem036`.
- **The control ID is the `NNN` in `MenuItemNNN`.** For example, `MenuItem035=AspectRatio` is the control with ID **35 (`0x23`)**, and `MenuItem036=AspectRatioText` is its label.
- **`[AspectRatio]`:**
  - `MenuItemType=LIST`
  - `Position=200,80`, `Width=240`, `Height=22`
  - `ListFieldSetting=MediumListField`, `DropDownListSize=10`
  - `InfoTextSetting=InfoTextSetting`, `InfoTextStringID=TXT_MENU_ASPECT_RATIO_INFO`
  - Some lists also set `GenerateEventMessage=TRUE` (for example `DisplayResolutionList`), so the screen is told the moment the selection changes.
- **`[AspectRatioText]`:**
  - `MenuItemType=TEXT`, `TextSetting=MediumTextLeft`
  - `TextStringID=TXT_MENU_DISPLAYASPECTRATIOTEXT`
  - `TextPosition=0,83`
- **Item types used in this file:** `LIST` ×35, `TEXT` ×47, `TEXTBUTTON` ×8, `TEXTLIST` ×6, `SUBMENU` ×6, `SLIDER` ×5 and `INACTIVE` ×14.
- **Spare IDs:** the next free slots in the graphics sub-menu are `MenuItem037` and later.

The game probes the mod folder before the cat (for example `Saved Games\ThirdWire\StrikeFighters2\Menu\MenuSystem.ini` and `Objects\*.ini`; see recon-notes §3). A loose `Menu\OptionsScreen.ini` in the mod folder would therefore probably override the cat copy. **Still to verify** with Process Monitor while the Options screen opens.

## 2. Where the strings come from

`Menu\MenuText.cat` contains the entry `OPTIONSSCREEN.STR`, a binary string table:

| Offset | Content |
|---|---|
| `0x00` | `u32` pool size in bytes |
| `0x04` | `u32` count |
| `0x08` | `count` × 32-byte ASCII keys, NUL-padded (a key can fill all 32 bytes) |
| `8 + 32×count` | the pool: `count` NUL-terminated 8-bit (cp1252) strings, in key order |

The keys AspectRatio uses are:
- `TXT_MENU_DISPLAYASPECTRATIOTEXT` (the label);
- `TXT_MENU_ASPECT_RATIO_1` to `_4` (the four values);
- `TXT_MENU_ASPECT_RATIO_INFO` (the hover help).

The Options screen asks for strings by `(section "OptionsScreen", key)`, through the `TXT` service.

## 3. The screen code: `Menu\OptionsScreen.dll`

The graphics sub-menu is a `SubMenuClass`-derived object. Its vtable is at **`+0x1e958`**:

| Slot | RVA | Role | What it does for AspectRatio |
|---|---|---|---|
| 1 | `+0x58f0` | **Init** | Sets the ini section to `GraphicsOptions`, then calls **Load** (`+0x5210`). Gets control `0x23` with type 8 = list, through `menu->GetItem(id, type, &ctrl)` (vtable `+0x28` of `this+4`). For i = 0…3 it calls `ctrl->AddItem(TXT->GetText(L"OptionsScreen", L"TXT_MENU_ASPECT_RATIO_<i+1>", i))` (TXT vtable `+0x1c`, control vtable `+0x20`). |
| 3 | `+0x5dd0` | **Refresh**: state to controls | `GetItem(0x23)`, then `ctrl->SetSelection(this->aspect)` (control vtable `+0x30`). `this->aspect` is at `this+0x58`. |
| 4 | `+0x4e20` | **Changed**: controls to state | `GetItem(0x23)`, then `this->aspect = ctrl->GetSelection()` (control vtable `+0x34`). |
| 2 | `+0x5da0` | **Exit** | Calls **Save** (`+0x5460`), then tears down. |
| 5 | `+0x50a0` | Event handler | Probably handles `GenerateEventMessage` controls, for example rebuilding the resolution list. |
| 0 | `+0x5f60` | Destructor | |

**Load (`+0x5210`)** reads the int keys with `ini_read_int(ini, section, key, default)` (`+0xa790`), from `DisplayDeviceID` through `Shadow`. For AspectRatio, it reads the float `AspectRatio` and maps it to a list index 0…3, stored at `this+0x58`. The rest are stored at `this+0x0c` and later; for example LensFlare is at `this+0x20` and DetailLevel at `this+0x24`.

**Save (`+0x5460`)** writes them back:
- `ini_write_int(ini, section, key, value)` (`+0xa0e0`) for the int keys;
- `ini_write_float` (`+0xa210`) for `AspectRatio`, which maps the index back to a ratio.

The ini object is the global at **`+0x26a90`**, the Options.ini service. The section-name buffer is the global at `+0x26880`, set to `GraphicsOptions` in Init.

**Interfaces used, by vtable offset:**
- **Menu** (`this+4`): `+0x28` is `GetItem(int id, int type, Control** out)`; type 8 means a list.
- **List control:** `+0x18` `GetCount()`, `+0x20` `AddItem(const wchar_t*)`, `+0x30` `SetSelection(int)`, `+0x34` `GetSelection()` (returns -1 for none).
- **TXT service** (global `+0x26a94`): `+0x1c` is `GetText(const wchar_t* section, const wchar_t* key, int)`. The third argument is the item index; its exact meaning is not confirmed.

## 4. Options.ini and engine startup

- The file is `Saved Games\ThirdWire\StrikeFighters2\Options.ini`, section `[GraphicsOptions]`, written by the Save above. The stock value on this machine is `AspectRatio=1.777778`.
- At startup, the exe reads `[GraphicsOptions]` at **`StrikeFighters2.exe +0x3af0`** using typed reads with defaults: int, float for `AspectRatio`, and bool for `ForceDX9`, `ForceVSyncOff` and `Use32BitZBuffer`. The values reach the graphics engine as part of the display-settings block (recon-notes §4.2).
- **Not fully traced:** the exact copy from the exe's options struct into GraphicsEngine's 15-DWORD block. Phase 4 doesn't need it, because our DLL can read its own key directly.

## 5. Recipe for Phase 4: a "Display Mode" list

Everything below can be done **from our DLL, without shipping modified ThirdWire files**:

1. **Menu data.** When the menu system loads `OPTIONSSCREEN.INI`, add these to the text in memory:
   - `MenuItem037=DisplayModeList` and `MenuItem038=DisplayModeText` in `[GraphicsOptionsSubMenu]`;
   - a `[DisplayModeList]` section modelled on `[AspectRatio]` (a free `Position`, and `InfoTextStringID=TXT_LAZY_DISPLAYMODE_INFO`);
   - a `[DisplayModeText]` section modelled on `[AspectRatioText]`.

   The fallback is a loose override file in the mod folder. That is easier, but it means distributing a modified copy of a game file.
2. **Strings.** Hook `TXT::GetText` (`+0x1c`) to answer our own keys: `TXT_LAZY_DISPLAYMODE` ("Display Mode"), `TXT_LAZY_DISPLAYMODE_FULLSCREEN` ("Fullscreen"), `TXT_LAZY_DISPLAYMODE_BORDERLESS` ("Borderless") and `TXT_LAZY_DISPLAYMODE_INFO`. Every other key passes through.
3. **Screen logic.** Hook the four graphics sub-menu methods once `OptionsScreen.dll` loads. Call the original in each, then handle control 37:
   - **Init:** `GetItem(37, 8)`, then two `AddItem` calls, then read `DisplayMode` from the ini object.
   - **Refresh:** `SetSelection(mode)`.
   - **Changed:** `mode = GetSelection()`.
   - **Exit:** `ini_write_int(ini, L"GraphicsOptions", L"DisplayMode", mode)` (`+0xa0e0`).
4. **Startup.** Our DLL reads `[GraphicsOptions] DisplayMode` from Options.ini itself.
   - `0`, missing or invalid gives stock exclusive fullscreen: no DXGI and window hooks.
   - `1` gives borderless.

## Phase 4 implementation (what was actually built)

The code is in `src/hooks/menu_hooks.cpp`. It differs from the recipe above in four ways:

- **Hook point for menu data: the exe's file service, not MenuSystem.** The file service vtable is at exe `+0x31d0c`:
  - `+0x10` is `Open(name, mode)` (RVA `+0x94e0`). Mode 0 looks in the mod folder first (`this+0x1c`), then the game folder (`this+0x224`), then the `.cat` files. Mode 1 is loose files only; mode 2 is cats only.
  - `+0x14` is `Close` (`+0x9590`).
  - `+0x18` is `Read(handle, buf, bytes)` (`+0x8270`).

  MenuSystem opens `.\OptionsScreen.INI` and reads it whole into a buffer of its size plus `0xf000` characters. We change that first read before MenuSystem's ini parser (`MenuSystem.dll +0x170f0`, then `+0x16d50`) sees it. This also confirms that a loose override in the mod folder would win.
- **No string-table hook is needed.** `TXT` slot 7 (`TextUSEnglish.dll +0x1f50`, `GetText(file, key)`, `ret 8`) returns the key itself when the key is unknown. So `TextStringID=Display Mode` displays literally. List items are passed straight to `AddItem(text, value)`.
- **The `AddItem` signature** is `AddItem(const wchar_t* text, int value)`. The third value seen in the decompiled `GetText` call is really `AddItem`'s second argument, which the compiler pushed first.
- **Layout.** The new row goes directly below Aspect Ratio, at `Position=200,120` and `TextPosition=0,123`, styled like Aspect Ratio. Every Graphics-page item at y≥120 moves down 40, so the lowest row now ends at y≈407 of 460. The control IDs are the next free `MenuItemNNN` numbers, which is 37 and 38 in stock.
- **Saving.** On Exit, before the stock save, we call `WriteInt(ini, L"GraphicsOptions", L"DisplayMode", v)` (`OptionsScreen.dll +0xa0e0`, a `__thiscall` with the ini object at `+0x26a90` as `this`). The game rewrites `Options.ini` straight away, and the key lands at the end of `[GraphicsOptions]`.
- **Startup.** `config::display_mode()` reads the key once, through `GetPrivateProfileIntW`, from `Saved Games\ThirdWire\<exe name>\Options.ini`. Only the value 1 means Borderless.
  - **It must not run under the loader lock.** A first version called it from the module-load watch, and `SHGetKnownFolderPath` deadlocked the game at startup. Now the window and DXGI hooks are always installed, and they check the mode when they're called.

**Checked on the machine** (over RustDesk): the control shows below Aspect Ratio, and the rows below it move neatly. Choosing Borderless saved `DisplayMode=1`, and the next start came up borderless. With the key missing, the game started in stock exclusive fullscreen.

## Open items

- ~~Mod-folder probing, the hook point, a free row, and when Options.ini is written~~: settled in Phase 4 (above).
- Other languages: the labels are literal English, because they don't go through the string table.
- Side finding for the network brief: `OPTIONSSCREEN.INI` still has a `NetworkOptionsButton` and an empty `NetworkOptionsSubMenu`, and the strings include `TXT_MENU_NETWORKOPTIONS_INFO` = "Set multiplayer settings."
