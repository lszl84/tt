#pragma once
#include <wx/wx.h>
#include <wx/listctrl.h>
#include <wx/timer.h>
#include "data.h"

class TTFrame : public wxFrame {
public:
    TTFrame();
    ~TTFrame() override;

private:
    TTState state_;
    SummaryRange range_ = SummaryRange::Today;

    wxTextCtrl*  input_         = nullptr;
    wxButton*    addBtn_        = nullptr;
    wxListView*  taskList_      = nullptr;
    wxButton*    toggleBtn_     = nullptr;
    wxStaticText* totalLabel_   = nullptr;
    wxStaticText* rangeLabel_   = nullptr;
    wxButton*    rangePrevBtn_  = nullptr;
    wxButton*    rangeNextBtn_  = nullptr;
    wxListView*  summaryList_   = nullptr;

    wxTimer ticker_;

    void BuildLayout();
    void RefreshTaskList();
    void RefreshTimes();
    void RefreshSummary();
    void RefreshToggleButton();

    int GetSelectedTask() const;
    void Save();

    void OnAdd(wxCommandEvent&);
    void OnInputEnter(wxCommandEvent&);
    void OnToggle(wxCommandEvent&);
    void OnTaskActivated(wxListEvent&);
    void OnRangePrev(wxCommandEvent&);
    void OnRangeNext(wxCommandEvent&);
    void OnTaskKey(wxKeyEvent&);
    void OnTick(wxTimerEvent&);
    void OnClose(wxCloseEvent&);
};
