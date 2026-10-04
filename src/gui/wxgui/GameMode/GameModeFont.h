#pragma once

// Game Mode's typeface: Lato Medium, the font Cemu already ships for its in-game text (ImGui
// overlay, notifications, the F10 menu). The launcher uses it too, so both look the same.

#include <wx/font.h>

namespace GameMode
{
	// Makes the built-in Lato Medium available to wxWidgets. Call once, early (before windows are
	// created): some platforms only see private fonts added before the first font is used.
	void RegisterUIFont();

	// A Game Mode font of the given pixel height: Lato Medium when it registered, otherwise the
	// system's sans-serif.
	wxFont MakeUIFont(double px, bool bold = false);
}
