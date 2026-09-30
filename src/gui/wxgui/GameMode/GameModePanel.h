#pragma once

#include "GameModeBackend.h"

#include <wx/bitmap.h>
#include <wx/panel.h>
#include <wx/timer.h>

#include <map>
#include <memory>
#include <vector>

class wxGraphicsContext;

// Game Mode launcher: a full-window, controller-first UI modelled on the Cemu Android
// (MH3U Revival) app. Everything is drawn by hand so it looks the same on every platform and
// needs no native focus handling, which is what makes it drivable by a controller alone.
//
// Screens: the game library, a per-game options page (play, favourite, screen layout, custom
// textures, texture packs, cheats) and Settings (graphics, audio, input, exit Game Mode).
// Navigation: D-pad/stick, A accept, B back, X game options, Y settings. Keyboard (arrows,
// Enter, Esc/Backspace, X, Y) and mouse (hover, click, right-click, wheel) work too.
class GameModePanel : public wxPanel
{
public:
	GameModePanel(wxWindow* parent, std::unique_ptr<GameMode::Backend> backend);
	~GameModePanel() override;

	// Re-reads the game list (e.g. after a scan finished).
	void RefreshGames();

	// For tests and scripted walkthroughs.
	void InjectNav(GameMode::Nav nav) { HandleNav(nav); }
	int GetPageDepth() const { return (int)m_pages.size(); }
	// Draws the whole UI into dc at the panel's current size (OnPaint uses it too).
	void Render(wxDC& dc);

private:
	enum class RowKind
	{
		Header,
		Action,
		Toggle,
		Choice,
		Slider,
		Link,
		Info,
	};

	struct Row
	{
		RowKind kind = RowKind::Action;
		wxString label;
		wxString description;
		bool danger = false;
		std::function<bool()> isEnabled;
		// Toggle
		std::function<bool()> getBool;
		std::function<void(bool)> setBool;
		// Choice
		std::function<GameMode::Choice()> getChoice;
		std::function<void(int)> setChoice;
		// Slider
		std::function<int()> getInt;
		std::function<void(int)> setInt;
		int minValue = 0, maxValue = 100, step = 5;
		// Action / Link
		std::function<void()> action;
		// Right-hand text for Action/Link rows
		std::function<wxString()> getValueText;

		bool Focusable() const { return kind != RowKind::Header && kind != RowKind::Info; }
		bool Enabled() const { return !isEnabled || isEnabled(); }
	};

	struct Page
	{
		bool isLibrary = false;
		bool isSettingsRoot = false;
		wxString title;
		uint64_t headerTitleId = 0; // shows the game's icon and name above the rows
		std::function<std::vector<Row>()> build;
		std::vector<Row> rows;
		int focus = 0;
		double scroll = 0, scrollTarget = 0;
	};

	struct Dialog
	{
		enum class Type { None, Choice, Confirm, Capture } type = Type::None;
		wxString title;
		wxString message;
		std::vector<wxString> options;
		int focus = 0;
		std::function<void(int)> onChoose; // Choice: option index. Confirm: 1 = yes.
		wxLongLong startedMs = 0;
	};

	struct HitRect
	{
		wxRect rect;
		int index; // row/tile index, or -(1 + Nav) for hint buttons
	};

	// navigation
	void HandleNav(GameMode::Nav nav);
	void HandleLibraryNav(GameMode::Nav nav);
	void HandleListNav(Page& page, GameMode::Nav nav);
	void HandleDialogNav(GameMode::Nav nav);
	void ActivateRow(Row& row);
	void AdjustRow(Row& row, int direction);
	void MoveListFocus(Page& page, int direction);

	// pages
	void PushPage(Page page);
	void PopPage();
	Page& CurrentPage() { return m_pages.back(); }
	void RebuildCurrent();
	Page MakeGamePage(const GameMode::GameEntry& game);
	Page MakeTexturePacksPage(uint64_t titleId);
	Page MakeCheatsPage(uint64_t titleId);
	Page MakeSettingsPage();
	Page MakeGraphicsPage();
	Page MakeAudioPage();
	Page MakeInputPage();

	void OpenChoiceDialog(const wxString& title, const GameMode::Choice& choice, std::function<void(int)> onChoose);
	void OpenConfirmDialog(const wxString& title, const wxString& message, std::function<void()> onYes);
	void StartGameMenuCapture();
	void ShowToast(const wxString& message);

	// drawing
	double S(double v) const { return v * m_scale; }
	void OnPaint(wxPaintEvent& event);
	void DrawTopBar(wxGraphicsContext* gc, const wxRect& area);
	void DrawHints(wxGraphicsContext* gc, const wxRect& area);
	void DrawLibrary(wxGraphicsContext* gc, const wxRect& area);
	void DrawList(wxGraphicsContext* gc, Page& page, const wxRect& area);
	void DrawRow(wxGraphicsContext* gc, const Row& row, const wxRect& rect, bool focused);
	void DrawDialog(wxGraphicsContext* gc, const wxRect& area);
	void DrawToast(wxGraphicsContext* gc, const wxRect& area);
	void DrawIcon(wxGraphicsContext* gc, uint64_t titleId, const wxString& name, const wxRect& rect, double radius);
	void DrawGlyph(wxGraphicsContext* gc, const wxString& letter, double cx, double cy, double radius);
	wxBitmap GetScaledIcon(uint64_t titleId, int size);
	int LibraryColumns(int width) const;
	double RowHeight(const Row& row) const;
	std::vector<std::pair<GameMode::Nav, wxString>> CurrentHints() const;

	// events
	void OnTimer(wxTimerEvent& event);
	void OnKeyDown(wxKeyEvent& event);
	void OnMouseMove(wxMouseEvent& event);
	void OnMouseDown(wxMouseEvent& event);
	void OnMouseWheel(wxMouseEvent& event);
	void OnSize(wxSizeEvent& event);
	int HitTest(const wxPoint& pos) const;

	std::unique_ptr<GameMode::Backend> m_backend;
	wxTimer m_timer;
	double m_scale = 1.0;

	std::vector<Page> m_pages;
	Dialog m_dialog;
	std::vector<GameMode::GameEntry> m_games;
	int m_libraryFocus = 0;
	double m_libraryScroll = 0, m_libraryScrollTarget = 0;
	bool m_wasScanning = false;
	int m_scanTick = 0;

	std::map<uint64_t, wxImage> m_icons;
	std::map<std::pair<uint64_t, int>, wxBitmap> m_iconCache;

	wxString m_toast;
	wxLongLong m_toastUntilMs = 0;

	std::vector<HitRect> m_hitRects;
	wxPoint m_lastMouse{-1, -1};
	std::vector<GameMode::Nav> m_navScratch;
	std::shared_ptr<bool> m_alive = std::make_shared<bool>(true);
};
