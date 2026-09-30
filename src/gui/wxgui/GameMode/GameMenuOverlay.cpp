// The Game Menu: a side menu drawn over the running game with ImGui, in the style of the Cemu
// Android app's in-game drawer. Runs on the GPU thread inside the overlay pass; anything that
// touches wx or the GUI config is handed to the UI thread with CallAfter.

#include "wxgui/GameMode/GameMode.h"

#include "wxgui/MainWindow.h"
#include "wxgui/wxCemuConfig.h"

#include "Cafe/CafeSystem.h"
#include "Cafe/Cheats/CheatManager.h"
#include "Cafe/HW/Latte/Core/Latte.h"
#include "Cafe/HW/Latte/Core/LatteAsyncCommands.h"
#include "Cafe/HW/Latte/Core/LatteTextureReplace.h"
#include "config/CemuConfig.h"
#include "imgui/imgui_extension.h"

#include <imgui.h>
#include <wx/app.h>

#include <algorithm>
#include <cfloat>
#include <string>

namespace GameMode
{
	namespace
	{
		// same palette as the launcher (Material 3 baseline dark)
		constexpr ImU32 kScrim = IM_COL32(0, 0, 0, 140);
		constexpr ImU32 kSurfaceLow = IM_COL32(0x1D, 0x1B, 0x20, 0xF5);
		constexpr ImU32 kSurfaceHighest = IM_COL32(0x36, 0x34, 0x3B, 0xFF);
		constexpr ImU32 kPrimary = IM_COL32(0xD0, 0xBC, 0xFF, 0xFF);
		constexpr ImU32 kOnPrimary = IM_COL32(0x38, 0x1E, 0x72, 0xFF);
		constexpr ImU32 kSecondaryContainer = IM_COL32(0x4A, 0x44, 0x58, 0xFF);
		constexpr ImU32 kOnSecondaryContainer = IM_COL32(0xE8, 0xDE, 0xF8, 0xFF);
		constexpr ImU32 kOnSurface = IM_COL32(0xE6, 0xE0, 0xE9, 0xFF);
		constexpr ImU32 kOnSurfaceVariant = IM_COL32(0xCA, 0xC4, 0xD0, 0xFF);
		constexpr ImU32 kOutline = IM_COL32(0x93, 0x8F, 0x99, 0xFF);
		constexpr ImU32 kError = IM_COL32(0xF2, 0xB8, 0xB5, 0xFF);
		constexpr ImU32 kGlyphText = IM_COL32(0x14, 0x12, 0x18, 0xFF);

		enum class Page
		{
			Main,
			Layout,
			Cheats,
			ConfirmExit,
		};

		enum class ItemKind
		{
			Action,
			Toggle,
			Link,
			Radio,
		};

		struct Item
		{
			ItemKind kind = ItemKind::Action;
			std::string label;
			std::string value;
			bool on = false;
			bool danger = false;
			std::function<void()> activate;
			std::function<void(int)> adjust; // left/right
		};

		struct MenuState
		{
			bool wasOpen = false;
			Page page = Page::Main;
			int focus = 0;
			uint64 titleId = 0;
			std::string gameName;
			std::vector<CheatManager::Cheat> cheats;
			std::string message;
			bool texturesEnabled = true;
			sint32 layout = -1; // -1: the global setting
			ControllerNav nav;
			ImVec2 lastMouse{-1, -1};
			Face acceptFace = Face::East;
			Face backFace = Face::South;
		};

		MenuState s_state;

		std::string U8(const wxString& s)
		{
			return s.utf8_string();
		}

		ImFont* Font(float size)
		{
			ImFont* font = ImGui_GetFont(size);
			return font ? font : ImGui::GetFont();
		}

		ImVec2 TextSize(float size, const std::string& text)
		{
			return Font(size)->CalcTextSizeA(size, FLT_MAX, 0.0f, text.c_str());
		}

		void Text(ImDrawList* dl, float size, ImVec2 pos, ImU32 colour, const std::string& text)
		{
			dl->AddText(Font(size), size, pos, colour, text.c_str());
		}

		// text vertically centred on cy
		void TextV(ImDrawList* dl, float size, float x, float cy, ImU32 colour, const std::string& text)
		{
			Text(dl, size, ImVec2(x, cy - TextSize(size, text).y / 2), colour, text);
		}

		std::string Ellipsize(float size, const std::string& text, float maxWidth)
		{
			if (TextSize(size, text).x <= maxWidth)
				return text;
			std::string cut = text;
			while (!cut.empty() && TextSize(size, cut + "...").x > maxWidth)
			{
				cut.pop_back();
				while (!cut.empty() && (cut.back() & 0xC0) == 0x80)
					cut.pop_back(); // do not split a UTF-8 sequence
			}
			return cut + "...";
		}

		std::string LayoutLabel(sint32 layout)
		{
			if (layout < 0)
			{
				const sint32 global = std::clamp<sint32>(GetConfig().fullscreen_scaling, 0, kFullscreenScalingCount - 1);
				return U8(wxString::Format(_("Default: %s"), ScreenLayoutName(global)));
			}
			return U8(ScreenLayoutName(layout));
		}

		void Close()
		{
			SetMenuOpen(false);
			s_state.message.clear();
		}

		// ---- actions (GPU thread) ----

		void SetLayout(sint32 layout)
		{
			s_state.layout = layout;
			LatteRenderTarget_setScreenLayoutOverride(layout);
			const uint64 titleId = s_state.titleId;
			wxTheApp->CallAfter([titleId, layout]() {
				auto& layouts = GetWxGUIConfig().screen_layouts;
				if (layout < 0)
					layouts.erase(titleId);
				else
					layouts[titleId] = layout;
				g_wxConfig.Save();
			});
		}

		void SetTexturesEnabled(bool enabled)
		{
			s_state.texturesEnabled = enabled;
			const uint64 titleId = s_state.titleId;
			wxTheApp->CallAfter([titleId, enabled]() {
				auto& all = GetWxGUIConfig().custom_textures;
				if (all.find(titleId) == all.end())
					all[titleId].packs = LatteTextureReplace::ListPacks(titleId); // never configured: all packs
				auto& entry = all[titleId];
				entry.enabled = enabled;
				g_wxConfig.Save();
				LatteTextureReplace::SetTitleSettings(titleId, entry.enabled, entry.packs);
				LatteAsyncCommands_queueReloadTextures();
			});
		}

		void SetCheat(size_t index, bool enabled)
		{
			auto& cheats = s_state.cheats;
			if (index >= cheats.size())
				return;
			std::string error;
			if (enabled && !CheatManager::Validate(cheats[index], error))
			{
				s_state.message = error;
				return;
			}
			cheats[index].enabled = enabled;
			if (!CheatManager::Save(s_state.titleId, cheats, error))
			{
				cheats[index].enabled = !enabled;
				s_state.message = error;
				return;
			}
			s_state.message.clear();
		}

		void TogglePadView()
		{
			wxTheApp->CallAfter([]() {
				if (!g_mainFrame)
					return;
				auto& config = GetWxGUIConfig();
				config.pad_open = !config.pad_open;
				g_wxConfig.Save();
				g_mainFrame->TogglePadView();
			});
		}

		void ExitGame()
		{
			Close();
			wxTheApp->CallAfter([]() {
				if (g_mainFrame && g_mainFrame->IsGameLaunched())
					g_mainFrame->EndEmulation();
			});
		}

		// ---- pages ----

		std::vector<Item> BuildItems()
		{
			std::vector<Item> items;
			auto& st = s_state;
			switch (st.page)
			{
			case Page::Main:
			{
				Item resume;
				resume.label = U8(_("Resume"));
				resume.activate = []() { Close(); };
				items.push_back(resume);

				Item layout;
				layout.kind = ItemKind::Link;
				layout.label = U8(_("Screen layout"));
				layout.value = LayoutLabel(st.layout);
				layout.activate = [&st]() {
					st.page = Page::Layout;
					st.focus = st.layout + 1;
				};
				layout.adjust = [&st](int dir) { SetLayout(std::clamp<sint32>(st.layout + dir, -1, kFullscreenScalingCount - 1)); };
				items.push_back(layout);

				Item textures;
				textures.kind = ItemKind::Toggle;
				textures.label = U8(_("Custom textures"));
				textures.on = st.texturesEnabled;
				textures.activate = [&st]() { SetTexturesEnabled(!st.texturesEnabled); };
				textures.adjust = [](int dir) { SetTexturesEnabled(dir > 0); };
				items.push_back(textures);

				Item cheats;
				cheats.kind = ItemKind::Link;
				cheats.label = U8(_("Cheats"));
				const auto enabled = std::count_if(st.cheats.begin(), st.cheats.end(), [](const auto& c) { return c.enabled; });
				cheats.value = st.cheats.empty() ? U8(_("None")) : U8(wxString::Format(_("%d of %d on"), (int)enabled, (int)st.cheats.size()));
				cheats.activate = [&st]() {
					st.page = Page::Cheats;
					st.focus = 0;
				};
				items.push_back(cheats);

				Item pad;
				pad.kind = ItemKind::Toggle;
				pad.label = U8(_("Show GamePad"));
				pad.on = GetWxGUIConfig().pad_open;
				pad.activate = []() { TogglePadView(); };
				items.push_back(pad);

				Item swap;
				swap.kind = ItemKind::Toggle;
				swap.label = U8(_("GamePad screen on TV"));
				swap.on = LatteGPUState.isDRCPrimary;
				swap.activate = []() { LatteGPUState.isDRCPrimary = !LatteGPUState.isDRCPrimary; };
				swap.adjust = [](int dir) { LatteGPUState.isDRCPrimary = dir > 0; };
				items.push_back(swap);

				Item exit;
				exit.label = U8(_("Exit game"));
				exit.danger = true;
				exit.activate = [&st]() {
					st.page = Page::ConfirmExit;
					st.focus = 0;
				};
				items.push_back(exit);
				break;
			}
			case Page::Layout:
			{
				for (sint32 layout = -1; layout < kFullscreenScalingCount; layout++)
				{
					Item item;
					item.kind = ItemKind::Radio;
					item.label = LayoutLabel(layout);
					item.on = layout == st.layout;
					item.activate = [&st, layout]() {
						SetLayout(layout);
						st.page = Page::Main;
						st.focus = 1;
					};
					items.push_back(item);
				}
				break;
			}
			case Page::Cheats:
			{
				for (size_t i = 0; i < st.cheats.size(); i++)
				{
					Item item;
					item.kind = ItemKind::Toggle;
					item.label = st.cheats[i].name;
					item.on = st.cheats[i].enabled;
					item.activate = [&st, i]() { SetCheat(i, !st.cheats[i].enabled); };
					item.adjust = [i](int dir) { SetCheat(i, dir > 0); };
					items.push_back(item);
				}
				break;
			}
			case Page::ConfirmExit:
			{
				Item cancel;
				cancel.label = U8(_("Keep playing"));
				cancel.activate = [&st]() {
					st.page = Page::Main;
					st.focus = 6;
				};
				items.push_back(cancel);
				Item exit;
				exit.label = U8(_("Exit game"));
				exit.danger = true;
				exit.activate = []() { ExitGame(); };
				items.push_back(exit);
				break;
			}
			}
			return items;
		}

		std::string PageTitle()
		{
			switch (s_state.page)
			{
			case Page::Layout: return U8(_("Screen layout"));
			case Page::Cheats: return U8(_("Cheats"));
			case Page::ConfirmExit: return U8(_("Exit game?"));
			default: return U8(_("Game Menu"));
			}
		}

		void Back()
		{
			auto& st = s_state;
			st.message.clear();
			switch (st.page)
			{
			case Page::Main:
				Close();
				break;
			case Page::Layout:
				st.page = Page::Main;
				st.focus = 1;
				break;
			case Page::Cheats:
				st.page = Page::Main;
				st.focus = 3;
				break;
			case Page::ConfirmExit:
				st.page = Page::Main;
				st.focus = 6;
				break;
			}
		}

		void HandleNav(Nav nav, std::vector<Item>& items)
		{
			auto& st = s_state;
			const int count = (int)items.size();
			switch (nav)
			{
			case Nav::Up:
				if (st.focus > 0)
					st.focus--;
				break;
			case Nav::Down:
				if (st.focus + 1 < count)
					st.focus++;
				break;
			case Nav::Left:
			case Nav::Right:
				if (st.focus < count && items[st.focus].adjust)
					items[st.focus].adjust(nav == Nav::Right ? 1 : -1);
				break;
			case Nav::Accept:
				if (st.focus < count && items[st.focus].activate)
					items[st.focus].activate();
				break;
			case Nav::Back:
				Back();
				break;
			default:
				break;
			}
			items = BuildItems();
			st.focus = std::clamp(st.focus, 0, std::max(0, (int)items.size() - 1));
		}

		void OnOpened()
		{
			auto& st = s_state;
			st.page = Page::Main;
			st.focus = 0;
			st.message.clear();
			st.titleId = CafeSystem::GetForegroundTitleId();
			st.gameName = CafeSystem::GetForegroundTitleName();
			st.cheats = CheatManager::Load(st.titleId);
			st.layout = LatteRenderTarget_getScreenLayoutOverride();
			const auto& textures = GetWxGUIConfig().custom_textures;
			const auto it = textures.find(st.titleId);
			st.texturesEnabled = it == textures.end() || it->second.enabled;
			st.nav.Reset(); // the button that opened the menu must not also select something
			st.acceptFace = FaceForNav(Nav::Accept);
			st.backFace = FaceForNav(Nav::Back);
		}

		// Same glyph families as the launcher (see GameModePanel::DrawGlyph).
		// Keyboard style: the keys the Game Menu listens to
		std::string KeyName(Nav nav)
		{
			return nav == Nav::Accept ? U8(_("Enter")) : U8(_("Esc"));
		}

		float GlyphWidth(Nav nav, float radius)
		{
			if (GetButtonStyle() != ButtonStyle::Keyboard)
				return radius * 2;
			return std::max(radius * 2, TextSize(radius * 0.95f, KeyName(nav)).x + radius * 1.1f);
		}

		// Draws the glyph with its left edge at x and returns its width.
		float DrawGlyphAt(ImDrawList* dl, Nav nav, float x, float cy, float radius);

		void DrawGlyph(ImDrawList* dl, Nav nav, ImVec2 c, float radius)
		{
			const ButtonStyle style = GetButtonStyle();
			const Face face = nav == Nav::Accept ? s_state.acceptFace : s_state.backFace;
			const ImU32 darkFill = kSurfaceHighest;
			if (style == ButtonStyle::PlayStation)
			{
				dl->AddCircleFilled(c, radius, darkFill, 24);
				dl->AddCircle(c, radius, kOutline, 24, radius * 0.08f);
				const float r = radius * 0.46f, w = std::max(1.5f, radius * 0.16f);
				switch (face)
				{
				case Face::South:
					dl->AddLine(ImVec2(c.x - r, c.y - r), ImVec2(c.x + r, c.y + r), IM_COL32(0x7C, 0xB2, 0xE8, 0xFF), w);
					dl->AddLine(ImVec2(c.x + r, c.y - r), ImVec2(c.x - r, c.y + r), IM_COL32(0x7C, 0xB2, 0xE8, 0xFF), w);
					break;
				case Face::East:
					dl->AddCircle(c, r, IM_COL32(0xFF, 0x6B, 0x6B, 0xFF), 24, w);
					break;
				case Face::West:
					dl->AddRect(ImVec2(c.x - r * 0.9f, c.y - r * 0.9f), ImVec2(c.x + r * 0.9f, c.y + r * 0.9f), IM_COL32(0xE0, 0x9E, 0xE0, 0xFF), 0, 0, w);
					break;
				default:
					dl->AddTriangle(ImVec2(c.x, c.y - r), ImVec2(c.x + r * 1.05f, c.y + r * 0.75f), ImVec2(c.x - r * 1.05f, c.y + r * 0.75f), IM_COL32(0x4C, 0xD9, 0xA6, 0xFF), w);
					break;
				}
				return;
			}
			ImU32 fill = kOnSurface, text = kGlyphText;
			if (style == ButtonStyle::Xbox)
			{
				fill = face == Face::South ? IM_COL32(0x6C, 0xC0, 0x4A, 0xFF) : face == Face::East ? IM_COL32(0xE8, 0x5A, 0x4F, 0xFF)
					: face == Face::West ? IM_COL32(0x4A, 0x9C, 0xE8, 0xFF) : IM_COL32(0xF2, 0xC6, 0x3C, 0xFF);
			}
			else if (style == ButtonStyle::SteamDeck)
			{
				fill = darkFill;
				text = kOnSurface;
			}
			dl->AddCircleFilled(c, radius, fill, 24);
			if (style == ButtonStyle::SteamDeck)
				dl->AddCircle(c, radius, kOutline, 24, radius * 0.08f);
			const std::string letter = U8(FaceLetter(style, face));
			const float size = radius * 1.15f;
			const ImVec2 ts = TextSize(size, letter);
			Text(dl, size, ImVec2(c.x - ts.x / 2, c.y - ts.y / 2), text, letter);
		}

		float DrawGlyphAt(ImDrawList* dl, Nav nav, float x, float cy, float radius)
		{
			const float width = GlyphWidth(nav, radius);
			if (GetButtonStyle() == ButtonStyle::Keyboard)
			{
				// keycap: light key with a darker bottom edge
				const float h = radius * 2, r = radius * 0.35f;
				dl->AddRectFilled(ImVec2(x, cy - radius), ImVec2(x + width, cy - radius + h), kOutline, r);
				dl->AddRectFilled(ImVec2(x, cy - radius), ImVec2(x + width, cy - radius + h - radius * 0.22f), kOnSurface, r);
				const float size = radius * 0.95f;
				const std::string name = KeyName(nav);
				const ImVec2 ts = TextSize(size, name);
				Text(dl, size, ImVec2(x + (width - ts.x) / 2, cy - radius * 0.1f - ts.y / 2), kGlyphText, name);
				return width;
			}
			DrawGlyph(dl, nav, ImVec2(x + radius, cy), radius);
			return width;
		}

		void DrawChevron(ImDrawList* dl, float cx, float cy, float size, ImU32 colour, float thickness)
		{
			const ImVec2 points[3] = {{cx - size / 2, cy - size}, {cx + size / 2, cy}, {cx - size / 2, cy + size}};
			dl->AddPolyline(points, 3, colour, ImDrawFlags_None, thickness);
		}
	}

	void RenderMenu(bool mainWindow)
	{
		auto& st = s_state;
		const bool open = IsMenuOpen();
		if (!open)
		{
			if (st.wasOpen)
				st.wasOpen = false;
			// after closing: keep the game's input blocked until the buttons are let go
			if (mainWindow && IsGameInputBlocked())
			{
				std::vector<Nav> ignored;
				st.nav.Poll(ignored, false);
				if (!st.nav.AnyDown())
					ClearReleasePending();
			}
			return;
		}
		if (!mainWindow)
			return;
		if (!CafeSystem::IsTitleRunning())
		{
			Close();
			return;
		}
		if (!st.wasOpen)
		{
			st.wasOpen = true;
			OnOpened();
		}

		std::vector<Item> items = BuildItems();
		st.focus = std::clamp(st.focus, 0, std::max(0, (int)items.size() - 1));

		// input: the players' controllers (the game polls them; its reads are blocked) and keys
		std::vector<Nav> navs;
		st.nav.Poll(navs, false);
		Nav queued;
		while (TakeMenuNav(queued))
			navs.push_back(queued);
		for (Nav nav : navs)
		{
			HandleNav(nav, items);
			if (!IsMenuOpen())
				return;
		}

		ImGuiIO& io = ImGui::GetIO();
		const float W = io.DisplaySize.x, H = io.DisplaySize.y;
		if (W <= 0 || H <= 0)
			return;
		const float s = std::clamp(H / 1080.0f, 0.5f, 3.0f);
		const float titleSize = std::round(40 * s), rowSize = std::round(30 * s), smallSize = std::round(22 * s);

		ImGui::SetNextWindowPos(ImVec2(0, 0));
		ImGui::SetNextWindowSize(ImVec2(W, H));
		ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
		ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
		const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize |
			ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoScrollbar;
		if (ImGui::Begin("##GameModeMenu", nullptr, flags))
		{
			ImDrawList* dl = ImGui::GetWindowDrawList();
			dl->AddRectFilled(ImVec2(0, 0), ImVec2(W, H), kScrim);
			const float drawerW = std::clamp(640 * s, std::min(340.0f, W), W * 0.92f);
			dl->AddRectFilled(ImVec2(0, 0), ImVec2(drawerW, H), kSurfaceLow, 32 * s, ImDrawFlags_RoundCornersRight);

			// header
			const float padX = 40 * s;
			TextV(dl, titleSize, padX, 70 * s, kOnSurface, PageTitle());
			TextV(dl, smallSize, padX, 118 * s, kOnSurfaceVariant, Ellipsize(smallSize, st.page == Page::ConfirmExit ? U8(_("Unsaved progress will be lost.")) : st.gameName, drawerW - padX * 2));

			float y = 160 * s;
			if (!st.message.empty())
			{
				TextV(dl, smallSize, padX, y + 14 * s, kError, Ellipsize(smallSize, st.message, drawerW - padX * 2));
				y += 40 * s;
			}
			if (st.page == Page::Cheats && items.empty())
			{
				TextV(dl, rowSize, padX, y + 40 * s, kOnSurface, U8(_("No cheats for this game")));
				TextV(dl, smallSize, padX, y + 84 * s, kOnSurfaceVariant, Ellipsize(smallSize, U8(_("Add them from Tools > Cheats in the regular Cemu window.")), drawerW - padX * 2));
			}

			// rows, scrolled so the focused one stays above the hint bar
			const float rowH = 84 * s, gap = 6 * s, rowX0 = 20 * s, rowX1 = drawerW - 20 * s;
			const float listBottom = H - 110 * s;
			const float focusBottom = y + (st.focus + 1) * (rowH + gap);
			const float scroll = std::max(0.0f, focusBottom - listBottom);
			const bool mouseMoved = io.MousePos.x != st.lastMouse.x || io.MousePos.y != st.lastMouse.y;
			st.lastMouse = io.MousePos;
			dl->PushClipRect(ImVec2(0, y), ImVec2(drawerW, listBottom), true);
			for (int i = 0; i < (int)items.size(); i++)
			{
				const Item& item = items[i];
				const float top = y + i * (rowH + gap) - scroll;
				const ImVec2 a(rowX0, top), b(rowX1, top + rowH);
				const bool hovered = io.MousePos.x >= a.x && io.MousePos.x < b.x && io.MousePos.y >= a.y && io.MousePos.y < b.y && top >= y - 1 && b.y <= listBottom + 1;
				if (hovered && mouseMoved)
					st.focus = i;
				if (hovered && ImGui::IsMouseClicked(0))
				{
					st.focus = i;
					HandleNav(Nav::Accept, items);
					break; // items may have changed
				}
				const bool focused = i == st.focus;
				if (focused)
				{
					dl->AddRectFilled(a, b, kSecondaryContainer, 20 * s);
					dl->AddRect(a, b, kPrimary, 20 * s, 0, 3 * s);
				}
				const float cy = top + rowH / 2;
				const ImU32 labelColour = item.danger ? kError : focused ? kOnSecondaryContainer : kOnSurface;
				float rightEdge = b.x - 24 * s;
				float labelX = a.x + 24 * s;
				if (item.kind == ItemKind::Radio)
				{
					const ImVec2 c(labelX + 14 * s, cy);
					dl->AddCircle(c, 14 * s, item.on ? kPrimary : kOnSurfaceVariant, 24, 3 * s);
					if (item.on)
						dl->AddCircleFilled(c, 8 * s, kPrimary, 24);
					labelX += 48 * s;
				}
				else if (item.kind == ItemKind::Toggle)
				{
					const float tw = 72 * s, th = 40 * s;
					const ImVec2 ta(rightEdge - tw, cy - th / 2), tb(rightEdge, cy + th / 2);
					if (item.on)
					{
						dl->AddRectFilled(ta, tb, kPrimary, th / 2);
						dl->AddCircleFilled(ImVec2(tb.x - th / 2, cy), 14 * s, kOnPrimary, 24);
					}
					else
					{
						dl->AddRectFilled(ta, tb, kSurfaceHighest, th / 2);
						dl->AddRect(ta, tb, kOutline, th / 2, 0, 3 * s);
						dl->AddCircleFilled(ImVec2(ta.x + th / 2, cy), 10 * s, kOutline, 24);
					}
					rightEdge -= tw + 16 * s;
				}
				else if (item.kind == ItemKind::Link)
				{
					DrawChevron(dl, rightEdge - 6 * s, cy, 10 * s, focused ? kOnSecondaryContainer : kOnSurfaceVariant, 3 * s);
					rightEdge -= 30 * s;
				}
				if (!item.value.empty())
				{
					const std::string value = Ellipsize(smallSize, item.value, (b.x - a.x) * 0.5f);
					const float vw = TextSize(smallSize, value).x;
					TextV(dl, smallSize, rightEdge - vw, cy, focused ? kOnSecondaryContainer : kPrimary, value);
					rightEdge -= vw + 16 * s;
				}
				TextV(dl, rowSize, labelX, cy, labelColour, Ellipsize(rowSize, item.label, rightEdge - labelX));
			}
			dl->PopClipRect();

			// click outside the drawer closes the menu
			if (ImGui::IsMouseClicked(0) && io.MousePos.x > drawerW && io.MousePos.x < W)
				Close();

			// button hints
			const float hy = H - 56 * s;
			float hx = padX;
			const std::pair<Nav, std::string> hints[] = {{Nav::Accept, U8(_("Select"))}, {Nav::Back, st.page == Page::Main ? U8(_("Resume")) : U8(_("Back"))}};
			for (const auto& [glyph, label] : hints)
			{
				const float glyphW = DrawGlyphAt(dl, glyph, hx, hy, 20 * s);
				TextV(dl, smallSize, hx + glyphW + 10 * s, hy, kOnSurface, label);
				hx += glyphW + 10 * s + TextSize(smallSize, label).x + 36 * s;
			}
		}
		ImGui::End();
		ImGui::PopStyleVar(2);
	}
}
