#pragma once

#include <wx/dialog.h>

#include <string>
#include <vector>

class wxCheckBox;
class wxCheckListBox;
class wxStaticText;

// Per-title custom texture settings. Opened from the game list context menu, so it works whether
// or not the game is running. Packs are the folders inside load/textures/<titleId>/.
class CustomTexturesWindow : public wxDialog
{
public:
	CustomTexturesWindow(wxWindow* parent, uint64 titleId, const wxString& gameName);

private:
	void RefreshPackList();
	void ApplyAndSave();
	void UpdateStatus();

	void OnEnableToggled(wxCommandEvent& event);
	void OnPackToggled(wxCommandEvent& event);
	void OnRescan(wxCommandEvent& event);
	void OnOpenFolder(wxCommandEvent& event);

	std::vector<std::string> GetTickedPacks() const;

	uint64 m_titleId;
	wxCheckBox* m_enable{};
	wxCheckListBox* m_packList{};
	wxStaticText* m_status{};
	std::vector<std::string> m_packs; // list-box order; parallel to the control's items
};
