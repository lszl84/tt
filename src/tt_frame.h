#pragma once
#include <wx/wx.h>
#include <wx/dataview.h>
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
    wxDataViewListCtrl* taskList_    = nullptr;
    wxButton*    toggleBtn_     = nullptr;
    wxStaticText* totalLabel_   = nullptr;
    wxStaticText* rangeLabel_   = nullptr;
    wxButton*    rangePrevBtn_  = nullptr;
    wxButton*    rangeNextBtn_  = nullptr;
    wxDataViewListCtrl* summaryList_ = nullptr;

    wxTimer ticker_;

    void BuildLayout();
    void RefreshTaskList();
    void RefreshTimes();
    void RefreshSummary();
    void RefreshToggleButton();
    void AutoFitColumns(wxDataViewListCtrl* list);

    int GetSelectedTask() const;
    void Save();

    void OnAdd(wxCommandEvent&);
    void OnInputEnter(wxCommandEvent&);
    void OnToggle(wxCommandEvent&);
    void OnTaskActivated(wxDataViewEvent&);
    void OnRangePrev(wxCommandEvent&);
    void OnRangeNext(wxCommandEvent&);
    void OnTaskKey(wxKeyEvent&);
    void OnTick(wxTimerEvent&);
    void OnClose(wxCloseEvent&);
};
