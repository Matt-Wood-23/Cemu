#pragma once

// Game Mode: a controller-first interface for the desktop build, modelled on the Cemu Android
// (MH3U Revival) app. Two parts:
//  - GameModePanel: the launcher (game library, per-game options, settings) that replaces the
//    game list while Game Mode is on.
//  - The Game Menu: a side menu drawn over the running game (GameMenuOverlay.cpp), opened with the
//    "Game Menu" hotkey (Guide button / F10 by default, see Hotkey settings).

#include "GameModeBackend.h"

#include <chrono>
#include <vector>

namespace GameMode
{
	// ---- Game Menu ----
	bool IsMenuOpen();
	// Opening is ignored while no game is running.
	void SetMenuOpen(bool open);
	void ToggleMenu();
	// True while the menu is open, and after it closes until every button is released, so the
	// press that closed the menu never reaches the game. The game's pad reads return nothing then.
	bool IsGameInputBlocked();
	// Keyboard navigation for the menu, queued from the UI thread and consumed by the overlay.
	void QueueMenuNav(Nav nav);
	// Draws the menu. Called on the GPU thread inside the ImGui frame of each window.
	void RenderMenu(bool mainWindow);
	// used by the overlay
	bool TakeMenuNav(Nav& nav);
	void ClearReleasePending();

	// "Fit (keep aspect ratio)", "Stretch", ... for a FullscreenScaling value
	wxString ScreenLayoutName(int layout);

	// Menu navigation read from the players' emulated controllers, so every player's own button
	// mapping applies ("A" is whatever they press for A in games). Directions repeat when held.
	class ControllerNav
	{
	public:
		// updateStates: poll the physical controllers first. Needed whenever the game is not
		// reading them itself (no game running, or the Game Menu blocking the game's reads).
		void Poll(std::vector<Nav>& out, bool updateStates);
		// Forget the current state; the next Poll only records it, so held buttons do not fire.
		void Reset() { m_primed = false; }
		// True if any button on any of the players' controllers is held (after the last Poll),
		// not only the navigation ones, e.g. a Guide button that is also mapped to HOME.
		bool AnyDown() const;

	private:
		static constexpr int kCount = 8;
		bool m_down[kCount]{};
		std::chrono::steady_clock::time_point m_nextRepeat[kCount]{};
		bool m_primed = false;
		bool m_anyRawDown = false;
	};
}
