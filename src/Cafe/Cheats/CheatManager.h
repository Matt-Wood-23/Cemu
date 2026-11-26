#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

// Per-title cheat lists, stored the way Citra/Azahar store them:
//   <UserData>/cheats/<titleId>.txt
//
//   [Cheat name]
//   {Optional notes, may span lines}
//   *cemu_enabled
//   02123450 38A00000
//
// "*citra_enabled" is accepted as well, so a Citra cheat file's structure carries over (its codes
// still need Wii U addresses). Enabled cheats run once per vsync (about 60 times a second) while
// the title is running. See GatewayCheat.h for the code types.
//
// Cemu writes "*cemu_user_choices" as the file's first line when it saves it. The enabled lines are
// only read from a file that has it, so a cheat file someone downloads or copies in starts with
// every cheat off, and the player turns on the ones they want.
//
// Two enabled cheats must not write different values to the same address: they would overwrite
// each other every frame, and when the address is code the game runs, the recompiler throws its
// code away under the running game (crashes). The first of the two in the list keeps running; the
// later one is stopped for the session and the clash is logged.
namespace CheatManager
{
	inline constexpr const char* kUserChoicesMarker = "*cemu_user_choices";

	struct Cheat
	{
		std::string name;
		std::string notes;
		bool enabled = false;
		std::vector<std::string> code;
	};

	std::filesystem::path GetCheatFolder();
	std::filesystem::path GetCheatFile(uint64_t titleId);

	std::vector<Cheat> Parse(const std::string& text);
	std::string Serialize(const std::vector<Cheat>& cheats);

	// Returns an empty list if the title has no cheat file.
	std::vector<Cheat> Load(uint64_t titleId);
	// Writes the file and, if this title is running, applies the list straight away.
	bool Save(uint64_t titleId, const std::vector<Cheat>& cheats, std::string& error);
	// Makes the running game use this list without touching the file. No-op for other titles.
	void Apply(uint64_t titleId, const std::vector<Cheat>& cheats);

	// Checks that a cheat's code parses. On failure, error says which line and why.
	bool Validate(const Cheat& cheat, std::string& error);

	// Emulator lifecycle
	void OnTitleStart(uint64_t titleId);
	void OnTitleStop();
	bool IsRunningTitle(uint64_t titleId);

	// Called once per vsync from the GPU/vsync thread.
	void RunFrame();
}
