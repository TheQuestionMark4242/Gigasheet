#include "formula_autocomplete.hpp"

#include <algorithm>
#include <cctype>

#include <wx/listbox.h>
#include <wx/popupwin.h>

#include "../parser/expr_eval.hpp"

namespace formulahint {

wxString FormulaName(const wxString& label) {
    const std::string s = label.ToStdString();
    bool identifier = !s.empty() && !std::isdigit(static_cast<unsigned char>(s[0]));
    for (const char ch : s) {
        if (!std::isalnum(static_cast<unsigned char>(ch)) && ch != '_') {
            identifier = false;
            break;
        }
    }
    return identifier ? label : "\"" + label + "\"";
}

std::vector<Candidate> BuildFormulaCandidates(const wxArrayString& columnLabels) {
    std::vector<Candidate> out;
    for (const std::string& name : exprparse::FunctionNames()) {
        const wxString display(name);
        out.push_back({display, display + "("});
    }
    for (const wxString& label : columnLabels) {
        if (!label.empty()) {
            out.push_back({label, FormulaName(label)});
        }
    }
    return out;
}

namespace {

bool IsTokenChar(wxUniChar c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '_';
}

} // namespace

MatchResult MatchToken(const wxString& text, long caret,
                       const std::vector<Candidate>& candidates)
{
    MatchResult r;
    if (caret < 0 || caret > static_cast<long>(text.length())) {
        return r;
    }
    long start = caret;
    while (start > 0 && IsTokenChar(text[start - 1])) {
        --start;
    }
    r.tokenStart = start;
    // Empty token or a number like the "1" in "=1": no suggestions.
    if (start == caret || (text[start] >= '0' && text[start] <= '9')) {
        return r;
    }
    const wxString token = text.Mid(start, caret - start).Upper();
    for (const Candidate& c : candidates) {
        if (c.display.Upper().StartsWith(token)) {
            r.matches.push_back(c);
        }
    }
    return r;
}

} // namespace formulahint

FormulaAutocomplete::FormulaAutocomplete(
    wxTextCtrl* ctrlPtr, std::vector<formulahint::Candidate> candidatesIn)
    : ctrl(ctrlPtr), candidates(std::move(candidatesIn))
{
    ctrl->Bind(wxEVT_TEXT, &FormulaAutocomplete::OnText, this);
    ctrl->Bind(wxEVT_KEY_DOWN, &FormulaAutocomplete::OnKeyDown, this);
    ctrl->Bind(wxEVT_KILL_FOCUS, &FormulaAutocomplete::OnKillFocus, this);
}

void FormulaAutocomplete::OnText(wxCommandEvent& event) {
    event.Skip();
    if (!inserting) {
        RefreshPopup();
    }
}

void FormulaAutocomplete::RefreshPopup() {
    const auto match = formulahint::MatchToken(
        ctrl->GetValue(), ctrl->GetInsertionPoint(), candidates);
    if (match.matches.empty()) {
        HidePopup();
        return;
    }
    tokenStart = match.tokenStart;
    current = match.matches;

    if (!popup) {
        popup = new wxPopupWindow(ctrl->GetParent(), wxBORDER_SIMPLE);
        list = new wxListBox(popup, wxID_ANY);
        // Accept on mouse-down, before the listbox can steal focus from
        // the text control (which would hide the popup first).
        list->Bind(wxEVT_LEFT_DOWN, &FormulaAutocomplete::OnListClick, this);
    }

    wxArrayString items;
    for (const auto& c : current) {
        items.Add(c.display);
    }
    list->Set(items);
    list->SetSelection(0);

    const int rows = std::min<int>(8, static_cast<int>(items.size()));
    const int rowHeight = list->GetCharHeight() + 4;
    const wxSize size(ctrl->GetSize().x, rows * rowHeight + 6);
    list->SetSize(size);
    popup->SetClientSize(size);
    popup->SetPosition(ctrl->ClientToScreen(wxPoint(0, ctrl->GetSize().y)));
    popup->Show();
}

void FormulaAutocomplete::HidePopup() {
    if (popup && popup->IsShown()) {
        popup->Hide();
    }
    current.clear();
}

void FormulaAutocomplete::MoveSelection(int delta) {
    const int count = static_cast<int>(list->GetCount());
    int sel = list->GetSelection();
    sel = (sel == wxNOT_FOUND) ? 0 : sel + delta;
    sel = std::clamp(sel, 0, count - 1);
    list->SetSelection(sel);
}

void FormulaAutocomplete::Accept() {
    int sel = list->GetSelection();
    if (sel == wxNOT_FOUND) {
        sel = 0;
    }
    const wxString& insert = current[sel].insert;

    inserting = true;
    ctrl->Replace(tokenStart, ctrl->GetInsertionPoint(), insert);
    ctrl->SetInsertionPoint(tokenStart + static_cast<long>(insert.length()));
    inserting = false;
    HidePopup();
}

void FormulaAutocomplete::OnKeyDown(wxKeyEvent& event) {
    if (!popup || !popup->IsShown()) {
        event.Skip();
        return;
    }
    switch (event.GetKeyCode()) {
        case WXK_DOWN:
            MoveSelection(1);
            return;
        case WXK_UP:
            MoveSelection(-1);
            return;
        case WXK_RETURN:
        case WXK_NUMPAD_ENTER:
        case WXK_TAB:
            Accept();
            return;
        case WXK_ESCAPE:
            HidePopup();
            return;
        default:
            event.Skip();
            return;
    }
}

void FormulaAutocomplete::OnKillFocus(wxFocusEvent& event) {
    event.Skip();
    HidePopup();
}

void FormulaAutocomplete::OnListClick(wxMouseEvent& event) {
    const int item = list->HitTest(event.GetPosition());
    if (item != wxNOT_FOUND) {
        list->SetSelection(item);
        Accept();
    }
}
