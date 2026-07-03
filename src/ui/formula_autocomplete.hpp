#pragma once

#include <vector>

#include <wx/wx.h>

class wxListBox;
class wxPopupWindow;

namespace formulahint {

// One dropdown entry: what the list shows and what gets inserted into the
// formula (functions insert "NAME(", non-identifier column labels insert
// a quoted "label").
struct Candidate {
    wxString display;
    wxString insert;
};

// Column labels that aren't plain identifiers need "..." quoting to be
// referenced in a formula.
wxString FormulaName(const wxString& label);

// Suggestion list for a formula field: every function (uppercase) plus the
// given column labels.
std::vector<Candidate> BuildFormulaCandidates(const wxArrayString& columnLabels);

// The identifier token ending at the caret and the candidates it prefixes
// (case-insensitive, brute force). No token / no match -> empty matches.
struct MatchResult {
    long tokenStart = 0;   // token is text[tokenStart, caret)
    std::vector<Candidate> matches;
};

MatchResult MatchToken(const wxString& text, long caret,
                       const std::vector<Candidate>& candidates);

} // namespace formulahint

// Suggestion dropdown for a formula wxTextCtrl: typing =S pops up SIN, SQRT,
// SUM, ... under the field. Up/Down select, Tab/Enter accept, Esc dismisses;
// Enter falls through to the dialog when the popup is hidden. Keep the
// object alive as long as the text control (a dialog member works).
class FormulaAutocomplete {
public:
    FormulaAutocomplete(wxTextCtrl* ctrl,
                        std::vector<formulahint::Candidate> candidates);

private:
    void OnText(wxCommandEvent&);
    void OnKeyDown(wxKeyEvent&);
    void OnKillFocus(wxFocusEvent&);
    void OnListClick(wxMouseEvent&);
    void RefreshPopup();
    void HidePopup();
    void Accept();
    void MoveSelection(int delta);

    wxTextCtrl* ctrl;
    std::vector<formulahint::Candidate> candidates;
    std::vector<formulahint::Candidate> current;  // rows shown in the list
    wxPopupWindow* popup = nullptr;               // owned by ctrl's parent
    wxListBox* list = nullptr;
    long tokenStart = 0;
    bool inserting = false;  // suppress OnText while Accept() edits the ctrl
};
