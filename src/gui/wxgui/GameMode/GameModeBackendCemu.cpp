#include "wxgui/GameMode/GameModeBackendCemu.h"

#include "wxgui/MainWindow.h"
#include "wxgui/wxCemuConfig.h"
#include "wxgui/input/HotkeySettings.h"

#include "Cafe/Cheats/CheatManager.h"
#include "Cafe/Filesystem/fsc.h"
#include "Cafe/HW/Latte/Core/LatteTextureReplace.h"
#include "Cafe/TitleList/GameInfo.h"
#include "Cafe/TitleList/TitleList.h"
#include "audio/IAudioAPI.h"
#include "config/CemuConfig.h"
#include "input/InputManager.h"
#include "input/api/Controller.h"
#include "input/emulated/ClassicController.h"
#include "input/emulated/ProController.h"
#include "input/emulated/VPADController.h"
#include "input/emulated/WiimoteController.h"
#include "util/helpers/helpers.h"

#include <wx/app.h>
#include <wx/mstream.h>

#include <algorithm>
#include <set>

using GameMode::Choice;

namespace
{
	// graphics APIs this build was compiled with, in menu order
	std::vector<std::pair<GraphicAPI, wxString>> AvailableApis()
	{
		std::vector<std::pair<GraphicAPI, wxString>> apis;
#ifdef ENABLE_VULKAN
		apis.emplace_back(kVulkan, "Vulkan");
#endif
#ifdef ENABLE_OPENGL
		apis.emplace_back(kOpenGL, "OpenGL");
#endif
#ifdef ENABLE_METAL
		apis.emplace_back(kMetal, "Metal");
#endif
		return apis;
	}

	std::vector<wxString> FilterNames()
	{
		// order of the ScalingFilter enum
		return {_("Bilinear"), _("Bicubic"), _("Hermite"), _("Nearest neighbor")};
	}

	void SaveConfig()
	{
		GetConfigHandle().Save();
	}

	void SaveGuiConfig()
	{
		g_wxConfig.Save();
	}
}

GameModeBackendCemu::GameModeBackendCemu(MainWindow* mainWindow)
	: m_mainWindow(mainWindow)
{
	m_iconThread = std::thread(&GameModeBackendCemu::IconWorker, this);
}

GameModeBackendCemu::~GameModeBackendCemu()
{
	{
		std::scoped_lock lock(m_iconMutex);
		m_iconStop = true;
		m_iconQueue.clear();
	}
	m_iconCv.notify_all();
	if (m_iconThread.joinable())
		m_iconThread.join();
}

// ---- caches ----

namespace
{
	constexpr auto kCacheTime = std::chrono::seconds(2);
}

const std::vector<std::string>& GameModeBackendCemu::CachedPacks(uint64_t titleId)
{
	const auto now = std::chrono::steady_clock::now();
	if (m_packsCache.key != titleId || now >= m_packsCache.until)
	{
		m_packsCache.value = LatteTextureReplace::ListPacks(titleId);
		m_packsCache.key = titleId;
		m_packsCache.until = now + kCacheTime;
	}
	return m_packsCache.value;
}

const std::vector<CheatManager::Cheat>& GameModeBackendCemu::CachedCheats(uint64_t titleId)
{
	const auto now = std::chrono::steady_clock::now();
	if (m_cheatsCache.key != titleId || now >= m_cheatsCache.until)
	{
		m_cheatsCache.value = CheatManager::Load(titleId);
		m_cheatsCache.key = titleId;
		m_cheatsCache.until = now + kCacheTime;
	}
	return m_cheatsCache.value;
}

const std::vector<std::string>& GameModeBackendCemu::CachedProfiles()
{
	const auto now = std::chrono::steady_clock::now();
	if (now >= m_profilesCache.until)
	{
		m_profilesCache.value = InputManager::get_profiles();
		m_profilesCache.until = now + kCacheTime;
	}
	return m_profilesCache.value;
}

// ---- library ----

std::vector<GameMode::GameEntry> GameModeBackendCemu::ListGames()
{
	std::vector<GameMode::GameEntry> games;
	std::set<uint64> seen;
	auto& config = GetConfig();
	for (const TitleId titleId : CafeTitleList::GetAllTitleIds())
	{
		GameInfo2 info = CafeTitleList::GetGameInfo(titleId);
		if (!info.IsValid() || info.IsSystemDataTitle())
			continue;
		const TitleId baseId = info.GetBaseTitleId();
		if (!seen.insert(baseId).second)
			continue;
		std::string name;
		if (!config.GetGameListCustomName(baseId, name))
			name = info.GetTitleName();
		games.push_back({baseId, wxString::FromUTF8(name), config.IsGameListFavorite(baseId)});
	}
	std::sort(games.begin(), games.end(), [](const auto& a, const auto& b) {
		if (a.favorite != b.favorite)
			return a.favorite;
		return a.name.CmpNoCase(b.name) < 0;
	});
	return games;
}

bool GameModeBackendCemu::IsScanningGames()
{
	return CafeTitleList::IsScanning();
}

void GameModeBackendCemu::RequestIcon(uint64_t titleId, std::function<void(uint64_t, const wxImage&)> onLoaded)
{
	{
		std::scoped_lock lock(m_iconMutex);
		m_iconQueue.emplace_back(titleId, std::move(onLoaded));
	}
	m_iconCv.notify_one();
}

// Same loading as the regular game list: mount the title and read meta/iconTex.tga(.gz).
void GameModeBackendCemu::IconWorker()
{
	SetThreadName("GameModeIcons");
	while (true)
	{
		std::pair<uint64_t, std::function<void(uint64_t, const wxImage&)>> job;
		{
			std::unique_lock lock(m_iconMutex);
			m_iconCv.wait(lock, [this]() { return m_iconStop || !m_iconQueue.empty(); });
			if (m_iconStop)
				return;
			job = std::move(m_iconQueue.front());
			m_iconQueue.pop_front();
		}
		TitleInfo titleInfo;
		if (!CafeTitleList::GetFirstByTitleId(job.first, titleInfo))
			continue;
		const std::string mountPath = TitleInfo::GetUniqueTempMountingPath();
		if (!titleInfo.Mount(mountPath, "", FSC_PRIORITY_BASE))
			continue;
		auto tga = fsc_extractFile((mountPath + "/meta/iconTex.tga").c_str());
		if (!tga)
		{
			tga = fsc_extractFile((mountPath + "/meta/iconTex.tga.gz").c_str());
			if (tga)
			{
				auto decompressed = zlibDecompress(*tga, 70 * 1024);
				std::swap(tga, decompressed);
			}
		}
		titleInfo.Unmount(mountPath);
		if (!tga || tga->size() <= 16)
			continue;
		wxMemoryInputStream stream(tga->data(), tga->size());
		wxImage image(stream);
		if (!image.IsOk())
			continue;
		// the panel checks that it still exists before using the image
		wxTheApp->CallAfter([callback = std::move(job.second), id = job.first, image]() { callback(id, image); });
	}
}

void GameModeBackendCemu::LaunchGame(uint64_t titleId)
{
	GameInfo2 info = CafeTitleList::GetGameInfo(titleId);
	if (!info.IsValid())
		return;
	MainWindow::RequestLaunchGame(info.GetBase().GetPath(), wxLaunchGameEvent::INITIATED_BY::GAME_LIST);
}

void GameModeBackendCemu::SetFavorite(uint64_t titleId, bool favorite)
{
	GetConfig().SetGameListFavorite(titleId, favorite);
	SaveConfig();
}

// ---- per game ----

Choice GameModeBackendCemu::GetGameScreenLayout(uint64_t titleId)
{
	Choice choice;
	const sint32 global = std::clamp<sint32>(GetConfig().fullscreen_scaling, 0, kFullscreenScalingCount - 1);
	choice.options.push_back(wxString::Format(_("Default: %s"), GameMode::ScreenLayoutName(global)));
	for (int layout = 0; layout < kFullscreenScalingCount; layout++)
		choice.options.push_back(GameMode::ScreenLayoutName(layout));
	const auto& layouts = GetWxGUIConfig().screen_layouts;
	const auto it = layouts.find(titleId);
	choice.selected = it != layouts.end() ? it->second + 1 : 0;
	return choice;
}

void GameModeBackendCemu::SetGameScreenLayout(uint64_t titleId, int choiceIndex)
{
	auto& layouts = GetWxGUIConfig().screen_layouts;
	if (choiceIndex <= 0)
		layouts.erase(titleId);
	else
		layouts[titleId] = choiceIndex - 1;
	SaveGuiConfig();
}

// A title with no entry has never been configured: enabled, every pack on. Same rule as the
// Custom textures window and the texture module itself.
GameModeBackendCemu::TextureSettings GameModeBackendCemu::ReadTextureSettings(uint64_t titleId)
{
	TextureSettings settings;
	const auto& all = GetWxGUIConfig().custom_textures;
	const auto it = all.find(titleId);
	if (it == all.end())
	{
		settings.packs = CachedPacks(titleId);
		return settings;
	}
	settings.enabled = it->second.enabled;
	settings.packs = it->second.packs;
	return settings;
}

void GameModeBackendCemu::WriteTextureSettings(uint64_t titleId, const TextureSettings& settings)
{
	auto& entry = GetWxGUIConfig().custom_textures[titleId];
	entry.enabled = settings.enabled;
	entry.packs = settings.packs;
	SaveGuiConfig();
	LatteTextureReplace::SetTitleSettings(titleId, settings.enabled, settings.packs);
}

bool GameModeBackendCemu::GetCustomTexturesEnabled(uint64_t titleId)
{
	return ReadTextureSettings(titleId).enabled;
}

void GameModeBackendCemu::SetCustomTexturesEnabled(uint64_t titleId, bool enabled)
{
	auto settings = ReadTextureSettings(titleId);
	settings.enabled = enabled;
	WriteTextureSettings(titleId, settings);
}

std::vector<GameMode::NamedToggle> GameModeBackendCemu::GetTexturePacks(uint64_t titleId)
{
	const auto settings = ReadTextureSettings(titleId);
	std::vector<GameMode::NamedToggle> packs;
	for (const auto& pack : CachedPacks(titleId))
	{
		const bool on = std::find(settings.packs.begin(), settings.packs.end(), pack) != settings.packs.end();
		packs.push_back({wxString::FromUTF8(pack), on});
	}
	return packs;
}

void GameModeBackendCemu::SetTexturePackEnabled(uint64_t titleId, const wxString& pack, bool enabled)
{
	auto settings = ReadTextureSettings(titleId);
	const std::string name = pack.utf8_string();
	auto it = std::find(settings.packs.begin(), settings.packs.end(), name);
	if (enabled && it == settings.packs.end())
		settings.packs.push_back(name);
	else if (!enabled && it != settings.packs.end())
		settings.packs.erase(it);
	std::sort(settings.packs.begin(), settings.packs.end());
	WriteTextureSettings(titleId, settings);
}

std::vector<GameMode::NamedToggle> GameModeBackendCemu::GetCheats(uint64_t titleId)
{
	std::vector<GameMode::NamedToggle> cheats;
	for (const auto& cheat : CachedCheats(titleId))
		cheats.push_back({wxString::FromUTF8(cheat.name), cheat.enabled});
	return cheats;
}

std::optional<wxString> GameModeBackendCemu::SetCheatEnabled(uint64_t titleId, size_t index, bool enabled)
{
	auto cheats = CheatManager::Load(titleId);
	if (index >= cheats.size())
		return std::nullopt;
	std::string error;
	if (enabled && !CheatManager::Validate(cheats[index], error))
		return wxString::FromUTF8(error);
	cheats[index].enabled = enabled;
	m_cheatsCache.key = ~0ull;
	if (!CheatManager::Save(titleId, cheats, error))
		return wxString::FromUTF8(error);
	return std::nullopt;
}

// ---- graphics ----

Choice GameModeBackendCemu::GetGraphicsApi()
{
	Choice choice;
	const auto apis = AvailableApis();
	const GraphicAPI current = GetConfig().graphic_api;
	for (size_t i = 0; i < apis.size(); i++)
	{
		choice.options.push_back(apis[i].second);
		if (apis[i].first == current)
			choice.selected = (int)i;
	}
	return choice;
}

void GameModeBackendCemu::SetGraphicsApi(int index)
{
	const auto apis = AvailableApis();
	if (index < 0 || index >= (int)apis.size())
		return;
	GetConfig().graphic_api = apis[index].first;
	SaveConfig();
}

// Options follow the General settings dialog for each API.
Choice GameModeBackendCemu::GetVSync()
{
	Choice choice;
	if (GetConfig().graphic_api == kVulkan)
	{
		choice.options = {_("Off"), _("Double buffering"), _("Triple buffering")};
#if BOOST_OS_WINDOWS
		choice.options.push_back(_("Match emulated display (Experimental)"));
#endif
	}
	else
		choice.options = {_("Off"), _("On")};
	choice.selected = std::clamp<int>(GetConfig().vsync, 0, (int)choice.options.size() - 1);
	return choice;
}

void GameModeBackendCemu::SetVSync(int index)
{
	GetConfig().vsync = index;
	SaveConfig();
}

Choice GameModeBackendCemu::GetDefaultScreenLayout()
{
	Choice choice;
	for (int layout = 0; layout < kFullscreenScalingCount; layout++)
		choice.options.push_back(GameMode::ScreenLayoutName(layout));
	choice.selected = std::clamp<int>(GetConfig().fullscreen_scaling, 0, kFullscreenScalingCount - 1);
	return choice;
}

void GameModeBackendCemu::SetDefaultScreenLayout(int index)
{
	GetConfig().fullscreen_scaling = index;
	SaveConfig();
}

Choice GameModeBackendCemu::GetUpscaleFilter()
{
	return {FilterNames(), std::clamp<int>(GetConfig().upscale_filter, 0, 3)};
}

void GameModeBackendCemu::SetUpscaleFilter(int index)
{
	GetConfig().upscale_filter = index;
	SaveConfig();
}

Choice GameModeBackendCemu::GetDownscaleFilter()
{
	return {FilterNames(), std::clamp<int>(GetConfig().downscale_filter, 0, 3)};
}

void GameModeBackendCemu::SetDownscaleFilter(int index)
{
	GetConfig().downscale_filter = index;
	SaveConfig();
}

bool GameModeBackendCemu::GetFullscreen()
{
	return GetWxGUIConfig().fullscreen;
}

void GameModeBackendCemu::SetFullscreen(bool enabled)
{
	GetWxGUIConfig().fullscreen = enabled;
	SaveGuiConfig();
	MainWindow* window = m_mainWindow;
	wxTheApp->CallAfter([window, enabled]() { window->ApplyGameModeFullscreen(enabled); });
}

// ---- audio ----

int GameModeBackendCemu::GetTvVolume()
{
	return GetConfig().tv_volume;
}

void GameModeBackendCemu::SetTvVolume(int volume)
{
	GetConfig().tv_volume = volume;
	SaveConfig();
	std::shared_lock lock(g_audioMutex);
	if (g_tvAudio)
		g_tvAudio->SetVolume(volume);
}

int GameModeBackendCemu::GetPadVolume()
{
	return GetConfig().pad_volume;
}

void GameModeBackendCemu::SetPadVolume(int volume)
{
	GetConfig().pad_volume = volume;
	SaveConfig();
	std::shared_lock lock(g_audioMutex);
	if (g_padAudio)
		g_padAudio->SetVolume(volume);
	g_padVolume = volume;
}

// ---- input ----

int GameModeBackendCemu::GetPlayerCount()
{
	return 4;
}

// Options: Disabled, [the current setup when it is not a saved profile], saved profiles.
Choice GameModeBackendCemu::GetPlayerProfile(int player)
{
	Choice choice;
	const auto controller = InputManager::instance().get_controller(player);
	choice.options.push_back(_("Disabled"));
	const bool custom = controller && !controller->has_profile_name();
	if (custom)
		choice.options.push_back(wxString::Format(_("Custom (%s)"), wxString::FromUTF8(std::string(controller->type_string()))));
	const auto& profiles = CachedProfiles();
	for (const auto& profile : profiles)
		choice.options.push_back(wxString::FromUTF8(profile));
	if (!controller)
		choice.selected = 0;
	else if (custom)
		choice.selected = 1;
	else
	{
		const auto it = std::find(profiles.begin(), profiles.end(), controller->get_profile_name());
		choice.selected = it != profiles.end() ? 1 + (int)(it - profiles.begin()) : 0;
	}
	return choice;
}

std::optional<wxString> GameModeBackendCemu::SetPlayerProfile(int player, int choiceIndex)
{
	auto& input = InputManager::instance();
	const auto controller = input.get_controller(player);
	if (choiceIndex == 0)
	{
		// same as picking "Disabled" in Input settings: also forget the player's saved setup
		input.delete_controller(player, true);
		input.save();
		m_nav.Reset();
		return std::nullopt;
	}
	const bool custom = controller && !controller->has_profile_name();
	int index = choiceIndex - 1;
	if (custom)
	{
		if (index == 0)
			return std::nullopt; // kept as it is
		index--;
	}
	const auto profiles = CachedProfiles();
	if (index < 0 || index >= (int)profiles.size())
		return std::nullopt;
	if (!input.load(player, profiles[index]))
		return wxString::Format(_("Couldn't load profile %s"), wxString::FromUTF8(profiles[index]));
	input.save();
	m_nav.Reset(); // the controllers behind this player changed
	return std::nullopt;
}

std::vector<std::shared_ptr<ControllerBase>> GameModeBackendCemu::AllControllers()
{
	std::vector<std::shared_ptr<ControllerBase>> all;
	auto& input = InputManager::instance();
	for (size_t i = 0; i < InputManager::kMaxController; i++)
	{
		if (const auto emulated = input.get_controller(i))
		{
			for (const auto& controller : emulated->get_controllers())
			{
				if (std::find(all.begin(), all.end(), controller) == all.end())
					all.push_back(controller);
			}
		}
	}
	return all;
}

wxString GameModeBackendCemu::GetGameMenuBindingLabel()
{
	const auto& binding = GetWxGUIConfig().hotkeys.gameMenu;
	wxString controllerText;
	if (binding.controller != sHotkeyCfg::controllerNone)
	{
		const auto controllers = AllControllers();
		controllerText = controllers.empty()
			? wxString::Format(_("Button %d"), (int)binding.controller)
			: wxString::FromUTF8(controllers.front()->get_button_name(binding.controller));
	}
	const wxString keyText = HotkeySettings::KeyboardHotkeyLabel(binding.keyboard);
	if (controllerText.empty() && binding.keyboard.raw == sHotkeyCfg::keyboardNone)
		return _("Not set");
	if (controllerText.empty())
		return keyText;
	if (binding.keyboard.raw == sHotkeyCfg::keyboardNone)
		return controllerText;
	return controllerText + " / " + keyText;
}

void GameModeBackendCemu::BeginGameMenuCapture()
{
	m_captureBaseline.clear();
	for (const auto& controller : AllControllers())
	{
		const auto& state = controller->update_state();
		std::vector<uint64> held;
		for (const auto button : state.buttons.GetButtonList())
			held.push_back(button);
		m_captureBaseline.emplace_back(controller, std::move(held));
	}
}

bool GameModeBackendCemu::PollGameMenuCapture()
{
	for (auto& [weak, held] : m_captureBaseline)
	{
		const auto controller = weak.lock();
		if (!controller)
			continue;
		const auto& state = controller->update_state();
		for (const auto button : state.buttons.GetButtonList())
		{
			if (std::find(held.begin(), held.end(), (uint64)button) != held.end())
				continue;
			HotkeySettings::SetGameMenuControllerHotkey((ControllerHotkey_t)button);
			m_captureBaseline.clear();
			m_nav.Reset();
			return true;
		}
		// buttons held when capture started count once they have been released
		held.erase(std::remove_if(held.begin(), held.end(), [&](uint64 b) { return !state.buttons.GetButtonState((uint32)b); }), held.end());
	}
	return false;
}

void GameModeBackendCemu::BindGameMenuKey(int wxKeyCode, bool alt, bool ctrl, bool shift)
{
	uKeyboardHotkey hotkey{};
	hotkey.key = wxKeyCode;
	hotkey.alt = alt;
	hotkey.ctrl = ctrl;
	hotkey.shift = shift;
	HotkeySettings::SetGameMenuKeyboardHotkey(hotkey);
	m_captureBaseline.clear();
}

// ---- remapper ----

namespace
{
	wxString DeviceLabel(const ControllerBase& device)
	{
		if (device.api() == InputAPI::Keyboard)
			return _("Keyboard");
		return wxString::FromUTF8(device.display_name()) + " (" + wxString::FromUTF8(std::string(device.api_name())) + ")";
	}

	constexpr EmulatedController::Type kTypes[] = {EmulatedController::VPAD, EmulatedController::Pro, EmulatedController::Classic, EmulatedController::Wiimote};

	wxString TypeName(EmulatedController::Type type)
	{
		switch (type)
		{
		case EmulatedController::VPAD: return _("Wii U GamePad");
		case EmulatedController::Pro: return _("Wii U Pro Controller");
		case EmulatedController::Classic: return _("Classic Controller");
		case EmulatedController::Wiimote: return _("Wii Remote");
		default: return wxString();
		}
	}

	struct LayoutEntry
	{
		wxString group;
		uint64 id;
		wxString name;
	};

	// Every mappable button of a controller type, grouped the way a player thinks about them.
	// Cemu's own names ("up", "click") repeat across the D-pad and both sticks.
	std::vector<LayoutEntry> MappingLayout(EmulatedController::Type type)
	{
		std::vector<LayoutEntry> layout;
		const wxString buttons = _("Buttons"), dpad = _("D-Pad"), left = _("Left Stick"), right = _("Right Stick");
		auto add = [&](const wxString& group, uint64 id, const wxString& name) { layout.push_back({group, id, name}); };
		auto directions = [&](const wxString& group, uint64 up, uint64 down, uint64 l, uint64 r) {
			add(group, up, _("Up"));
			add(group, down, _("Down"));
			add(group, l, _("Left"));
			add(group, r, _("Right"));
		};
		switch (type)
		{
		case EmulatedController::VPAD:
		{
			using C = VPADController;
			for (auto [id, name] : {std::pair<uint64, const char*>{C::kButtonId_A, "A"}, {C::kButtonId_B, "B"}, {C::kButtonId_X, "X"}, {C::kButtonId_Y, "Y"},
				{C::kButtonId_L, "L"}, {C::kButtonId_R, "R"}, {C::kButtonId_ZL, "ZL"}, {C::kButtonId_ZR, "ZR"}})
				add(buttons, id, name);
			add(buttons, C::kButtonId_Plus, _("+ (Plus)"));
			add(buttons, C::kButtonId_Minus, _("- (Minus)"));
			add(buttons, C::kButtonId_Home, _("HOME"));
			directions(dpad, C::kButtonId_Up, C::kButtonId_Down, C::kButtonId_Left, C::kButtonId_Right);
			directions(left, C::kButtonId_StickL_Up, C::kButtonId_StickL_Down, C::kButtonId_StickL_Left, C::kButtonId_StickL_Right);
			add(left, C::kButtonId_StickL, _("Click (L3)"));
			directions(right, C::kButtonId_StickR_Up, C::kButtonId_StickR_Down, C::kButtonId_StickR_Left, C::kButtonId_StickR_Right);
			add(right, C::kButtonId_StickR, _("Click (R3)"));
			add(_("GamePad"), C::kButtonId_Mic, _("Blow into microphone"));
			add(_("GamePad"), C::kButtonId_Screen, _("Show GamePad screen"));
			break;
		}
		case EmulatedController::Pro:
		{
			using C = ProController;
			for (auto [id, name] : {std::pair<uint64, const char*>{C::kButtonId_A, "A"}, {C::kButtonId_B, "B"}, {C::kButtonId_X, "X"}, {C::kButtonId_Y, "Y"},
				{C::kButtonId_L, "L"}, {C::kButtonId_R, "R"}, {C::kButtonId_ZL, "ZL"}, {C::kButtonId_ZR, "ZR"}})
				add(buttons, id, name);
			add(buttons, C::kButtonId_Plus, _("+ (Plus)"));
			add(buttons, C::kButtonId_Minus, _("- (Minus)"));
			add(buttons, C::kButtonId_Home, _("HOME"));
			directions(dpad, C::kButtonId_Up, C::kButtonId_Down, C::kButtonId_Left, C::kButtonId_Right);
			directions(left, C::kButtonId_StickL_Up, C::kButtonId_StickL_Down, C::kButtonId_StickL_Left, C::kButtonId_StickL_Right);
			add(left, C::kButtonId_StickL, _("Click (L3)"));
			directions(right, C::kButtonId_StickR_Up, C::kButtonId_StickR_Down, C::kButtonId_StickR_Left, C::kButtonId_StickR_Right);
			add(right, C::kButtonId_StickR, _("Click (R3)"));
			break;
		}
		case EmulatedController::Classic:
		{
			using C = ClassicController;
			for (auto [id, name] : {std::pair<uint64, const char*>{C::kButtonId_A, "A"}, {C::kButtonId_B, "B"}, {C::kButtonId_X, "X"}, {C::kButtonId_Y, "Y"},
				{C::kButtonId_L, "L"}, {C::kButtonId_R, "R"}, {C::kButtonId_ZL, "ZL"}, {C::kButtonId_ZR, "ZR"}})
				add(buttons, id, name);
			add(buttons, C::kButtonId_Plus, _("+ (Plus)"));
			add(buttons, C::kButtonId_Minus, _("- (Minus)"));
			add(buttons, C::kButtonId_Home, _("HOME"));
			directions(dpad, C::kButtonId_Up, C::kButtonId_Down, C::kButtonId_Left, C::kButtonId_Right);
			directions(left, C::kButtonId_StickL_Up, C::kButtonId_StickL_Down, C::kButtonId_StickL_Left, C::kButtonId_StickL_Right);
			directions(right, C::kButtonId_StickR_Up, C::kButtonId_StickR_Down, C::kButtonId_StickR_Left, C::kButtonId_StickR_Right);
			break;
		}
		case EmulatedController::Wiimote:
		{
			using C = WiimoteController;
			add(buttons, C::kButtonId_A, "A");
			add(buttons, C::kButtonId_B, "B");
			add(buttons, C::kButtonId_1, "1");
			add(buttons, C::kButtonId_2, "2");
			add(buttons, C::kButtonId_Plus, _("+ (Plus)"));
			add(buttons, C::kButtonId_Minus, _("- (Minus)"));
			add(buttons, C::kButtonId_Home, _("HOME"));
			directions(dpad, C::kButtonId_Up, C::kButtonId_Down, C::kButtonId_Left, C::kButtonId_Right);
			const wxString nunchuk = _("Nunchuk");
			add(nunchuk, C::kButtonId_Nunchuck_C, "C");
			add(nunchuk, C::kButtonId_Nunchuck_Z, "Z");
			add(nunchuk, C::kButtonId_Nunchuck_Up, _("Stick up"));
			add(nunchuk, C::kButtonId_Nunchuck_Down, _("Stick down"));
			add(nunchuk, C::kButtonId_Nunchuck_Left, _("Stick left"));
			add(nunchuk, C::kButtonId_Nunchuck_Right, _("Stick right"));
			break;
		}
		default:
			break;
		}
		return layout;
	}
}

void GameModeBackendCemu::SavePlayer(int player)
{
	InputManager::instance().save();
	m_profilesCache.until = {};
	m_nav.Reset();
}

Choice GameModeBackendCemu::GetPlayerControllerType(int player)
{
	Choice choice;
	const auto controller = InputManager::instance().get_controller(player);
	if (!controller)
		return choice;
	for (size_t i = 0; i < std::size(kTypes); i++)
	{
		choice.options.push_back(TypeName(kTypes[i]));
		if (kTypes[i] == controller->type())
			choice.selected = (int)i;
	}
	return choice;
}

// Mirrors Input settings: switching the type keeps the player's devices and gives them the
// default mapping of the new type.
std::optional<wxString> GameModeBackendCemu::SetPlayerControllerType(int player, int index)
{
	if (index < 0 || index >= (int)std::size(kTypes))
		return std::nullopt;
	auto& input = InputManager::instance();
	const auto old = input.get_controller(player);
	if (old && old->type() == kTypes[index])
		return std::nullopt;
	std::vector<std::shared_ptr<ControllerBase>> devices;
	if (old)
		devices = old->get_controllers();
	try
	{
		const auto controller = input.set_controller(player, kTypes[index]);
		if (!controller)
			return _("This controller type is not available for this player");
		if (controller->get_controllers().empty())
		{
			for (const auto& device : devices)
				controller->add_controller(device);
		}
		for (const auto& device : controller->get_controllers())
			controller->set_default_mapping(device);
	}
	catch (const std::exception& e)
	{
		// e.g. only two GamePads can be connected
		return wxString::FromUTF8(e.what());
	}
	SavePlayer(player);
	return std::nullopt;
}

void GameModeBackendCemu::RefreshDevices()
{
	m_devices.clear();
	auto& input = InputManager::instance();
	// gamepads and the keyboard; scanning for Wii Remotes or DSU servers can block
	for (const auto api : {InputAPI::SDLController, InputAPI::XInput, InputAPI::WGIGamepad, InputAPI::Keyboard})
	{
		for (const auto& provider : input.get_api_providers()[api])
		{
			for (const auto& device : provider->get_controllers())
				m_devices.push_back(device);
		}
	}
}

Choice GameModeBackendCemu::GetPlayerDevice(int player)
{
	Choice choice;
	const auto controller = InputManager::instance().get_controller(player);
	std::shared_ptr<ControllerBase> current;
	if (controller && !controller->get_controllers().empty())
		current = controller->get_controllers().front();
	for (size_t i = 0; i < m_devices.size(); i++)
	{
		const auto& device = m_devices[i];
		choice.options.push_back(DeviceLabel(*device));
		if (current && current->api() == device->api() && current->uuid() == device->uuid())
			choice.selected = (int)i;
	}
	// a device that is set up but not connected, or a keyboard, still shows as the current one
	if (current && (m_devices.empty() || choice.options.size() == m_devices.size()) && std::none_of(m_devices.begin(), m_devices.end(), [&](const auto& d) { return d->api() == current->api() && d->uuid() == current->uuid(); }))
	{
		choice.options.insert(choice.options.begin(), DeviceLabel(*current));
		choice.selected = 0;
	}
	if (choice.options.empty())
		choice.options.push_back(_("No controller found"));
	return choice;
}

std::optional<wxString> GameModeBackendCemu::SetPlayerDevice(int player, int index)
{
	const auto controller = InputManager::instance().get_controller(player);
	if (!controller)
		return _("Pick a profile or controller type first");
	// index may be shifted by the "current device" entry GetPlayerDevice inserted
	const auto choice = GetPlayerDevice(player);
	if (index < 0 || index >= (int)choice.options.size())
		return std::nullopt;
	const int offset = (int)choice.options.size() - (int)m_devices.size();
	const int deviceIndex = index - offset;
	if (deviceIndex < 0 || deviceIndex >= (int)m_devices.size())
		return std::nullopt; // the current device, unchanged
	const auto& device = m_devices[deviceIndex];
	device->connect();
	controller->clear_controllers();
	controller->add_controller(device);
	controller->set_default_mapping(device);
	SavePlayer(player);
	return std::nullopt;
}

std::vector<GameMode::MappingEntry> GameModeBackendCemu::GetMappings(int player)
{
	std::vector<GameMode::MappingEntry> entries;
	const auto controller = InputManager::instance().get_controller(player);
	if (!controller)
		return entries;
	for (const auto& entry : MappingLayout(controller->type()))
		entries.push_back({entry.id, entry.group, entry.name, wxString::FromUTF8(controller->get_mapping_name(entry.id))});
	return entries;
}

void GameModeBackendCemu::ClearMapping(int player, uint64_t mapping)
{
	if (const auto controller = InputManager::instance().get_controller(player))
	{
		controller->delete_mapping(mapping);
		SavePlayer(player);
	}
}

void GameModeBackendCemu::ResetMappings(int player)
{
	const auto controller = InputManager::instance().get_controller(player);
	if (!controller)
		return;
	controller->clear_mappings();
	for (const auto& device : controller->get_controllers())
		controller->set_default_mapping(device);
	SavePlayer(player);
}

void GameModeBackendCemu::BeginMappingCapture(int player, uint64_t mapping)
{
	m_capturePlayer = player;
	m_captureMapping = mapping;
	m_captureWasIdle = false; // wait for everything to be let go first (the A that opened this)
}

// Same rules as the Input settings panel: one direction per stick, and analog inputs have to be
// pushed at least a third of the way.
bool GameModeBackendCemu::PollMappingCapture()
{
	const auto controller = InputManager::instance().get_controller(m_capturePlayer);
	if (!controller)
		return false;
	bool allIdle = true;
	// the player's devices, plus the keyboard so a key can always be bound (it is added to the
	// player when used, like picking a key in Input settings)
	auto devices = controller->get_controllers();
	std::shared_ptr<ControllerBase> keyboard;
	if (std::none_of(devices.begin(), devices.end(), [](const auto& d) { return d->api() == InputAPI::Keyboard; }))
	{
		for (const auto& provider : InputManager::instance().get_api_providers()[InputAPI::Keyboard])
		{
			for (const auto& device : provider->get_controllers())
			{
				if (!keyboard)
					keyboard = device;
			}
		}
		if (keyboard)
			devices.push_back(keyboard);
	}
	for (const auto& device : devices)
	{
		const auto& state = device->update_state();
		if (state.buttons.IsIdle())
			continue;
		allIdle = false;
		if (!m_captureWasIdle)
			continue;
		for (const auto id : state.buttons.GetButtonList())
		{
			auto dominated = [&](uint64 a, uint64 b, float av, float bv) {
				return (id == a || id == b) && std::abs(bv) > std::abs(av);
			};
			if (device->has_axis())
			{
				if ((id == kAxisXP || id == kAxisXN) && std::abs(state.axis.y) > std::abs(state.axis.x)) continue;
				if ((id == kAxisYP || id == kAxisYN) && std::abs(state.axis.x) > std::abs(state.axis.y)) continue;
				if ((id == kRotationXP || id == kRotationXN) && std::abs(state.rotation.y) > std::abs(state.rotation.x)) continue;
				if ((id == kRotationYP || id == kRotationYN) && std::abs(state.rotation.x) > std::abs(state.rotation.y)) continue;
				if (dominated(kTriggerXP, kTriggerXN, state.trigger.x, state.trigger.y)) continue;
				if (dominated(kTriggerYP, kTriggerYN, state.trigger.y, state.trigger.x)) continue;
				if (id >= kButtonAxisStart && device->get_axis_value(id) < 0.33f)
					continue;
			}
			if (device == keyboard)
				controller->add_controller(device);
			controller->set_mapping(m_captureMapping, device, id);
			SavePlayer(m_capturePlayer);
			m_capturePlayer = -1;
			return true;
		}
	}
	if (allIdle)
		m_captureWasIdle = true;
	return false;
}

std::vector<wxString> GameModeBackendCemu::GetProfileNames()
{
	std::vector<wxString> names;
	for (const auto& profile : CachedProfiles())
		names.push_back(wxString::FromUTF8(profile));
	return names;
}

std::optional<wxString> GameModeBackendCemu::SaveProfile(int player, const wxString& name)
{
	const std::string utf8 = name.utf8_string();
	if (!InputManager::is_valid_profilename(utf8))
		return wxString::Format(_("Invalid profile name: %s"), name);
	if (!InputManager::instance().save(player, utf8))
		return _("Couldn't save the profile");
	// make the player use the profile it was just saved as
	InputManager::instance().load(player, utf8);
	SavePlayer(player);
	return std::nullopt;
}

void GameModeBackendCemu::ResetGameMenuBinding()
{
	uKeyboardHotkey key{};
	key.key = WXK_F10;
	HotkeySettings::SetGameMenuKeyboardHotkey(key);
	HotkeySettings::SetGameMenuControllerHotkey(5); // Guide
}

// ---- Game Mode preferences ----

bool GameModeBackendCemu::GetAlwaysBootGameMode()
{
	return GetWxGUIConfig().game_mode_boot;
}

void GameModeBackendCemu::SetAlwaysBootGameMode(bool enabled)
{
	GetWxGUIConfig().game_mode_boot = enabled;
	SaveGuiConfig();
}

GameMode::ButtonStyle GameModeBackendCemu::GetButtonStyle()
{
	return GameMode::GetButtonStyle();
}

void GameModeBackendCemu::SetButtonStyle(GameMode::ButtonStyle style)
{
	GetWxGUIConfig().game_mode_buttons = (sint32)style;
	SaveGuiConfig();
}

GameMode::Face GameModeBackendCemu::GetNavFace(GameMode::Nav nav)
{
	return GameMode::FaceForNav(nav);
}

// ---- navigation ----

void GameModeBackendCemu::PollControllerNav(std::vector<GameMode::Nav>& out)
{
	// No game is running while the launcher is visible, so nothing else polls the controllers.
	m_nav.Poll(out, true);
}

void GameModeBackendCemu::ExitGameMode()
{
	MainWindow* window = m_mainWindow;
	// not from inside the panel's own event handler: this destroys the panel
	wxTheApp->CallAfter([window]() { window->SetGameModeEnabled(false); });
}
