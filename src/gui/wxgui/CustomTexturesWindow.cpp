#include "wxgui/CustomTexturesWindow.h"

#include "wxCemuConfig.h"
#include "wxgui/wxgui.h"
#include "wxgui/wxHelper.h"
#include "wxgui/MainWindow.h"

#include "util/helpers/helpers.h"
#include "config/ActiveSettings.h"
#include "Cafe/CafeSystem.h"
#include "Cafe/HW/Latte/Core/LatteTextureReplace.h"
// LatteAsyncCommands.h declares LatteConst::ShaderType without including it; every other
// includer happens to pull in Renderer.h first, so it has to come in explicitly here
#include "Cafe/HW/Latte/Core/LatteConst.h"
#include "Cafe/HW/Latte/Core/LatteAsyncCommands.h"

#include <wx/checkbox.h>
#include <wx/checklst.h>
#include <wx/sizer.h>
#include <wx/stattext.h>
#include <wx/statline.h>
#include <wx/button.h>
#include <wx/settings.h>

#include <algorithm>

enum
{
	ID_CUSTOMTEX_ENABLE = wxID_HIGHEST + 1,
	ID_CUSTOMTEX_PACKLIST,
	ID_CUSTOMTEX_RESCAN,
	ID_CUSTOMTEX_OPENFOLDER,
};

CustomTexturesWindow::CustomTexturesWindow(wxWindow* parent, uint64 titleId, const wxString& gameName)
	: wxDialog(parent, wxID_ANY, _("Custom textures"), wxDefaultPosition, wxSize(460, 420), wxCLOSE_BOX | wxCLIP_CHILDREN | wxCAPTION | wxRESIZE_BORDER),
	  m_titleId(titleId)
{
	auto* sizer = new wxBoxSizer(wxVERTICAL);

	auto* heading = new wxStaticText(this, wxID_ANY, gameName);
	wxFont headingFont = heading->GetFont();
	headingFont.MakeBold();
	heading->SetFont(headingFont);
	sizer->Add(heading, 0, wxALL, 10);

	sizer->Add(new wxStaticText(this, wxID_ANY, wxString::Format("%s: load/textures/%016llx/", _("Folder"), (unsigned long long)titleId)),
			   0, wxLEFT | wxRIGHT | wxBOTTOM, 10);

	m_enable = new wxCheckBox(this, ID_CUSTOMTEX_ENABLE, _("Enable custom textures for this game"));
	sizer->Add(m_enable, 0, wxLEFT | wxRIGHT | wxBOTTOM, 10);

	sizer->Add(new wxStaticLine(this), 0, wxEXPAND | wxLEFT | wxRIGHT, 10);

	sizer->Add(new wxStaticText(this, wxID_ANY, _("Texture packs (each folder inside the game's texture folder):")),
			   0, wxALL, 10);

	m_packList = new wxCheckListBox(this, ID_CUSTOMTEX_PACKLIST);
	sizer->Add(m_packList, 1, wxEXPAND | wxLEFT | wxRIGHT, 10);

	m_status = new wxStaticText(this, wxID_ANY, wxEmptyString);
	m_status->Wrap(420);
	sizer->Add(m_status, 0, wxEXPAND | wxALL, 10);

	auto* buttonSizer = new wxBoxSizer(wxHORIZONTAL);
	buttonSizer->Add(new wxButton(this, ID_CUSTOMTEX_OPENFOLDER, _("Open folder")), 0, wxRIGHT, 5);
	buttonSizer->Add(new wxButton(this, ID_CUSTOMTEX_RESCAN, _("Rescan")), 0, wxRIGHT, 5);
	buttonSizer->AddStretchSpacer();
	buttonSizer->Add(new wxButton(this, wxID_CLOSE, _("Close")), 0);
	sizer->Add(buttonSizer, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 10);

	SetSizer(sizer);

	Bind(wxEVT_CHECKBOX, &CustomTexturesWindow::OnEnableToggled, this, ID_CUSTOMTEX_ENABLE);
	Bind(wxEVT_CHECKLISTBOX, &CustomTexturesWindow::OnPackToggled, this, ID_CUSTOMTEX_PACKLIST);
	Bind(wxEVT_BUTTON, &CustomTexturesWindow::OnRescan, this, ID_CUSTOMTEX_RESCAN);
	Bind(wxEVT_BUTTON, &CustomTexturesWindow::OnOpenFolder, this, ID_CUSTOMTEX_OPENFOLDER);
	Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { Close(); }, wxID_CLOSE);
	// shown non-modally from the game list, so it has to clean itself up
	Bind(wxEVT_CLOSE_WINDOW, [this](wxCloseEvent&) { if (IsModal()) EndModal(wxID_CLOSE); else Destroy(); });

	RefreshPackList();
}

// Fill the list from disk and tick whatever the config says. A title that has never been
// configured gets everything ticked, which matches what the texture module does by default.
void CustomTexturesWindow::RefreshPackList()
{
	auto& guiConfig = GetWxGUIConfig();
	const auto configured = guiConfig.custom_textures.find(m_titleId);
	const bool isConfigured = (configured != guiConfig.custom_textures.end());

	m_enable->SetValue(isConfigured ? configured->second.enabled : true);

	m_packs = LatteTextureReplace::ListPacks(m_titleId);
	m_packList->Clear();
	for (const auto& pack : m_packs)
		m_packList->Append(wxString::FromUTF8(pack.c_str()));

	for (size_t i = 0; i < m_packs.size(); i++)
	{
		bool ticked = true;
		if (isConfigured)
		{
			const auto& enabledPacks = configured->second.packs;
			ticked = std::find(enabledPacks.begin(), enabledPacks.end(), m_packs[i]) != enabledPacks.end();
		}
		m_packList->Check((unsigned int)i, ticked);
	}

	m_packList->Enable(m_enable->GetValue());
	UpdateStatus();
}

std::vector<std::string> CustomTexturesWindow::GetTickedPacks() const
{
	std::vector<std::string> ticked;
	for (size_t i = 0; i < m_packs.size(); i++)
	{
		if (m_packList->IsChecked((unsigned int)i))
			ticked.push_back(m_packs[i]);
	}
	return ticked;
}

// Settings apply immediately, like the graphic packs window. If this title is the one currently
// running, the textures are reloaded so the change is visible without a restart.
void CustomTexturesWindow::ApplyAndSave()
{
	auto& guiConfig = GetWxGUIConfig();
	auto& settings = guiConfig.custom_textures[m_titleId];
	settings.enabled = m_enable->GetValue();
	settings.packs = GetTickedPacks();
	g_wxConfig.Save();

	LatteTextureReplace::SetTitleSettings(m_titleId, settings.enabled, settings.packs);

	if (g_mainFrame && g_mainFrame->IsGameLaunched() && CafeSystem::GetForegroundTitleId() == m_titleId)
		LatteAsyncCommands_queueReloadTextures();

	UpdateStatus();
}

void CustomTexturesWindow::UpdateStatus()
{
	if (m_packs.empty())
	{
		m_status->SetLabel(_("No packs found. Create a folder inside the game's texture folder and put the .dds files in it."));
		m_status->Wrap(GetClientSize().GetWidth() - 20);
		Layout();
		return;
	}

	const auto ticked = GetTickedPacks();
	const auto conflicts = LatteTextureReplace::FindPackConflicts(m_titleId, ticked);
	if (conflicts.empty())
	{
		m_status->SetForegroundColour(wxSystemSettings::GetColour(wxSYS_COLOUR_WINDOWTEXT));
		m_status->SetLabel(wxString::Format(_("%d of %d packs enabled. No overlapping textures."), (int)ticked.size(), (int)m_packs.size()));
	}
	else
	{
		// The index keeps the alphabetically first pack for a texture two packs both provide.
		wxString label = wxString::Format(_("Warning: %d texture(s) are provided by more than one enabled pack. The pack that comes first alphabetically will be used for those."),
										  (int)conflicts.size());
		for (size_t i = 0; i < conflicts.size() && i < 3; i++)
			label += "\n" + wxString::FromUTF8(conflicts[i].c_str());
		if (conflicts.size() > 3)
			label += "\n...";
		m_status->SetForegroundColour(*wxRED);
		m_status->SetLabel(label);
	}
	m_status->Wrap(GetClientSize().GetWidth() - 20);
	Layout();
}

void CustomTexturesWindow::OnEnableToggled(wxCommandEvent& event)
{
	m_packList->Enable(m_enable->GetValue());
	ApplyAndSave();
}

void CustomTexturesWindow::OnPackToggled(wxCommandEvent& event)
{
	ApplyAndSave();
}

void CustomTexturesWindow::OnRescan(wxCommandEvent& event)
{
	// keep whatever is ticked now, then re-read the folder so newly added packs show up
	ApplyAndSave();
	RefreshPackList();
}

void CustomTexturesWindow::OnOpenFolder(wxCommandEvent& event)
{
	const fs::path folder = LatteTextureReplace::GetTitleFolder(m_titleId);
	std::error_code ec;
	fs::create_directories(folder, ec);
	wxLaunchDefaultApplication(wxHelper::FromPath(folder));
}
