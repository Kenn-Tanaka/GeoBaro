#pragma once
#include <wx/wx.h>
#include <wx/listctrl.h>
#include <wx/datectrl.h>
#include <wx/dateevt.h>
#include <wx/spinctrl.h>
#include <wx/timer.h>
#include <wx/eventfilter.h>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <cstdint>
#include <vector>
#include <string>
#include "gsi_service.h"
#include "jma_service.h"
#include "map_panel.h"
#include "preset_store.h"

class MainFrame : public wxFrame, public wxEventFilter {
public:
    MainFrame(const wxString& title);
    ~MainFrame() override;

    int FilterEvent(wxEvent& event) override;

private:
    enum class JobKind {
        None,
        FullSearch,
        FineTune,
        WeatherOnly,
        TileOnly
    };

    struct WorkerJob {
        uint64_t seq = 0;
        JobKind kind = JobKind::None;
        std::string input_location;
        bool has_direct_coords = false;
        double lat = 0.0;
        double lon = 0.0;
        double fixed_elevation_m = 0.0;

        std::string base_input_text;
        std::string base_address;
        double base_lat = 0.0;
        double base_lon = 0.0;
        bool is_fine_tuned = false;
        bool is_reset = false;
        bool from_preset = false;

        int year = 0;
        int month = 0;
        int day = 0;
        double height_agl_m = 0.0;
        int zoom = 15;
        std::string layer = "std";
        int map_w = 320;
        int map_h = 320;

        bool have_prev_station = false;
        bool can_reuse_spatial_neighbors = false;
        std::string prev_prec_no;
        std::string prev_block_no;
        int prev_year = 0;
        int prev_month = 0;
        int prev_day = 0;
    };

    struct WorkerResult {
        uint64_t seq = 0;
        JobKind kind = JobKind::None;
        bool elev_failed = false;
        bool reuse_existing_jma = false;
        double new_distance_km = 0.0;
        int year = 0;
        int month = 0;
        int day = 0;

        LocationInfo loc;
        TileInfo tile;
        JmaDailyResult jma_res;
        double height_agl_m = 0.0;

        std::string base_input_text;
        std::string base_address;
        double base_lat = 0.0;
        double base_lon = 0.0;
        bool is_fine_tuned = false;
        bool is_reset = false;
        bool from_preset = false;
    };

    void InitUi();
    void OnSearch(wxCommandEvent& event);
    void OnLocationTextChanged(wxCommandEvent& event);
    void OnLayerChanged(wxCommandEvent& event);
    void OnHeightAglChanged(wxSpinDoubleEvent& event);
    void OnHeightAglEnter(wxCommandEvent& event);
    void OnDateChanged(wxDateEvent& event);
    void OnDatePrev(wxCommandEvent& event);
    void OnDateToday(wxCommandEvent& event);
    void OnDateNext(wxCommandEvent& event);
    void OnCopyCoords(wxCommandEvent& event);
    void OnZoomOut(wxCommandEvent& event);
    void OnZoomIn(wxCommandEvent& event);
    void OnZoomTimer(wxTimerEvent& event);
    void OnResetPosition(wxCommandEvent& event);
    void OnPresetSelected(wxCommandEvent& event);
    void OnAddPreset(wxCommandEvent& event);
    void OnDeletePreset(wxCommandEvent& event);
    void OnPresetContextMenu(wxContextMenuEvent& event);
    void OnCopyPresetDisplayText(wxCommandEvent& event);
    void OnCopyPresetAddress(wxCommandEvent& event);
    void OnExportCsv(wxCommandEvent& event);
    void OnCopyList(wxCommandEvent& event);
    void OnListContextMenu(wxContextMenuEvent& event);
    void OnListKeyDown(wxKeyEvent& event);
    void OnListItemActivated(wxListEvent& event);
    void OnCopySelectedRow(wxCommandEvent& event);
    void OnCopySelectedTargetPressure(wxCommandEvent& event);
    void OnClose(wxCloseEvent& event);

    void OnMapLocationMoved(double new_lat, double new_lon);
    void AdjustZoom(int delta);
    void UpdateZoomControls();
    std::string GetSelectedLayerCode() const;
    wxSize GetCurrentMapSize() const;

    void StartWeatherOnlySearch();
    void ShiftTargetDate(int delta_days);
    void UpdateDateNavButtons();
    void SetSearchBusy(bool busy);
    double ReadSpinHeightAgl(bool commit_text);
    void ApplyHeightAglValue(double new_height);

    void AttachPrevStationSnapshot(WorkerJob& job, double new_lat, double new_lon) const;
    void EnqueueJob(WorkerJob job);
    void WorkerLoop();
    void ApplyWorkerResult(const WorkerResult& res);

    void UpdateUiWithResult(
        const LocationInfo& loc,
        const TileInfo& tile,
        const JmaDailyResult& jma_res,
        double height_agl_m,
        int req_year,
        int req_month,
        int req_day
    );
    void PopulateHourlyList(bool select_latest_on_realtime = false);
    void UpdateElevationLabel();
    void UpdateAddressLabel();
    void UpdateStationLabels();
    void UpdateSpatialSummaryLabel();
    void UpdatePressureSummaryLabel();
    void UpdateStatusBarInfo(const wxString& mainText, bool updateRightPane = true);
    void ClearWeatherSummaryUi();
    void SetExportControlsEnabled(bool enabled);
    bool SyncHeightAglIfNeeded();
    wxString BuildListRowTsv(long row) const;
    bool CopyTextToClipboard(const wxString& text, const wxString& statusMessage);

    wxString GetPresetFilePath() const;
    const PresetLocation* GetSelectedPreset() const;
    void LoadPresets();
    void SavePresets() const;
    void RefreshPresetChoice(int select_preset_index = -1);

    // プリセットコントロール
    wxChoice* m_choicePreset;
    wxButton* m_btnAddPreset;
    wxButton* m_btnDelPreset;
    std::vector<PresetLocation> m_presets;

    // 入力コントロール
    wxTextCtrl* m_txtLocation;
    wxDatePickerCtrl* m_datePicker;
    wxButton* m_btnDatePrev;
    wxButton* m_btnDateToday;
    wxButton* m_btnDateNext;
    wxSpinCtrlDouble* m_spinHeightAgl;
    wxChoice* m_choiceLayer;
    wxButton* m_btnSearch;
    wxButton* m_btnExport;
    wxButton* m_btnCopyList;
    bool m_suppressDateEvent = false;

    // 地図コントロール
    MapPanel* m_mapPanel;
    wxButton* m_btnZoomOut;
    wxButton* m_btnZoomIn;
    wxStaticText* m_lblZoom;
    wxButton* m_btnResetPos;
    wxTimer m_zoomTimer;
    int m_currentZoom = 15;
    int m_wheelAcc = 0;

    // サマリー表示
    wxStaticText* m_lblAddress;
    wxStaticText* m_lblCoords;
    wxButton* m_btnCopyCoords;
    wxStaticText* m_lblElevation;
    wxStaticText* m_lblStation;
    wxStaticText* m_lblDistance;
    wxStaticText* m_lblSpatialSummary;
    wxStaticText* m_lblPressureSummary;

    // 最新の取得結果と元住所保持状態
    wxListCtrl* m_listHourly;
    LocationInfo m_lastLoc;
    JmaDailyResult m_lastJma;
    double m_lastHeightAgl = 0.0;

    std::string m_baseInputText;
    std::string m_baseAddress;
    double m_baseLat = 0.0;
    double m_baseLon = 0.0;
    bool m_isFineTuned = false;
    wxString m_fineTunedCoordText;

    // バックグラウンドワーカースレッド管理
    std::thread m_workerThread;
    std::mutex m_jobMutex;
    std::condition_variable m_jobCv;
    WorkerJob m_pendingJob;
    bool m_hasPendingJob = false;
    bool m_stopWorker = false;
    bool m_dataJobInFlight = false;
    JobKind m_activeDataKind = JobKind::None;
    std::atomic<bool> m_isClosing{false};
    std::atomic<uint64_t> m_requestSeq{0};
    bool m_filterInstalled = false;

    wxDECLARE_EVENT_TABLE();
};
