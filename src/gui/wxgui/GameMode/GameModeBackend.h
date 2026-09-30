#pragma once

// Everything the Game Mode launcher needs from the emulator, behind one interface.
//
// The launcher (GameModePanel) only talks to this, never to Cemu directly. GameModeBackendCemu is
// the real implementation. Keeping the seam narrow also lets the launcher be built and exercised on
// its own with fake data, which is how its screens are checked without a full emulator build.

#include <wx/image.h>
#include <wx/string.h>

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace GameMode
{
	// Navigation actions. Face buttons are Wii U buttons as mapped in each player's input profile,
	// so "A" in the menus is whatever the player presses for A in games.
	enum class Nav
	{
		Up,
		Down,
		Left,
		Right,
		Accept,  // A
		Back,    // B
		Options, // X
		Settings // Y
	};

	// Which physical face button a menu action is on, so the hints can show the right glyph.
	enum class Face
	{
		South,
		East,
		West,
		North,
	};

	// Button glyph families for the hints
	enum class ButtonStyle
	{
		Nintendo = 0,
		Xbox = 1,
		PlayStation = 2,
		SteamDeck = 3,
		Count
	};

	// One Wii U button of a player's controller and what it is bound to
	struct MappingEntry
	{
		uint64_t id = 0;
		wxString name;  // "A", "ZL", "Left stick up", ...
		wxString bound; // physical button name, empty when unbound
	};

	struct GameEntry
	{
		uint64_t titleId = 0;
		wxString name;
		bool favorite = false;
	};

	struct NamedToggle
	{
		wxString name;
		bool enabled = false;
	};

	// A setting with a fixed list of values.
	struct Choice
	{
		std::vector<wxString> options;
		int selected = 0;
	};

	class Backend
	{
	public:
		virtual ~Backend() = default;

		// ---- library ----
		virtual std::vector<GameEntry> ListGames() = 0; // favourites first, then by name
		virtual bool IsScanningGames() = 0;
		// Loaded asynchronously. onLoaded runs on the UI thread; it may never run if the title has no icon.
		virtual void RequestIcon(uint64_t titleId, std::function<void(uint64_t, const wxImage&)> onLoaded) = 0;
		virtual void LaunchGame(uint64_t titleId) = 0;
		virtual void SetFavorite(uint64_t titleId, bool favorite) = 0;

		// ---- per game ----
		// index 0 is "Use default (...)"; 1.. are the layouts
		virtual Choice GetGameScreenLayout(uint64_t titleId) = 0;
		virtual void SetGameScreenLayout(uint64_t titleId, int choiceIndex) = 0;
		virtual bool GetCustomTexturesEnabled(uint64_t titleId) = 0;
		virtual void SetCustomTexturesEnabled(uint64_t titleId, bool enabled) = 0;
		virtual std::vector<NamedToggle> GetTexturePacks(uint64_t titleId) = 0;
		virtual void SetTexturePackEnabled(uint64_t titleId, const wxString& pack, bool enabled) = 0;
		virtual std::vector<NamedToggle> GetCheats(uint64_t titleId) = 0;
		// Returns an error message when the cheat could not be switched (e.g. invalid code).
		virtual std::optional<wxString> SetCheatEnabled(uint64_t titleId, size_t index, bool enabled) = 0;

		// ---- graphics ----
		virtual Choice GetGraphicsApi() = 0;
		virtual void SetGraphicsApi(int index) = 0;
		virtual Choice GetVSync() = 0; // options depend on the graphics API
		virtual void SetVSync(int index) = 0;
		virtual Choice GetDefaultScreenLayout() = 0;
		virtual void SetDefaultScreenLayout(int index) = 0;
		virtual Choice GetUpscaleFilter() = 0;
		virtual void SetUpscaleFilter(int index) = 0;
		virtual Choice GetDownscaleFilter() = 0;
		virtual void SetDownscaleFilter(int index) = 0;
		virtual bool GetFullscreen() = 0;
		virtual void SetFullscreen(bool enabled) = 0;

		// ---- audio (0..100) ----
		virtual int GetTvVolume() = 0;
		virtual void SetTvVolume(int volume) = 0;
		virtual int GetPadVolume() = 0;
		virtual void SetPadVolume(int volume) = 0;

		// ---- input ----
		virtual int GetPlayerCount() = 0;
		// options[0] is the player's current setup ("Current: ..."); the rest are saved profiles
		virtual Choice GetPlayerProfile(int player) = 0;
		// Returns an error message if the profile could not be loaded.
		virtual std::optional<wxString> SetPlayerProfile(int player, int choiceIndex) = 0;
		virtual wxString GetGameMenuBindingLabel() = 0;
		// Binding capture: call BeginCapture, then PollCapturedButton every frame until it returns
		// true (a controller button was bound) or the caller gives up. Keys go to BindGameMenuKey.
		virtual void BeginGameMenuCapture() = 0;
		virtual bool PollGameMenuCapture() = 0;
		virtual void BindGameMenuKey(int wxKeyCode, bool alt, bool ctrl, bool shift) = 0;

		// Per player setup (the remapper). Only used with no game running.
		virtual Choice GetPlayerControllerType(int player) = 0; // empty options when disabled
		virtual std::optional<wxString> SetPlayerControllerType(int player, int index) = 0;
		virtual Choice GetPlayerDevice(int player) = 0; // connected controllers
		virtual std::optional<wxString> SetPlayerDevice(int player, int index) = 0;
		virtual void RefreshDevices() = 0;
		virtual std::vector<MappingEntry> GetMappings(int player) = 0;
		virtual void ClearMapping(int player, uint64_t mapping) = 0;
		virtual void ResetMappings(int player) = 0;
		// Waits for a press on one of the player's devices; PollMappingCapture returns true once bound.
		virtual void BeginMappingCapture(int player, uint64_t mapping) = 0;
		virtual bool PollMappingCapture() = 0;
		virtual std::vector<wxString> GetProfileNames() = 0;
		// Saves the player's current setup as a profile. Returns an error message on failure.
		virtual std::optional<wxString> SaveProfile(int player, const wxString& name) = 0;
		virtual void ResetGameMenuBinding() = 0;

		// ---- Game Mode preferences ----
		virtual bool GetAlwaysBootGameMode() = 0;
		virtual void SetAlwaysBootGameMode(bool enabled) = 0;
		virtual ButtonStyle GetButtonStyle() = 0;
		virtual void SetButtonStyle(ButtonStyle style) = 0;
		// Physical position of the button behind a menu action for player 1's mapping.
		virtual Face GetNavFace(Nav nav) = 0;

		// ---- navigation ----
		// Called every frame; appends navigation edges from the players' controllers (with repeat
		// for held directions). Keyboard and mouse are handled by the panel itself.
		virtual void PollControllerNav(std::vector<Nav>& out) = 0;

		// ---- Game Mode ----
		virtual void ExitGameMode() = 0;
	};
}
