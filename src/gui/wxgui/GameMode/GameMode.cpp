#include "wxgui/GameMode/GameMode.h"

#include "Cafe/CafeSystem.h"
#include "config/CemuConfig.h"
#include "input/InputManager.h"
#include "input/api/Controller.h"
#include "wxgui/wxCemuConfig.h"
#include "input/emulated/ClassicController.h"
#include "input/emulated/ProController.h"
#include "input/emulated/VPADController.h"
#include "input/emulated/WiimoteController.h"

#include <wx/intl.h>

#include <atomic>
#include <deque>
#include <mutex>

namespace GameMode
{
	namespace
	{
		std::atomic<bool> s_menuOpen{false};
		std::atomic<bool> s_releasePending{false};
		std::mutex s_navMutex;
		std::deque<Nav> s_navQueue;
	}

	bool IsMenuOpen()
	{
		return s_menuOpen.load();
	}

	void SetMenuOpen(bool open)
	{
		if (open && !CafeSystem::IsTitleRunning())
			return;
		if (!open && s_menuOpen.load())
			s_releasePending = true;
		s_menuOpen = open;
		if (open)
		{
			std::scoped_lock lock(s_navMutex);
			s_navQueue.clear();
		}
	}

	void ToggleMenu()
	{
		SetMenuOpen(!IsMenuOpen());
	}

	bool IsGameInputBlocked()
	{
		return s_menuOpen.load() || s_releasePending.load();
	}

	// Called by the overlay once it has seen every button released after the menu closed.
	void ClearReleasePending()
	{
		s_releasePending = false;
	}

	void QueueMenuNav(Nav nav)
	{
		std::scoped_lock lock(s_navMutex);
		if (s_navQueue.size() < 32)
			s_navQueue.push_back(nav);
	}

	bool TakeMenuNav(Nav& nav)
	{
		std::scoped_lock lock(s_navMutex);
		if (s_navQueue.empty())
			return false;
		nav = s_navQueue.front();
		s_navQueue.pop_front();
		return true;
	}

	wxString ScreenLayoutName(int layout)
	{
		switch (layout)
		{
		case kKeepAspectRatio: return _("Fit (keep aspect ratio)");
		case kStretch: return _("Stretch");
		case kFill: return _("Fill (crop edges)");
		case kAspect16x9: return _("16:9");
		case kAspect16x10: return _("16:10");
		case kAspect4x3: return _("4:3");
		case kAspect21x9: return _("21:9");
		case kIntegerScale: return _("Integer scale (pixel-sharp)");
		default: return _("Fit (keep aspect ratio)");
		}
	}

	ButtonStyle GetButtonStyle()
	{
		const sint32 style = GetWxGUIConfig().game_mode_buttons;
		return (style >= 0 && style < (sint32)ButtonStyle::Count) ? (ButtonStyle)style : ButtonStyle::Nintendo;
	}

	wxString ButtonStyleName(ButtonStyle style)
	{
		switch (style)
		{
		case ButtonStyle::Xbox: return _("Xbox");
		case ButtonStyle::PlayStation: return _("PlayStation");
		case ButtonStyle::SteamDeck: return _("Steam Deck (SteamOS)");
		case ButtonStyle::Keyboard: return _("Keyboard");
		default: return _("Nintendo");
		}
	}

	wxString FaceLetter(ButtonStyle style, Face face)
	{
		if (style == ButtonStyle::Nintendo)
		{
			switch (face)
			{
			case Face::South: return "B";
			case Face::East: return "A";
			case Face::West: return "Y";
			default: return "X";
			}
		}
		// Xbox and Steam Deck share the letters
		switch (face)
		{
		case Face::South: return "A";
		case Face::East: return "B";
		case Face::West: return "X";
		default: return "Y";
		}
	}

	bool GetSwapAB()
	{
		return GetWxGUIConfig().game_mode_swap_ab;
	}

	void SetSwapAB(bool swap)
	{
		GetWxGUIConfig().game_mode_swap_ab = swap;
		g_wxConfig.Save();
	}

	Face FaceForNav(Nav nav)
	{
		// swapped: Accept sits on the player's B button and Back on their A button
		if (GetSwapAB())
		{
			if (nav == Nav::Accept)
				nav = Nav::Back;
			else if (nav == Nav::Back)
				nav = Nav::Accept;
		}
		// Cemu's default gamepad mapping is positional (Nintendo layout)
		Face fallback = Face::East;
		switch (nav)
		{
		case Nav::Back: fallback = Face::South; break;
		case Nav::Options: fallback = Face::North; break;
		case Nav::Settings: fallback = Face::West; break;
		default: break;
		}
		std::shared_ptr<EmulatedController> controller;
		for (size_t i = 0; i < InputManager::kMaxController && !controller; i++)
			controller = InputManager::instance().get_controller(i);
		if (!controller)
			return fallback;
		uint64 mapping = 0;
		const bool x = nav == Nav::Options, y = nav == Nav::Settings, a = nav == Nav::Accept, b = nav == Nav::Back;
		switch (controller->type())
		{
		case EmulatedController::VPAD:
			mapping = a ? VPADController::kButtonId_A : b ? VPADController::kButtonId_B : x ? VPADController::kButtonId_X : y ? VPADController::kButtonId_Y : 0;
			break;
		case EmulatedController::Pro:
			mapping = a ? ProController::kButtonId_A : b ? ProController::kButtonId_B : x ? ProController::kButtonId_X : y ? ProController::kButtonId_Y : 0;
			break;
		case EmulatedController::Classic:
			mapping = a ? ClassicController::kButtonId_A : b ? ClassicController::kButtonId_B : x ? ClassicController::kButtonId_X : y ? ClassicController::kButtonId_Y : 0;
			break;
		default:
			return fallback;
		}
		const auto device = controller->get_mapping_controller(mapping);
		const auto button = controller->get_mapping_button(mapping);
		if (!device || !button || device->api() != InputAPI::SDLController || *button > 3)
			return fallback;
		// SDL gamepad buttons: 0 south, 1 east, 2 west, 3 north
		static constexpr Face kSdlFaces[] = {Face::South, Face::East, Face::West, Face::North};
		return kSdlFaces[*button];
	}

	namespace
	{
		struct NavMappings
		{
			uint64 a, b, x, y, up, down, left, right, stickUp; // 0 = none
		};

		// X and Y are not part of the generic EmulatedController interface and each controller
		// type numbers its buttons differently. A Wiimote has neither, so its - and + stand in.
		NavMappings MappingsFor(EmulatedController::Type type)
		{
			switch (type)
			{
			case EmulatedController::VPAD:
			{
				using C = VPADController;
				return {C::kButtonId_A, C::kButtonId_B, C::kButtonId_X, C::kButtonId_Y, C::kButtonId_Up, C::kButtonId_Down, C::kButtonId_Left, C::kButtonId_Right, C::kButtonId_StickL_Up};
			}
			case EmulatedController::Pro:
			{
				using C = ProController;
				return {C::kButtonId_A, C::kButtonId_B, C::kButtonId_X, C::kButtonId_Y, C::kButtonId_Up, C::kButtonId_Down, C::kButtonId_Left, C::kButtonId_Right, C::kButtonId_StickL_Up};
			}
			case EmulatedController::Classic:
			{
				using C = ClassicController;
				return {C::kButtonId_A, C::kButtonId_B, C::kButtonId_X, C::kButtonId_Y, C::kButtonId_Up, C::kButtonId_Down, C::kButtonId_Left, C::kButtonId_Right, C::kButtonId_StickL_Up};
			}
			case EmulatedController::Wiimote:
			{
				using C = WiimoteController;
				return {C::kButtonId_A, C::kButtonId_B, C::kButtonId_Minus, C::kButtonId_Plus, C::kButtonId_Up, C::kButtonId_Down, C::kButtonId_Left, C::kButtonId_Right, C::kButtonId_Nunchuck_Up};
			}
			default:
				return {};
			}
		}

		// Keyboard-mapped buttons are skipped: the menus read the keyboard directly (arrows,
		// Enter, Esc), so counting them here as well would move twice per key press.
		bool FromKeyboard(const EmulatedController& controller, uint64 mapping)
		{
			const auto device = controller.get_mapping_controller(mapping);
			return device && device->api() == InputAPI::Keyboard;
		}

		bool Down(const EmulatedController& controller, uint64 mapping)
		{
			return mapping && controller.is_mapping_down(mapping) && !FromKeyboard(controller, mapping);
		}
	}

	void ControllerNav::Poll(std::vector<Nav>& out, bool updateStates)
	{
		bool now[kCount]{};
		bool anyRaw = false;
		auto& input = InputManager::instance();
		for (size_t i = 0; i < InputManager::kMaxController; i++)
		{
			const auto controller = input.get_controller(i);
			if (!controller)
				continue;
			if (updateStates)
				controller->controllers_update_states();
			for (const auto& physical : controller->get_controllers())
				anyRaw |= !physical->get_state().buttons.IsIdle();

			constexpr float kStick = 0.6f;
			const NavMappings m = MappingsFor(controller->type());
			const glm::vec2 axis = (m.stickUp && FromKeyboard(*controller, m.stickUp)) ? glm::vec2{} : controller->get_axis();
			now[(int)Nav::Up] |= Down(*controller, m.up) || axis.y >= kStick;
			now[(int)Nav::Down] |= Down(*controller, m.down) || axis.y <= -kStick;
			now[(int)Nav::Left] |= Down(*controller, m.left) || axis.x <= -kStick;
			now[(int)Nav::Right] |= Down(*controller, m.right) || axis.x >= kStick;
			now[(int)Nav::Accept] |= Down(*controller, m.a);
			now[(int)Nav::Back] |= Down(*controller, m.b);
			now[(int)Nav::Options] |= Down(*controller, m.x);
			now[(int)Nav::Settings] |= Down(*controller, m.y);
		}

		if (GetSwapAB())
			std::swap(now[(int)Nav::Accept], now[(int)Nav::Back]);

		const auto t = std::chrono::steady_clock::now();
		for (int i = 0; i < kCount; i++)
		{
			const bool isDirection = i <= (int)Nav::Right;
			if (now[i] && !m_down[i])
			{
				if (m_primed)
					out.push_back((Nav)i);
				m_nextRepeat[i] = t + std::chrono::milliseconds(400);
			}
			else if (now[i] && isDirection && m_primed && t >= m_nextRepeat[i])
			{
				out.push_back((Nav)i);
				m_nextRepeat[i] = t + std::chrono::milliseconds(90);
			}
			m_down[i] = now[i];
		}
		m_primed = true;
		m_anyRawDown = anyRaw;
	}

	bool ControllerNav::AnyDown() const
	{
		if (m_anyRawDown)
			return true;
		for (bool down : m_down)
			if (down)
				return true;
		return false;
	}
}
