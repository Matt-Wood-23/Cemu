#include "GameModePanel.h"

#include <wx/clipbrd.h>
#include <wx/dataobj.h>
#include <wx/dcbuffer.h>
#include <wx/graphics.h>
#include <wx/time.h>

#include <algorithm>
#include <cmath>

using GameMode::Nav;

namespace
{
	// Material 3 baseline dark scheme, the palette the Android app falls back to.
	const wxColour kBackground(0x14, 0x12, 0x18);
	const wxColour kSurfaceLow(0x1D, 0x1B, 0x20);
	const wxColour kSurface(0x21, 0x1F, 0x26);
	const wxColour kSurfaceHigh(0x2B, 0x29, 0x30);
	const wxColour kSurfaceHighest(0x36, 0x34, 0x3B);
	const wxColour kPrimary(0xD0, 0xBC, 0xFF);
	const wxColour kOnPrimary(0x38, 0x1E, 0x72);
	const wxColour kSecondaryContainer(0x4A, 0x44, 0x58);
	const wxColour kOnSecondaryContainer(0xE8, 0xDE, 0xF8);
	const wxColour kOnSurface(0xE6, 0xE0, 0xE9);
	const wxColour kOnSurfaceVariant(0xCA, 0xC4, 0xD0);
	const wxColour kOutline(0x93, 0x8F, 0x99);
	const wxColour kOutlineVariant(0x49, 0x45, 0x4F);
	const wxColour kError(0xF2, 0xB8, 0xB5);

	wxColour WithAlpha(const wxColour& c, int alpha)
	{
		return wxColour(c.Red(), c.Green(), c.Blue(), alpha);
	}

	wxFont MakeFont(double px, bool bold = false)
	{
		wxFontInfo info(wxSize(0, std::max(6, (int)std::lround(px))));
		info.Family(wxFONTFAMILY_SWISS);
		if (bold)
			info.Bold();
		return wxFont(info);
	}

	void FillRounded(wxGraphicsContext* gc, double x, double y, double w, double h, double r, const wxColour& colour)
	{
		gc->SetPen(*wxTRANSPARENT_PEN);
		gc->SetBrush(wxBrush(colour));
		gc->DrawRoundedRectangle(x, y, w, h, std::min(r, std::min(w, h) / 2));
	}

	void StrokeRounded(wxGraphicsContext* gc, double x, double y, double w, double h, double r, const wxColour& colour, double width)
	{
		gc->SetPen(wxPen(colour, std::max(1, (int)std::lround(width))));
		gc->SetBrush(*wxTRANSPARENT_BRUSH);
		gc->DrawRoundedRectangle(x, y, w, h, std::min(r, std::min(w, h) / 2));
	}

	void FillCircle(wxGraphicsContext* gc, double cx, double cy, double r, const wxColour& colour)
	{
		gc->SetPen(*wxTRANSPARENT_PEN);
		gc->SetBrush(wxBrush(colour));
		gc->DrawEllipse(cx - r, cy - r, r * 2, r * 2);
	}

	double TextWidth(wxGraphicsContext* gc, const wxString& text)
	{
		double w = 0, h = 0;
		gc->GetTextExtent(text, &w, &h);
		return w;
	}

	double TextHeight(wxGraphicsContext* gc, const wxString& text)
	{
		double w = 0, h = 0;
		gc->GetTextExtent(text.empty() ? wxString("Ag") : text, &w, &h);
		return h;
	}

	wxString Ellipsize(wxGraphicsContext* gc, const wxString& text, double maxWidth)
	{
		if (maxWidth <= 0)
			return wxString();
		if (TextWidth(gc, text) <= maxWidth)
			return text;
		const wxString ellipsis = wxString::FromUTF8("…");
		size_t lo = 0, hi = text.length();
		while (lo < hi)
		{
			const size_t mid = (lo + hi + 1) / 2;
			if (TextWidth(gc, text.Left(mid) + ellipsis) <= maxWidth)
				lo = mid;
			else
				hi = mid - 1;
		}
		return text.Left(lo) + ellipsis;
	}

	// Draws text with its vertical centre at cy.
	void DrawTextV(wxGraphicsContext* gc, const wxString& text, double x, double cy)
	{
		gc->DrawText(text, x, cy - TextHeight(gc, text) / 2);
	}

	void DrawChevron(wxGraphicsContext* gc, double cx, double cy, double size, bool pointsLeft, const wxColour& colour, double width)
	{
		gc->SetPen(wxPen(colour, std::max(1, (int)std::lround(width))));
		const double dx = pointsLeft ? -size / 2 : size / 2; // x offset of the tip from the centre
		wxGraphicsPath path = gc->CreatePath();
		path.MoveToPoint(cx - dx, cy - size);
		path.AddLineToPoint(cx + dx, cy);
		path.AddLineToPoint(cx - dx, cy + size);
		gc->StrokePath(path);
	}

	wxString ButtonStyleLabel(GameMode::ButtonStyle style)
	{
		switch (style)
		{
		case GameMode::ButtonStyle::Xbox: return _("Xbox");
		case GameMode::ButtonStyle::PlayStation: return _("PlayStation");
		case GameMode::ButtonStyle::SteamDeck: return _("Steam Deck (SteamOS)");
		case GameMode::ButtonStyle::Keyboard: return _("Keyboard");
		default: return _("Nintendo");
		}
	}

	// letter printed on a face button (same table as GameMode::FaceLetter; kept here so the
	// panel only depends on GameModeBackend.h)
	wxString FaceLabel(GameMode::ButtonStyle style, GameMode::Face face)
	{
		using GameMode::Face;
		if (style == GameMode::ButtonStyle::Nintendo)
			return face == Face::South ? "B" : face == Face::East ? "A" : face == Face::West ? "Y" : "X";
		return face == Face::South ? "A" : face == Face::East ? "B" : face == Face::West ? "X" : "Y";
	}

	// Keyboard style: the keys the launcher itself listens to
	wxString KeyName(GameMode::Nav nav)
	{
		switch (nav)
		{
		case GameMode::Nav::Accept: return _("Enter");
		case GameMode::Nav::Back: return _("Esc");
		case GameMode::Nav::Options: return "X";
		default: return "Y";
		}
	}

	wxLongLong NowMs()
	{
		return wxGetUTCTimeMillis();
	}

	// Hit-rect ids below zero. Hint buttons use kHintBase - (int)nav.
	constexpr int kHintBase = -100;
	constexpr int kHintLast = kHintBase - 7;
	constexpr int kHitBackArrow = -50;
	constexpr int kHitSettingsChip = -51;
	constexpr int kHitOutsideDialog = -52;
	constexpr int kHitDialogCard = -53;
	constexpr int kHitNone = -1000;

	// Rounds the corners of an image through its alpha channel, with a one-pixel soft edge.
	void RoundCorners(wxImage& image, double radius)
	{
		if (!image.HasAlpha())
			image.InitAlpha();
		const int w = image.GetWidth(), h = image.GetHeight();
		unsigned char* alpha = image.GetAlpha();
		for (int y = 0; y < h; y++)
		{
			for (int x = 0; x < w; x++)
			{
				const double cx = x < radius ? radius : (x >= w - radius ? w - radius : x + 0.5);
				const double cy = y < radius ? radius : (y >= h - radius ? h - radius : y + 0.5);
				const double dx = (x + 0.5) - cx, dy = (y + 0.5) - cy;
				const double coverage = std::clamp(radius - std::sqrt(dx * dx + dy * dy) + 0.5, 0.0, 1.0);
				if (coverage < 1.0)
					alpha[y * w + x] = (unsigned char)(alpha[y * w + x] * coverage);
			}
		}
	}
}

GameModePanel::GameModePanel(wxWindow* parent, std::unique_ptr<GameMode::Backend> backend)
	: wxPanel(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxWANTS_CHARS | wxNO_BORDER | wxFULL_REPAINT_ON_RESIZE),
	  m_backend(std::move(backend)), m_timer(this)
{
	SetBackgroundStyle(wxBG_STYLE_PAINT);
	SetBackgroundColour(kBackground);

	Page library;
	library.isLibrary = true;
	library.title = _("Library");
	m_pages.push_back(std::move(library));
	RefreshGames();

	Bind(wxEVT_PAINT, &GameModePanel::OnPaint, this);
	Bind(wxEVT_TIMER, &GameModePanel::OnTimer, this);
	Bind(wxEVT_CHAR_HOOK, &GameModePanel::OnKeyDown, this);
	Bind(wxEVT_CHAR, &GameModePanel::OnChar, this);
	Bind(wxEVT_MOTION, &GameModePanel::OnMouseMove, this);
	Bind(wxEVT_LEFT_DOWN, &GameModePanel::OnMouseDown, this);
	Bind(wxEVT_RIGHT_DOWN, &GameModePanel::OnMouseDown, this);
	Bind(wxEVT_MOUSEWHEEL, &GameModePanel::OnMouseWheel, this);
	Bind(wxEVT_SIZE, &GameModePanel::OnSize, this);

	// the boot video plays first; the launcher is drawn once it ends
	PlayVideo(m_backend->TakeStartupBootVideo());

	m_timer.Start(16);
	CallAfter([this]() { SetFocus(); });
}

GameModePanel::~GameModePanel()
{
	*m_alive = false;
	m_timer.Stop();
}

void GameModePanel::RefreshGames()
{
	uint64_t focusedId = 0;
	if (m_libraryFocus >= 0 && m_libraryFocus < (int)m_games.size())
		focusedId = m_games[m_libraryFocus].titleId;
	m_games = m_backend->ListGames();
	m_libraryFocus = 0;
	for (size_t i = 0; i < m_games.size(); i++)
	{
		if (m_games[i].titleId == focusedId)
			m_libraryFocus = (int)i;
	}
	Refresh();
}

// ---------------------------------------------------------------------------------------------
// pages

void GameModePanel::PushPage(Page page)
{
	page.rows = page.build ? page.build() : std::vector<Row>{};
	page.focus = 0;
	while (page.focus < (int)page.rows.size() && !page.rows[page.focus].Focusable())
		page.focus++;
	if (page.focus >= (int)page.rows.size())
		page.focus = 0;
	m_pages.push_back(std::move(page));
	Refresh();
}

void GameModePanel::PopPage()
{
	if (m_pages.size() <= 1)
		return;
	m_pages.pop_back();
	RebuildCurrent();
}

void GameModePanel::RebuildCurrent()
{
	Page& page = CurrentPage();
	if (page.isLibrary)
	{
		RefreshGames();
		return;
	}
	if (page.build)
		page.rows = page.build();
	page.focus = std::clamp(page.focus, 0, std::max(0, (int)page.rows.size() - 1));
	Refresh();
}

GameModePanel::Page GameModePanel::MakeGamePage(const GameMode::GameEntry& game)
{
	Page page;
	page.title = game.name;
	page.headerTitleId = game.titleId;
	const uint64_t titleId = game.titleId;
	GameMode::Backend* backend = m_backend.get();
	page.build = [this, titleId, backend]() {
		std::vector<Row> rows;
		Row play;
		play.kind = RowKind::Action;
		play.label = _("Play");
		play.action = [backend, titleId]() { backend->LaunchGame(titleId); };
		rows.push_back(play);

		Row favorite;
		favorite.kind = RowKind::Toggle;
		favorite.label = _("Favourite");
		favorite.description = _("Favourites are listed first");
		favorite.getBool = [this, titleId]() {
			for (const auto& g : m_games)
				if (g.titleId == titleId)
					return g.favorite;
			return false;
		};
		favorite.setBool = [this, backend, titleId](bool value) {
			backend->SetFavorite(titleId, value);
			m_games = backend->ListGames();
		};
		rows.push_back(favorite);

		Row layout;
		layout.kind = RowKind::Choice;
		layout.label = _("Screen layout");
		layout.description = _("For this game only");
		layout.getChoice = [backend, titleId]() { return backend->GetGameScreenLayout(titleId); };
		layout.setChoice = [backend, titleId](int index) { backend->SetGameScreenLayout(titleId, index); };
		rows.push_back(layout);

		Row header;
		header.kind = RowKind::Header;
		header.label = _("Enhancements");
		rows.push_back(header);

		Row textures;
		textures.kind = RowKind::Toggle;
		textures.label = _("Custom textures");
		textures.description = _("Replace the game's textures with the enabled packs");
		textures.getBool = [backend, titleId]() { return backend->GetCustomTexturesEnabled(titleId); };
		textures.setBool = [backend, titleId](bool value) { backend->SetCustomTexturesEnabled(titleId, value); };
		rows.push_back(textures);

		Row packs;
		packs.kind = RowKind::Link;
		packs.label = _("Texture packs");
		packs.getValueText = [backend, titleId]() {
			const auto list = backend->GetTexturePacks(titleId);
			if (list.empty())
				return wxString(_("None found"));
			const auto enabled = std::count_if(list.begin(), list.end(), [](const auto& p) { return p.enabled; });
			return wxString::Format(_("%d of %d on"), (int)enabled, (int)list.size());
		};
		packs.action = [this, titleId]() { PushPage(MakeTexturePacksPage(titleId)); };
		rows.push_back(packs);

		Row cheats;
		cheats.kind = RowKind::Link;
		cheats.label = _("Cheats");
		cheats.getValueText = [backend, titleId]() {
			const auto list = backend->GetCheats(titleId);
			if (list.empty())
				return wxString(_("None"));
			const auto enabled = std::count_if(list.begin(), list.end(), [](const auto& c) { return c.enabled; });
			return wxString::Format(_("%d of %d on"), (int)enabled, (int)list.size());
		};
		cheats.action = [this, titleId]() { PushPage(MakeCheatsPage(titleId)); };
		rows.push_back(cheats);
		return rows;
	};
	return page;
}

GameModePanel::Page GameModePanel::MakeTexturePacksPage(uint64_t titleId)
{
	Page page;
	page.title = _("Texture packs");
	page.headerTitleId = titleId;
	GameMode::Backend* backend = m_backend.get();
	page.build = [backend, titleId]() {
		std::vector<Row> rows;
		const auto packs = backend->GetTexturePacks(titleId);
		if (packs.empty())
		{
			Row info;
			info.kind = RowKind::Info;
			info.label = _("No texture packs found");
			info.description = _("Each folder in load/textures/<title ID>/ is a pack.");
			rows.push_back(info);
		}
		for (const auto& pack : packs)
		{
			Row row;
			row.kind = RowKind::Toggle;
			row.label = pack.name;
			const wxString name = pack.name;
			row.getBool = [backend, titleId, name]() {
				for (const auto& p : backend->GetTexturePacks(titleId))
					if (p.name == name)
						return p.enabled;
				return false;
			};
			row.setBool = [backend, titleId, name](bool value) { backend->SetTexturePackEnabled(titleId, name, value); };
			row.isEnabled = [backend, titleId]() { return backend->GetCustomTexturesEnabled(titleId); };
			rows.push_back(row);
		}
		return rows;
	};
	return page;
}

GameModePanel::Page GameModePanel::MakeCheatsPage(uint64_t titleId)
{
	Page page;
	page.title = _("Cheats");
	page.headerTitleId = titleId;
	GameMode::Backend* backend = m_backend.get();
	page.build = [this, backend, titleId]() {
		std::vector<Row> rows;
		Row add;
		add.kind = RowKind::Action;
		add.label = _("Add cheat");
		add.description = _("Type a name and the code");
		add.action = [this, titleId]() { StartAddCheat(titleId, wxString(), wxString(), wxString()); };
		rows.push_back(add);

		Row folder;
		folder.kind = RowKind::Action;
		folder.label = _("Open cheats folder");
		folder.description = _("Shows this game's cheat file in your file manager");
		folder.action = [this, backend, titleId]() {
			backend->OpenCheatsFolder(titleId);
			ShowToast(_("Cheats folder opened in your file manager"));
		};
		rows.push_back(folder);

		const auto cheats = backend->GetCheats(titleId);
		if (cheats.empty())
		{
			Row info;
			info.kind = RowKind::Info;
			info.label = _("No cheats for this game yet");
			info.description = _("Add one above, or put a cheat file in the cheats folder.");
			rows.push_back(info);
			return rows;
		}
		Row header;
		header.kind = RowKind::Header;
		header.label = wxString::Format(_("Cheats (%d)"), (int)cheats.size());
		rows.push_back(header);
		for (size_t i = 0; i < cheats.size(); i++)
		{
			Row row;
			row.kind = RowKind::Toggle;
			row.label = cheats[i].name;
			row.getBool = [backend, titleId, i]() {
				const auto list = backend->GetCheats(titleId);
				return i < list.size() && list[i].enabled;
			};
			row.setBool = [this, backend, titleId, i](bool value) {
				if (const auto error = backend->SetCheatEnabled(titleId, i, value))
					ShowToast(*error);
			};
			const wxString name = cheats[i].name;
			row.onOptions = [this, backend, titleId, i, name]() {
				OpenConfirmDialog(_("Delete cheat?"), wxString::Format(_("\"%s\" will be removed."), name),
					[this, backend, titleId, i, name]() {
						if (const auto error = backend->DeleteCheat(titleId, i))
							ShowToast(*error);
						else
							ShowToast(wxString::Format(_("Deleted \"%s\""), name));
						RebuildCurrent();
					});
			};
			row.optionsLabel = _("Delete");
			rows.push_back(row);
		}
		return rows;
	};
	return page;
}

// A new cheat in two steps: its name, then its code. When the code does not parse, the code
// keyboard comes back with what was typed and the reason, so nothing has to be typed again.
void GameModePanel::StartAddCheat(uint64_t titleId, const wxString& name, const wxString& code, const wxString& error)
{
	auto askCode = [this, titleId](const wxString& name, const wxString& code, const wxString& error) {
		OpenTextDialog(wxString::Format(_("Code for \"%s\""), name), _("One code per line, for example: 02123450 38A00000"), code, true,
			[this, titleId, name](const wxString& code) {
				if (const auto problem = m_backend->AddCheat(titleId, name, code))
				{
					StartAddCheat(titleId, name, code, *problem);
					return;
				}
				ShowToast(wxString::Format(_("Added \"%s\". Switch it on in the list."), name));
				RebuildCurrent();
				Page& page = CurrentPage();
				if (!page.rows.empty())
					page.focus = (int)page.rows.size() - 1; // the new cheat is last
			});
		m_dialog.keyLayer = kUpper; // codes are hex: capitals and digits
		m_dialog.hint = "02123450 38A00000";
		m_dialog.error = error;
	};
	if (!name.empty())
	{
		askCode(name, code, error);
		return;
	}
	OpenTextDialog(_("New cheat"), _("Name"), wxString(), false, [this, askCode](const wxString& typed) {
		const wxString trimmed = wxString(typed).Trim(true).Trim(false);
		if (trimmed.empty())
		{
			ShowToast(_("The cheat needs a name"));
			return;
		}
		askCode(trimmed, wxString(), wxString());
	});
	m_dialog.hint = _("For example: Infinite health");
}

GameModePanel::Page GameModePanel::MakeSettingsPage()
{
	Page page;
	page.title = _("Settings");
	page.isSettingsRoot = true;
	page.build = [this]() {
		std::vector<Row> rows;
		auto link = [&](const wxString& label, const wxString& description, std::function<Page()> make) {
			Row row;
			row.kind = RowKind::Link;
			row.label = label;
			row.description = description;
			row.action = [this, make]() { PushPage(make()); };
			rows.push_back(row);
		};
		link(_("Graphics"), _("Graphics API, VSync, screen layout, scaling filters, fullscreen"), [this]() { return MakeGraphicsPage(); });
		link(_("Audio"), _("TV and GamePad volume"), [this]() { return MakeAudioPage(); });
		link(_("Input"), _("Controller profiles and the Game Menu button"), [this]() { return MakeInputPage(); });

		Row header;
		header.kind = RowKind::Header;
		header.label = _("Game Mode");
		rows.push_back(header);

		Row boot;
		boot.kind = RowKind::Toggle;
		boot.label = _("Always boot to Game Mode");
		boot.description = _("Start Cemu in Game Mode, even after exiting it");
		boot.getBool = [this]() { return m_backend->GetAlwaysBootGameMode(); };
		boot.setBool = [this](bool v) { m_backend->SetAlwaysBootGameMode(v); };
		rows.push_back(boot);

		Row buttons;
		buttons.kind = RowKind::Choice;
		buttons.label = _("Button icons");
		buttons.description = _("Which controller's buttons the on-screen hints show");
		buttons.getChoice = []() {
			GameMode::Choice choice;
			for (int i = 0; i < (int)GameMode::ButtonStyle::Count; i++)
				choice.options.push_back(ButtonStyleLabel((GameMode::ButtonStyle)i));
			return choice;
		};
		auto baseChoice = buttons.getChoice;
		buttons.getChoice = [this, baseChoice]() {
			auto choice = baseChoice();
			choice.selected = (int)m_backend->GetButtonStyle();
			return choice;
		};
		buttons.setChoice = [this](int i) { m_backend->SetButtonStyle((GameMode::ButtonStyle)i); };
		rows.push_back(buttons);

		Row video;
		video.kind = RowKind::Link;
		video.label = _("Boot video");
		video.description = _("Plays before Game Mode opens");
		video.getValueText = [this]() {
			const auto info = m_backend->GetBootVideoInfo();
			if (!info.supported || !info.found)
				return wxString(_("Not found"));
			return m_backend->GetBootVideoEnabled() ? wxString(_("On")) : wxString(_("Off"));
		};
		video.action = [this]() { PushPage(MakeBootVideoPage()); };
		rows.push_back(video);

		Row swap;
		swap.kind = RowKind::Toggle;
		swap.label = _("Swap A and B");
		swap.description = _("In Game Mode menus only: B selects and A goes back. Games are not affected.");
		swap.getBool = [this]() { return m_backend->GetSwapAB(); };
		swap.setBool = [this](bool v) { m_backend->SetSwapAB(v); };
		rows.push_back(swap);

		Row exit;
		exit.kind = RowKind::Action;
		exit.label = _("Exit Game Mode");
		exit.description = _("Return to the regular Cemu window");
		exit.danger = true;
		exit.action = [this]() {
			OpenConfirmDialog(_("Exit Game Mode?"), _("You can come back to it from View > Game Mode."),
				[this]() { m_backend->ExitGameMode(); });
		};
		rows.push_back(exit);
		return rows;
	};
	return page;
}

GameModePanel::Page GameModePanel::MakeBootVideoPage()
{
	Page page;
	page.title = _("Boot video");
	GameMode::Backend* b = m_backend.get();
	page.build = [this, b]() {
		std::vector<Row> rows;
		const auto info = b->GetBootVideoInfo();
		const bool usable = info.supported && info.found;
		auto addFolder = [&]() {
			Row folder;
			folder.kind = RowKind::Action;
			folder.label = _("Open Cemu folder");
			folder.description = _("The boot video lives in its boot folder");
			folder.action = [this, b]() {
				b->OpenCemuFolder();
				ShowToast(_("Cemu folder opened in your file manager"));
			};
			rows.push_back(folder);
		};
		if (!info.supported)
		{
			Row error;
			error.kind = RowKind::Info;
			error.danger = true;
			error.label = _("Boot videos are not supported by this build.");
			error.description = _("Cemu was built without FFmpeg.");
			rows.push_back(error);
		}
		else if (!info.found)
		{
			Row error;
			error.kind = RowKind::Info;
			error.danger = true;
			error.label = _("Boot video or boot folder not found.");
			error.description = _("Open Cemu folder and place boot.mp4 or boot.webm inside the boot folder.");
			rows.push_back(error);
		}
		if (!usable)
			addFolder(); // the one thing to do here, so it comes first

		Row enabled;
		enabled.kind = RowKind::Toggle;
		enabled.label = _("Play boot video");
		enabled.description = usable ? wxString::Format(_("Plays boot/%s from the Cemu folder when Cemu starts in Game Mode"), info.fileName)
			: wxString(_("Plays boot/boot.mp4 or boot/boot.webm from the Cemu folder"));
		enabled.getBool = [b, usable]() { return usable && b->GetBootVideoEnabled(); };
		enabled.setBool = [b](bool v) { b->SetBootVideoEnabled(v); };
		enabled.isEnabled = [usable]() { return usable; };
		rows.push_back(enabled);

		Row preview;
		preview.kind = RowKind::Action;
		preview.label = _("Play it now");
		preview.description = _("Any button skips it");
		preview.isEnabled = [usable]() { return usable; };
		preview.action = [this, b]() {
			auto video = b->OpenBootVideo();
			if (!video)
			{
				ShowToast(_("The boot video could not be played (see log.txt)"));
				return;
			}
			PlayVideo(std::move(video));
		};
		rows.push_back(preview);

		Row restore;
		restore.kind = RowKind::Action;
		restore.label = _("Restore boot video");
		restore.description = info.hasBackup ? _("Puts the original video (boot.bak.mp4) back as boot.mp4")
			: _("No backup: boot.bak.mp4 is not in the boot folder");
		restore.isEnabled = [info]() { return info.supported && info.hasBackup; };
		restore.action = [this, b]() {
			OpenConfirmDialog(_("Restore boot video?"), _("boot.mp4 is replaced with the original video."), [this, b]() {
				if (const auto error = b->RestoreBootVideo())
					ShowToast(*error);
				else
					ShowToast(_("Boot video restored"));
				RebuildCurrent();
			});
		};
		rows.push_back(restore);

		if (usable)
			addFolder();
		return rows;
	};
	return page;
}

GameModePanel::Page GameModePanel::MakeGraphicsPage()
{
	Page page;
	page.title = _("Graphics");
	GameMode::Backend* b = m_backend.get();
	page.build = [b]() {
		std::vector<Row> rows;
		auto choice = [&](const wxString& label, const wxString& description, std::function<GameMode::Choice()> get, std::function<void(int)> set) {
			Row row;
			row.kind = RowKind::Choice;
			row.label = label;
			row.description = description;
			row.getChoice = std::move(get);
			row.setChoice = std::move(set);
			rows.push_back(row);
		};
		choice(_("Graphics API"), _("Takes effect the next time a game starts"),
			[b]() { return b->GetGraphicsApi(); }, [b](int i) { b->SetGraphicsApi(i); });
		choice(_("VSync"), wxString(),
			[b]() { return b->GetVSync(); }, [b](int i) { b->SetVSync(i); });
		choice(_("Screen layout"), _("Default for games without their own layout"),
			[b]() { return b->GetDefaultScreenLayout(); }, [b](int i) { b->SetDefaultScreenLayout(i); });
		choice(_("Upscale filter"), _("Used when the game image is smaller than the screen"),
			[b]() { return b->GetUpscaleFilter(); }, [b](int i) { b->SetUpscaleFilter(i); });
		choice(_("Downscale filter"), _("Used when the game image is larger than the screen"),
			[b]() { return b->GetDownscaleFilter(); }, [b](int i) { b->SetDownscaleFilter(i); });

		Row fullscreen;
		fullscreen.kind = RowKind::Toggle;
		fullscreen.label = _("Fullscreen");
		fullscreen.getBool = [b]() { return b->GetFullscreen(); };
		fullscreen.setBool = [b](bool v) { b->SetFullscreen(v); };
		rows.push_back(fullscreen);
		return rows;
	};
	return page;
}

GameModePanel::Page GameModePanel::MakeAudioPage()
{
	Page page;
	page.title = _("Audio");
	GameMode::Backend* b = m_backend.get();
	page.build = [b]() {
		std::vector<Row> rows;
		Row tv;
		tv.kind = RowKind::Slider;
		tv.label = _("TV volume");
		tv.getInt = [b]() { return b->GetTvVolume(); };
		tv.setInt = [b](int v) { b->SetTvVolume(v); };
		rows.push_back(tv);

		Row pad;
		pad.kind = RowKind::Slider;
		pad.label = _("GamePad volume");
		pad.getInt = [b]() { return b->GetPadVolume(); };
		pad.setInt = [b](int v) { b->SetPadVolume(v); };
		rows.push_back(pad);
		return rows;
	};
	return page;
}

GameModePanel::Page GameModePanel::MakeInputPage()
{
	Page page;
	page.title = _("Input");
	GameMode::Backend* b = m_backend.get();
	page.build = [this, b]() {
		std::vector<Row> rows;
		Row header;
		header.kind = RowKind::Header;
		header.label = _("Players");
		rows.push_back(header);
		for (int player = 0; player < b->GetPlayerCount(); player++)
		{
			Row row;
			row.kind = RowKind::Link;
			row.label = wxString::Format(_("Player %d"), player + 1);
			row.getValueText = [b, player]() {
				const auto choice = b->GetPlayerProfile(player);
				return (choice.selected >= 0 && choice.selected < (int)choice.options.size()) ? choice.options[choice.selected] : wxString();
			};
			row.action = [this, b, player]() {
				b->RefreshDevices();
				PushPage(MakePlayerPage(player));
			};
			rows.push_back(row);
		}

		Row header2;
		header2.kind = RowKind::Header;
		header2.label = _("Game Mode");
		rows.push_back(header2);

		Row bind;
		bind.kind = RowKind::Action;
		bind.label = _("Game Menu button");
		bind.description = _("Opens the side menu while playing. Select, then press the new button or key.");
		bind.getValueText = [b]() { return b->GetGameMenuBindingLabel(); };
		bind.action = [this]() { StartGameMenuCapture(); };
		bind.onOptions = [this, b]() {
			b->ResetGameMenuBinding();
			ShowToast(wxString::Format(_("Game Menu button: %s"), b->GetGameMenuBindingLabel()));
		};
		bind.optionsLabel = _("Reset");
		rows.push_back(bind);

		Row reset;
		reset.kind = RowKind::Action;
		reset.label = _("Reset Game Menu button");
		reset.description = _("Back to the default: Guide button and F10");
		reset.action = [this, b]() {
			b->ResetGameMenuBinding();
			ShowToast(wxString::Format(_("Game Menu button: %s"), b->GetGameMenuBindingLabel()));
		};
		rows.push_back(reset);
		return rows;
	};
	return page;
}

// One player's setup, like the Android app's input screen: profile, controller type, device, and
// every Wii U button with what it is bound to. Select a button and press the new one; X clears it.
GameModePanel::Page GameModePanel::MakePlayerPage(int player)
{
	Page page;
	page.title = wxString::Format(_("Player %d"), player + 1);
	GameMode::Backend* b = m_backend.get();
	page.build = [this, b, player]() {
		std::vector<Row> rows;
		auto report = [this](const std::optional<wxString>& error) {
			if (error)
				ShowToast(*error);
		};

		Row profile;
		profile.kind = RowKind::Choice;
		profile.label = _("Profile");
		profile.description = _("Disabled turns this player off");
		profile.getChoice = [b, player]() { return b->GetPlayerProfile(player); };
		profile.setChoice = [b, player, report](int i) { report(b->SetPlayerProfile(player, i)); };
		rows.push_back(profile);

		const bool enabled = !b->GetPlayerControllerType(player).options.empty();
		Row type;
		type.kind = RowKind::Choice;
		type.label = _("Controller type");
		type.description = _("The Wii U controller this player emulates");
		type.getChoice = [b, player]() {
			auto choice = b->GetPlayerControllerType(player);
			if (choice.options.empty())
				choice.options = {_("Wii U GamePad"), _("Wii U Pro Controller"), _("Classic Controller"), _("Wii Remote")};
			return choice;
		};
		type.setChoice = [b, player, report](int i) { report(b->SetPlayerControllerType(player, i)); };
		rows.push_back(type); // choosing a type also enables a disabled player

		Row device;
		device.kind = RowKind::Choice;
		device.label = _("Input device");
		device.description = _("Choosing a device gives it the default button layout");
		device.getChoice = [b, player]() { return b->GetPlayerDevice(player); };
		device.setChoice = [b, player, report](int i) { report(b->SetPlayerDevice(player, i)); };
		device.isEnabled = [b, player]() { return !b->GetPlayerControllerType(player).options.empty(); };
		device.onOptions = [this, b]() {
			b->RefreshDevices();
			ShowToast(_("Controller list refreshed"));
		};
		device.optionsLabel = _("Refresh");
		rows.push_back(device);

		if (!enabled)
		{
			Row info;
			info.kind = RowKind::Info;
			info.label = _("This player is disabled");
			info.description = _("Pick a profile or a controller type to set it up.");
			rows.push_back(info);
			return rows;
		}

		Row resetMapping;
		resetMapping.kind = RowKind::Action;
		resetMapping.label = _("Reset buttons to default");
		resetMapping.action = [this, b, player]() {
			OpenConfirmDialog(_("Reset buttons?"), _("Every button goes back to the default layout for this device."), [this, b, player]() {
				b->ResetMappings(player);
				RebuildCurrent();
			});
		};
		rows.push_back(resetMapping);

		Row save;
		save.kind = RowKind::Action;
		save.label = _("Save to profile");
		save.description = _("Store this setup so it can be picked for any player");
		save.action = [this, b, player]() {
			GameMode::Choice choice;
			const auto names = b->GetProfileNames();
			// a fresh name for a new profile
			wxString fresh;
			for (int n = 1;; n++)
			{
				fresh = n == 1 ? wxString::Format("Player %d", player + 1) : wxString::Format("Player %d (%d)", player + 1, n);
				if (std::find(names.begin(), names.end(), fresh) == names.end())
					break;
			}
			choice.options.push_back(wxString::Format(_("New profile: %s"), fresh));
			for (const auto& name : names)
				choice.options.push_back(wxString::Format(_("Overwrite: %s"), name));
			OpenChoiceDialog(_("Save to profile"), choice, [this, b, player, names, fresh](int i) {
				const wxString name = i == 0 ? fresh : names[i - 1];
				if (const auto error = b->SaveProfile(player, name))
					ShowToast(*error);
				else
					ShowToast(wxString::Format(_("Saved as %s"), name));
				RebuildCurrent();
			});
		};
		rows.push_back(save);

		wxString group;
		for (const auto& entry : b->GetMappings(player))
		{
			if (entry.group != group)
			{
				// section heading: Buttons, D-Pad, Left Stick, Right Stick, ...
				group = entry.group;
				Row header;
				header.kind = RowKind::Header;
				header.label = group;
				rows.push_back(header);
			}
			Row row;
			row.kind = RowKind::Action;
			row.label = entry.name;
			const uint64_t id = entry.id;
			row.getValueText = [b, player, id]() {
				for (const auto& e : b->GetMappings(player))
					if (e.id == id)
						return e.bound.empty() ? wxString(_("Not set")) : e.bound;
				return wxString();
			};
			// full name for the capture prompt, e.g. "Left Stick: Up"
			const wxString name = entry.group.empty() ? entry.name : entry.group + ": " + entry.name;
			row.action = [this, b, player, id, name]() {
				b->BeginMappingCapture(player, id);
				StartCapture(name, _("Press a button on the controller, or a key (Esc cancels)..."),
					[b]() { return b->PollMappingCapture(); }, nullptr,
					[this, name]() { ShowToast(wxString::Format(_("%s mapped"), name)); });
			};
			row.onOptions = [b, player, id]() { b->ClearMapping(player, id); };
			row.optionsLabel = _("Clear");
			rows.push_back(row);
		}
		return rows;
	};
	return page;
}

// ---------------------------------------------------------------------------------------------
// dialogs

void GameModePanel::OpenChoiceDialog(const wxString& title, const GameMode::Choice& choice, std::function<void(int)> onChoose)
{
	m_dialog = Dialog{};
	m_dialog.type = Dialog::Type::Choice;
	m_dialog.title = title;
	m_dialog.options = choice.options;
	m_dialog.focus = std::clamp(choice.selected, 0, std::max(0, (int)choice.options.size() - 1));
	m_dialog.onChoose = std::move(onChoose);
	Refresh();
}

void GameModePanel::OpenConfirmDialog(const wxString& title, const wxString& message, std::function<void()> onYes)
{
	m_dialog = Dialog{};
	m_dialog.type = Dialog::Type::Confirm;
	m_dialog.title = title;
	m_dialog.message = message;
	m_dialog.options = {_("Cancel"), _("OK")};
	m_dialog.focus = 0;
	m_dialog.onChoose = [onYes](int index) {
		if (index == 1)
			onYes();
	};
	Refresh();
}

void GameModePanel::StartCapture(const wxString& title, const wxString& message, std::function<bool()> poll,
	std::function<bool(int, bool, bool, bool)> onKey, std::function<void()> onDone)
{
	m_dialog = Dialog{};
	m_dialog.type = Dialog::Type::Capture;
	m_dialog.title = title;
	m_dialog.message = message;
	m_dialog.startedMs = NowMs();
	m_dialog.poll = std::move(poll);
	m_dialog.onKey = std::move(onKey);
	m_dialog.onDone = std::move(onDone);
	Refresh();
}

void GameModePanel::OpenTextDialog(const wxString& title, const wxString& message, const wxString& initial, bool multiline,
	std::function<void(const wxString&)> onText)
{
	m_dialog = Dialog{};
	m_dialog.type = Dialog::Type::Text;
	m_dialog.title = title;
	m_dialog.message = message;
	m_dialog.text = initial;
	m_dialog.multiline = multiline;
	m_dialog.onText = std::move(onText);
	// start on the first letter
	const auto keys = TextKeys();
	for (size_t i = 0; i < keys.size(); i++)
	{
		if (keys[i].row == 1)
		{
			m_dialog.keyFocus = (int)i;
			break;
		}
	}
	Refresh();
}

// The keyboard: four rows of characters (letters, capitals or symbols) and two rows of special keys.
std::vector<GameModePanel::Key> GameModePanel::TextKeys() const
{
	static const char* const kLowerRows[] = {"1234567890", "qwertyuiop", "asdfghjkl:", "zxcvbnm,.-"};
	static const char* const kUpperRows[] = {"1234567890", "QWERTYUIOP", "ASDFGHJKL:", "ZXCVBNM,.-"};
	static const char* const kSymbolRows[] = {"1234567890", "!@#$%^&*()", "-_=+[]{};:", "'\",./?<>\\|"};
	const char* const* rows = m_dialog.keyLayer == kSymbols ? kSymbolRows : m_dialog.keyLayer == kUpper ? kUpperRows : kLowerRows;
	std::vector<Key> keys;
	for (int r = 0; r < 4; r++)
	{
		const wxString chars = wxString::FromUTF8(rows[r]);
		for (size_t c = 0; c < chars.length(); c++)
		{
			Key key;
			key.label = key.insert = chars.Mid(c, 1);
			key.row = r;
			key.x = (double)c;
			keys.push_back(key);
		}
	}
	auto special = [&](KeyAction action, const wxString& label, int row, double x, double w) {
		Key key;
		key.action = action;
		key.label = label;
		key.row = row;
		key.x = x;
		key.w = w;
		keys.push_back(key);
	};
	special(KeyAction::Shift, m_dialog.keyLayer == kUpper ? _("abc") : _("ABC"), 4, 0, 1.5);
	special(KeyAction::Symbols, m_dialog.keyLayer == kSymbols ? _("ABC") : "#+=", 4, 1.5, 1.5);
	if (m_dialog.multiline)
	{
		special(KeyAction::Space, _("Space"), 4, 3, 3.5);
		special(KeyAction::NewLine, _("New line"), 4, 6.5, 1.75);
		special(KeyAction::Backspace, wxString::FromUTF8("\u232B"), 4, 8.25, 1.75);
	}
	else
	{
		special(KeyAction::Space, _("Space"), 4, 3, 5);
		special(KeyAction::Backspace, wxString::FromUTF8("\u232B"), 4, 8, 2);
	}
	special(KeyAction::Paste, _("Paste"), 5, 0, 3);
	special(KeyAction::Cancel, _("Cancel"), 5, 3, 3);
	special(KeyAction::Done, _("Done"), 5, 6, 4);
	return keys;
}

void GameModePanel::MoveTextFocus(Nav nav)
{
	const auto keys = TextKeys();
	if (keys.empty())
		return;
	const int current = std::clamp(m_dialog.keyFocus, 0, (int)keys.size() - 1);
	const Key& from = keys[current];
	if (nav == Nav::Left || nav == Nav::Right)
	{
		// along the row, wrapping around at the ends
		std::vector<int> row;
		for (int i = 0; i < (int)keys.size(); i++)
			if (keys[i].row == from.row)
				row.push_back(i);
		const int at = (int)(std::find(row.begin(), row.end(), current) - row.begin());
		const int n = (int)row.size();
		m_dialog.keyFocus = row[(at + (nav == Nav::Right ? 1 : n - 1)) % n];
		return;
	}
	// up/down: the key in the next row nearest to this one's centre
	const int lastRow = keys.back().row;
	const int targetRow = from.row + (nav == Nav::Down ? 1 : -1);
	if (targetRow < 0 || targetRow > lastRow)
		return;
	const double centre = from.x + from.w / 2;
	int best = current;
	double bestDistance = 1e9;
	for (int i = 0; i < (int)keys.size(); i++)
	{
		if (keys[i].row != targetRow)
			continue;
		const double distance = std::abs(keys[i].x + keys[i].w / 2 - centre);
		if (distance < bestDistance)
		{
			bestDistance = distance;
			best = i;
		}
	}
	m_dialog.keyFocus = best;
}

void GameModePanel::TypeText(const wxString& text)
{
	constexpr size_t kMaxLength = 8000;
	wxString add = text;
	add.Replace("\r\n", "\n");
	add.Replace("\r", "\n");
	if (!m_dialog.multiline)
		add.Replace("\n", " ");
	if (m_dialog.text.length() + add.length() > kMaxLength)
		add = add.Left(kMaxLength - std::min(kMaxLength, m_dialog.text.length()));
	m_dialog.text += add;
	m_dialog.error.clear();
}

void GameModePanel::PressTextKey(KeyAction action, const wxString& insert)
{
	Dialog& d = m_dialog;
	switch (action)
	{
	case KeyAction::Char:
		TypeText(insert);
		break;
	case KeyAction::Shift:
		d.keyLayer = d.keyLayer == kUpper ? kLower : kUpper;
		break;
	case KeyAction::Symbols:
		d.keyLayer = d.keyLayer == kSymbols ? kLower : kSymbols;
		break;
	case KeyAction::Space:
		TypeText(" ");
		break;
	case KeyAction::NewLine:
		TypeText("\n");
		break;
	case KeyAction::Backspace:
		if (!d.text.empty())
			d.text.RemoveLast();
		break;
	case KeyAction::Paste:
		if (wxTheClipboard->Open())
		{
			if (wxTheClipboard->IsSupported(wxDF_UNICODETEXT) || wxTheClipboard->IsSupported(wxDF_TEXT))
			{
				wxTextDataObject data;
				if (wxTheClipboard->GetData(data))
					TypeText(data.GetText());
			}
			wxTheClipboard->Close();
		}
		break;
	case KeyAction::Cancel:
		d = Dialog{};
		break;
	case KeyAction::Done:
	{
		auto onText = d.onText;
		const wxString text = d.text;
		d = Dialog{};
		if (onText)
			onText(text);
		break;
	}
	}
	Refresh();
}

void GameModePanel::StartGameMenuCapture()
{
	GameMode::Backend* b = m_backend.get();
	b->BeginGameMenuCapture();
	StartCapture(_("Game Menu button"), _("Press a controller button or a key..."),
		[b]() { return b->PollGameMenuCapture(); },
		[b](int key, bool alt, bool ctrl, bool shift) {
			b->BindGameMenuKey(key, alt, ctrl, shift);
			return true;
		},
		[this, b]() { ShowToast(wxString::Format(_("Game Menu button: %s"), b->GetGameMenuBindingLabel())); });
}

void GameModePanel::PlayVideo(std::unique_ptr<GameMode::VideoPlayer> video)
{
	if (!video)
		return;
	m_video = std::move(video);
	m_videoFrame = wxNullBitmap;
	m_videoStartMs = NowMs();
	m_dialog = Dialog{};
	Refresh();
}

void GameModePanel::EndVideo()
{
	if (!m_video)
		return;
	m_video.reset(); // stops the sound too
	m_videoFrame = wxNullBitmap;
	m_fadeInStartMs = NowMs();
	// the button that skipped must not also act in the launcher
	m_navScratch.clear();
	m_backend->PollControllerNav(m_navScratch);
	m_navScratch.clear();
	Refresh();
}

void GameModePanel::ShowToast(const wxString& message)
{
	m_toast = message;
	m_toastUntilMs = NowMs() + 3000;
	Refresh();
}

// ---------------------------------------------------------------------------------------------
// navigation

void GameModePanel::HandleNav(Nav nav)
{
	if (m_video)
	{
		EndVideo(); // any button skips the boot video
		return;
	}
	if (m_dialog.type != Dialog::Type::None)
	{
		HandleDialogNav(nav);
		return;
	}
	Page& page = CurrentPage();
	if (page.isLibrary)
		HandleLibraryNav(nav);
	else
		HandleListNav(page, nav);
	Refresh();
}

void GameModePanel::HandleLibraryNav(Nav nav)
{
	const int count = (int)m_games.size();
	const int cols = LibraryColumns(GetClientSize().GetWidth());
	switch (nav)
	{
	case Nav::Left:
		if (count && m_libraryFocus % cols > 0)
			m_libraryFocus--;
		break;
	case Nav::Right:
		if (count && m_libraryFocus % cols < cols - 1 && m_libraryFocus + 1 < count)
			m_libraryFocus++;
		break;
	case Nav::Up:
		if (m_libraryFocus - cols >= 0)
			m_libraryFocus -= cols;
		break;
	case Nav::Down:
		if (m_libraryFocus + cols < count)
			m_libraryFocus += cols;
		else if (count && (m_libraryFocus / cols) < ((count - 1) / cols))
			m_libraryFocus = count - 1; // partial last row
		break;
	case Nav::Accept:
		if (m_libraryFocus < count)
			m_backend->LaunchGame(m_games[m_libraryFocus].titleId);
		break;
	case Nav::Options:
		if (m_libraryFocus < count)
			PushPage(MakeGamePage(m_games[m_libraryFocus]));
		break;
	case Nav::Settings:
		PushPage(MakeSettingsPage());
		break;
	case Nav::Back:
		ShowToast(_("Exit Game Mode from Settings"));
		break;
	}
}

void GameModePanel::MoveListFocus(Page& page, int direction)
{
	int index = page.focus;
	while (true)
	{
		index += direction;
		if (index < 0 || index >= (int)page.rows.size())
			return;
		if (page.rows[index].Focusable())
		{
			page.focus = index;
			return;
		}
	}
}

void GameModePanel::HandleListNav(Page& page, Nav nav)
{
	Row* row = (page.focus >= 0 && page.focus < (int)page.rows.size()) ? &page.rows[page.focus] : nullptr;
	switch (nav)
	{
	case Nav::Up:
		MoveListFocus(page, -1);
		break;
	case Nav::Down:
		MoveListFocus(page, 1);
		break;
	case Nav::Left:
		if (row)
			AdjustRow(*row, -1);
		break;
	case Nav::Right:
		if (row)
			AdjustRow(*row, 1);
		break;
	case Nav::Accept:
		if (row)
			ActivateRow(*row);
		break;
	case Nav::Back:
		PopPage();
		break;
	case Nav::Settings:
		if (page.isSettingsRoot)
			PopPage(); // Y toggles Settings from the library
		break;
	case Nav::Options:
		if (row && row->onOptions && row->Enabled())
		{
			auto onOptions = row->onOptions;
			onOptions();
			RebuildCurrent();
		}
		break;
	}
}

void GameModePanel::ActivateRow(Row& row)
{
	if (!row.Focusable() || !row.Enabled())
		return;
	switch (row.kind)
	{
	case RowKind::Toggle:
		row.setBool(!row.getBool());
		break;
	case RowKind::Choice:
	{
		auto set = row.setChoice;
		OpenChoiceDialog(row.label, row.getChoice(), [this, set](int index) {
			set(index);
			RebuildCurrent();
		});
		return;
	}
	case RowKind::Slider:
		break;
	case RowKind::Action:
	case RowKind::Link:
		if (row.action)
		{
			auto action = row.action; // may replace the page (and this row) while running
			action();
		}
		return;
	default:
		break;
	}
	RebuildCurrent();
}

void GameModePanel::AdjustRow(Row& row, int direction)
{
	if (!row.Enabled())
		return;
	switch (row.kind)
	{
	case RowKind::Toggle:
		if (row.getBool() != (direction > 0))
			row.setBool(direction > 0);
		break;
	case RowKind::Choice:
	{
		const auto choice = row.getChoice();
		if (choice.options.empty())
			return;
		const int next = std::clamp(choice.selected + direction, 0, (int)choice.options.size() - 1);
		if (next != choice.selected)
			row.setChoice(next);
		break;
	}
	case RowKind::Slider:
	{
		const int value = std::clamp(row.getInt() + direction * row.step, row.minValue, row.maxValue);
		row.setInt(value);
		break;
	}
	default:
		return;
	}
	RebuildCurrent();
}

void GameModePanel::HandleDialogNav(Nav nav)
{
	Dialog& d = m_dialog;
	if (d.type == Dialog::Type::Capture)
	{
		// Buttons are being captured by the backend; only a key or the timeout ends it.
		return;
	}
	if (d.type == Dialog::Type::Text)
	{
		// A types the focused key, B deletes (cancels once the text is empty), X space, Y done
		switch (nav)
		{
		case Nav::Up:
		case Nav::Down:
		case Nav::Left:
		case Nav::Right:
			MoveTextFocus(nav);
			break;
		case Nav::Accept:
		{
			const auto keys = TextKeys();
			if (d.keyFocus >= 0 && d.keyFocus < (int)keys.size())
				PressTextKey(keys[d.keyFocus].action, keys[d.keyFocus].insert);
			break;
		}
		case Nav::Back:
			PressTextKey(d.text.empty() ? KeyAction::Cancel : KeyAction::Backspace);
			break;
		case Nav::Options:
			PressTextKey(KeyAction::Space);
			break;
		case Nav::Settings:
			PressTextKey(KeyAction::Done);
			break;
		}
		Refresh();
		return;
	}
	const bool horizontal = d.type == Dialog::Type::Confirm;
	switch (nav)
	{
	case Nav::Up:
		if (!horizontal && d.focus > 0)
			d.focus--;
		break;
	case Nav::Down:
		if (!horizontal && d.focus + 1 < (int)d.options.size())
			d.focus++;
		break;
	case Nav::Left:
		if (horizontal && d.focus > 0)
			d.focus--;
		break;
	case Nav::Right:
		if (horizontal && d.focus + 1 < (int)d.options.size())
			d.focus++;
		break;
	case Nav::Accept:
	{
		auto onChoose = d.onChoose;
		const int index = d.focus;
		d = Dialog{};
		if (onChoose)
			onChoose(index);
		break;
	}
	case Nav::Back:
		d = Dialog{};
		break;
	default:
		break;
	}
	Refresh();
}

// ---------------------------------------------------------------------------------------------
// events

void GameModePanel::OnTimer(wxTimerEvent& event)
{
	bool dirty = false;
	const wxLongLong now = NowMs();

	if (m_video)
	{
		// any button skips; frames are pulled while painting
		m_navScratch.clear();
		m_backend->PollControllerNav(m_navScratch);
		const bool skipped = !m_navScratch.empty() && now - m_videoStartMs > 300;
		m_navScratch.clear();
		if (skipped || m_video->IsFinished())
			EndVideo();
		else
			Refresh();
		return;
	}
	if (m_fadeInStartMs != 0)
	{
		if (now - m_fadeInStartMs > 400)
			m_fadeInStartMs = 0;
		dirty = true;
	}

	if (m_dialog.type == Dialog::Type::Capture)
	{
		if (m_dialog.poll && m_dialog.poll())
		{
			auto onDone = m_dialog.onDone;
			m_dialog = Dialog{};
			if (onDone)
				onDone();
			RebuildCurrent();
			// swallow the press that was just captured so it does not also navigate
			m_navScratch.clear();
			m_backend->PollControllerNav(m_navScratch);
			m_navScratch.clear();
		}
		else if (now - m_dialog.startedMs > 6000)
		{
			m_dialog = Dialog{};
			ShowToast(_("Nothing pressed, left unchanged"));
			m_navScratch.clear();
			m_backend->PollControllerNav(m_navScratch);
			m_navScratch.clear();
		}
		dirty = true;
	}
	else
	{
		m_navScratch.clear();
		m_backend->PollControllerNav(m_navScratch);
		for (Nav nav : m_navScratch)
			HandleNav(nav);
	}

	// keep the game list current while a scan is running and once more when it ends
	if (++m_scanTick >= 60)
	{
		m_scanTick = 0;
		const bool scanning = m_backend->IsScanningGames();
		if (scanning || m_wasScanning)
		{
			if (m_pages.size() == 1)
				RefreshGames();
			dirty = true;
		}
		m_wasScanning = scanning;
	}

	// smooth scrolling
	auto approach = [&](double& value, double target) {
		if (std::abs(value - target) < 0.5)
		{
			if (value != target)
			{
				value = target;
				dirty = true;
			}
			return;
		}
		value += (target - value) * 0.3;
		dirty = true;
	};
	approach(m_libraryScroll, m_libraryScrollTarget);
	for (auto& page : m_pages)
		approach(page.scroll, page.scrollTarget);

	if (!m_toast.empty() && now > m_toastUntilMs)
	{
		m_toast.clear();
		dirty = true;
	}
	if (dirty)
		Refresh();
}

void GameModePanel::OnKeyDown(wxKeyEvent& event)
{
	const int key = event.GetKeyCode();
	if (m_video)
	{
		if (key != WXK_SHIFT && key != WXK_CONTROL && key != WXK_ALT && key != WXK_RAW_CONTROL)
			EndVideo();
		return;
	}
	if (m_dialog.type == Dialog::Type::Capture)
	{
		// modifiers alone are not a binding
		if (key == WXK_SHIFT || key == WXK_CONTROL || key == WXK_ALT || key == WXK_RAW_CONTROL)
			return;
		if (!m_dialog.onKey)
		{
			if (key == WXK_ESCAPE)
				m_dialog = Dialog{}; // keys cannot be bound here; Esc cancels
			Refresh();
			return;
		}
		if (m_dialog.onKey(key, event.AltDown(), event.ControlDown(), event.ShiftDown()))
		{
			auto onDone = m_dialog.onDone;
			m_dialog = Dialog{};
			if (onDone)
				onDone();
			RebuildCurrent();
		}
		return;
	}
	if (m_dialog.type == Dialog::Type::Text)
	{
		// a real keyboard types straight into the text; arrows still move over the on-screen keys
		switch (key)
		{
		case WXK_ESCAPE:
			PressTextKey(KeyAction::Cancel);
			return;
		case WXK_BACK:
			PressTextKey(KeyAction::Backspace);
			return;
		case WXK_RETURN:
		case WXK_NUMPAD_ENTER:
			// multi-line: Enter is a new line, Ctrl+Enter finishes
			PressTextKey(m_dialog.multiline && !event.ControlDown() && !event.CmdDown() ? KeyAction::NewLine : KeyAction::Done);
			return;
		case WXK_UP:
		case WXK_NUMPAD_UP:
			HandleNav(Nav::Up);
			return;
		case WXK_DOWN:
		case WXK_NUMPAD_DOWN:
			HandleNav(Nav::Down);
			return;
		case WXK_LEFT:
		case WXK_NUMPAD_LEFT:
			HandleNav(Nav::Left);
			return;
		case WXK_RIGHT:
		case WXK_NUMPAD_RIGHT:
			HandleNav(Nav::Right);
			return;
		default:
			break;
		}
		if ((event.ControlDown() || event.CmdDown()) && key == 'V')
		{
			PressTextKey(KeyAction::Paste);
			return;
		}
		event.Skip(); // the character arrives in OnChar
		return;
	}
	switch (key)
	{
	case WXK_UP:
	case WXK_NUMPAD_UP:
		HandleNav(Nav::Up);
		break;
	case WXK_DOWN:
	case WXK_NUMPAD_DOWN:
		HandleNav(Nav::Down);
		break;
	case WXK_LEFT:
	case WXK_NUMPAD_LEFT:
		HandleNav(Nav::Left);
		break;
	case WXK_RIGHT:
	case WXK_NUMPAD_RIGHT:
		HandleNav(Nav::Right);
		break;
	case WXK_RETURN:
	case WXK_NUMPAD_ENTER:
	case WXK_SPACE:
		HandleNav(Nav::Accept);
		break;
	case WXK_ESCAPE:
	case WXK_BACK:
		HandleNav(Nav::Back);
		break;
	case 'X':
		HandleNav(Nav::Options);
		break;
	case 'Y':
		HandleNav(Nav::Settings);
		break;
	default:
		event.Skip();
	}
}

void GameModePanel::OnChar(wxKeyEvent& event)
{
	if (m_video)
		return;
	if (m_dialog.type != Dialog::Type::Text || event.ControlDown() || event.AltDown())
	{
		event.Skip();
		return;
	}
	const wxChar c = event.GetUnicodeKey();
	if (c == WXK_NONE || c < 32 || c == 127)
	{
		event.Skip();
		return;
	}
	TypeText(wxString(c));
	Refresh();
}

int GameModePanel::HitTest(const wxPoint& pos) const
{
	// last drawn wins (dialogs are drawn on top)
	for (auto it = m_hitRects.rbegin(); it != m_hitRects.rend(); ++it)
	{
		if (it->rect.Contains(pos))
			return it->index;
	}
	return kHitNone;
}

void GameModePanel::OnMouseMove(wxMouseEvent& event)
{
	const wxPoint pos = event.GetPosition();
	if (pos == m_lastMouse)
		return;
	m_lastMouse = pos;
	const int hit = HitTest(pos);
	if (hit < 0)
		return;
	if (m_dialog.type == Dialog::Type::Text)
	{
		if (hit != m_dialog.keyFocus && hit < (int)TextKeys().size())
		{
			m_dialog.keyFocus = hit;
			Refresh();
		}
		return;
	}
	if (m_dialog.type != Dialog::Type::None)
	{
		if (m_dialog.focus != hit && hit < (int)m_dialog.options.size())
		{
			m_dialog.focus = hit;
			Refresh();
		}
		return;
	}
	Page& page = CurrentPage();
	if (page.isLibrary)
	{
		if (hit < (int)m_games.size() && hit != m_libraryFocus)
		{
			m_libraryFocus = hit;
			Refresh();
		}
	}
	else if (hit < (int)page.rows.size() && page.rows[hit].Focusable() && hit != page.focus)
	{
		page.focus = hit;
		Refresh();
	}
}

void GameModePanel::OnMouseDown(wxMouseEvent& event)
{
	SetFocus();
	if (m_video)
	{
		EndVideo();
		return;
	}
	const bool right = event.RightDown();
	const int hit = HitTest(event.GetPosition());
	if (hit <= kHintBase && hit >= kHintLast)
	{
		HandleNav((Nav)(kHintBase - hit));
		return;
	}
	if (hit == kHitBackArrow)
	{
		HandleNav(Nav::Back);
		return;
	}
	if (hit == kHitSettingsChip)
	{
		HandleNav(Nav::Settings);
		return;
	}
	if (m_dialog.type == Dialog::Type::Text)
	{
		// clicks outside do not throw away what was typed; Cancel does that
		const auto keys = TextKeys();
		if (right)
			PressTextKey(KeyAction::Backspace);
		else if (hit >= 0 && hit < (int)keys.size())
		{
			m_dialog.keyFocus = hit;
			PressTextKey(keys[hit].action, keys[hit].insert);
		}
		return;
	}
	if (m_dialog.type != Dialog::Type::None)
	{
		if (hit == kHitOutsideDialog || right)
			HandleNav(Nav::Back);
		else if (hit >= 0 && hit < (int)m_dialog.options.size())
		{
			m_dialog.focus = hit;
			HandleNav(Nav::Accept);
		}
		return;
	}
	Page& page = CurrentPage();
	if (hit < 0)
	{
		if (right && !page.isLibrary)
			HandleNav(Nav::Back);
		return;
	}
	if (page.isLibrary)
	{
		m_libraryFocus = hit;
		HandleNav(right ? Nav::Options : Nav::Accept);
	}
	else
	{
		if (right)
		{
			HandleNav(Nav::Back);
			return;
		}
		page.focus = hit;
		HandleNav(Nav::Accept);
	}
}

void GameModePanel::OnMouseWheel(wxMouseEvent& event)
{
	if (m_video)
		return;
	const int steps = event.GetWheelRotation() / std::max(1, event.GetWheelDelta());
	for (int i = 0; i < std::abs(steps); i++)
		HandleNav(steps > 0 ? Nav::Up : Nav::Down);
}

void GameModePanel::OnSize(wxSizeEvent& event)
{
	const wxSize size = GetClientSize();
	m_scale = std::clamp(std::min(size.GetWidth() / 1920.0, size.GetHeight() / 1080.0), 0.45, 3.0);
	m_iconCache.clear();
	Refresh();
	event.Skip();
}

// ---------------------------------------------------------------------------------------------
// drawing

int GameModePanel::LibraryColumns(int width) const
{
	const double padding = S(48), gap = S(24), minTile = S(600);
	return std::max(1, (int)((width - 2 * padding + gap) / (minTile + gap)));
}

double GameModePanel::RowHeight(const Row& row) const
{
	switch (row.kind)
	{
	case RowKind::Header:
		return S(76);
	case RowKind::Info:
		return row.description.empty() ? S(84) : S(120);
	default:
		return row.description.empty() ? S(88) : S(116);
	}
}

std::vector<std::pair<Nav, wxString>> GameModePanel::CurrentHints() const
{
	if (m_dialog.type == Dialog::Type::Capture)
		return {};
	if (m_dialog.type == Dialog::Type::Text && m_backend->GetButtonStyle() == GameMode::ButtonStyle::Keyboard)
	{
		// typing on a real keyboard: X and Y are letters here, Enter and Esc finish or cancel
		return {{Nav::Accept, m_dialog.multiline ? _("New line (Ctrl+Enter: Done)") : _("Done")}, {Nav::Back, _("Cancel")}};
	}
	if (m_dialog.type == Dialog::Type::Text)
		return {{Nav::Accept, _("Type")}, {Nav::Back, m_dialog.text.empty() ? _("Cancel") : _("Delete")}, {Nav::Options, _("Space")}, {Nav::Settings, _("Done")}};
	if (m_dialog.type != Dialog::Type::None)
		return {{Nav::Accept, _("Select")}, {Nav::Back, _("Cancel")}};
	const Page& page = m_pages.back();
	if (page.isLibrary)
	{
		if (m_games.empty())
			return {{Nav::Settings, _("Settings")}};
		return {{Nav::Accept, _("Play")}, {Nav::Options, _("Game options")}, {Nav::Settings, _("Settings")}};
	}
	std::vector<std::pair<Nav, wxString>> hints{{Nav::Accept, _("Select")}};
	if (page.focus >= 0 && page.focus < (int)page.rows.size() && page.rows[page.focus].onOptions)
		hints.emplace_back(Nav::Options, page.rows[page.focus].optionsLabel);
	hints.emplace_back(Nav::Back, _("Back"));
	return hints;
}

wxBitmap GameModePanel::GetScaledIcon(uint64_t titleId, int size)
{
	const auto key = std::make_pair(titleId, size);
	if (auto it = m_iconCache.find(key); it != m_iconCache.end())
		return it->second;
	auto source = m_icons.find(titleId);
	if (source == m_icons.end())
		return wxNullBitmap;
	wxImage scaled = source->second.Scale(size, size, wxIMAGE_QUALITY_BICUBIC);
	RoundCorners(scaled, size * 0.12);
	wxBitmap bitmap(scaled);
	m_iconCache[key] = bitmap;
	return bitmap;
}

void GameModePanel::DrawIcon(wxGraphicsContext* gc, uint64_t titleId, const wxString& name, const wxRect& rect, double radius)
{
	if (m_icons.find(titleId) == m_icons.end())
	{
		// ask once; a title without an icon keeps the placeholder
		m_icons.emplace(titleId, wxImage());
		std::weak_ptr<bool> alive = m_alive;
		m_backend->RequestIcon(titleId, [this, alive](uint64_t id, const wxImage& image) {
			if (alive.expired() || !image.IsOk())
				return;
			m_icons[id] = image;
			for (auto it = m_iconCache.begin(); it != m_iconCache.end();)
				it = (it->first.first == id) ? m_iconCache.erase(it) : std::next(it);
			Refresh();
		});
	}
	const wxImage& image = m_icons[titleId];
	if (image.IsOk())
	{
		gc->DrawBitmap(GetScaledIcon(titleId, rect.width), rect.x, rect.y, rect.width, rect.height);
		return;
	}
	FillRounded(gc, rect.x, rect.y, rect.width, rect.height, radius, kSurfaceHighest);
	gc->SetFont(MakeFont(rect.height * 0.42, true), kPrimary);
	const wxString initial = name.empty() ? wxString("?") : name.Left(1).Upper();
	const double tw = TextWidth(gc, initial);
	DrawTextV(gc, initial, rect.x + (rect.width - tw) / 2, rect.y + rect.height / 2.0);
}

double GameModePanel::GlyphWidth(wxGraphicsContext* gc, Nav nav, double radius)
{
	if (m_backend->GetButtonStyle() != GameMode::ButtonStyle::Keyboard)
		return radius * 2;
	gc->SetFont(MakeFont(radius * 0.95, true), kOnSurface);
	return std::max(radius * 2, TextWidth(gc, KeyName(nav)) + radius * 1.1);
}

double GameModePanel::DrawGlyph(wxGraphicsContext* gc, Nav nav, double left, double cy, double radius)
{
	const double width = GlyphWidth(gc, nav, radius);
	const double cx = left + width / 2;
	if (m_backend->GetButtonStyle() == GameMode::ButtonStyle::Keyboard)
	{
		// a keycap: light key with a darker bottom edge
		const double h = radius * 2;
		FillRounded(gc, left, cy - radius, width, h, radius * 0.35, kOutline);
		FillRounded(gc, left, cy - radius, width, h - radius * 0.22, radius * 0.35, kOnSurface);
		gc->SetFont(MakeFont(radius * 0.95, true), kBackground);
		const wxString name = KeyName(nav);
		DrawTextV(gc, name, cx - TextWidth(gc, name) / 2, cy - radius * 0.1);
		return width;
	}
	using GameMode::ButtonStyle;
	using GameMode::Face;
	const ButtonStyle style = m_backend->GetButtonStyle();
	const Face face = m_backend->GetNavFace(nav);
	if (style == ButtonStyle::PlayStation)
	{
		// dark button with a coloured symbol
		FillCircle(gc, cx, cy, radius, kSurfaceHighest);
		gc->SetPen(wxPen(kOutline, std::max(1, (int)std::lround(radius * 0.08))));
		gc->SetBrush(*wxTRANSPARENT_BRUSH);
		gc->DrawEllipse(cx - radius, cy - radius, radius * 2, radius * 2);
		const double r = radius * 0.46;
		const double w = std::max(1.5, radius * 0.16);
		wxGraphicsPath path = gc->CreatePath();
		wxColour colour;
		switch (face)
		{
		case Face::South: // cross
			colour = wxColour(0x7C, 0xB2, 0xE8);
			path.MoveToPoint(cx - r, cy - r); path.AddLineToPoint(cx + r, cy + r);
			path.MoveToPoint(cx + r, cy - r); path.AddLineToPoint(cx - r, cy + r);
			break;
		case Face::East: // circle
			colour = wxColour(0xFF, 0x6B, 0x6B);
			path.AddCircle(cx, cy, r);
			break;
		case Face::West: // square
			colour = wxColour(0xE0, 0x9E, 0xE0);
			path.AddRectangle(cx - r * 0.9, cy - r * 0.9, r * 1.8, r * 1.8);
			break;
		default: // triangle
			colour = wxColour(0x4C, 0xD9, 0xA6);
			path.MoveToPoint(cx, cy - r);
			path.AddLineToPoint(cx + r * 1.05, cy + r * 0.75);
			path.AddLineToPoint(cx - r * 1.05, cy + r * 0.75);
			path.CloseSubpath();
			break;
		}
		gc->SetPen(wxPen(colour, (int)std::lround(w)));
		gc->StrokePath(path);
		return width;
	}

	const wxString letter = FaceLabel(style, face);
	wxColour fill = kOnSurface, text = kBackground;
	if (style == ButtonStyle::Xbox)
	{
		// the classic colours: A green, B red, X blue, Y yellow
		switch (face)
		{
		case Face::South: fill = wxColour(0x6C, 0xC0, 0x4A); break;
		case Face::East: fill = wxColour(0xE8, 0x5A, 0x4F); break;
		case Face::West: fill = wxColour(0x4A, 0x9C, 0xE8); break;
		default: fill = wxColour(0xF2, 0xC6, 0x3C); break;
		}
		text = wxColour(0x14, 0x12, 0x18);
	}
	else if (style == ButtonStyle::SteamDeck)
	{
		// dark buttons with light letters
		fill = kSurfaceHighest;
		text = kOnSurface;
	}
	FillCircle(gc, cx, cy, radius, fill);
	if (style == ButtonStyle::SteamDeck)
	{
		gc->SetPen(wxPen(kOutline, std::max(1, (int)std::lround(radius * 0.08))));
		gc->SetBrush(*wxTRANSPARENT_BRUSH);
		gc->DrawEllipse(cx - radius, cy - radius, radius * 2, radius * 2);
	}
	gc->SetFont(MakeFont(radius * 1.15, true), text);
	const double tw = TextWidth(gc, letter);
	DrawTextV(gc, letter, cx - tw / 2, cy);
	return width;
}

void GameModePanel::OnPaint(wxPaintEvent& event)
{
	wxAutoBufferedPaintDC dc(this);
	Render(dc);
}

void GameModePanel::Render(wxDC& dc)
{
	dc.SetBackground(wxBrush(kBackground));
	dc.Clear();
	std::unique_ptr<wxGraphicsContext> gc(wxGraphicsRenderer::GetDefaultRenderer()->CreateContextFromUnknownDC(dc));
	if (!gc)
		return;
	gc->SetAntialiasMode(wxANTIALIAS_DEFAULT);
	m_hitRects.clear();

	const wxSize size = GetClientSize();
	if (m_scale <= 0 || size.GetWidth() <= 0)
		return;
	if (m_video)
	{
		DrawVideo(gc.get(), size);
		return;
	}
	const int topH = (int)S(120), hintH = (int)S(92);
	const wxRect top(0, 0, size.GetWidth(), topH);
	const wxRect content(0, topH, size.GetWidth(), std::max(0, size.GetHeight() - topH - hintH));
	const wxRect hints(0, size.GetHeight() - hintH, size.GetWidth(), hintH);

	gc->PushState();
	gc->Clip(content.x, content.y, content.width, content.height);
	if (CurrentPage().isLibrary)
		DrawLibrary(gc.get(), content);
	else
		DrawList(gc.get(), CurrentPage(), content);
	gc->PopState();

	DrawTopBar(gc.get(), top);
	DrawHints(gc.get(), hints);
	DrawToast(gc.get(), content);
	if (m_dialog.type != Dialog::Type::None)
		DrawDialog(gc.get(), wxRect(wxPoint(0, 0), size));
	// the keyboard's button hints (Type, Delete, Space, Done) stay readable over the scrim
	if (m_dialog.type == Dialog::Type::Text)
		DrawHints(gc.get(), hints);

	// after the boot video: the launcher fades in from the background colour
	if (m_fadeInStartMs != 0)
	{
		const double t = std::clamp((NowMs() - m_fadeInStartMs).ToDouble() / 400.0, 0.0, 1.0);
		const int alpha = (int)std::lround(255 * (1 - t) * (1 - t));
		if (alpha > 0)
		{
			gc->SetPen(*wxTRANSPARENT_PEN);
			gc->SetBrush(wxBrush(WithAlpha(kBackground, alpha)));
			gc->DrawRectangle(0, 0, size.GetWidth(), size.GetHeight());
		}
	}
}

void GameModePanel::DrawVideo(wxGraphicsContext* gc, const wxSize& size)
{
	// the launcher's background around the picture, so a video made on it blends in
	gc->SetPen(*wxTRANSPARENT_PEN);
	gc->SetBrush(wxBrush(kBackground));
	gc->DrawRectangle(0, 0, size.GetWidth(), size.GetHeight());
	m_video->SetOutputSize(size.GetWidth(), size.GetHeight());
	wxImage image;
	if (m_video->TakeFrame(image) && image.IsOk())
		m_videoFrame = wxBitmap(image);
	if (m_videoFrame.IsOk())
	{
		const int w = m_videoFrame.GetWidth(), h = m_videoFrame.GetHeight();
		gc->DrawBitmap(m_videoFrame, (size.GetWidth() - w) / 2, (size.GetHeight() - h) / 2, w, h);
	}
}

void GameModePanel::DrawTopBar(wxGraphicsContext* gc, const wxRect& area)
{
	const Page& page = m_pages.back();
	const double pad = S(48);
	const double cy = area.y + area.height / 2.0;
	if (page.isLibrary)
	{
		gc->SetFont(MakeFont(S(46), true), kOnSurface);
		DrawTextV(gc, "Cemu", area.x + pad, cy);
		const double w = TextWidth(gc, "Cemu");
		gc->SetFont(MakeFont(S(26)), kOnSurfaceVariant);
		DrawTextV(gc, _("Game Mode"), area.x + pad + w + S(18), cy + S(4));

		// right side: status and a settings chip (also clickable)
		const wxString chipText = _("Settings");
		const double chipGlyphW = GlyphWidth(gc, Nav::Settings, S(18));
		gc->SetFont(MakeFont(S(26), true), kOnSecondaryContainer);
		const double chipW = TextWidth(gc, chipText) + chipGlyphW + S(28) + S(40);
		const double chipH = S(60);
		const double chipX = area.GetRight() - pad - chipW;
		FillRounded(gc, chipX, cy - chipH / 2, chipW, chipH, chipH / 2, kSecondaryContainer);
		DrawGlyph(gc, Nav::Settings, chipX + S(16), cy, S(18));
		gc->SetFont(MakeFont(S(26), true), kOnSecondaryContainer); // the glyph changed the font
		DrawTextV(gc, chipText, chipX + S(16) + chipGlyphW + S(12), cy);
		m_hitRects.push_back({wxRect((int)chipX, (int)(cy - chipH / 2), (int)chipW, (int)chipH), kHitSettingsChip});

		wxString status;
		if (m_backend->IsScanningGames())
			status = _("Looking for games...");
		else if (!m_games.empty())
			status = wxString::Format(m_games.size() == 1 ? _("%d game") : _("%d games"), (int)m_games.size());
		if (!status.empty())
		{
			gc->SetFont(MakeFont(S(24)), kOnSurfaceVariant);
			DrawTextV(gc, status, chipX - S(28) - TextWidth(gc, status), cy);
		}
		return;
	}

	// back arrow + page title
	const double arrowCx = area.x + pad + S(16);
	DrawChevron(gc, arrowCx, cy, S(14), true, kOnSurface, S(4));
	m_hitRects.push_back({wxRect((int)(arrowCx - S(32)), (int)(cy - S(32)), (int)S(64), (int)S(64)), kHitBackArrow});
	gc->SetFont(MakeFont(S(40), true), kOnSurface);
	DrawTextV(gc, Ellipsize(gc, page.title, area.width - pad * 2 - S(80)), area.x + pad + S(56), cy);
}

void GameModePanel::DrawHints(wxGraphicsContext* gc, const wxRect& area)
{
	gc->SetPen(*wxTRANSPARENT_PEN);
	gc->SetBrush(wxBrush(kSurfaceLow));
	gc->DrawRectangle(area.x, area.y, area.width, area.height);
	const auto hints = CurrentHints();
	double x = area.GetRight() - S(48);
	const double cy = area.y + area.height / 2.0;
	gc->SetFont(MakeFont(S(26)), kOnSurface);
	for (auto it = hints.rbegin(); it != hints.rend(); ++it)
	{
		const wxString& label = it->second;
		gc->SetFont(MakeFont(S(26)), kOnSurface);
		const double labelW = TextWidth(gc, label);
		const double glyphW = GlyphWidth(gc, it->first, S(22));
		const double itemW = glyphW + S(12) + labelW;
		const double itemX = x - itemW;
		DrawGlyph(gc, it->first, itemX, cy, S(22));
		gc->SetFont(MakeFont(S(26)), kOnSurface);
		DrawTextV(gc, label, itemX + glyphW + S(12), cy);
		m_hitRects.push_back({wxRect((int)itemX, (int)(cy - S(30)), (int)itemW, (int)S(60)), kHintBase - (int)it->first});
		x = itemX - S(40);
	}
}

void GameModePanel::DrawLibrary(wxGraphicsContext* gc, const wxRect& area)
{
	const double pad = S(48), gap = S(24), tileH = S(152), radius = S(24);
	if (m_games.empty())
	{
		const bool scanning = m_backend->IsScanningGames();
		gc->SetFont(MakeFont(S(38), true), kOnSurface);
		const wxString title = scanning ? _("Looking for games...") : _("No games found");
		const double cy = area.y + area.height / 2.0;
		DrawTextV(gc, title, area.x + (area.width - TextWidth(gc, title)) / 2, cy - S(28));
		if (!scanning)
		{
			gc->SetFont(MakeFont(S(26)), kOnSurfaceVariant);
			const wxString hint = _("Add game folders in General settings. Exit Game Mode from Settings to get there.");
			DrawTextV(gc, hint, area.x + (area.width - TextWidth(gc, hint)) / 2, cy + S(28));
		}
		return;
	}

	const int cols = LibraryColumns(area.width);
	const double tileW = (area.width - 2 * pad - (cols - 1) * gap) / cols;
	m_libraryFocus = std::clamp(m_libraryFocus, 0, (int)m_games.size() - 1);

	// keep the focused row in view
	const int rowsTotal = ((int)m_games.size() + cols - 1) / cols;
	const double contentH = S(16) + rowsTotal * (tileH + gap);
	const int focusRow = m_libraryFocus / cols;
	const double focusTop = S(16) + focusRow * (tileH + gap);
	const double margin = S(24);
	double target = m_libraryScrollTarget;
	if (focusTop - target < margin)
		target = focusTop - margin;
	if (focusTop + tileH - target > area.height - margin)
		target = focusTop + tileH - area.height + margin;
	target = std::clamp(target, 0.0, std::max(0.0, contentH - area.height + margin));
	m_libraryScrollTarget = target;

	for (size_t i = 0; i < m_games.size(); i++)
	{
		const int row = (int)i / cols, col = (int)i % cols;
		const double x = area.x + pad + col * (tileW + gap);
		const double y = area.y + S(16) + row * (tileH + gap) - m_libraryScroll;
		if (y + tileH < area.y || y > area.GetBottom())
			continue;
		const bool focused = (int)i == m_libraryFocus;
		const auto& game = m_games[i];
		FillRounded(gc, x, y, tileW, tileH, radius, focused ? kSecondaryContainer : kSurfaceLow);
		if (focused)
			StrokeRounded(gc, x + S(1.5), y + S(1.5), tileW - S(3), tileH - S(3), radius, kPrimary, S(3));
		const int iconSize = (int)(tileH - S(36));
		DrawIcon(gc, game.titleId, game.name, wxRect((int)(x + S(18)), (int)(y + S(18)), iconSize, iconSize), S(16));
		const double textX = x + S(18) + iconSize + S(24);
		const double textW = tileW - (textX - x) - S(24);
		gc->SetFont(MakeFont(S(32), focused), focused ? kOnSecondaryContainer : kOnSurface);
		if (game.favorite)
		{
			DrawTextV(gc, Ellipsize(gc, game.name, textW), textX, y + tileH / 2 - S(18));
			gc->SetFont(MakeFont(S(22)), kPrimary);
			DrawTextV(gc, wxString::FromUTF8("★ ") + _("Favourite"), textX, y + tileH / 2 + S(24));
		}
		else
			DrawTextV(gc, Ellipsize(gc, game.name, textW), textX, y + tileH / 2);
		m_hitRects.push_back({wxRect((int)x, (int)std::max<double>(y, area.y), (int)tileW, (int)tileH), (int)i});
	}
}

void GameModePanel::DrawList(wxGraphicsContext* gc, Page& page, const wxRect& area)
{
	const double columnW = std::min<double>(area.width - S(96), S(1160));
	const double x = area.x + (area.width - columnW) / 2;
	double y = S(16);

	// header with the game's icon
	double headerH = 0;
	if (page.headerTitleId)
		headerH = S(196);

	// layout pass: row tops relative to the content origin
	std::vector<double> tops(page.rows.size());
	double cursor = y + headerH;
	for (size_t i = 0; i < page.rows.size(); i++)
	{
		tops[i] = cursor;
		cursor += RowHeight(page.rows[i]) + S(8);
	}
	const double contentH = cursor + S(16);

	// keep focus in view (and the header visible when the first row is focused)
	if (!page.rows.empty())
	{
		const int f = std::clamp(page.focus, 0, (int)page.rows.size() - 1);
		// on the first focusable row, scroll right to the top so the header and any section
		// title above it stay visible
		bool firstFocusable = true;
		for (int i = 0; i < f; i++)
			firstFocusable &= !page.rows[i].Focusable();
		const double top = firstFocusable ? 0 : tops[f];
		const double bottom = tops[f] + RowHeight(page.rows[f]);
		double target = page.scrollTarget;
		const double margin = S(32);
		if (top - target < margin)
			target = top - margin;
		if (bottom - target > area.height - margin)
			target = bottom - area.height + margin;
		page.scrollTarget = std::clamp(target, 0.0, std::max(0.0, contentH - area.height));
	}

	const double originY = area.y - page.scroll;
	if (page.headerTitleId)
	{
		const int iconSize = (int)S(148);
		const double hy = originY + y + S(8);
		wxString name = page.title;
		for (const auto& g : m_games)
			if (g.titleId == page.headerTitleId)
				name = g.name;
		DrawIcon(gc, page.headerTitleId, name, wxRect((int)x, (int)hy, iconSize, iconSize), S(22));
		gc->SetFont(MakeFont(S(40), true), kOnSurface);
		const double tx = x + iconSize + S(32);
		DrawTextV(gc, Ellipsize(gc, name, columnW - iconSize - S(32)), tx, hy + iconSize / 2.0 - S(20));
		gc->SetFont(MakeFont(S(22)), kOnSurfaceVariant);
		DrawTextV(gc, wxString::Format("%016llx", (unsigned long long)page.headerTitleId), tx, hy + iconSize / 2.0 + S(28));
	}

	for (size_t i = 0; i < page.rows.size(); i++)
	{
		const double h = RowHeight(page.rows[i]);
		const double ry = originY + tops[i];
		if (ry + h < area.y || ry > area.GetBottom())
			continue;
		const wxRect rect((int)x, (int)ry, (int)columnW, (int)h);
		DrawRow(gc, page.rows[i], rect, (int)i == page.focus);
		if (page.rows[i].Focusable())
			m_hitRects.push_back({rect, (int)i});
	}
}

void GameModePanel::DrawRow(wxGraphicsContext* gc, const Row& row, const wxRect& rect, bool focused)
{
	const double padX = S(32);
	const double cy = rect.y + rect.height / 2.0;
	if (row.kind == RowKind::Header)
	{
		gc->SetFont(MakeFont(S(24), true), kPrimary);
		DrawTextV(gc, row.label, rect.x + padX, rect.y + rect.height - S(24));
		return;
	}
	const bool enabled = row.Enabled();
	if (row.kind == RowKind::Info)
	{
		FillRounded(gc, rect.x, rect.y, rect.width, rect.height, S(20), kSurfaceLow);
	}
	else if (focused)
	{
		FillRounded(gc, rect.x, rect.y, rect.width, rect.height, S(20), kSecondaryContainer);
		StrokeRounded(gc, rect.x + S(1.5), rect.y + S(1.5), rect.width - S(3), rect.height - S(3), S(20), kPrimary, S(3));
	}

	const wxColour labelColour = !enabled ? WithAlpha(kOnSurface, 97) : row.danger ? kError : focused ? kOnSecondaryContainer : kOnSurface;
	const wxColour descColour = !enabled ? WithAlpha(kOnSurfaceVariant, 97) : kOnSurfaceVariant;

	// right-hand widget, drawn first so the label can be ellipsized against it
	double widgetW = 0;
	const double right = rect.GetRight() - padX;
	switch (row.kind)
	{
	case RowKind::Toggle:
	{
		const bool on = row.getBool();
		const double tw = S(80), th = S(44);
		widgetW = tw;
		const double tx = right - tw, ty = cy - th / 2;
		if (on)
		{
			FillRounded(gc, tx, ty, tw, th, th / 2, enabled ? kPrimary : WithAlpha(kOnSurface, 31));
			FillCircle(gc, tx + tw - th / 2, cy, S(15), enabled ? kOnPrimary : kSurface);
		}
		else
		{
			FillRounded(gc, tx, ty, tw, th, th / 2, kSurfaceHighest);
			StrokeRounded(gc, tx, ty, tw, th, th / 2, enabled ? kOutline : WithAlpha(kOnSurface, 31), S(3));
			FillCircle(gc, tx + th / 2, cy, S(11), enabled ? kOutline : WithAlpha(kOnSurface, 97));
		}
		break;
	}
	case RowKind::Choice:
	{
		const auto choice = row.getChoice();
		wxString value = (choice.selected >= 0 && choice.selected < (int)choice.options.size()) ? choice.options[choice.selected] : wxString();
		gc->SetFont(MakeFont(S(26), true), enabled ? (focused ? kOnSecondaryContainer : kPrimary) : descColour);
		value = Ellipsize(gc, value, rect.width * 0.42);
		const double vw = TextWidth(gc, value);
		const double arrows = focused ? S(34) : 0;
		widgetW = vw + arrows * 2;
		DrawTextV(gc, value, right - arrows - vw, cy);
		if (focused)
		{
			const wxColour a = enabled ? kOnSecondaryContainer : descColour;
			DrawChevron(gc, right - arrows - vw - S(22), cy, S(9), true, WithAlpha(a, choice.selected > 0 ? 255 : 80), S(3));
			DrawChevron(gc, right - S(12), cy, S(9), false, WithAlpha(a, choice.selected + 1 < (int)choice.options.size() ? 255 : 80), S(3));
		}
		break;
	}
	case RowKind::Slider:
	{
		const int value = row.getInt();
		const wxString text = wxString::Format("%d%%", value);
		gc->SetFont(MakeFont(S(26), true), focused ? kOnSecondaryContainer : kOnSurface);
		const double textW = S(84);
		DrawTextV(gc, text, right - TextWidth(gc, text), cy);
		const double barW = S(300), barH = S(10);
		const double bx = right - textW - S(20) - barW;
		const double frac = (value - row.minValue) / (double)std::max(1, row.maxValue - row.minValue);
		FillRounded(gc, bx, cy - barH / 2, barW, barH, barH / 2, kOutlineVariant);
		FillRounded(gc, bx, cy - barH / 2, std::max(barH, barW * frac), barH, barH / 2, kPrimary);
		FillCircle(gc, bx + barW * frac, cy, S(14), kPrimary);
		widgetW = barW + S(20) + textW;
		break;
	}
	case RowKind::Link:
	case RowKind::Action:
	{
		double w = 0;
		if (row.kind == RowKind::Link)
		{
			DrawChevron(gc, right - S(8), cy, S(11), false, focused ? kOnSecondaryContainer : kOnSurfaceVariant, S(3.5));
			w = S(34);
		}
		if (row.getValueText)
		{
			const wxString value = row.getValueText();
			gc->SetFont(MakeFont(S(26)), focused ? kOnSecondaryContainer : kOnSurfaceVariant);
			const double vw = TextWidth(gc, value);
			DrawTextV(gc, value, right - w - vw, cy);
			w += vw + S(8);
		}
		widgetW = w;
		break;
	}
	default:
		break;
	}

	const double labelMaxW = rect.width - padX * 2 - widgetW - S(24);
	if (row.description.empty())
	{
		gc->SetFont(MakeFont(S(30), row.kind == RowKind::Info), labelColour);
		DrawTextV(gc, Ellipsize(gc, row.label, labelMaxW), rect.x + padX, cy);
	}
	else
	{
		gc->SetFont(MakeFont(S(30), row.kind == RowKind::Info), labelColour);
		DrawTextV(gc, Ellipsize(gc, row.label, labelMaxW), rect.x + padX, cy - S(18));
		gc->SetFont(MakeFont(S(22)), descColour);
		DrawTextV(gc, Ellipsize(gc, row.description, labelMaxW), rect.x + padX, cy + S(22));
	}
}

void GameModePanel::DrawDialog(wxGraphicsContext* gc, const wxRect& area)
{
	gc->SetPen(*wxTRANSPARENT_PEN);
	gc->SetBrush(wxBrush(wxColour(0, 0, 0, 150)));
	gc->DrawRectangle(area.x, area.y, area.width, area.height);
	m_hitRects.push_back({area, kHitOutsideDialog});

	if (m_dialog.type == Dialog::Type::Text)
	{
		DrawTextDialog(gc, area);
		return;
	}
	const Dialog& d = m_dialog;
	const double w = std::min<double>(area.width - S(64), S(760));
	const double pad = S(40);
	const double optionH = S(80);
	double h = pad + S(56); // title
	if (!d.message.empty())
		h += S(64);
	if (d.type == Dialog::Type::Choice)
		h += optionH * std::min<size_t>(d.options.size(), 8) + S(8);
	else if (d.type == Dialog::Type::Confirm)
		h += S(96);
	else
		h += S(24);
	h += pad - S(8);
	const double x = area.x + (area.width - w) / 2;
	const double y = area.y + (area.height - h) / 2;
	FillRounded(gc, x, y, w, h, S(28), kSurfaceHigh);
	m_hitRects.push_back({wxRect((int)x, (int)y, (int)w, (int)h), kHitDialogCard}); // swallow clicks on the card

	double cy = y + pad + S(20);
	gc->SetFont(MakeFont(S(34), true), kOnSurface);
	DrawTextV(gc, Ellipsize(gc, d.title, w - pad * 2), x + pad, cy);
	cy += S(40);
	if (!d.message.empty())
	{
		gc->SetFont(MakeFont(S(26)), kOnSurfaceVariant);
		wxString message = d.message;
		if (d.type == Dialog::Type::Capture)
		{
			const int left = std::max(0, 6 - (int)((NowMs() - d.startedMs).GetValue() / 1000));
			message += wxString::Format(" (%d)", left);
		}
		DrawTextV(gc, Ellipsize(gc, message, w - pad * 2), x + pad, cy + S(24));
		cy += S(64);
	}

	if (d.type == Dialog::Type::Choice)
	{
		// scroll the option list if it is long
		const int visible = (int)std::min<size_t>(d.options.size(), 8);
		const int first = std::clamp(d.focus - visible + 1, 0, std::max(0, (int)d.options.size() - visible));
		double oy = cy + S(4);
		for (int i = first; i < first + visible; i++)
		{
			const bool focused = i == d.focus;
			const wxRect r((int)(x + S(16)), (int)oy, (int)(w - S(32)), (int)(optionH - S(6)));
			if (focused)
				FillRounded(gc, r.x, r.y, r.width, r.height, S(18), kSecondaryContainer);
			const double rcx = r.x + S(40), rcy = r.y + r.height / 2.0;
			StrokeRounded(gc, rcx - S(14), rcy - S(14), S(28), S(28), S(14), focused ? kPrimary : kOnSurfaceVariant, S(3));
			if (focused)
				FillCircle(gc, rcx, rcy, S(8), kPrimary);
			gc->SetFont(MakeFont(S(28), focused), focused ? kOnSecondaryContainer : kOnSurface);
			DrawTextV(gc, Ellipsize(gc, d.options[i], r.width - S(96)), r.x + S(76), rcy);
			m_hitRects.push_back({r, i});
			oy += optionH;
		}
	}
	else if (d.type == Dialog::Type::Confirm)
	{
		// buttons on the right, OK last
		double bx = x + w - pad;
		const double by = cy + S(20);
		const double bh = S(64);
		for (int i = (int)d.options.size() - 1; i >= 0; i--)
		{
			gc->SetFont(MakeFont(S(28), true), i == d.focus ? kOnPrimary : kPrimary);
			const double bw = TextWidth(gc, d.options[i]) + S(64);
			bx -= bw;
			if (i == d.focus)
				FillRounded(gc, bx, by, bw, bh, bh / 2, kPrimary);
			else
				StrokeRounded(gc, bx, by, bw, bh, bh / 2, kOutline, S(2));
			DrawTextV(gc, d.options[i], bx + S(32), by + bh / 2);
			m_hitRects.push_back({wxRect((int)bx, (int)by, (int)bw, (int)bh), i});
			bx -= S(16);
		}
	}
}

void GameModePanel::DrawTextDialog(wxGraphicsContext* gc, const wxRect& area)
{
	const Dialog& d = m_dialog;
	const auto keys = TextKeys();
	const int keyRows = keys.empty() ? 0 : keys.back().row + 1;
	// fit between the top of the window and the hint bar, which stays visible above the scrim
	const double hintH = S(92);
	const double avail = area.height - hintH - S(24);
	const double w = std::min<double>(area.width - S(48), S(1240));
	const double pad = S(36);
	const double gap = S(10);
	double keyH = S(76);
	const double lineH = S(40);
	const int textLines = d.multiline ? 5 : 1;
	const double boxH = textLines * lineH + S(28);
	auto heightFor = [&](double kh) {
		return pad + S(48) + (d.message.empty() ? 0 : S(44)) + (d.error.empty() ? 0 : S(40)) + S(12) + boxH + S(24) + keyRows * (kh + gap) - gap + pad;
	};
	if (heightFor(keyH) > avail)
		keyH = std::max(S(40), keyH - (heightFor(keyH) - avail) / std::max(1, keyRows));
	const double h = std::min(avail, heightFor(keyH));
	const double x = area.x + (area.width - w) / 2;
	const double y = area.y + std::max(S(12), (avail - h) / 2);
	FillRounded(gc, x, y, w, h, S(28), kSurfaceHigh);
	m_hitRects.push_back({wxRect((int)x, (int)y, (int)w, (int)h), kHitDialogCard});

	double cy = y + pad + S(18);
	gc->SetFont(MakeFont(S(34), true), kOnSurface);
	DrawTextV(gc, Ellipsize(gc, d.title, w - pad * 2), x + pad, cy);
	cy += S(30);
	if (!d.message.empty())
	{
		gc->SetFont(MakeFont(S(24)), kOnSurfaceVariant);
		DrawTextV(gc, Ellipsize(gc, d.message, w - pad * 2), x + pad, cy + S(22));
		cy += S(44);
	}
	if (!d.error.empty())
	{
		gc->SetFont(MakeFont(S(24), true), kError);
		DrawTextV(gc, Ellipsize(gc, d.error, w - pad * 2), x + pad, cy + S(20));
		cy += S(40);
	}
	cy += S(12);

	// the text box: the last lines that fit, the end of a long line, and a caret
	const double bx = x + pad, bw = w - pad * 2, by = cy;
	FillRounded(gc, bx, by, bw, boxH, S(16), kSurface);
	StrokeRounded(gc, bx, by, bw, boxH, S(16), kPrimary, S(2));
	const wxFont textFont = d.multiline ? wxFont(wxFontInfo(wxSize(0, std::max(6, (int)std::lround(S(28))))).Family(wxFONTFAMILY_TELETYPE)) : MakeFont(S(30));
	const double innerW = bw - S(40);
	if (d.text.empty())
	{
		gc->SetFont(textFont, WithAlpha(kOnSurfaceVariant, 140));
		DrawTextV(gc, Ellipsize(gc, d.hint, innerW), bx + S(20), by + S(14) + lineH / 2);
		gc->SetPen(*wxTRANSPARENT_PEN);
		gc->SetBrush(wxBrush(kPrimary));
		gc->DrawRectangle(bx + S(18), by + S(14) + S(4), S(3), lineH - S(8));
	}
	else
	{
		gc->SetFont(textFont, kOnSurface);
		wxArrayString lines = wxSplit(d.text, '\n', '\0');
		if (lines.empty())
			lines.push_back(wxString());
		const int first = std::max(0, (int)lines.size() - textLines);
		double ly = by + S(14);
		double caretX = bx + S(20), caretY = ly;
		for (int i = first; i < (int)lines.size(); i++)
		{
			wxString line = lines[i];
			// keep the end of the line in view
			if (TextWidth(gc, line) > innerW - S(8))
			{
				const wxString ellipsis = wxString::FromUTF8("\u2026");
				size_t cut = 0;
				while (cut < line.length() && TextWidth(gc, ellipsis + line.Mid(cut)) > innerW - S(8))
					cut++;
				line = ellipsis + line.Mid(cut);
			}
			DrawTextV(gc, line, bx + S(20), ly + lineH / 2);
			caretX = bx + S(20) + TextWidth(gc, line);
			caretY = ly;
			ly += lineH;
		}
		gc->SetPen(*wxTRANSPARENT_PEN);
		gc->SetBrush(wxBrush(kPrimary));
		gc->DrawRectangle(caretX + S(2), caretY + S(4), S(3), lineH - S(8));
	}
	cy = by + boxH + S(24);

	// keys
	const double unit = (bw + gap) / 10.0;
	for (int i = 0; i < (int)keys.size(); i++)
	{
		const Key& key = keys[i];
		const double kx = bx + key.x * unit;
		const double ky = cy + key.row * (keyH + gap);
		const double kw = key.w * unit - gap;
		const bool focused = i == d.keyFocus;
		const bool special = key.action != KeyAction::Char;
		const bool active = (key.action == KeyAction::Shift && d.keyLayer == kUpper) || (key.action == KeyAction::Symbols && d.keyLayer == kSymbols);
		wxColour fill = special ? kSurfaceHighest : kSurfaceLow;
		wxColour text = kOnSurface;
		if (key.action == KeyAction::Done)
		{
			fill = kPrimary;
			text = kOnPrimary;
		}
		else if (active)
		{
			fill = kSecondaryContainer;
			text = kOnSecondaryContainer;
		}
		if (focused && key.action != KeyAction::Done)
		{
			fill = kSecondaryContainer;
			text = kOnSecondaryContainer;
		}
		FillRounded(gc, kx, ky, kw, keyH, S(14), fill);
		if (focused)
			StrokeRounded(gc, kx + S(1.5), ky + S(1.5), kw - S(3), keyH - S(3), S(14), key.action == KeyAction::Done ? kOnSurface : kPrimary, S(3));
		gc->SetFont(MakeFont(special ? S(26) : S(32), special), text);
		const wxString label = Ellipsize(gc, key.label, kw - S(12));
		DrawTextV(gc, label, kx + (kw - TextWidth(gc, label)) / 2, ky + keyH / 2);
		m_hitRects.push_back({wxRect((int)kx, (int)ky, (int)kw, (int)keyH), i});
	}
}

void GameModePanel::DrawToast(wxGraphicsContext* gc, const wxRect& area)
{
	if (m_toast.empty())
		return;
	gc->SetFont(MakeFont(S(26)), kSurface);
	const wxString text = Ellipsize(gc, m_toast, area.width - S(160));
	const double w = TextWidth(gc, text) + S(64), h = S(68);
	const double x = area.x + (area.width - w) / 2, y = area.GetBottom() - h - S(24);
	FillRounded(gc, x, y, w, h, S(14), kOnSurface);
	gc->SetFont(MakeFont(S(26)), kSurface);
	DrawTextV(gc, text, x + S(32), y + h / 2);
}
