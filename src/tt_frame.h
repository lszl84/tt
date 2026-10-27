#pragma once
#include <wx/wx.h>
#include <wx/dataview.h>
#include <wx/timer.h>
#include <atomic>
#include "data.h"
#include "macbar.h"

class TTFrame : public wxFrame, private MacBarDelegate {
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
    bool quitting_ = false;

    void BuildLayout();
    void RefreshTaskList();
    void RefreshTimes();
    void RefreshSummary();
    void RefreshToggleButton();
    void AutoFitColumns(wxDataViewListCtrl* list);
    void UpdateStartupStatus();

    int GetSelectedTask() const;
    void Save();

    void TryGitPush();
    void OnGitResult(const wxString& message);

    // macOS menu bar item (src/macbar.mm).
    MacBarSnapshot MacBarState() override;
    void* MacBarNativeWindow() override;
    void MacBarShowWindow() override;
    void MacBarHideWindow() override;
    void MacBarReloadState() override;
    void MacBarQuit() override;

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
