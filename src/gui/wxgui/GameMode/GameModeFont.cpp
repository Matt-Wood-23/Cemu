#include "GameModeFont.h"

#include <wx/filename.h>
#include <wx/fontenum.h>
#include <wx/stdpaths.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <vector>

// src/resource/CafeDefaultFont.cpp: Cemu's built-in font (Lato Medium), zlib-packed in the binary.
// Returns a malloc'd buffer.
uint8_t* extractCafeDefaultFont(int32_t* size);

namespace GameMode
{
	namespace
	{
		bool s_registered = false;
		wxString s_face;            // face name to ask wxWidgets for; empty = system sans-serif
		bool s_faceIsMedium = false; // the face name already means the Medium weight

		// wxWidgets registers fonts from files, so the built-in one is written to the temp folder
		// (rewritten only when missing or different).
		wxString WriteFontFile(const uint8_t* data, size_t size)
		{
			const wxFileName file(wxStandardPaths::Get().GetTempDir(), "cemu-gamemode-Lato-Medium.ttf");
			const wxString path = file.GetFullPath();
			std::ifstream in(path.fn_str(), std::ios::binary);
			if (in)
			{
				std::vector<char> existing((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
				if (existing.size() == size && std::memcmp(existing.data(), data, size) == 0)
					return path;
			}
			in.close();
			std::ofstream out(path.fn_str(), std::ios::binary | std::ios::trunc);
			out.write((const char*)data, (std::streamsize)size);
			return out.good() ? path : wxString();
		}
	}

	void RegisterUIFont()
	{
		if (s_registered)
			return;
		s_registered = true;
#if wxUSE_PRIVATE_FONTS
		int32_t size = 0;
		uint8_t* data = extractCafeDefaultFont(&size);
		if (!data)
			return;
		const wxString path = size > 0 ? WriteFontFile(data, (size_t)size) : wxString();
		free(data);
		if (path.empty() || !wxFont::AddPrivateFont(path))
			return;
		// Windows' GDI lists the file by its legacy family name, "Lato Medium". Pango and Core Text
		// list it as family "Lato" with a Medium (500) weight.
		if (wxFontEnumerator::IsValidFacename("Lato"))
			s_face = "Lato";
		else if (wxFontEnumerator::IsValidFacename("Lato Medium"))
		{
			s_face = "Lato Medium";
			s_faceIsMedium = true;
		}
#endif
	}

	wxFont MakeUIFont(double px, bool bold)
	{
		wxFontInfo info(wxSize(0, std::max(6, (int)std::lround(px))));
		if (s_face.empty())
		{
			info.Family(wxFONTFAMILY_SWISS);
			if (bold)
				info.Bold();
		}
		else
		{
			info.FaceName(s_face);
			// Only Lato Medium is built in: text uses it as is, and bold is drawn from it.
			if (bold)
				info.Bold();
			else if (!s_faceIsMedium)
				info.Weight(wxFONTWEIGHT_MEDIUM);
		}
		return wxFont(info);
	}
}
