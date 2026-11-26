#include "Cafe/Cheats/CheatManager.h"
#include "Cafe/Cheats/GatewayCheat.h"

#include "Cafe/HW/MMU/MMU.h"
#include "Cafe/HW/Espresso/Recompiler/PPCRecompiler.h"
#include "config/ActiveSettings.h"

#include <atomic>
#include <fstream>
#include <mutex>
#include <sstream>
#include <unordered_map>

namespace CheatManager
{
	// Guest code lives below this address; writes that change bytes there must drop any recompiled
	// copy of that code, or the game keeps running the old instructions.
	static constexpr uint32_t kCodeRegionEnd = 0x10000000;

	// Which cheat wrote what to an address. Code addresses (below kCodeRegionEnd) are kept for the
	// whole activation, since code doesn't move; other addresses only for the current frame, since
	// heap memory gets reused.
	struct WriteClaim
	{
		size_t cheat;
		uint32_t value;
		uint32_t size;
	};
	using ClaimMap = std::unordered_map<uint32_t, WriteClaim>;

	class CemuMemory final : public GatewayCheat::Memory
	{
	public:
		CemuMemory(ClaimMap& codeClaims, ClaimMap& frameClaims) : m_codeClaims(codeClaims), m_frameClaims(frameClaims) {}

		// Starts a cheat's run. Its writes are checked against the other cheats' writes.
		void Begin(size_t cheat)
		{
			m_cheat = cheat;
			m_blocked = false;
		}
		// Set when the cheat tried to write a different value to an address another cheat writes.
		// It made no writes from that point on.
		bool Blocked() const { return m_blocked; }
		size_t ClashCheat() const { return m_clashCheat; }
		uint32_t ClashAddress() const { return m_clashAddress; }

		bool IsValid(uint32_t address, uint32_t size) override
		{
			if (size == 0 || (uint64_t)address + size > 0x100000000ull)
				return false;
			return memory_isAddressRangeAccessible(address, size);
		}
		uint32_t Read32(uint32_t address) override { return memory_readU32(address); }
		uint16_t Read16(uint32_t address) override { return memory_readU16(address); }
		uint8_t Read8(uint32_t address) override { return memory_readU8(address); }

		// Only store when the value differs: cheats rewrite the same values every frame, and an
		// unchanged instruction must not throw away recompiled code 60 times a second.
		void Write32(uint32_t address, uint32_t value) override
		{
			if (!Claim(address, 4, value) || memory_readU32(address) == value)
				return;
			memory_writeU32(address, value);
			Invalidate(address, 4);
		}
		void Write16(uint32_t address, uint16_t value) override
		{
			if (!Claim(address, 2, value) || memory_readU16(address) == value)
				return;
			memory_writeU16(address, value);
			Invalidate(address, 2);
		}
		void Write8(uint32_t address, uint8_t value) override
		{
			if (!Claim(address, 1, value) || memory_readU8(address) == value)
				return;
			memory_writeU8(address, value);
			Invalidate(address, 1);
		}

	private:
		// False if another cheat already writes a different value here: the write must not happen,
		// or the two cheats would rewrite each other every frame.
		bool Claim(uint32_t address, uint32_t size, uint32_t value)
		{
			if (m_blocked)
				return false;
			ClaimMap& claims = address < kCodeRegionEnd ? m_codeClaims : m_frameClaims;
			auto [it, inserted] = claims.try_emplace(address, WriteClaim{m_cheat, value, size});
			if (inserted || it->second.cheat == m_cheat)
			{
				it->second = WriteClaim{m_cheat, value, size};
				return true;
			}
			if (it->second.value == value && it->second.size == size)
				return true;
			m_blocked = true;
			m_clashCheat = it->second.cheat;
			m_clashAddress = address;
			return false;
		}

		ClaimMap& m_codeClaims;
		ClaimMap& m_frameClaims;
		size_t m_cheat = 0;
		bool m_blocked = false;
		size_t m_clashCheat = 0;
		uint32_t m_clashAddress = 0;

		static void Invalidate(uint32_t address, uint32_t size)
		{
			if (address < kCodeRegionEnd)
				PPCRecompiler_invalidateRange(address & ~3u, (address + size + 3) & ~3u);
		}
	};

	struct ActiveCheat
	{
		std::string name;
		GatewayCheat::Code code;
		bool errorReported = false;
		bool stopped = false; // clashed with an earlier cheat in the list; skipped until the list changes
	};

	static std::mutex s_mutex;
	static uint64_t s_runningTitle = 0;
	static bool s_titleRunning = false;
	static std::vector<ActiveCheat> s_active;
	static std::atomic<bool> s_hasActive{false};
	// write claims (see CemuMemory), guarded by s_mutex
	static ClaimMap s_codeClaims;
	static ClaimMap s_frameClaims;

	fs::path GetCheatFolder()
	{
		return ActiveSettings::GetUserDataPath("cheats");
	}

	static std::string TitleIdHex(uint64_t titleId, bool upper)
	{
		char buf[32];
		snprintf(buf, sizeof(buf), upper ? "%016llX" : "%016llx", (unsigned long long)titleId);
		return buf;
	}

	fs::path GetCheatFile(uint64_t titleId)
	{
		const fs::path folder = GetCheatFolder();
		// prefer an existing file in either case, so a copied-in Citra-style uppercase name works
		// on case-sensitive filesystems too
		std::error_code ec;
		const fs::path upper = folder / (TitleIdHex(titleId, true) + ".txt");
		const fs::path lower = folder / (TitleIdHex(titleId, false) + ".txt");
		if (!fs::exists(lower, ec) && fs::exists(upper, ec))
			return upper;
		return lower;
	}

	std::vector<Cheat> Load(uint64_t titleId)
	{
		std::ifstream f(GetCheatFile(titleId), std::ios::binary);
		if (!f)
			return {};
		std::stringstream ss;
		ss << f.rdbuf();
		return Parse(ss.str());
	}

	bool Validate(const Cheat& cheat, std::string& error)
	{
		GatewayCheat::Code code;
		return GatewayCheat::Code::Compile(cheat.code, code, error);
	}

	// Rebuilds the set of cheats that run each frame. Caller holds s_mutex.
	static void ActivateLocked(const std::vector<Cheat>& cheats)
	{
		s_active.clear();
		s_codeClaims.clear();
		for (const auto& c : cheats)
		{
			if (!c.enabled)
				continue;
			ActiveCheat a;
			a.name = c.name;
			std::string error;
			if (!GatewayCheat::Code::Compile(c.code, a.code, error))
			{
				cemuLog_log(LogType::Force, "[Cheats] '{}' not enabled: {}", c.name, error);
				continue;
			}
			if (a.code.Empty())
				continue;
			cemuLog_log(LogType::Force, "[Cheats] Enabled '{}'", c.name);
			s_active.push_back(std::move(a));
		}
		s_hasActive = !s_active.empty();
	}

	bool Save(uint64_t titleId, const std::vector<Cheat>& cheats, std::string& error)
	{
		std::error_code ec;
		fs::create_directories(GetCheatFolder(), ec);
		const fs::path target = GetCheatFile(titleId);
		const fs::path temp = fs::path(target).concat(".tmp");
		{
			std::ofstream f(temp, std::ios::binary | std::ios::trunc);
			if (!f)
			{
				error = "Could not write " + _pathToUtf8(temp);
				return false;
			}
			f << Serialize(cheats);
			if (!f)
			{
				error = "Could not write " + _pathToUtf8(temp);
				return false;
			}
		}
		fs::rename(temp, target, ec);
		if (ec)
		{
			fs::remove(temp, ec);
			error = "Could not replace " + _pathToUtf8(target);
			return false;
		}

		Apply(titleId, cheats);
		return true;
	}

	void Apply(uint64_t titleId, const std::vector<Cheat>& cheats)
	{
		std::lock_guard lock(s_mutex);
		if (s_titleRunning && s_runningTitle == titleId)
			ActivateLocked(cheats);
	}

	void OnTitleStart(uint64_t titleId)
	{
		const auto cheats = Load(titleId);
		std::lock_guard lock(s_mutex);
		s_runningTitle = titleId;
		s_titleRunning = true;
		ActivateLocked(cheats);
		if (!cheats.empty())
			cemuLog_log(LogType::Force, "[Cheats] {} cheat(s) in file, {} enabled", cheats.size(), s_active.size());
	}

	void OnTitleStop()
	{
		std::lock_guard lock(s_mutex);
		s_titleRunning = false;
		s_runningTitle = 0;
		s_active.clear();
		s_hasActive = false;
	}

	bool IsRunningTitle(uint64_t titleId)
	{
		std::lock_guard lock(s_mutex);
		return s_titleRunning && s_runningTitle == titleId;
	}

	void RunFrame()
	{
		if (!s_hasActive.load(std::memory_order_relaxed))
			return;
		// never stall the vsync thread behind the UI saving a file; just skip this frame
		std::unique_lock lock(s_mutex, std::try_to_lock);
		if (!lock.owns_lock())
			return;
		s_frameClaims.clear();
		CemuMemory mem(s_codeClaims, s_frameClaims);
		for (size_t i = 0; i < s_active.size(); i++)
		{
			auto& a = s_active[i];
			if (a.stopped)
				continue;
			mem.Begin(i);
			std::string error;
			const bool ok = a.code.Execute(mem, &error);
			if (mem.Blocked())
			{
				a.stopped = true;
				cemuLog_log(LogType::Force, "[Cheats] '{}' stopped: it writes a different value to {:08x} than '{}'. "
					"Two cheats that change the same address rewrite each other every frame and can crash the game. Keep only one of them on.",
					a.name, mem.ClashAddress(), s_active[mem.ClashCheat()].name);
				continue;
			}
			if (!ok && !a.errorReported)
			{
				// keep running it: the address may become valid later (e.g. after a heap allocation)
				cemuLog_log(LogType::Force, "[Cheats] '{}': {}", a.name, error);
				a.errorReported = true;
			}
		}
	}
}
