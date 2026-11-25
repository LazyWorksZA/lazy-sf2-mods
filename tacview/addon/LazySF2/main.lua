-- LazySF2 terrain switch
-- Shows the ground of the recorded Strike Fighters 2 theatre and hides the other LazySF2 grounds, and all of them for other recordings.
-- Part of the lazy-sf2-mods winmm.dll mod. The mod writes this file; do not edit it here.

local Tacview = require("Tacview195")

local Prefix = "LazySF2."
local SettingName = "TheatreSwitch"

local currentAddOn = Tacview.AddOns.Current
currentAddOn.SetTitle("LazySF2")
currentAddOn.SetVersion("1.0.0")
currentAddOn.SetAuthor("lazy-sf2-mods")
currentAddOn.SetNotes("Shows the terrain of the Strike Fighters 2 theatre of the recording, with its airfield patches, and hides the other LazySF2 terrains.")

-- false: not applied yet; nil: no LazySF2 theatre; text: the theatre applied.
local lastTheatre = false
local reapply = false
local releasePending = false

local function IsSwitchEnabled()
	return currentAddOn.Settings.GetBoolean(SettingName, true)
end

local function OnToggle()
	local enabled = not IsSwitchEnabled()
	currentAddOn.Settings.SetBoolean(SettingName, enabled)
	-- Switched off: the next update shows every LazySF2 terrain again. Switched on: it applies the theatre again.
	releasePending = not enabled
	lastTheatre = false
end

local function StartsWith(text, prefix)
	return string.sub(text, 1, string.len(prefix)) == prefix
end

-- Shows the ids that belong to the theatre (the id itself and its "." patches); hides the other LazySF2 ids.
-- With no theatre, hides every LazySF2 id (they sit on real places), or shows them all when showAll is set.
-- Ids of other add-ons and games are left alone.
local function Apply(theatre, showAll, list, show, hide)
	if not list then
		return
	end

	for _, id in ipairs(list) do
		if StartsWith(id, Prefix) then
			if showAll or (theatre and (id == theatre or StartsWith(id, theatre .. "."))) then
				show(id)
			else
				hide(id)
			end
		end
	end
end

local function ApplyAll(theatre, showAll)
	Apply(theatre, showAll, Tacview.Terrain.GetTextureList(), Tacview.Terrain.ShowTexture, Tacview.Terrain.HideTexture)
	Apply(theatre, showAll, Tacview.Terrain.GetElevationMapList(), Tacview.Terrain.ShowElevationMap, Tacview.Terrain.HideElevationMap)
end

local function ReadText(name, absoluteTime)
	local index = Tacview.Telemetry.GetGlobalTextPropertyIndex(name, false)
	if index == Tacview.Telemetry.InvalidPropertyIndex then
		return nil
	end
	return Tacview.Telemetry.GetTextSample(0, absoluteTime, index)
end

local function OnUpdate(dt, absoluteTime)
	if not IsSwitchEnabled() then
		if releasePending then
			releasePending = false
			ApplyAll(nil, true)
		end
		return
	end

	local theatre = ReadText("LazySF2Theater", absoluteTime) or ReadText("Theater", absoluteTime)
	if theatre and not StartsWith(theatre, Prefix) then
		theatre = nil
	end

	if theatre == lastTheatre then
		-- Tacview's own switch may have reset the visibility in the frame of the change: apply once more.
		if reapply then
			reapply = false
			ApplyAll(theatre)
		end
		return
	end

	lastTheatre = theatre
	reapply = true
	ApplyAll(theatre)
	Tacview.Log.Info("LazySF2: terrain " .. (theatre or "no LazySF2 terrain"))
end

Tacview.UI.Menus.AddOption(nil, "LazySF2 theatre switch", IsSwitchEnabled(), OnToggle)
Tacview.Events.Update.RegisterListener(OnUpdate)
