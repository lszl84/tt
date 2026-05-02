#include "tt_frame.h"
#include <wx/sizer.h>
#include <wx/statline.h>

namespace {

constexpr int ID_INPUT      = wxID_HIGHEST + 1;
constexpr int ID_ADD        = wxID_HIGHEST + 2;
constexpr int ID_TASKLIST   = wxID_HIGHEST + 3;
constexpr int ID_TOGGLE     = wxID_HIGHEST + 4;
constexpr int ID_RANGE_PREV = wxID_HIGHEST + 5;
constexpr int ID_RANGE_NEXT = wxID_HIGHEST + 6;

constexpr int kTickerMs = 1000;  // 1 Hz; FormatDuration is second-resolution

const wxColour kActiveBg(70, 145, 90);   // green tint for the active row

}  // namespace

TTFrame::TTFrame()
    : wxFrame(nullptr, wxID_ANY, "Time Tracker",
              wxDefaultPosition, wxSize(560, 640)),
      ticker_(this) {

    LoadState(state_);

    BuildLayout();
    RefreshTaskList();
    RefreshSummary();
    RefreshToggleButton();

    Bind(wxEVT_TEXT_ENTER, &TTFrame::OnInputEnter, this, ID_INPUT);
    Bind(wxEVT_BUTTON,     &TTFrame::OnAdd,        this, ID_ADD);
    Bind(wxEVT_BUTTON,     &TTFrame::OnToggle,     this, ID_TOGGLE);
    Bind(wxEVT_BUTTON,     &TTFrame::OnRangePrev,  this, ID_RANGE_PREV);
    Bind(wxEVT_BUTTON,     &TTFrame::OnRangeNext,  this, ID_RANGE_NEXT);
    Bind(wxEVT_LIST_ITEM_ACTIVATED, &TTFrame::OnTaskActivated, this, ID_TASKLIST);
    Bind(wxEVT_LIST_ITEM_SELECTED,
         [this](wxListEvent&) { RefreshToggleButton(); }, ID_TASKLIST);
    Bind(wxEVT_CLOSE_WINDOW, &TTFrame::OnClose, this);
    Bind(wxEVT_TIMER,      &TTFrame::OnTick,       this);
    taskList_->Bind(wxEVT_KEY_DOWN, &TTFrame::OnTaskKey, this);

    if (!state_.tasks.empty()) {
        taskList_->Select(0);
        taskList_->Focus(0);
    }

    ticker_.Start(kTickerMs);
    SetMinSize(wxSize(420, 480));
    input_->SetFocus();
}

TTFrame::~TTFrame() {
    ticker_.Stop();
}

void TTFrame::BuildLayout() {
    auto* panel = new wxPanel(this);
    auto* outer = new wxBoxSizer(wxVERTICAL);

    // ---- Input row ----
    auto* inputRow = new wxBoxSizer(wxHORIZONTAL);
    input_ = new wxTextCtrl(panel, ID_INPUT, wxString{},
                            wxDefaultPosition, wxDefaultSize,
                            wxTE_PROCESS_ENTER);
    input_->SetHint("New task name…");
    addBtn_ = new wxButton(panel, ID_ADD, "+ Add");
    inputRow->Add(input_, 1, wxALIGN_CENTER_VERTICAL | wxRIGHT, 6);
    inputRow->Add(addBtn_, 0, wxALIGN_CENTER_VERTICAL);

    // ---- Task list ----
    taskList_ = new wxListView(panel, ID_TASKLIST,
                               wxDefaultPosition, wxDefaultSize,
                               wxLC_REPORT | wxLC_SINGLE_SEL);
    taskList_->AppendColumn("Task", wxLIST_FORMAT_LEFT, FromDIP(320));
    taskList_->AppendColumn("Time", wxLIST_FORMAT_RIGHT, FromDIP(120));

    // ---- Toggle button ----
    toggleBtn_ = new wxButton(panel, ID_TOGGLE, "Start");

    // ---- Summary header / range selector ----
    auto* sumHeader = new wxBoxSizer(wxHORIZONTAL);
    rangeLabel_ = new wxStaticText(panel, wxID_ANY, RangeHeader(range_));
    auto headerFont = rangeLabel_->GetFont();
    headerFont.MakeBold();
    rangeLabel_->SetFont(headerFont);
    rangePrevBtn_ = new wxButton(panel, ID_RANGE_PREV, "<",
                                 wxDefaultPosition, FromDIP(wxSize(36, -1)));
    rangeNextBtn_ = new wxButton(panel, ID_RANGE_NEXT, ">",
                                 wxDefaultPosition, FromDIP(wxSize(36, -1)));
    sumHeader->Add(rangeLabel_, 1, wxALIGN_CENTER_VERTICAL);
    sumHeader->Add(rangePrevBtn_, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 4);
    sumHeader->Add(rangeNextBtn_, 0, wxALIGN_CENTER_VERTICAL);

    totalLabel_ = new wxStaticText(panel, wxID_ANY, "Total: 0s");

    summaryList_ = new wxListView(panel, wxID_ANY,
                                  wxDefaultPosition, FromDIP(wxSize(-1, 160)),
                                  wxLC_REPORT | wxLC_NO_HEADER | wxLC_SINGLE_SEL);
    summaryList_->AppendColumn("Task", wxLIST_FORMAT_LEFT, FromDIP(320));
    summaryList_->AppendColumn("Time", wxLIST_FORMAT_RIGHT, FromDIP(120));

    constexpr int kPad = 8;
    outer->Add(inputRow,    0, wxEXPAND | wxALL, kPad);
    outer->Add(taskList_,   1, wxEXPAND | wxLEFT | wxRIGHT, kPad);
    outer->Add(toggleBtn_,  0, wxEXPAND | wxALL, kPad);
    outer->Add(new wxStaticLine(panel), 0, wxEXPAND | wxLEFT | wxRIGHT, kPad);
    outer->Add(sumHeader,   0, wxEXPAND | wxALL, kPad);
    outer->Add(totalLabel_, 0, wxLEFT | wxRIGHT | wxBOTTOM, kPad);
    outer->Add(summaryList_, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, kPad);

    panel->SetSizer(outer);
}

int TTFrame::GetSelectedTask() const {
    long sel = taskList_->GetFirstSelected();
    if (sel < 0 || sel >= (long)state_.tasks.size()) return -1;
    return (int)sel;
}

void TTFrame::Save() {
    if (!SaveState(state_)) {
        std::fprintf(stderr, "Failed to save state\n");
    }
}

void TTFrame::RefreshTaskList() {
    long prevSel = taskList_->GetFirstSelected();
    taskList_->DeleteAllItems();
    auto now = std::chrono::steady_clock::now();
    for (size_t i = 0; i < state_.tasks.size(); ++i) {
        long row = taskList_->InsertItem((long)i, wxString::FromUTF8(state_.tasks[i].name));
        taskList_->SetItem(row, 1,
            wxString::FromUTF8(FormatDuration(GetTaskTime(state_, (int)i, range_, now))));
        if (state_.tasks[i].active) {
            taskList_->SetItemBackgroundColour(row, kActiveBg);
            taskList_->SetItemTextColour(row, *wxWHITE);
        }
    }
    if (prevSel >= 0 && prevSel < taskList_->GetItemCount()) {
        taskList_->Select(prevSel);
        taskList_->Focus(prevSel);
    } else if (taskList_->GetItemCount() > 0) {
        taskList_->Select(0);
        taskList_->Focus(0);
    }
}

void TTFrame::RefreshTimes() {
    auto now = std::chrono::steady_clock::now();
    for (size_t i = 0; i < state_.tasks.size(); ++i) {
        taskList_->SetItem((long)i, 1,
            wxString::FromUTF8(FormatDuration(GetTaskTime(state_, (int)i, range_, now))));
    }
    RefreshSummary();
}

void TTFrame::RefreshSummary() {
    auto now = std::chrono::steady_clock::now();
    rangeLabel_->SetLabel(RangeHeader(range_));
    double total = 0;
    summaryList_->DeleteAllItems();
    for (size_t i = 0; i < state_.tasks.size(); ++i) {
        double t = GetTaskTime(state_, (int)i, range_, now);
        total += t;
        long row = summaryList_->InsertItem((long)i,
            wxString::FromUTF8(state_.tasks[i].name));
        summaryList_->SetItem(row, 1, wxString::FromUTF8(FormatDuration(t)));
    }
    totalLabel_->SetLabel(wxString::Format("Total: %s",
        wxString::FromUTF8(FormatDuration(total))));
}

void TTFrame::RefreshToggleButton() {
    int sel = GetSelectedTask();
    bool running = (sel >= 0 && state_.tasks[sel].active);
    toggleBtn_->SetLabel(running ? "Stop" : "Start");
}

void TTFrame::OnAdd(wxCommandEvent&) {
    wxString text = input_->GetValue();
    text.Trim().Trim(false);
    if (text.IsEmpty()) return;
    Task t;
    t.id = GenerateUuid();
    t.name = text.ToStdString(wxConvUTF8);
    state_.tasks.push_back(std::move(t));
    input_->Clear();
    Save();
    RefreshTaskList();
    RefreshSummary();
    // Focus the newly-added task so a subsequent Start hits it.
    long last = taskList_->GetItemCount() - 1;
    if (last >= 0) {
        taskList_->Select(last);
        taskList_->Focus(last);
    }
    RefreshToggleButton();
}

void TTFrame::OnInputEnter(wxCommandEvent& e) {
    OnAdd(e);
}

void TTFrame::OnToggle(wxCommandEvent&) {
    int sel = GetSelectedTask();
    if (sel < 0) return;
    // Stop any other running task first.
    if (state_.activeTask >= 0 && state_.activeTask != sel) {
        StopTask(state_, state_.activeTask);
    }
    if (state_.tasks[sel].active) {
        StopTask(state_, sel);
    } else {
        StartTask(state_, sel);
    }
    Save();
    RefreshTaskList();
    RefreshSummary();
    RefreshToggleButton();
}

void TTFrame::OnTaskActivated(wxListEvent& e) {
    // Double-click / Enter on a task toggles tracking on it.
    wxCommandEvent ev(wxEVT_BUTTON, ID_TOGGLE);
    OnToggle(ev);
}

void TTFrame::OnTaskKey(wxKeyEvent& e) {
    if (e.GetKeyCode() == WXK_SPACE) {
        wxCommandEvent ev(wxEVT_BUTTON, ID_TOGGLE);
        OnToggle(ev);
        return;
    }
    e.Skip();
}

void TTFrame::OnRangePrev(wxCommandEvent&) {
    int n = (int)SummaryRange::Count;
    range_ = static_cast<SummaryRange>(((int)range_ - 1 + n) % n);
    RefreshTimes();
}

void TTFrame::OnRangeNext(wxCommandEvent&) {
    int n = (int)SummaryRange::Count;
    range_ = static_cast<SummaryRange>(((int)range_ + 1) % n);
    RefreshTimes();
}

void TTFrame::OnTick(wxTimerEvent&) {
    if (state_.activeTask < 0) return;
    RefreshTimes();
    // Selection state also reflects which row is "the toggle target", so the
    // button label only needs refresh on selection change. The list raises
    // SELECTED events, but we don't need fine-grained tracking — the toggle
    // button picks up the current selection at click time.
}

void TTFrame::OnClose(wxCloseEvent& evt) {
    Save();
    evt.Skip();
}
