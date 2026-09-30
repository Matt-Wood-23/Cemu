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

Choice GameModeBackendCemu::GetPlayerProfile(int player)
{
	Choice choice;
	const auto controller = InputManager::instance().get_controller(player);
	wxString current;
	if (!controller)
		current = _("none");
	else if (controller->has_profile_name())
		current = wxString::FromUTF8(controller->get_profile_name());
	else
		current = wxString::FromUTF8(std::string(controller->type_string()));
	choice.options.push_back(wxString::Format(_("Current: %s"), current));
	for (const auto& profile : CachedProfiles())
		choice.options.push_back(wxString::FromUTF8(profile));
	choice.selected = 0;
	return choice;
}

std::optional<wxString> GameModeBackendCemu::SetPlayerProfile(int player, int choiceIndex)
{
	if (choiceIndex <= 0)
		return std::nullopt;
	const auto profiles = CachedProfiles();
	if (choiceIndex - 1 >= (int)profiles.size())
		return std::nullopt;
	if (!InputManager::instance().load(player, profiles[choiceIndex - 1]))
		return wxString::Format(_("Couldn't load profile %s"), wxString::FromUTF8(profiles[choiceIndex - 1]));
	InputManager::instance().save();
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
