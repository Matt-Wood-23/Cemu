#pragma once

#include "wxgui/GameMode/GameMode.h"
#include "Cafe/Cheats/CheatManager.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>

class MainWindow;
class ControllerBase;

// The launcher's view of Cemu: game list, per-title settings, global settings and input.
// Settings are read from and written to the same config the regular windows use, so both
// interfaces always agree.
class GameModeBackendCemu : public GameMode::Backend
{
public:
	explicit GameModeBackendCemu(MainWindow* mainWindow);
	~GameModeBackendCemu() override;

	std::vector<GameMode::GameEntry> ListGames() override;
	bool IsScanningGames() override;
	void RequestIcon(uint64_t titleId, std::function<void(uint64_t, const wxImage&)> onLoaded) override;
	void LaunchGame(uint64_t titleId) override;
	void SetFavorite(uint64_t titleId, bool favorite) override;

	GameMode::Choice GetGameScreenLayout(uint64_t titleId) override;
	void SetGameScreenLayout(uint64_t titleId, int choiceIndex) override;
	bool GetCustomTexturesEnabled(uint64_t titleId) override;
	void SetCustomTexturesEnabled(uint64_t titleId, bool enabled) override;
	std::vector<GameMode::NamedToggle> GetTexturePacks(uint64_t titleId) override;
	void SetTexturePackEnabled(uint64_t titleId, const wxString& pack, bool enabled) override;
	std::vector<GameMode::NamedToggle> GetCheats(uint64_t titleId) override;
	std::optional<wxString> SetCheatEnabled(uint64_t titleId, size_t index, bool enabled) override;
	std::optional<wxString> AddCheat(uint64_t titleId, const wxString& name, const wxString& code) override;
	std::optional<wxString> DeleteCheat(uint64_t titleId, size_t index) override;
	void OpenCheatsFolder(uint64_t titleId) override;

	GameMode::Choice GetGraphicsApi() override;
	void SetGraphicsApi(int index) override;
	GameMode::Choice GetVSync() override;
	void SetVSync(int index) override;
	GameMode::Choice GetDefaultScreenLayout() override;
	void SetDefaultScreenLayout(int index) override;
	GameMode::Choice GetUpscaleFilter() override;
	void SetUpscaleFilter(int index) override;
	GameMode::Choice GetDownscaleFilter() override;
	void SetDownscaleFilter(int index) override;
	bool GetFullscreen() override;
	void SetFullscreen(bool enabled) override;

	int GetTvVolume() override;
	void SetTvVolume(int volume) override;
	int GetPadVolume() override;
	void SetPadVolume(int volume) override;

	int GetPlayerCount() override;
	GameMode::Choice GetPlayerProfile(int player) override;
	std::optional<wxString> SetPlayerProfile(int player, int choiceIndex) override;
	wxString GetGameMenuBindingLabel() override;
	void BeginGameMenuCapture() override;
	bool PollGameMenuCapture() override;
	void BindGameMenuKey(int wxKeyCode, bool alt, bool ctrl, bool shift) override;

	GameMode::Choice GetPlayerControllerType(int player) override;
	std::optional<wxString> SetPlayerControllerType(int player, int index) override;
	GameMode::Choice GetPlayerDevice(int player) override;
	std::optional<wxString> SetPlayerDevice(int player, int index) override;
	void RefreshDevices() override;
	std::vector<GameMode::MappingEntry> GetMappings(int player) override;
	void ClearMapping(int player, uint64_t mapping) override;
	void ResetMappings(int player) override;
	void BeginMappingCapture(int player, uint64_t mapping) override;
	bool PollMappingCapture() override;
	std::vector<wxString> GetProfileNames() override;
	std::optional<wxString> SaveProfile(int player, const wxString& name) override;
	void ResetGameMenuBinding() override;

	bool GetAlwaysBootGameMode() override;
	void SetAlwaysBootGameMode(bool enabled) override;
	GameMode::ButtonStyle GetButtonStyle() override;
	void SetButtonStyle(GameMode::ButtonStyle style) override;
	bool GetSwapAB() override;
	void SetSwapAB(bool swap) override;
	GameMode::Face GetNavFace(GameMode::Nav nav) override;
	GameMode::BootVideoInfo GetBootVideoInfo() override;
	bool GetBootVideoEnabled() override;
	void SetBootVideoEnabled(bool enabled) override;
	std::optional<wxString> UseBuiltInBootVideo() override;
	std::unique_ptr<GameMode::VideoPlayer> TakeStartupBootVideo() override;
	std::unique_ptr<GameMode::VideoPlayer> OpenBootVideo() override;
	void OpenBootFolder() override;

	void PollControllerNav(std::vector<GameMode::Nav>& out) override;
	bool IsNavDown(GameMode::Nav nav) override;
	// Game Mode was turned on: the next launcher plays the boot video (if enabled)
	static void QueueBootVideo();
	void ExitGameMode() override;

private:
	struct TextureSettings
	{
		bool enabled = true;
		std::vector<std::string> packs; // enabled packs
	};
	TextureSettings ReadTextureSettings(uint64_t titleId);
	void WriteTextureSettings(uint64_t titleId, const TextureSettings& settings);
	std::vector<std::shared_ptr<ControllerBase>> AllControllers();
	void IconWorker();

	// The launcher redraws often and several rows show values that live on disk (pack folders,
	// cheat files, controller profiles). Those reads are cached briefly; writes drop the cache.
	template<typename T>
	struct Cached
	{
		T value{};
		uint64_t key = ~0ull;
		std::chrono::steady_clock::time_point until{};
	};
	const std::vector<std::string>& CachedPacks(uint64_t titleId);
	const std::vector<CheatManager::Cheat>& CachedCheats(uint64_t titleId);
	const std::vector<std::string>& CachedProfiles();
	Cached<std::vector<std::string>> m_packsCache;
	Cached<std::vector<CheatManager::Cheat>> m_cheatsCache;
	Cached<std::vector<std::string>> m_profilesCache;

	MainWindow* m_mainWindow;
	GameMode::ControllerNav m_nav;

	// captured button states when a Game Menu rebind started
	std::vector<std::pair<std::weak_ptr<ControllerBase>, std::vector<uint64>>> m_captureBaseline;

	// remapper
	std::vector<std::shared_ptr<ControllerBase>> m_devices; // connected controllers, from RefreshDevices
	int m_capturePlayer = -1;
	uint64_t m_captureMapping = 0;
	bool m_captureWasIdle = false;
	void SavePlayer(int player);

	// icon loading
	std::thread m_iconThread;
	std::mutex m_iconMutex;
	std::condition_variable m_iconCv;
	std::deque<std::pair<uint64_t, std::function<void(uint64_t, const wxImage&)>>> m_iconQueue;
	bool m_iconStop = false;
};
