#include "tt_frame.h"
#include "tt_icon.h"
#include <wx/sizer.h>
#include <wx/bmpbndl.h>
#include <wx/iconbndl.h>

namespace {

constexpr int ID_INPUT       = wxID_HIGHEST + 1;
constexpr int ID_ADD         = wxID_HIGHEST + 2;
constexpr int ID_TASKLIST    = wxID_HIGHEST + 3;
constexpr int ID_TOGGLE      = wxID_HIGHEST + 4;
constexpr int ID_RANGE_PREV  = wxID_HIGHEST + 5;
constexpr int ID_RANGE_NEXT  = wxID_HIGHEST + 6;
constexpr int ID_SUMMARYLIST = wxID_HIGHEST + 7;

constexpr int kTickerMs = 1000;  // 1 Hz; FormatDuration is second-resolution

}  // namespace

TTFrame::TTFrame()
    : wxFrame(nullptr, wxID_ANY, "Time Tracker",
              wxDefaultPosition, wxSize(253, 560)),
      ticker_(this) {

    {
        auto bb = wxBitmapBundle::FromSVG(
            reinterpret_cast<const wxByte*>(kTTIconSVG),
            sizeof(kTTIconSVG) - 1, wxSize(256, 256));
        wxIconBundle icons;
        for (int sz : {16, 24, 32, 48, 64, 128, 256}) {
            wxIcon ic;
            ic.CopyFromBitmap(bb.GetBitmap(wxSize(sz, sz)));
            icons.AddIcon(ic);
        }
        SetIcons(icons);
    }

    LoadState(state_);
    if (state_.tasks.empty()) {
        for (const char* n : {"Development", "Code Review", "Meetings", "Planning"}) {
            Task t;
            t.id = GenerateUuid();
            t.name = n;
            state_.tasks.push_back(std::move(t));
        }
        SaveState(state_);
    }

    BuildLayout();
    RefreshTaskList();
    RefreshSummary();
    RefreshToggleButton();

    Bind(wxEVT_TEXT_ENTER, &TTFrame::OnInputEnter, this, ID_INPUT);
    Bind(wxEVT_BUTTON,     &TTFrame::OnAdd,        this, ID_ADD);
    Bind(wxEVT_BUTTON,     &TTFrame::OnToggle,     this, ID_TOGGLE);
    Bind(wxEVT_BUTTON,     &TTFrame::OnRangePrev,  this, ID_RANGE_PREV);
    Bind(wxEVT_BUTTON,     &TTFrame::OnRangeNext,  this, ID_RANGE_NEXT);
    Bind(wxEVT_DATAVIEW_ITEM_ACTIVATED, &TTFrame::OnTaskActivated, this, ID_TASKLIST);
    Bind(wxEVT_DATAVIEW_SELECTION_CHANGED,
         [this](wxDataViewEvent&) { RefreshToggleButton(); }, ID_TASKLIST);
    Bind(wxEVT_CLOSE_WINDOW, &TTFrame::OnClose, this);
    Bind(wxEVT_TIMER,      &TTFrame::OnTick,       this);
    taskList_->Bind(wxEVT_KEY_DOWN, &TTFrame::OnTaskKey, this);

    taskList_->Bind(wxEVT_SIZE, [this](wxSizeEvent& e) {
        AutoFitColumns(taskList_); e.Skip();
    });
    summaryList_->Bind(wxEVT_SIZE, [this](wxSizeEvent& e) {
        AutoFitColumns(summaryList_); e.Skip();
    });

    if (taskList_->GetItemCount() > 0) {
        int sel = (state_.activeTask >= 0) ? state_.activeTask : 0;
        taskList_->SelectRow(sel);
    }
    RefreshToggleButton();

    ticker_.Start(kTickerMs);
    SetMinSize(wxSize(280, 480));
    input_->SetFocus();

    // After the frame has settled at its final size, fit columns once more —
    // initial wxEVT_SIZE events fire before the lists have their real width.
    CallAfter([this] {
        AutoFitColumns(taskList_);
        AutoFitColumns(summaryList_);
    });
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
    taskList_ = new wxDataViewListCtrl(panel, ID_TASKLIST,
                                       wxDefaultPosition, wxDefaultSize,
                                       wxDV_SINGLE | wxDV_ROW_LINES);
    taskList_->AppendTextColumn("Task", wxDATAVIEW_CELL_INERT,
        FromDIP(320), wxALIGN_LEFT);
    taskList_->AppendTextColumn("Time", wxDATAVIEW_CELL_INERT,
        FromDIP(110), wxALIGN_RIGHT);

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

    summaryList_ = new wxDataViewListCtrl(panel, ID_SUMMARYLIST,
                                          wxDefaultPosition, FromDIP(wxSize(-1, 160)),
                                          wxDV_SINGLE | wxDV_ROW_LINES | wxDV_NO_HEADER);
    summaryList_->AppendTextColumn("Task", wxDATAVIEW_CELL_INERT,
        FromDIP(320), wxALIGN_LEFT);
    summaryList_->AppendTextColumn("Time", wxDATAVIEW_CELL_INERT,
        FromDIP(110), wxALIGN_RIGHT);

    constexpr int kPad = 8;
    outer->Add(inputRow,     0, wxEXPAND | wxALL, kPad);
    outer->Add(taskList_,    1, wxEXPAND | wxLEFT | wxRIGHT, kPad);
    outer->Add(toggleBtn_,   0, wxEXPAND | wxALL, kPad);
    outer->Add(sumHeader,    0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, kPad);
    outer->Add(totalLabel_,  0, wxLEFT | wxRIGHT | wxBOTTOM, kPad);
    outer->Add(summaryList_, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, kPad);

    panel->SetSizer(outer);
}

void TTFrame::AutoFitColumns(wxDataViewListCtrl* list) {
    if (!list) return;
    auto* taskCol = list->GetColumn(0);
    auto* timeCol = list->GetColumn(1);
    if (!taskCol || !timeCol) return;
    int total = list->GetClientSize().GetWidth();
    if (total <= 0) return;
    // Reserve space for the vertical scrollbar so no horizontal scrollbar appears.
    total -= wxSystemSettings::GetMetric(wxSYS_VSCROLL_X, list);
    int timeW = total / 3;
    int taskW = total - timeW;
    if (taskW < FromDIP(80)) taskW = FromDIP(80);
    taskCol->SetWidth(taskW);
    timeCol->SetWidth(timeW);
}

int TTFrame::GetSelectedTask() const {
    int sel = taskList_->GetSelectedRow();
    if (sel < 0 || sel >= (int)state_.tasks.size()) return -1;
    return sel;
}

void TTFrame::Save() {
    if (!SaveState(state_)) {
        std::fprintf(stderr, "Failed to save state\n");
    }
}

void TTFrame::RefreshTaskList() {
    int prevSel = taskList_->GetSelectedRow();
    taskList_->DeleteAllItems();
    std::time_t now = std::time(nullptr);
    for (size_t i = 0; i < state_.tasks.size(); ++i) {
        const bool active = state_.tasks[i].active;
        wxString name = wxString::FromUTF8(state_.tasks[i].name);
        if (active) name.Prepend(wxString::FromUTF8("\xE2\x96\xB6 "));  // ▶
        wxVector<wxVariant> row;
        row.push_back(wxVariant(name));
        row.push_back(wxVariant(wxString::FromUTF8(
            FormatDuration(GetTaskTime(state_, (int)i, range_, now)))));
        taskList_->AppendItem(row);
    }
    int count = taskList_->GetItemCount();
    if (prevSel >= 0 && prevSel < count) {
        taskList_->SelectRow(prevSel);
    } else if (count > 0) {
        taskList_->SelectRow(0);
    }
}

void TTFrame::RefreshTimes() {
    std::time_t now = std::time(nullptr);
    for (size_t i = 0; i < state_.tasks.size(); ++i) {
        taskList_->SetValue(wxVariant(wxString::FromUTF8(
            FormatDuration(GetTaskTime(state_, (int)i, range_, now)))),
            (unsigned)i, 1);
    }
    RefreshSummary();
}

void TTFrame::RefreshSummary() {
    std::time_t now = std::time(nullptr);
    rangeLabel_->SetLabel(RangeHeader(range_));
    double total = 0;
    summaryList_->DeleteAllItems();
    for (size_t i = 0; i < state_.tasks.size(); ++i) {
        double t = GetTaskTime(state_, (int)i, range_, now);
        total += t;
        wxVector<wxVariant> row;
        row.push_back(wxVariant(wxString::FromUTF8(state_.tasks[i].name)));
        row.push_back(wxVariant(wxString::FromUTF8(FormatDuration(t))));
        summaryList_->AppendItem(row);
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
    int last = taskList_->GetItemCount() - 1;
    if (last >= 0) {
        taskList_->SelectRow(last);
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

void TTFrame::OnTaskActivated(wxDataViewEvent&) {
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
}

void TTFrame::OnClose(wxCloseEvent& evt) {
    Save();
    evt.Skip();
}
