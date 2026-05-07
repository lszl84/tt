#pragma once
#include <wx/wx.h>
#include <wx/dataview.h>
#include <wx/timer.h>
#include <atomic>
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
    wxStatusBar* statusBar_     = nullptr;

    wxTimer ticker_;
    std::atomic<bool> gitPushInProgress_{false};

    void BuildLayout();
    void RefreshTaskList();
    void RefreshTimes();
    void RefreshSummary();
    void RefreshToggleButton();
    void AutoFitColumns(wxDataViewListCtrl* list);
    void UpdateStartupStatus();

    int GetSelectedTask() const;
    void Save();

    bool IsGitRepo(const std::filesystem::path& dir);
    void TryGitPush();
    void OnGitResult(const wxString& message);

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
