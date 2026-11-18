#include "main_frame.h"
#include <wx/filedlg.h>
#include <wx/wfstream.h>
#include <wx/stdpaths.h>
#include <wx/filename.h>
#include <wx/textdlg.h>
#include <wx/clipbrd.h>
#include <wx/dateevt.h>
#include <algorithm>
#include <cmath>
#include <iomanip>
#include <sstream>

enum {
    ID_BTN_SEARCH = wxID_HIGHEST + 1,
    ID_BTN_EXPORT,
    ID_BTN_COPY_LIST,
    ID_CHOICE_LAYER,
    ID_SPIN_HEIGHT_AGL,
    ID_TXT_LOCATION,
    ID_DATE_PICKER,
    ID_BTN_DATE_PREV,
    ID_BTN_DATE_TODAY,
    ID_BTN_DATE_NEXT,
    ID_BTN_COPY_COORDS,
    ID_BTN_ZOOM_OUT,
    ID_BTN_ZOOM_IN,
    ID_BTN_RESET_POS,
    ID_TIMER_ZOOM,
    ID_CHOICE_PRESET,
    ID_BTN_ADD_PRESET,
    ID_BTN_DEL_PRESET,
    ID_MENU_COPY_ROW,
    ID_MENU_COPY_TARGET_PRESS,
    ID_MENU_COPY_PRESET_DISPLAY,
    ID_MENU_COPY_PRESET_ADDRESS
};

wxBEGIN_EVENT_TABLE(MainFrame, wxFrame)
    EVT_BUTTON(ID_BTN_SEARCH, MainFrame::OnSearch)
    EVT_BUTTON(ID_BTN_EXPORT, MainFrame::OnExportCsv)
    EVT_BUTTON(ID_BTN_COPY_LIST, MainFrame::OnCopyList)
    EVT_TEXT(ID_TXT_LOCATION, MainFrame::OnLocationTextChanged)
    EVT_CHOICE(ID_CHOICE_LAYER, MainFrame::OnLayerChanged)
    EVT_SPINCTRLDOUBLE(ID_SPIN_HEIGHT_AGL, MainFrame::OnHeightAglChanged)
    EVT_DATE_CHANGED(ID_DATE_PICKER, MainFrame::OnDateChanged)
    EVT_BUTTON(ID_BTN_DATE_PREV, MainFrame::OnDatePrev)
    EVT_BUTTON(ID_BTN_DATE_TODAY, MainFrame::OnDateToday)
    EVT_BUTTON(ID_BTN_DATE_NEXT, MainFrame::OnDateNext)
    EVT_BUTTON(ID_BTN_COPY_COORDS, MainFrame::OnCopyCoords)
    EVT_BUTTON(ID_BTN_ZOOM_OUT, MainFrame::OnZoomOut)
    EVT_BUTTON(ID_BTN_ZOOM_IN, MainFrame::OnZoomIn)
    EVT_BUTTON(ID_BTN_RESET_POS, MainFrame::OnResetPosition)
    EVT_TIMER(ID_TIMER_ZOOM, MainFrame::OnZoomTimer)
    EVT_CHOICE(ID_CHOICE_PRESET, MainFrame::OnPresetSelected)
    EVT_BUTTON(ID_BTN_ADD_PRESET, MainFrame::OnAddPreset)
    EVT_BUTTON(ID_BTN_DEL_PRESET, MainFrame::OnDeletePreset)
    EVT_MENU(ID_MENU_COPY_ROW, MainFrame::OnCopySelectedRow)
    EVT_MENU(ID_MENU_COPY_TARGET_PRESS, MainFrame::OnCopySelectedTargetPressure)
    EVT_MENU(ID_MENU_COPY_PRESET_DISPLAY, MainFrame::OnCopyPresetDisplayText)
    EVT_MENU(ID_MENU_COPY_PRESET_ADDRESS, MainFrame::OnCopyPresetAddress)
    EVT_CLOSE(MainFrame::OnClose)
wxEND_EVENT_TABLE()

static wxString BuildJmaStatusSuffix(const JmaDailyResult& jma) {
    wxString suffix;
    if (jma.used_fallback_station) {
        suffix += wxT("・代替官署");
    }
    if (jma.has_large_alt_diff) {
        suffix += wxT("・標高差大");
    }
    if (jma.has_chunk_error) {
        suffix += wxT("・一部時間帯取得失敗");
    }
    if (jma.has_missing_pressure) {
        suffix += wxT("・一部気圧欠測");
    }
    const HourlyWeather* spHour = JmaService::FindLatestValidSpatialHour(jma);
    if (spHour && spHour->spatial.valid) {
        suffix += wxString::Format(wxT("・空間補間(%s %zu点)"),
            wxString::FromUTF8(SpatialPressureCalculator::MethodToString(spHour->spatial.method_used)),
            spHour->spatial.stations_used.size());
    }
    return suffix;
}

static std::string CsvEscape(const std::string& field) {
    bool needs_quote = false;
    for (char c : field) {
        if (c == ',' || c == '"' || c == '\n' || c == '\r') {
            needs_quote = true;
            break;
        }
    }
    if (!needs_quote) return field;

    std::string out = "\"";
    for (char c : field) {
        if (c == '"') out += "\"\"";
        else out.push_back(c);
    }
    out.push_back('"');
    return out;
}

MainFrame::MainFrame(const wxString& title)
    : wxFrame(NULL, wxID_ANY, title, wxDefaultPosition, wxDefaultSize),
      m_zoomTimer(this, ID_TIMER_ZOOM) {
    SetIcon(wxICON(app_icon));
    wxRect workArea = wxGetClientDisplayRect();
    wxSize initSz = FromDIP(wxSize(1080, 820));
    wxSize minSz = FromDIP(wxSize(920, 700));
    if (workArea.GetWidth() > 0 && workArea.GetHeight() > 0) {
        initSz.SetWidth(std::min(initSz.GetWidth(), workArea.GetWidth()));
        initSz.SetHeight(std::min(initSz.GetHeight(), workArea.GetHeight()));
        minSz.SetWidth(std::min(minSz.GetWidth(), workArea.GetWidth()));
        minSz.SetHeight(std::min(minSz.GetHeight(), workArea.GetHeight()));
    }
    SetMinSize(minSz);
    SetSize(initSz);
    InitUi();
    LoadPresets();
    CreateStatusBar(2);
    int statusWidths[] = { -1, FromDIP(280) };
    SetStatusWidths(2, statusWidths);
    UpdateStatusBarInfo(wxT("準備完了（住所・プリセット・地上高を入力して照会してください）"), true);
    UpdateDateNavButtons();
    Center();

    wxEvtHandler::AddFilter(this);
    m_filterInstalled = true;
    m_workerThread = std::thread(&MainFrame::WorkerLoop, this);
}

MainFrame::~MainFrame() {
    m_isClosing = true;
    m_zoomTimer.Stop();
    if (m_filterInstalled) {
        wxEvtHandler::RemoveFilter(this);
        m_filterInstalled = false;
    }
    {
        std::lock_guard<std::mutex> lk(m_jobMutex);
        m_stopWorker = true;
        m_hasPendingJob = false;
    }
    m_jobCv.notify_all();
    if (m_workerThread.joinable()) {
        m_workerThread.join();
    }
}

void MainFrame::OnClose(wxCloseEvent& event) {
    m_isClosing = true;
    ++m_requestSeq;
    m_zoomTimer.Stop();
    m_btnSearch->Enable(false);
    {
        std::lock_guard<std::mutex> lk(m_jobMutex);
        m_stopWorker = true;
        m_hasPendingJob = false;
    }
    m_jobCv.notify_all();
    if (m_workerThread.joinable()) {
        SetStatusText(wxT("通信処理の終了を待機しています..."));
        m_workerThread.join();
    }
    event.Skip();
}

int MainFrame::FilterEvent(wxEvent& event) {
    if (event.GetEventType() == wxEVT_MOUSEWHEEL && !m_isClosing.load()) {
        if (m_mapPanel && m_mapPanel->IsShownOnScreen() && IsActive()) {
            wxPoint mouseScreen = wxGetMousePosition();
            if (m_mapPanel->GetScreenRect().Contains(mouseScreen)) {
                const wxMouseEvent& me = static_cast<const wxMouseEvent&>(event);
                int delta = (me.GetWheelDelta() > 0) ? me.GetWheelDelta() : 120;
                m_wheelAcc += me.GetWheelRotation();
                int steps = 0;
                while (m_wheelAcc >= delta) {
                    m_wheelAcc -= delta;
                    steps++;
                }
                while (m_wheelAcc <= -delta) {
                    m_wheelAcc += delta;
                    steps--;
                }
                if (steps != 0) {
                    AdjustZoom(steps);
                }
                return Event_Processed;
            }
        }
    }
    return Event_Skip;
}

void MainFrame::InitUi() {
    wxPanel* rootPanel = new wxPanel(this);
    wxBoxSizer* mainSizer = new wxBoxSizer(wxVERTICAL);

    // 1. 上部：登録地点プリセット ＆ 検索条件
    wxStaticBoxSizer* topGroup = new wxStaticBoxSizer(wxVERTICAL, rootPanel, wxT("検索条件・地点プリセット"));

    // Row 1: プリセット行
    wxBoxSizer* presetRow = new wxBoxSizer(wxHORIZONTAL);
    presetRow->Add(new wxStaticText(rootPanel, wxID_ANY, wxT("登録地点:")), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(6));
    wxArrayString emptyPreset;
    emptyPreset.Add(wxT("(登録地点を選択...)"));
    m_choicePreset = new wxChoice(rootPanel, ID_CHOICE_PRESET, wxDefaultPosition, FromDIP(wxSize(240, -1)), emptyPreset);
    m_choicePreset->SetSelection(0);
    m_choicePreset->Bind(wxEVT_CONTEXT_MENU, &MainFrame::OnPresetContextMenu, this);
    presetRow->Add(m_choicePreset, 1, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(8));

    m_btnAddPreset = new wxButton(rootPanel, ID_BTN_ADD_PRESET, wxT("現在地を登録..."));
    m_btnAddPreset->SetToolTip(wxT("現在の座標（微修正後）・地上高・倍率・元住所をプリセットとして保存します"));
    presetRow->Add(m_btnAddPreset, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(6));

    m_btnDelPreset = new wxButton(rootPanel, ID_BTN_DEL_PRESET, wxT("選択地点を削除"));
    m_btnDelPreset->Enable(false);
    presetRow->Add(m_btnDelPreset, 0, wxALIGN_CENTER_VERTICAL);
    topGroup->Add(presetRow, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(6));

    // Row 2: 検索条件行
    wxBoxSizer* inputRow = new wxBoxSizer(wxHORIZONTAL);
    inputRow->Add(new wxStaticText(rootPanel, wxID_ANY, wxT("現在地(住所/緯度経度):")), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(5));
    m_txtLocation = new wxTextCtrl(rootPanel, ID_TXT_LOCATION, wxT("東京都千代田区千代田1-1"), wxDefaultPosition, FromDIP(wxSize(210, -1)), wxTE_PROCESS_ENTER);
    m_txtLocation->SetToolTip(wxT("住所表記（例: 東京都千代田区千代田1-1）または緯度経度（カンマ区切り・空白区切り両対応）を入力"));
    m_txtLocation->Bind(wxEVT_TEXT_ENTER, &MainFrame::OnSearch, this);
    inputRow->Add(m_txtLocation, 1, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(10));

    inputRow->Add(new wxStaticText(rootPanel, wxID_ANY, wxT("地上高(m):")), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(5));
    m_spinHeightAgl = new wxSpinCtrlDouble(rootPanel, ID_SPIN_HEIGHT_AGL, wxT("0.0"), wxDefaultPosition, FromDIP(wxSize(82, -1)), wxSP_ARROW_KEYS | wxTE_PROCESS_ENTER, 0.0, 2000.0, 0.0, 1.0);
    m_spinHeightAgl->SetDigits(1);
    m_spinHeightAgl->SetToolTip(wxT("建物の高層階など、地表面からの高さ(m)を指定します（Enterで即時再計算）"));
    m_spinHeightAgl->Bind(wxEVT_TEXT_ENTER, &MainFrame::OnHeightAglEnter, this);
    inputRow->Add(m_spinHeightAgl, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(10));

    inputRow->Add(new wxStaticText(rootPanel, wxID_ANY, wxT("対象日:")), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(4));
    m_btnDatePrev = new wxButton(rootPanel, ID_BTN_DATE_PREV, wxT("◀"), wxDefaultPosition, FromDIP(wxSize(28, -1)));
    m_btnDatePrev->SetToolTip(wxT("前日へ移動して再照会"));
    inputRow->Add(m_btnDatePrev, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(2));

    int jstY = 0, jstM = 0, jstD = 0;
    JmaService::GetCurrentJstDate(jstY, jstM, jstD);
    wxDateTime initialDate(
        static_cast<wxDateTime::wxDateTime_t>(jstD),
        static_cast<wxDateTime::Month>(jstM - 1),
        jstY
    );
    m_datePicker = new wxDatePickerCtrl(rootPanel, ID_DATE_PICKER, initialDate, wxDefaultPosition, wxDefaultSize, wxDP_DROPDOWN);
    wxSize dateBest = m_datePicker->GetBestSize();
    int dateTextW = m_datePicker->GetTextExtent(wxT("2026/12/31")).GetWidth();
    int dateMinW = std::max({ dateBest.GetWidth() + FromDIP(32), dateTextW + FromDIP(56), FromDIP(140) });
    m_datePicker->SetMinSize(wxSize(dateMinW, -1));
    m_datePicker->SetInitialSize(wxSize(dateMinW, -1));
    inputRow->Add(m_datePicker, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(2));

    m_btnDateNext = new wxButton(rootPanel, ID_BTN_DATE_NEXT, wxT("▶"), wxDefaultPosition, FromDIP(wxSize(28, -1)));
    m_btnDateNext->SetToolTip(wxT("翌日へ移動して再照会（JST当日まで）"));
    inputRow->Add(m_btnDateNext, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(4));

    m_btnDateToday = new wxButton(rootPanel, ID_BTN_DATE_TODAY, wxT("今日"), wxDefaultPosition, FromDIP(wxSize(46, -1)));
    m_btnDateToday->SetToolTip(wxT("対象日を本日の最新時刻(JST)へ戻して再照会"));
    inputRow->Add(m_btnDateToday, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(10));

    m_btnSearch = new wxButton(rootPanel, ID_BTN_SEARCH, wxT("照会・換算実行"));
    m_btnSearch->SetDefault();
    inputRow->Add(m_btnSearch, 0, wxALIGN_CENTER_VERTICAL);
    topGroup->Add(inputRow, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(4));

    mainSizer->Add(topGroup, 0, wxEXPAND | wxALL, FromDIP(8));

    // 2. 中段：地図プレビュー (左) + サマリー情報 (右)
    wxBoxSizer* middleSizer = new wxBoxSizer(wxHORIZONTAL);

    // 地図パネル
    wxStaticBoxSizer* mapBox = new wxStaticBoxSizer(wxVERTICAL, rootPanel, wxT("現在地 地図プレビュー (国土地理院タイル)"));
    m_mapPanel = new MapPanel(rootPanel);
    m_mapPanel->SetLocationMovedCallback([this](double new_lat, double new_lon) {
        OnMapLocationMoved(new_lat, new_lon);
    });
    mapBox->Add(m_mapPanel, 0, wxALIGN_CENTER_HORIZONTAL | wxALL, FromDIP(5));

    wxBoxSizer* layerSizer = new wxBoxSizer(wxHORIZONTAL);
    layerSizer->Add(new wxStaticText(rootPanel, wxID_ANY, wxT("種別:")), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(5));
    wxArrayString layers;
    layers.Add(wxT("標準地図"));
    layers.Add(wxT("淡色地図"));
    layers.Add(wxT("色別標高図"));
    m_choiceLayer = new wxChoice(rootPanel, ID_CHOICE_LAYER, wxDefaultPosition, wxDefaultSize, layers);
    m_choiceLayer->SetSelection(0);
    layerSizer->Add(m_choiceLayer, 1, wxALIGN_CENTER_VERTICAL);
    mapBox->Add(layerSizer, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(4));

    wxBoxSizer* zoomSizer = new wxBoxSizer(wxHORIZONTAL);
    zoomSizer->Add(new wxStaticText(rootPanel, wxID_ANY, wxT("倍率:")), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(5));
    m_btnZoomOut = new wxButton(rootPanel, ID_BTN_ZOOM_OUT, wxT("-"), wxDefaultPosition, FromDIP(wxSize(30, -1)));
    m_lblZoom = new wxStaticText(rootPanel, wxID_ANY, wxT("Z15"), wxDefaultPosition, FromDIP(wxSize(38, -1)), wxALIGN_CENTER_HORIZONTAL | wxST_NO_AUTORESIZE);
    m_btnZoomIn = new wxButton(rootPanel, ID_BTN_ZOOM_IN, wxT("+"), wxDefaultPosition, FromDIP(wxSize(30, -1)));
    m_btnResetPos = new wxButton(rootPanel, ID_BTN_RESET_POS, wxT("元位置へ"));
    m_btnResetPos->SetToolTip(wxT("地図上で微修正する前の検索地点へ戻します"));
    m_btnResetPos->Enable(false);

    zoomSizer->Add(m_btnZoomOut, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(3));
    zoomSizer->Add(m_lblZoom, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(3));
    zoomSizer->Add(m_btnZoomIn, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(8));
    zoomSizer->AddStretchSpacer();
    zoomSizer->Add(m_btnResetPos, 0, wxALIGN_CENTER_VERTICAL);
    mapBox->Add(zoomSizer, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(4));

    wxStaticText* mapHint = new wxStaticText(rootPanel, wxID_ANY, wxT("左ドラッグ・ダブルクリック: 位置微修正 / ホイール: 拡大縮小"));
    wxFont hintFont = mapHint->GetFont();
    hintFont.SetPointSize(8);
    mapHint->SetFont(hintFont);
    mapHint->SetForegroundColour(wxColor(90, 90, 90));
    mapHint->Wrap(FromDIP(320));
    mapBox->Add(mapHint, 0, wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(5));

    middleSizer->Add(mapBox, 0, wxEXPAND | wxRIGHT, FromDIP(10));

    // サマリーパネル
    wxStaticBoxSizer* infoBox = new wxStaticBoxSizer(wxVERTICAL, rootPanel, wxT("現在地および最寄り観測所情報"));
    wxFlexGridSizer* gridSizer = new wxFlexGridSizer(7, 2, FromDIP(7), FromDIP(10));
    gridSizer->AddGrowableCol(1, 1);

    gridSizer->Add(new wxStaticText(rootPanel, wxID_ANY, wxT("解決住所:")), 0, wxALIGN_CENTER_VERTICAL);
    m_lblAddress = new wxStaticText(rootPanel, wxID_ANY, wxT("-"), wxDefaultPosition, wxDefaultSize, wxST_ELLIPSIZE_END);
    gridSizer->Add(m_lblAddress, 1, wxEXPAND | wxALIGN_CENTER_VERTICAL);

    gridSizer->Add(new wxStaticText(rootPanel, wxID_ANY, wxT("現在地座標:")), 0, wxALIGN_CENTER_VERTICAL);
    wxBoxSizer* coordsRow = new wxBoxSizer(wxHORIZONTAL);
    m_lblCoords = new wxStaticText(rootPanel, wxID_ANY, wxT("-"), wxDefaultPosition, wxDefaultSize, wxST_ELLIPSIZE_END);
    m_btnCopyCoords = new wxButton(rootPanel, ID_BTN_COPY_COORDS, wxT("コピー"), wxDefaultPosition, FromDIP(wxSize(54, -1)));
    m_btnCopyCoords->SetToolTip(wxT("現在地の緯度・経度を「緯度, 経度」形式でクリップボードにコピーします"));
    m_btnCopyCoords->Enable(false);
    coordsRow->Add(m_lblCoords, 1, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(6));
    coordsRow->Add(m_btnCopyCoords, 0, wxALIGN_CENTER_VERTICAL);
    gridSizer->Add(coordsRow, 1, wxEXPAND | wxALIGN_CENTER_VERTICAL);

    gridSizer->Add(new wxStaticText(rootPanel, wxID_ANY, wxT("現在地標高・地上高:")), 0, wxALIGN_CENTER_VERTICAL);
    m_lblElevation = new wxStaticText(rootPanel, wxID_ANY, wxT("-"), wxDefaultPosition, wxDefaultSize, wxST_ELLIPSIZE_END);
    wxFont boldFont = m_lblElevation->GetFont();
    boldFont.SetWeight(wxFONTWEIGHT_BOLD);
    m_lblElevation->SetFont(boldFont);
    m_lblElevation->SetForegroundColour(wxColor(0, 100, 200));
    gridSizer->Add(m_lblElevation, 1, wxEXPAND | wxALIGN_CENTER_VERTICAL);

    gridSizer->Add(new wxStaticText(rootPanel, wxID_ANY, wxT("最寄り気象官署:")), 0, wxALIGN_CENTER_VERTICAL);
    m_lblStation = new wxStaticText(rootPanel, wxID_ANY, wxT("-"), wxDefaultPosition, wxDefaultSize, wxST_ELLIPSIZE_END);
    gridSizer->Add(m_lblStation, 1, wxEXPAND | wxALIGN_CENTER_VERTICAL);

    gridSizer->Add(new wxStaticText(rootPanel, wxID_ANY, wxT("官署までの距離:")), 0, wxALIGN_CENTER_VERTICAL);
    m_lblDistance = new wxStaticText(rootPanel, wxID_ANY, wxT("-"));
    gridSizer->Add(m_lblDistance, 1, wxEXPAND | wxALIGN_CENTER_VERTICAL);

    gridSizer->Add(new wxStaticText(rootPanel, wxID_ANY, wxT("空間補間補正:")), 0, wxALIGN_CENTER_VERTICAL);
    m_lblSpatialSummary = new wxStaticText(rootPanel, wxID_ANY, wxT("-"), wxDefaultPosition, wxDefaultSize, wxST_ELLIPSIZE_END);
    gridSizer->Add(m_lblSpatialSummary, 1, wxEXPAND | wxALIGN_CENTER_VERTICAL);

    gridSizer->Add(new wxStaticText(rootPanel, wxID_ANY, wxT("換算気圧レンジ:")), 0, wxALIGN_CENTER_VERTICAL);
    m_lblPressureSummary = new wxStaticText(rootPanel, wxID_ANY, wxT("-"), wxDefaultPosition, wxDefaultSize, wxST_ELLIPSIZE_END);
    m_lblPressureSummary->SetFont(boldFont);
    m_lblPressureSummary->SetForegroundColour(wxColor(20, 85, 130));
    gridSizer->Add(m_lblPressureSummary, 1, wxEXPAND | wxALIGN_CENTER_VERTICAL);

    infoBox->Add(gridSizer, 1, wxEXPAND | wxALL, FromDIP(10));
    middleSizer->Add(infoBox, 1, wxEXPAND);

    mainSizer->Add(middleSizer, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(8));

    // 3. 下段：24時間推移リストビュー
    wxStaticBoxSizer* listGroup = new wxStaticBoxSizer(wxVERTICAL, rootPanel, wxT("指定日の推移データ (気象庁観測値 ＆ 現在地換算気圧)"));
    m_listHourly = new wxListCtrl(rootPanel, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxLC_REPORT | wxLC_SINGLE_SEL | wxBORDER_SUNKEN);

    m_listHourly->InsertColumn(0, wxT("時刻"), wxLIST_FORMAT_CENTER, FromDIP(68));
    m_listHourly->InsertColumn(1, wxT("天候"), wxLIST_FORMAT_CENTER, FromDIP(80));
    m_listHourly->InsertColumn(2, wxT("気温 (℃)"), wxLIST_FORMAT_RIGHT, FromDIP(85));
    m_listHourly->InsertColumn(3, wxT("湿度 (%)"), wxLIST_FORMAT_RIGHT, FromDIP(80));
    m_listHourly->InsertColumn(4, wxT("観測所気圧 (hPa)"), wxLIST_FORMAT_RIGHT, FromDIP(120));
    m_listHourly->InsertColumn(5, wxT("海面気圧 (hPa)"), wxLIST_FORMAT_RIGHT, FromDIP(120));
    m_listHourly->InsertColumn(6, wxT("★ 現在地換算気圧 (hPa)"), wxLIST_FORMAT_RIGHT, FromDIP(165));
    m_listHourly->InsertColumn(7, wxT("気圧差 (hPa)"), wxLIST_FORMAT_RIGHT, FromDIP(100));
    m_listHourly->Bind(wxEVT_CONTEXT_MENU, &MainFrame::OnListContextMenu, this);
    m_listHourly->Bind(wxEVT_KEY_DOWN, &MainFrame::OnListKeyDown, this);
    m_listHourly->Bind(wxEVT_LIST_ITEM_ACTIVATED, &MainFrame::OnListItemActivated, this);

    listGroup->Add(m_listHourly, 1, wxEXPAND | wxALL, FromDIP(5));

    // フッター
    wxBoxSizer* footerSizer = new wxBoxSizer(wxHORIZONTAL);
    m_btnExport = new wxButton(rootPanel, ID_BTN_EXPORT, wxT("CSVファイルとして保存..."));
    m_btnCopyList = new wxButton(rootPanel, ID_BTN_COPY_LIST, wxT("クリップボードにコピー"));
    SetExportControlsEnabled(false);
    footerSizer->AddStretchSpacer();
    footerSizer->Add(m_btnExport, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(8));
    footerSizer->Add(m_btnCopyList, 0, wxALIGN_CENTER_VERTICAL);
    listGroup->Add(footerSizer, 0, wxEXPAND | wxTOP, FromDIP(5));

    mainSizer->Add(listGroup, 1, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(8));

    rootPanel->SetSizer(mainSizer);
    UpdateZoomControls();
}

std::string MainFrame::GetSelectedLayerCode() const {
    int sel = m_choiceLayer->GetSelection();
    if (sel == 1) return "pale";
    if (sel == 2) return "relief";
    return "std";
}

wxSize MainFrame::GetCurrentMapSize() const {
    if (!m_mapPanel) return wxSize(320, 320);
    wxSize sz = m_mapPanel->GetClientSize();
    if (sz.GetWidth() < 128 || sz.GetHeight() < 128) {
        return wxSize(320, 320);
    }
    return sz;
}

void MainFrame::UpdateZoomControls() {
    m_lblZoom->SetLabel(wxString::Format(wxT("Z%d"), m_currentZoom));
    m_btnZoomOut->Enable(m_currentZoom > 10);
    m_btnZoomIn->Enable(m_currentZoom < 18);
}

void MainFrame::EnqueueJob(WorkerJob job) {
    job.seq = ++m_requestSeq;
    {
        std::lock_guard<std::mutex> lk(m_jobMutex);
        m_pendingJob = std::move(job);
        m_hasPendingJob = true;
    }
    m_jobCv.notify_one();
}

void MainFrame::SetSearchBusy(bool busy) {
    if (!m_btnSearch) return;
    if (busy) {
        m_btnSearch->SetLabel(wxT("中止"));
        m_btnSearch->SetToolTip(wxT("実行中のデータ取得をキャンセルします"));
    } else {
        m_btnSearch->SetLabel(wxT("照会・換算実行"));
        m_btnSearch->SetToolTip(wxT("入力した現在地と対象日で標高・気象データを取得して気圧を換算します"));
    }
    m_btnSearch->Enable(true);
}

double MainFrame::ReadSpinHeightAgl(bool commit_text) {
    if (!m_spinHeightAgl) return 0.0;
    wxString txt = m_spinHeightAgl->GetTextValue().Trim().Trim(false);
    double val = 0.0;
    if (txt.ToDouble(&val) && std::isfinite(val)) {
        val = std::clamp(val, 0.0, 2000.0);
        if (commit_text) {
            m_spinHeightAgl->SetValue(val);
        }
        return val;
    }
    val = m_spinHeightAgl->GetValue();
    if (commit_text) {
        m_spinHeightAgl->SetValue(val);
    }
    return val;
}

void MainFrame::ApplyHeightAglValue(double new_height) {
    if (!m_lastLoc.success) return;

    m_lastHeightAgl = new_height;
    UpdateElevationLabel();

    if (m_lastJma.success && !m_lastJma.hours.empty()) {
        JmaService::RecalculatePressures(m_lastJma, m_lastLoc.elevation + m_lastHeightAgl);
        UpdateSpatialSummaryLabel();
        UpdatePressureSummaryLabel();
        PopulateHourlyList(false);
        UpdateStatusBarInfo(
            wxString::Format(wxT("地上高 %.1f m（海抜 %.1f m）で気圧を再計算しました"),
                m_lastHeightAgl, m_lastLoc.elevation + m_lastHeightAgl),
            true
        );
    }
}

void MainFrame::UpdateDateNavButtons() {
    if (!m_datePicker || !m_btnDateNext) return;
    int jstY = 0, jstM = 0, jstD = 0;
    JmaService::GetCurrentJstDate(jstY, jstM, jstD);
    wxDateTime jstToday(
        static_cast<wxDateTime::wxDateTime_t>(jstD),
        static_cast<wxDateTime::Month>(jstM - 1),
        jstY
    );
    wxDateTime cur = m_datePicker->GetValue();
    bool beforeToday = cur.IsValid() &&
        (cur.GetYear() < jstToday.GetYear() ||
        (cur.GetYear() == jstToday.GetYear() && cur.GetMonth() < jstToday.GetMonth()) ||
        (cur.GetYear() == jstToday.GetYear() && cur.GetMonth() == jstToday.GetMonth() && cur.GetDay() < jstToday.GetDay()));
    m_btnDateNext->Enable(beforeToday);
}

void MainFrame::StartWeatherOnlySearch() {
    if (!m_lastLoc.success || m_isClosing.load()) return;

    if (m_dataJobInFlight && (m_activeDataKind == JobKind::FullSearch || m_activeDataKind == JobKind::FineTune)) {
        UpdateStatusBarInfo(wxT("現在の照会完了後に選択日の気象データを更新します..."), false);
        return;
    }

    wxDateTime date = m_datePicker->GetValue();
    if (!date.IsValid()) return;

    WorkerJob job;
    job.kind = JobKind::WeatherOnly;
    job.lat = m_lastLoc.lat;
    job.lon = m_lastLoc.lon;
    job.fixed_elevation_m = m_lastLoc.elevation;
    job.year = date.GetYear();
    job.month = date.GetMonth() + 1;
    job.day = date.GetDay();
    job.height_agl_m = ReadSpinHeightAgl(true);

    m_dataJobInFlight = true;
    m_activeDataKind = JobKind::WeatherOnly;
    SetSearchBusy(true);
    SetExportControlsEnabled(false);
    UpdateStatusBarInfo(wxString::Format(wxT("%04d/%02d/%02d の気象庁データを取得しています..."), job.year, job.month, job.day), false);
    EnqueueJob(std::move(job));
}

void MainFrame::ShiftTargetDate(int delta_days) {
    if (!m_datePicker) return;
    wxDateTime cur = m_datePicker->GetValue();
    if (!cur.IsValid()) return;

    wxDateTime shifted = cur + wxDateSpan::Days(delta_days);
    int jstY = 0, jstM = 0, jstD = 0;
    JmaService::GetCurrentJstDate(jstY, jstM, jstD);
    wxDateTime jstToday(
        static_cast<wxDateTime::wxDateTime_t>(jstD),
        static_cast<wxDateTime::Month>(jstM - 1),
        jstY
    );
    if (shifted.IsLaterThan(jstToday)) {
        shifted = jstToday;
    }

    m_suppressDateEvent = true;
    m_datePicker->SetValue(shifted);
    m_suppressDateEvent = false;
    UpdateDateNavButtons();

    if (m_lastLoc.success) {
        StartWeatherOnlySearch();
    }
}

void MainFrame::OnDateChanged(wxDateEvent& WXUNUSED(event)) {
    if (m_suppressDateEvent) return;
    UpdateDateNavButtons();
    if (m_lastLoc.success) {
        StartWeatherOnlySearch();
    }
}

void MainFrame::OnDatePrev(wxCommandEvent& WXUNUSED(event)) {
    ShiftTargetDate(-1);
}

void MainFrame::OnDateNext(wxCommandEvent& WXUNUSED(event)) {
    ShiftTargetDate(+1);
}

void MainFrame::OnDateToday(wxCommandEvent& WXUNUSED(event)) {
    int jstY = 0, jstM = 0, jstD = 0;
    JmaService::GetCurrentJstDate(jstY, jstM, jstD);
    wxDateTime jstToday(
        static_cast<wxDateTime::wxDateTime_t>(jstD),
        static_cast<wxDateTime::Month>(jstM - 1),
        jstY
    );
    m_suppressDateEvent = true;
    m_datePicker->SetValue(jstToday);
    m_suppressDateEvent = false;
    UpdateDateNavButtons();

    if (m_lastLoc.success) {
        StartWeatherOnlySearch();
    }
}

void MainFrame::OnCopyCoords(wxCommandEvent& WXUNUSED(event)) {
    if (!m_lastLoc.success) return;
    wxString coords = wxString::Format(wxT("%.6f, %.6f"), m_lastLoc.lat, m_lastLoc.lon);
    CopyTextToClipboard(coords, wxString::Format(wxT("現在地座標 (%s) をクリップボードにコピーしました"), coords));
}

void MainFrame::OnSearch(wxCommandEvent& event) {
    if (m_dataJobInFlight && event.GetEventType() == wxEVT_BUTTON) {
        ++m_requestSeq;
        m_dataJobInFlight = false;
        m_activeDataKind = JobKind::None;
        SetSearchBusy(false);
        m_mapPanel->ResetDragOffset();
        SetExportControlsEnabled(m_lastJma.success && !m_lastJma.hours.empty());
        UpdateStatusBarInfo(wxT("照会をキャンセルしました"), false);
        return;
    }

    wxString input = m_txtLocation->GetValue().Trim().Trim(false);
    if (input.IsEmpty()) {
        wxMessageBox(wxT("現在地（住所または緯度経度）を入力してください。"), wxT("入力エラー"), wxOK | wxICON_WARNING, this);
        return;
    }

    m_zoomTimer.Stop();
    wxDateTime date = m_datePicker->GetValue();
    wxSize mapSz = GetCurrentMapSize();

    WorkerJob job;
    job.kind = JobKind::FullSearch;
    job.input_location = std::string(input.ToUTF8());
    job.year = date.GetYear();
    job.month = date.GetMonth() + 1;
    job.day = date.GetDay();
    job.height_agl_m = ReadSpinHeightAgl(true);
    job.zoom = m_currentZoom;
    job.layer = GetSelectedLayerCode();
    job.map_w = mapSz.GetWidth();
    job.map_h = mapSz.GetHeight();

    // 地図上で位置微修正した後の座標文字列のまま日付等を変えて再照会する場合は、元の住所表記を保持する
    if (m_isFineTuned && !m_fineTunedCoordText.IsEmpty() && input == m_fineTunedCoordText && m_lastLoc.success) {
        job.has_direct_coords = true;
        job.lat = m_lastLoc.lat;
        job.lon = m_lastLoc.lon;
        job.base_input_text = m_baseInputText;
        job.base_address = m_baseAddress;
        job.base_lat = m_baseLat;
        job.base_lon = m_baseLon;
        job.is_fine_tuned = true;
    }

    SetSearchBusy(true);
    SetExportControlsEnabled(false);
    UpdateStatusBarInfo(wxT("国土地理院および気象庁からデータを取得しています..."), false);

    m_dataJobInFlight = true;
    m_activeDataKind = JobKind::FullSearch;
    EnqueueJob(std::move(job));
}

void MainFrame::OnLocationTextChanged(wxCommandEvent& WXUNUSED(event)) {
    if (m_choicePreset->GetSelection() != 0) {
        m_choicePreset->SetSelection(0);
        m_btnDelPreset->Enable(false);
    }
}

void MainFrame::OnLayerChanged(wxCommandEvent& WXUNUSED(event)) {
    if (!m_lastLoc.success) return;
    if (m_dataJobInFlight) {
        UpdateStatusBarInfo(wxT("照会完了後に地図種別を更新します..."), false);
        return;
    }

    m_zoomTimer.Stop();
    wxSize mapSz = GetCurrentMapSize();

    WorkerJob job;
    job.kind = JobKind::TileOnly;
    job.lat = m_lastLoc.lat;
    job.lon = m_lastLoc.lon;
    job.zoom = m_currentZoom;
    job.layer = GetSelectedLayerCode();
    job.map_w = mapSz.GetWidth();
    job.map_h = mapSz.GetHeight();

    UpdateStatusBarInfo(wxT("地図タイルを更新しています..."), false);
    EnqueueJob(std::move(job));
}

void MainFrame::AdjustZoom(int delta) {
    int nextZoom = std::clamp(m_currentZoom + delta, 10, 18);
    if (nextZoom == m_currentZoom) return;

    m_currentZoom = nextZoom;
    UpdateZoomControls();

    if (m_lastLoc.success) {
        if (m_dataJobInFlight) {
            UpdateStatusBarInfo(wxString::Format(wxT("照会完了後に地図倍率を Z%d へ更新します..."), m_currentZoom), false);
        } else {
            UpdateStatusBarInfo(wxString::Format(wxT("地図倍率を Z%d へ変更中..."), m_currentZoom), false);
            m_zoomTimer.StartOnce(180);
        }
    }
}

void MainFrame::OnZoomOut(wxCommandEvent& WXUNUSED(event)) {
    AdjustZoom(-1);
}

void MainFrame::OnZoomIn(wxCommandEvent& WXUNUSED(event)) {
    AdjustZoom(+1);
}

void MainFrame::OnZoomTimer(wxTimerEvent& WXUNUSED(event)) {
    if (!m_lastLoc.success || m_isClosing.load() || m_dataJobInFlight) return;

    wxSize mapSz = GetCurrentMapSize();
    WorkerJob job;
    job.kind = JobKind::TileOnly;
    job.lat = m_lastLoc.lat;
    job.lon = m_lastLoc.lon;
    job.zoom = m_currentZoom;
    job.layer = GetSelectedLayerCode();
    job.map_w = mapSz.GetWidth();
    job.map_h = mapSz.GetHeight();

    EnqueueJob(std::move(job));
}

void MainFrame::OnMapLocationMoved(double new_lat, double new_lon) {
    if (!m_lastLoc.success || m_isClosing.load()) {
        m_mapPanel->ResetDragOffset();
        return;
    }

    m_zoomTimer.Stop();
    wxDateTime date = m_datePicker->GetValue();
    wxSize mapSz = GetCurrentMapSize();

    WorkerJob job;
    job.kind = JobKind::FineTune;
    job.lat = new_lat;
    job.lon = new_lon;
    job.base_input_text = m_baseInputText;
    job.base_address = m_baseAddress;
    job.base_lat = m_baseLat;
    job.base_lon = m_baseLon;
    job.is_fine_tuned = true;
    job.is_reset = false;
    job.year = date.GetYear();
    job.month = date.GetMonth() + 1;
    job.day = date.GetDay();
    job.height_agl_m = ReadSpinHeightAgl(true);
    job.zoom = m_currentZoom;
    job.layer = GetSelectedLayerCode();
    job.map_w = mapSz.GetWidth();
    job.map_h = mapSz.GetHeight();

    AttachPrevStationSnapshot(job, new_lat, new_lon);

    UpdateStatusBarInfo(wxString::Format(wxT("選択地点 (%.6f, %.6f) の標高を取得しています..."), new_lat, new_lon), false);
    m_dataJobInFlight = true;
    m_activeDataKind = JobKind::FineTune;
    SetSearchBusy(true);
    EnqueueJob(std::move(job));
}

void MainFrame::AttachPrevStationSnapshot(WorkerJob& job, double new_lat, double new_lon) const {
    if (m_lastJma.success) {
        job.have_prev_station = true;
        job.prev_prec_no = m_lastJma.station.prec_no;
        job.prev_block_no = m_lastJma.station.block_no;
        job.prev_year = m_lastJma.year;
        job.prev_month = m_lastJma.month;
        job.prev_day = m_lastJma.day;
        job.can_reuse_spatial_neighbors = JmaService::CanReuseNeighborDataForLocation(m_lastJma, new_lat, new_lon);
    }
}

void MainFrame::OnResetPosition(wxCommandEvent& WXUNUSED(event)) {
    if (!m_lastLoc.success || !m_isFineTuned || m_isClosing.load()) return;

    m_zoomTimer.Stop();
    wxDateTime date = m_datePicker->GetValue();
    wxSize mapSz = GetCurrentMapSize();

    WorkerJob job;
    job.kind = JobKind::FineTune;
    job.lat = m_baseLat;
    job.lon = m_baseLon;
    job.base_input_text = m_baseInputText;
    job.base_address = m_baseAddress;
    job.base_lat = m_baseLat;
    job.base_lon = m_baseLon;
    job.is_fine_tuned = false;
    job.is_reset = true;
    job.year = date.GetYear();
    job.month = date.GetMonth() + 1;
    job.day = date.GetDay();
    job.height_agl_m = ReadSpinHeightAgl(true);
    job.zoom = m_currentZoom;
    job.layer = GetSelectedLayerCode();
    job.map_w = mapSz.GetWidth();
    job.map_h = mapSz.GetHeight();

    AttachPrevStationSnapshot(job, m_baseLat, m_baseLon);

    UpdateStatusBarInfo(wxT("元の検索位置へ戻しています..."), false);
    m_dataJobInFlight = true;
    m_activeDataKind = JobKind::FineTune;
    SetSearchBusy(true);
    EnqueueJob(std::move(job));
}

void MainFrame::OnHeightAglChanged(wxSpinDoubleEvent& WXUNUSED(event)) {
    ApplyHeightAglValue(m_spinHeightAgl->GetValue());
}

void MainFrame::OnHeightAglEnter(wxCommandEvent& WXUNUSED(event)) {
    ApplyHeightAglValue(ReadSpinHeightAgl(true));
}

void MainFrame::WorkerLoop() {
    while (true) {
        WorkerJob job;
        {
            std::unique_lock<std::mutex> lk(m_jobMutex);
            m_jobCv.wait(lk, [this]() {
                return m_stopWorker || m_hasPendingJob;
            });
            if (m_stopWorker) break;
            job = std::move(m_pendingJob);
            m_hasPendingJob = false;
        }

        if (m_isClosing.load() || job.seq != m_requestSeq.load()) {
            continue;
        }

        WorkerResult res;
        res.seq = job.seq;
        res.kind = job.kind;
        res.year = job.year;
        res.month = job.month;
        res.day = job.day;
        res.height_agl_m = job.height_agl_m;
        res.base_input_text = job.base_input_text;
        res.base_address = job.base_address;
        res.base_lat = job.base_lat;
        res.base_lon = job.base_lon;
        res.is_fine_tuned = job.is_fine_tuned;
        res.is_reset = job.is_reset;
        res.from_preset = job.from_preset;

        auto shouldCancel = [this, seq = job.seq]() {
            return m_isClosing.load() || seq != m_requestSeq.load();
        };

        if (job.kind == JobKind::FullSearch) {
            if (job.has_direct_coords) {
                res.loc.query = job.input_location;
                res.loc.lat = job.lat;
                res.loc.lon = job.lon;
                if (GsiService::GetElevation(job.lat, job.lon, res.loc.elevation, res.loc.hsrc, shouldCancel)) {
                    res.loc.success = true;
                    res.loc.resolved_address = job.base_address;
                } else {
                    res.loc.success = false;
                    res.loc.error_message = "指定地点の標高データを取得できませんでした (海域または日本国外の可能性があります)。";
                }
            } else {
                res.loc = GsiService::QueryLocation(job.input_location, shouldCancel);
                if (res.loc.success) {
                    res.base_input_text = job.input_location;
                    res.base_address = res.loc.resolved_address;
                    res.base_lat = res.loc.lat;
                    res.base_lon = res.loc.lon;
                    res.is_fine_tuned = false;
                }
            }

            if (res.loc.success && !shouldCancel()) {
                res.tile = GsiService::GetMapTile(
                    res.loc.lat, res.loc.lon, job.zoom, job.layer, job.map_w, job.map_h, shouldCancel
                );
                if (!shouldCancel()) {
                    res.jma_res = JmaService::FetchDailyWeatherForLocation(
                        res.loc.lat, res.loc.lon, job.year, job.month, job.day,
                        res.loc.elevation + job.height_agl_m, shouldCancel, res.loc.elevation);
                }
            }
        } else if (job.kind == JobKind::FineTune) {
            double elev = 0.0;
            std::string hsrc;
            if (!GsiService::GetElevation(job.lat, job.lon, elev, hsrc, shouldCancel)) {
                res.elev_failed = true;
            } else if (!shouldCancel()) {
                res.loc.success = true;
                res.loc.lat = job.lat;
                res.loc.lon = job.lon;
                res.loc.elevation = elev;
                res.loc.hsrc = hsrc;
                res.loc.resolved_address = job.base_address;

                res.tile = GsiService::GetMapTile(
                    job.lat, job.lon, job.zoom, job.layer, job.map_w, job.map_h, shouldCancel
                );
                if (!shouldCancel()) {
                    double dist = 0.0;
                    bool realtimeCandidate = JmaService::IsRealtimeDateCandidate(job.year, job.month, job.day);
                    JmaStation stn = JmaService::FindNearestStation(job.lat, job.lon, dist, realtimeCandidate, elev);
                    if (job.have_prev_station &&
                        job.can_reuse_spatial_neighbors &&
                        job.prev_year == job.year && job.prev_month == job.month && job.prev_day == job.day &&
                        stn.prec_no == job.prev_prec_no && stn.block_no == job.prev_block_no) {
                        res.reuse_existing_jma = true;
                        res.new_distance_km = dist;
                    } else {
                        res.jma_res = JmaService::FetchDailyWeatherForLocation(
                            job.lat, job.lon, job.year, job.month, job.day,
                            elev + job.height_agl_m, shouldCancel, elev);
                    }
                }
            }
        } else if (job.kind == JobKind::WeatherOnly) {
            res.jma_res = JmaService::FetchDailyWeatherForLocation(
                job.lat, job.lon, job.year, job.month, job.day,
                job.fixed_elevation_m + job.height_agl_m, shouldCancel, job.fixed_elevation_m);
        } else if (job.kind == JobKind::TileOnly) {
            res.tile = GsiService::GetMapTile(
                job.lat, job.lon, job.zoom, job.layer, job.map_w, job.map_h, shouldCancel
            );
        }

        if (m_isClosing.load() || job.seq != m_requestSeq.load()) {
            continue;
        }

        CallAfter([this, res]() {
            if (m_isClosing.load() || res.seq != m_requestSeq.load()) return;
            ApplyWorkerResult(res);
        });
    }
}

void MainFrame::ApplyWorkerResult(const WorkerResult& res) {
    if (res.kind == JobKind::TileOnly) {
        if (!m_dataJobInFlight) {
            SetSearchBusy(false);
        }
        m_mapPanel->SetMapTile(res.tile);
        wxString scaleNote = (res.tile.digital_scale > 1)
            ? wxString::Format(wxT(" (Z%d拡大表示)"), res.tile.fetch_z)
            : wxString();
        UpdateStatusBarInfo(wxString::Format(wxT("地図プレビューを更新しました (Z%d%s)"), res.tile.z, scaleNote), false);
        return;
    }

    m_dataJobInFlight = false;
    m_activeDataKind = JobKind::None;
    SetSearchBusy(false);

    auto checkPendingDateOrMap = [&](int completed_y, int completed_m, int completed_d, bool check_tile) {
        if (m_lastLoc.success) {
            wxDateTime curDate = m_datePicker->GetValue();
            if (curDate.IsValid() &&
                (curDate.GetYear() != completed_y || (curDate.GetMonth() + 1) != completed_m || curDate.GetDay() != completed_d)) {
                StartWeatherOnlySearch();
                return;
            }
            if (check_tile &&
                (m_mapPanel->GetCurrentTileZoom() != m_currentZoom ||
                 m_mapPanel->GetCurrentTileLayer() != GetSelectedLayerCode())) {
                m_zoomTimer.StartOnce(50);
            }
        }
    };

    if (res.kind == JobKind::WeatherOnly) {
        if (!m_lastLoc.success) return;

        m_lastJma = res.jma_res;
        m_lastHeightAgl = ReadSpinHeightAgl(false);
        UpdateElevationLabel();

        if (!m_lastJma.success) {
            wxDateTime curDate = m_datePicker->GetValue();
            if (curDate.IsValid() &&
                (curDate.GetYear() != res.year || (curDate.GetMonth() + 1) != res.month || curDate.GetDay() != res.day)) {
                StartWeatherOnlySearch();
                return;
            }
            ClearWeatherSummaryUi();
            m_listHourly->DeleteAllItems();
            SetExportControlsEnabled(false);
            UpdateStatusBarInfo(wxT("気象庁データの取得に失敗しました"), true);
            wxMessageBox(wxString::FromUTF8(m_lastJma.error_message), wxT("気象庁データ取得エラー"), wxOK | wxICON_WARNING, this);
            return;
        }

        UpdateStationLabels();
        JmaService::RecalculatePressures(m_lastJma, m_lastLoc.elevation + m_lastHeightAgl);
        UpdateSpatialSummaryLabel();
        UpdatePressureSummaryLabel();
        PopulateHourlyList(m_lastJma.is_realtime);

        SetExportControlsEnabled(!m_lastJma.hours.empty());
        wxString suffix = BuildJmaStatusSuffix(m_lastJma);
        wxString modeText = m_lastJma.is_realtime ? wxT("AMeDAS速報・天候なし") : wxT("過去確定データ");
        UpdateStatusBarInfo(
            wxString::Format(wxT("照会完了 (%s%s / 最寄り官署: %s)"),
                modeText, suffix, wxString::FromUTF8(m_lastJma.station.name)),
            true
        );
        checkPendingDateOrMap(res.year, res.month, res.day, true);
        return;
    }

    if (res.kind == JobKind::FineTune) {
        if (res.elev_failed) {
            m_mapPanel->ResetDragOffset();
            UpdateStatusBarInfo(wxT("選択地点の標高を取得できませんでした（海域・データ範囲外のため直前の地点を維持します）"), false);
            checkPendingDateOrMap(res.year, res.month, res.day, true);
            return;
        }

        m_lastLoc = res.loc;
        m_baseInputText = res.base_input_text;
        m_baseAddress = res.base_address;
        m_baseLat = res.base_lat;
        m_baseLon = res.base_lon;
        m_isFineTuned = res.is_fine_tuned;

        if (m_isFineTuned) {
            m_fineTunedCoordText = wxString::Format(wxT("%.6f, %.6f"), m_lastLoc.lat, m_lastLoc.lon);
            m_txtLocation->ChangeValue(m_fineTunedCoordText);
            if (m_choicePreset->GetSelection() != 0) {
                m_choicePreset->SetSelection(0);
                m_btnDelPreset->Enable(false);
            }
        } else {
            m_fineTunedCoordText.Clear();
            m_txtLocation->ChangeValue(wxString::FromUTF8(m_baseInputText));
        }

        m_btnResetPos->Enable(m_isFineTuned);
        m_mapPanel->SetMapTile(res.tile);

        UpdateAddressLabel();
        wxString coordsStr = wxString::Format(wxT("北緯 %.6f, 東経 %.6f"), m_lastLoc.lat, m_lastLoc.lon);
        m_lblCoords->SetLabel(coordsStr);
        m_lblCoords->SetToolTip(wxString::Format(wxT("%.6f, %.6f"), m_lastLoc.lat, m_lastLoc.lon));
        m_btnCopyCoords->Enable(true);

        m_lastHeightAgl = ReadSpinHeightAgl(false);
        UpdateElevationLabel();

        if (res.reuse_existing_jma) {
            m_lastJma.distance_km = res.new_distance_km;
            m_lastJma.target_lat = m_lastLoc.lat;
            m_lastJma.target_lon = m_lastLoc.lon;
            m_lastJma.has_target_coords = true;
            UpdateStationLabels();
            if (m_lastJma.success && !m_lastJma.hours.empty()) {
                JmaService::RecalculatePressures(m_lastJma, m_lastLoc.elevation + m_lastHeightAgl);
                UpdateSpatialSummaryLabel();
                UpdatePressureSummaryLabel();
                PopulateHourlyList(false);
            }
        } else {
            m_lastJma = res.jma_res;
            if (!m_lastJma.success) {
                ClearWeatherSummaryUi();
                m_listHourly->DeleteAllItems();
                SetExportControlsEnabled(false);
                UpdateStatusBarInfo(wxT("気象庁データの取得に失敗しました"), true);
                checkPendingDateOrMap(res.year, res.month, res.day, true);
                wxMessageBox(wxString::FromUTF8(m_lastJma.error_message), wxT("気象庁データ取得エラー"), wxOK | wxICON_WARNING, this);
                return;
            }
            UpdateStationLabels();
            JmaService::RecalculatePressures(m_lastJma, m_lastLoc.elevation + m_lastHeightAgl);
            UpdateSpatialSummaryLabel();
            UpdatePressureSummaryLabel();
            PopulateHourlyList(m_lastJma.is_realtime);
        }

        SetExportControlsEnabled(m_lastJma.success && !m_lastJma.hours.empty());
        wxString suffix = BuildJmaStatusSuffix(m_lastJma);
        if (res.is_reset) {
            UpdateStatusBarInfo(wxString::Format(
                wxT("元の検索位置へ戻しました (地盤 %.1fm / 海抜 %.1fm%s)"),
                m_lastLoc.elevation, m_lastLoc.elevation + m_lastHeightAgl, suffix), true);
        } else {
            UpdateStatusBarInfo(wxString::Format(
                wxT("現在地を微修正しました (%.6f, %.6f / 地盤 %.1fm / 海抜 %.1fm%s)"),
                m_lastLoc.lat, m_lastLoc.lon, m_lastLoc.elevation, m_lastLoc.elevation + m_lastHeightAgl, suffix), true);
        }
        checkPendingDateOrMap(res.year, res.month, res.day, true);
        return;
    }

    // JobKind::FullSearch
    if (res.loc.success) {
        m_baseInputText = res.base_input_text;
        m_baseAddress = res.base_address;
        m_baseLat = res.base_lat;
        m_baseLon = res.base_lon;
        m_isFineTuned = res.is_fine_tuned;

        if (m_isFineTuned) {
            m_fineTunedCoordText = wxString::Format(wxT("%.6f, %.6f"), res.loc.lat, res.loc.lon);
        } else {
            m_fineTunedCoordText.Clear();
        }
        m_btnResetPos->Enable(m_isFineTuned);
    }

    UpdateUiWithResult(res.loc, res.tile, res.jma_res, res.height_agl_m, res.year, res.month, res.day);
}

void MainFrame::UpdateAddressLabel() {
    if (!m_lastLoc.success) {
        m_lblAddress->SetLabel(wxT("-"));
        m_lblAddress->SetToolTip(wxString());
    } else {
        wxString base = wxString::FromUTF8(m_baseAddress.empty() ? m_lastLoc.resolved_address : m_baseAddress);
        wxString text = m_isFineTuned ? (base + wxT(" (位置微修正済み)")) : base;
        m_lblAddress->SetLabel(text);
        m_lblAddress->SetToolTip(text);
    }
    if (m_lblAddress->GetParent()) m_lblAddress->GetParent()->Layout();
}

void MainFrame::UpdateElevationLabel() {
    if (!m_lastLoc.success) {
        m_lblElevation->SetLabel(wxT("-"));
        m_lblElevation->SetToolTip(wxString());
    } else {
        double total_alt = m_lastLoc.elevation + m_lastHeightAgl;
        wxString text;
        if (m_lastHeightAgl > 0.0) {
            text = wxString::Format(
                wxT("地盤 %.1f m + 地上高 %.1f m = 海抜 %.1f m (%s)"),
                m_lastLoc.elevation, m_lastHeightAgl, total_alt, wxString::FromUTF8(m_lastLoc.hsrc));
        } else {
            text = wxString::Format(
                wxT("地盤 %.1f m (地上高 0.0 m / %s)"),
                m_lastLoc.elevation, wxString::FromUTF8(m_lastLoc.hsrc));
        }
        m_lblElevation->SetLabel(text);
        m_lblElevation->SetToolTip(text);
    }
    if (m_lblElevation->GetParent()) m_lblElevation->GetParent()->Layout();
}

void MainFrame::UpdateStationLabels() {
    if (!m_lastJma.success) {
        m_lblStation->SetLabel(wxT("-"));
        m_lblStation->SetToolTip(wxString());
        m_lblDistance->SetLabel(wxT("-"));
        return;
    }
    wxString stnText = wxString::Format(wxT("%s (%s / 標高 %.1fm)"),
        wxString::FromUTF8(m_lastJma.station.name),
        wxString::FromUTF8(m_lastJma.station.pref),
        m_lastJma.station.alt);
    m_lblStation->SetLabel(stnText);
    m_lblStation->SetToolTip(stnText);
    m_lblDistance->SetLabel(wxString::Format(wxT("約 %.2f km"), m_lastJma.distance_km));
}

void MainFrame::UpdateSpatialSummaryLabel() {
    if (!m_lblSpatialSummary) return;
    if (!m_lastJma.success) {
        m_lblSpatialSummary->SetLabel(wxT("-"));
        m_lblSpatialSummary->SetToolTip(wxString());
        return;
    }

    const HourlyWeather* latest = JmaService::FindLatestValidSpatialHour(m_lastJma);
    if (!latest || !latest->spatial.valid) {
        wxString text = wxT("単点換算 (SingleNearest / 空間補間データなし)");
        m_lblSpatialSummary->SetLabel(text);
        m_lblSpatialSummary->SetToolTip(text);
        if (m_lblSpatialSummary->GetParent()) m_lblSpatialSummary->GetParent()->Layout();
        return;
    }

    const SpatialPressureResult& sp = latest->spatial;
    wxString methodStr = wxString::FromUTF8(SpatialPressureCalculator::MethodToString(sp.method_used));
    wxString corrStr = latest->has_spatial_correction
        ? wxString::Format(wxT("%+.2f hPa"), latest->spatial_correction)
        : wxT("-");

    wxString gradSummary;
    if (sp.has_gradient) {
        double grad_mag = std::hypot(sp.pressure_gradient_x, sp.pressure_gradient_y);
        gradSummary = wxString::Format(wxT(" / 勾配 %.3f hPa/km"), grad_mag);
    }

    wxString summary = wxString::Format(
        wxT("%s (%s %zu地点 / 推定P0 %.1f / 現地T %.1f℃%s)"),
        corrStr,
        methodStr,
        sp.stations_used.size(),
        sp.interpolated_sea_level_pressure,
        sp.estimated_temp,
        gradSummary);

    wxString tip = wxString::Format(
        wxT("対象時刻: %s / 補間手法: %s (%zu地点)\n空間補間補正(対単点): %s\n推定海面気圧: %.2f hPa / 推定現地気温: %.2f ℃"),
        wxString::FromUTF8(latest->FormatTimeHHMM()),
        methodStr,
        sp.stations_used.size(),
        corrStr,
        sp.interpolated_sea_level_pressure,
        sp.estimated_temp);
    if (sp.has_gradient) {
        tip += wxString::Format(wxT("\n気圧傾度 (東西/南北): %+.4f / %+.4f hPa/km"),
            sp.pressure_gradient_x, sp.pressure_gradient_y);
    }
    if (!sp.stations_used.empty()) {
        tip += wxT("\n使用観測所:");
        for (const auto& st : sp.stations_used) {
            tip += wxString::Format(wxT("\n  %s: 距離 %.1f km / 重み %.3f / 海面 %.1f hPa"),
                wxString::FromUTF8(st.name), st.distance_km, st.weight, st.sea_level_pressure);
        }
    }

    m_lblSpatialSummary->SetLabel(summary);
    m_lblSpatialSummary->SetToolTip(tip);
    if (m_lblSpatialSummary->GetParent()) m_lblSpatialSummary->GetParent()->Layout();
}

void MainFrame::UpdatePressureSummaryLabel() {
    if (!m_lastJma.success) {
        m_lblPressureSummary->SetLabel(wxT("-"));
        m_lblPressureSummary->SetToolTip(wxString());
        return;
    }
    PressureStats stats = JmaService::ComputePressureStats(m_lastJma);
    if (!stats.has_valid) {
        m_lblPressureSummary->SetLabel(wxT("欠測のみ"));
        m_lblPressureSummary->SetToolTip(wxString());
        return;
    }
    wxString summary = wxString::Format(
        wxT("%.1f hPa (%s) / 最高 %.1f (%s) 〜 最低 %.1f (%s)"),
        stats.latest_press, wxString::FromUTF8(stats.latest_time),
        stats.max_press, wxString::FromUTF8(stats.max_time),
        stats.min_press, wxString::FromUTF8(stats.min_time));
    wxString tip = summary;
    const HourlyWeather* latestSp = JmaService::FindLatestValidSpatialHour(m_lastJma);
    if (latestSp && latestSp->has_spatial_correction) {
        tip += wxString::Format(wxT("\n最新時刻(%s): 単点換算 %.2f hPa → 空間補間後 %.2f hPa (補正 %+.2f hPa)"),
            wxString::FromUTF8(latestSp->FormatTimeHHMM()),
            latestSp->single_target_press,
            latestSp->target_press,
            latestSp->spatial_correction);
    }
    m_lblPressureSummary->SetLabel(summary);
    m_lblPressureSummary->SetToolTip(tip);
    if (m_lblPressureSummary->GetParent()) m_lblPressureSummary->GetParent()->Layout();
}

void MainFrame::ClearWeatherSummaryUi() {
    m_lblStation->SetLabel(wxT("-"));
    m_lblStation->SetToolTip(wxString());
    m_lblDistance->SetLabel(wxT("-"));
    if (m_lblSpatialSummary) {
        m_lblSpatialSummary->SetLabel(wxT("-"));
        m_lblSpatialSummary->SetToolTip(wxString());
    }
    m_lblPressureSummary->SetLabel(wxT("-"));
    m_lblPressureSummary->SetToolTip(wxString());
}

void MainFrame::UpdateStatusBarInfo(const wxString& mainText, bool updateRightPane) {
    SetStatusText(mainText, 0);
    if (updateRightPane) {
        wxString rightText;
        if (m_lastLoc.success && m_lastJma.success) {
            if (m_lastJma.is_realtime && !m_lastJma.latest_time_str.empty()) {
                rightText = wxString::Format(
                    wxT("%04d-%02d-%02d %s時点 / 海抜 %.1fm"),
                    m_lastJma.year, m_lastJma.month, m_lastJma.day,
                    wxString::FromUTF8(m_lastJma.latest_time_str),
                    m_lastLoc.elevation + m_lastHeightAgl);
            } else {
                rightText = wxString::Format(
                    wxT("%04d-%02d-%02d / 海抜 %.1fm"),
                    m_lastJma.year, m_lastJma.month, m_lastJma.day,
                    m_lastLoc.elevation + m_lastHeightAgl);
            }
        } else if (m_lastLoc.success) {
            rightText = wxString::Format(wxT("海抜 %.1fm"), m_lastLoc.elevation + m_lastHeightAgl);
        }
        SetStatusText(rightText, 1);
    }
}

void MainFrame::PopulateHourlyList(bool select_latest_on_realtime) {
    if (!m_listHourly) return;

    long prev_sel = m_listHourly->GetNextItem(-1, wxLIST_NEXT_ALL, wxLIST_STATE_SELECTED);
    long prev_top = m_listHourly->GetTopItem();
    int per_page = m_listHourly->GetCountPerPage();

    wxColour bg = m_listHourly->GetBackgroundColour();
    bool isDark = bg.IsOk() && ((static_cast<int>(bg.Red()) + bg.Green() + bg.Blue()) < 384);
    wxColour zebraBg = isDark ? wxColor(42, 46, 54) : wxColor(246, 248, 251);
    wxColour latestBg = isDark ? wxColor(38, 60, 88) : wxColor(230, 241, 254);
    wxColour mutedFg = isDark ? wxColor(135, 135, 135) : wxColor(150, 150, 150);
    wxFont boldFont = m_listHourly->GetFont();
    boldFont.MakeBold();

    m_listHourly->Freeze();
    m_listHourly->DeleteAllItems();
    const size_t total = m_lastJma.hours.size();
    for (size_t i = 0; i < total; ++i) {
        const auto& h = m_lastJma.hours[i];
        long idx = m_listHourly->InsertItem(i, wxString::FromUTF8(h.FormatTimeHHMM()));

        m_listHourly->SetItem(idx, 1, wxString::FromUTF8(h.weather));

        if (h.temp > -900.0) {
            m_listHourly->SetItem(idx, 2, wxString::Format(wxT("%.1f"), h.temp));
        } else {
            m_listHourly->SetItem(idx, 2, wxT("-"));
        }

        if (h.humidity > -900.0) {
            m_listHourly->SetItem(idx, 3, wxString::Format(wxT("%.0f"), h.humidity));
        } else {
            m_listHourly->SetItem(idx, 3, wxT("-"));
        }

        if (h.station_press > -900.0) {
            m_listHourly->SetItem(idx, 4, wxString::Format(wxT("%.1f"), h.station_press));
        } else {
            m_listHourly->SetItem(idx, 4, wxT("-"));
        }

        if (h.sea_press > -900.0) {
            m_listHourly->SetItem(idx, 5, wxString::Format(wxT("%.1f"), h.sea_press));
        } else {
            m_listHourly->SetItem(idx, 5, wxT("-"));
        }

        if (h.target_press > -900.0) {
            m_listHourly->SetItem(idx, 6, wxString::Format(wxT("%.1f"), h.target_press));
        } else {
            m_listHourly->SetItem(idx, 6, wxT("-"));
        }

        if (h.has_press_diff) {
            m_listHourly->SetItem(idx, 7, wxString::Format(wxT("%+.1f"), h.press_diff));
        } else {
            m_listHourly->SetItem(idx, 7, wxT("-"));
        }

        bool isLatestRow = (m_lastJma.is_realtime && i + 1 == total);
        if (isLatestRow) {
            m_listHourly->SetItemBackgroundColour(idx, latestBg);
            m_listHourly->SetItemFont(idx, boldFont);
        } else if (i % 2 == 1) {
            m_listHourly->SetItemBackgroundColour(idx, zebraBg);
        }
        if (h.target_press <= -900.0) {
            m_listHourly->SetItemTextColour(idx, mutedFg);
        }
    }
    m_listHourly->Thaw();

    long count = m_listHourly->GetItemCount();
    if (count > 0) {
        if (select_latest_on_realtime && m_lastJma.is_realtime) {
            m_listHourly->SetItemState(count - 1, wxLIST_STATE_SELECTED | wxLIST_STATE_FOCUSED, wxLIST_STATE_SELECTED | wxLIST_STATE_FOCUSED);
            m_listHourly->EnsureVisible(count - 1);
        } else if (prev_sel >= 0 && prev_sel < count) {
            m_listHourly->SetItemState(prev_sel, wxLIST_STATE_SELECTED | wxLIST_STATE_FOCUSED, wxLIST_STATE_SELECTED | wxLIST_STATE_FOCUSED);
            if (prev_top >= 0 && prev_top < count) {
                long bottom = std::min<long>(count - 1, prev_top + std::max(0, per_page - 1));
                m_listHourly->EnsureVisible(bottom);
                m_listHourly->EnsureVisible(prev_top);
            }
        }
    }
}

void MainFrame::UpdateUiWithResult(
    const LocationInfo& loc,
    const TileInfo& tile,
    const JmaDailyResult& jma_res,
    double height_agl_m,
    int req_year,
    int req_month,
    int req_day
) {
    if (!loc.success) {
        m_lastLoc = LocationInfo{};
        m_lastJma = JmaDailyResult{};
        m_isFineTuned = false;
        m_fineTunedCoordText.Clear();
        m_btnResetPos->Enable(false);
        m_mapPanel->ClearMap();
        UpdateAddressLabel();
        m_lblCoords->SetLabel(wxT("-"));
        m_lblCoords->SetToolTip(wxString());
        m_btnCopyCoords->Enable(false);
        UpdateElevationLabel();
        ClearWeatherSummaryUi();
        m_listHourly->DeleteAllItems();
        SetExportControlsEnabled(false);
        UpdateStatusBarInfo(wxT("照会エラー"), true);
        wxMessageBox(wxString::FromUTF8(loc.error_message), wxT("国土地理院 照会エラー"), wxOK | wxICON_ERROR, this);
        return;
    }

    m_lastLoc = loc;
    m_lastJma = jma_res;
    m_lastHeightAgl = height_agl_m;

    m_mapPanel->SetMapTile(tile);

    UpdateAddressLabel();
    m_lblCoords->SetLabel(wxString::Format(wxT("北緯 %.6f, 東経 %.6f"), loc.lat, loc.lon));
    m_lblCoords->SetToolTip(wxString::Format(wxT("%.6f, %.6f"), loc.lat, loc.lon));
    m_btnCopyCoords->Enable(true);
    UpdateElevationLabel();

    if (!jma_res.success) {
        ClearWeatherSummaryUi();
        m_listHourly->DeleteAllItems();
        SetExportControlsEnabled(false);
        UpdateStatusBarInfo(wxT("気象庁データの取得に失敗しました"), true);
        wxDateTime curDate = m_datePicker->GetValue();
        if (curDate.IsValid() &&
            (curDate.GetYear() != req_year || (curDate.GetMonth() + 1) != req_month || curDate.GetDay() != req_day)) {
            StartWeatherOnlySearch();
            return;
        }
        wxMessageBox(wxString::FromUTF8(jma_res.error_message), wxT("気象庁データ取得エラー"), wxOK | wxICON_WARNING, this);
        return;
    }

    UpdateStationLabels();

    double current_spin = ReadSpinHeightAgl(false);
    if (std::abs(current_spin - m_lastHeightAgl) > 1e-6) {
        m_lastHeightAgl = current_spin;
        UpdateElevationLabel();
        JmaService::RecalculatePressures(m_lastJma, m_lastLoc.elevation + m_lastHeightAgl);
    }

    UpdateSpatialSummaryLabel();
    UpdatePressureSummaryLabel();
    PopulateHourlyList(m_lastJma.is_realtime);

    SetExportControlsEnabled(!m_lastJma.hours.empty());
    wxString suffix = BuildJmaStatusSuffix(m_lastJma);
    wxString modeText = m_lastJma.is_realtime ? wxT("AMeDAS速報・天候なし") : wxT("過去確定データ");
    UpdateStatusBarInfo(
        wxString::Format(wxT("照会完了 (%s%s / 最寄り官署: %s)"),
            modeText, suffix, wxString::FromUTF8(m_lastJma.station.name)),
        true
    );

    wxDateTime curDate = m_datePicker->GetValue();
    if (curDate.IsValid() &&
        (curDate.GetYear() != req_year || (curDate.GetMonth() + 1) != req_month || curDate.GetDay() != req_day)) {
        StartWeatherOnlySearch();
        return;
    }
    if (m_mapPanel->GetCurrentTileZoom() != m_currentZoom ||
        m_mapPanel->GetCurrentTileLayer() != GetSelectedLayerCode()) {
        m_zoomTimer.StartOnce(50);
    }
}

wxString MainFrame::GetPresetFilePath() const {
    wxFileName exeFn(wxStandardPaths::Get().GetExecutablePath());
    wxString exeDir = exeFn.GetPath();
    wxString exePreset = exeFn.GetPathWithSep() + wxT("presets.dat");
    if (wxFileExists(exePreset) || wxFileName::IsDirWritable(exeDir)) {
        return exePreset;
    }

    wxString userDir = wxStandardPaths::Get().GetUserDataDir();
    wxFileName::Mkdir(userDir, wxS_DIR_DEFAULT, wxPATH_MKDIR_FULL);
    return userDir + wxFileName::GetPathSeparator() + wxT("presets.dat");
}

void MainFrame::LoadPresets() {
    m_presets.clear();
    wxString path = GetPresetFilePath();
    if (wxFileExists(path)) {
        wxFileInputStream in(path);
        if (in.IsOk()) {
            std::string raw;
            char buf[4096];
            while (!in.Eof()) {
                in.Read(buf, sizeof(buf));
                size_t n = in.LastRead();
                if (n == 0) break;
                raw.append(buf, n);
            }
            m_presets = PresetStore::Deserialize(raw);
        }
    }
    RefreshPresetChoice(0);
}

void MainFrame::SavePresets() const {
    wxString path = GetPresetFilePath();
    wxString tmpPath = path + wxT(".tmp");
    std::string serialized = PresetStore::Serialize(m_presets);
    bool writeOk = false;
    {
        wxFileOutputStream out(tmpPath);
        if (out.IsOk()) {
            out.Write(serialized.data(), serialized.size());
            if (out.LastWrite() == serialized.size() && out.Close()) {
                writeOk = true;
            }
        }
    }
    if (!writeOk || !wxRenameFile(tmpPath, path, true)) {
        if (wxFileExists(tmpPath)) {
            wxRemoveFile(tmpPath);
        }
        wxMessageBox(wxT("プリセットファイルの保存に失敗しました。"), wxT("保存エラー"), wxOK | wxICON_WARNING, const_cast<MainFrame*>(this));
    }
}

void MainFrame::RefreshPresetChoice(int select_choice_index) {
    m_choicePreset->Clear();
    m_choicePreset->Append(wxT("(登録地点を選択...)"));
    for (const auto& p : m_presets) {
        wxString label = wxString::FromUTF8(p.name);
        if (p.height_agl_m > 0.0) {
            label += wxString::Format(wxT(" [地上高 %.1fm]"), p.height_agl_m);
        }
        m_choicePreset->Append(label);
    }
    if (select_choice_index >= 0 && static_cast<unsigned int>(select_choice_index) < m_choicePreset->GetCount()) {
        m_choicePreset->SetSelection(select_choice_index);
    } else {
        m_choicePreset->SetSelection(0);
    }
    m_btnDelPreset->Enable(m_choicePreset->GetSelection() > 0);
}

void MainFrame::OnPresetSelected(wxCommandEvent& WXUNUSED(event)) {
    int sel = m_choicePreset->GetSelection();
    m_btnDelPreset->Enable(sel > 0);
    if (sel <= 0 || static_cast<size_t>(sel - 1) >= m_presets.size()) {
        return;
    }

    const PresetLocation p = m_presets[static_cast<size_t>(sel - 1)];
    m_zoomTimer.Stop();

    m_spinHeightAgl->SetValue(p.height_agl_m);
    m_lastHeightAgl = p.height_agl_m;
    m_currentZoom = std::clamp(p.zoom, 10, 18);
    UpdateZoomControls();

    if (p.is_fine_tuned) {
        wxString coordStr = wxString::Format(wxT("%.6f, %.6f"), p.lat, p.lon);
        m_txtLocation->ChangeValue(coordStr);
    } else {
        m_txtLocation->ChangeValue(wxString::FromUTF8(p.input_text));
    }

    wxDateTime date = m_datePicker->GetValue();
    wxSize mapSz = GetCurrentMapSize();

    WorkerJob job;
    job.kind = JobKind::FullSearch;
    job.input_location = p.input_text;
    job.has_direct_coords = true;
    job.lat = p.lat;
    job.lon = p.lon;
    job.base_input_text = p.input_text;
    job.base_address = p.base_address.empty() ? p.input_text : p.base_address;
    job.base_lat = p.base_lat;
    job.base_lon = p.base_lon;
    job.is_fine_tuned = p.is_fine_tuned;
    job.from_preset = true;
    job.year = date.GetYear();
    job.month = date.GetMonth() + 1;
    job.day = date.GetDay();
    job.height_agl_m = p.height_agl_m;
    job.zoom = m_currentZoom;
    job.layer = GetSelectedLayerCode();
    job.map_w = mapSz.GetWidth();
    job.map_h = mapSz.GetHeight();

    m_dataJobInFlight = true;
    m_activeDataKind = JobKind::FullSearch;
    SetSearchBusy(true);
    SetExportControlsEnabled(false);
    UpdateStatusBarInfo(wxString::Format(wxT("プリセット「%s」を読み込んでいます..."), wxString::FromUTF8(p.name)), false);

    EnqueueJob(std::move(job));
}

void MainFrame::OnAddPreset(wxCommandEvent& WXUNUSED(event)) {
    if (!m_lastLoc.success) {
        wxMessageBox(wxT("先に「照会・換算実行」で現在地を取得してから登録してください。"), wxT("地点登録"), wxOK | wxICON_INFORMATION, this);
        return;
    }

    double cur_height = ReadSpinHeightAgl(true);
    wxString defaultName = wxString::FromUTF8(m_baseAddress.empty() ? m_lastLoc.resolved_address : m_baseAddress);
    if (cur_height > 0.0) {
        defaultName += wxString::Format(wxT(" (%.0fm)"), cur_height);
    }

    wxTextEntryDialog dlg(this, wxT("登録するプリセット名を入力してください:"), wxT("現在地のプリセット登録"), defaultName);
    if (dlg.ShowModal() != wxID_OK) return;

    wxString nameWx = dlg.GetValue().Trim().Trim(false);
    if (nameWx.IsEmpty()) {
        wxMessageBox(wxT("登録名を入力してください。"), wxT("入力エラー"), wxOK | wxICON_WARNING, this);
        return;
    }

    std::string nameUtf8 = std::string(nameWx.ToUTF8());
    bool exists = false;
    for (const auto& existing : m_presets) {
        if (existing.name == nameUtf8) {
            exists = true;
            break;
        }
    }
    if (exists) {
        int ans = wxMessageBox(
            wxString::Format(wxT("同名の地点「%s」がすでに登録されています。上書きしますか？"), nameWx),
            wxT("上書き確認"),
            wxYES_NO | wxICON_QUESTION,
            this
        );
        if (ans != wxYES) return;
    }

    PresetLocation item;
    item.name = nameUtf8;
    item.input_text = m_baseInputText.empty() ? std::string(m_txtLocation->GetValue().ToUTF8()) : m_baseInputText;
    item.base_address = m_baseAddress.empty() ? m_lastLoc.resolved_address : m_baseAddress;
    item.is_fine_tuned = m_isFineTuned;
    item.lat = m_lastLoc.lat;
    item.lon = m_lastLoc.lon;
    item.base_lat = (m_baseLat != 0.0 || m_baseLon != 0.0) ? m_baseLat : m_lastLoc.lat;
    item.base_lon = (m_baseLat != 0.0 || m_baseLon != 0.0) ? m_baseLon : m_lastLoc.lon;
    item.height_agl_m = cur_height;
    item.zoom = m_currentZoom;

    PresetStore::Upsert(m_presets, item);
    SavePresets();

    int newIdx = 0;
    for (size_t i = 0; i < m_presets.size(); ++i) {
        if (m_presets[i].name == item.name) {
            newIdx = static_cast<int>(i + 1);
            break;
        }
    }
    RefreshPresetChoice(newIdx);
    SetStatusText(wxString::Format(wxT("現在地をプリセット「%s」として登録しました"), nameWx));
}

void MainFrame::OnDeletePreset(wxCommandEvent& WXUNUSED(event)) {
    int sel = m_choicePreset->GetSelection();
    if (sel <= 0 || static_cast<size_t>(sel - 1) >= m_presets.size()) return;

    wxString targetName = wxString::FromUTF8(m_presets[static_cast<size_t>(sel - 1)].name);
    int ans = wxMessageBox(
        wxString::Format(wxT("登録地点「%s」を削除しますか？"), targetName),
        wxT("削除確認"),
        wxYES_NO | wxICON_QUESTION,
        this
    );
    if (ans != wxYES) return;

    PresetStore::RemoveByIndex(m_presets, static_cast<size_t>(sel - 1));
    SavePresets();
    RefreshPresetChoice(0);
    SetStatusText(wxString::Format(wxT("プリセット「%s」を削除しました"), targetName));
}

const PresetLocation* MainFrame::GetSelectedPreset() const {
    if (!m_choicePreset) return nullptr;
    int sel = m_choicePreset->GetSelection();
    if (sel <= 0 || static_cast<size_t>(sel - 1) >= m_presets.size()) {
        return nullptr;
    }
    return &m_presets[static_cast<size_t>(sel - 1)];
}

void MainFrame::OnPresetContextMenu(wxContextMenuEvent& WXUNUSED(event)) {
    const PresetLocation* preset = GetSelectedPreset();
    if (!preset) return;

    std::string addr = !preset->base_address.empty() ? preset->base_address : preset->input_text;
    wxMenu menu;
    menu.Append(ID_MENU_COPY_PRESET_DISPLAY, wxT("プルダウン表示テキストをコピー"));
    menu.Append(ID_MENU_COPY_PRESET_ADDRESS, wxT("住所/入力値をコピー"));
    menu.Enable(ID_MENU_COPY_PRESET_ADDRESS, !addr.empty());
    m_choicePreset->PopupMenu(&menu);
}

void MainFrame::OnCopyPresetDisplayText(wxCommandEvent& WXUNUSED(event)) {
    if (!GetSelectedPreset()) return;
    wxString displayText = m_choicePreset->GetStringSelection();
    if (displayText.IsEmpty()) return;

    CopyTextToClipboard(displayText, wxT("登録地点の表示テキストをクリップボードにコピーしました"));
}

void MainFrame::OnCopyPresetAddress(wxCommandEvent& WXUNUSED(event)) {
    const PresetLocation* preset = GetSelectedPreset();
    if (!preset) return;

    std::string addr = !preset->base_address.empty() ? preset->base_address : preset->input_text;
    if (addr.empty()) return;

    CopyTextToClipboard(wxString::FromUTF8(addr), wxT("登録地点の住所/入力値をクリップボードにコピーしました"));
}

void MainFrame::SetExportControlsEnabled(bool enabled) {
    m_btnExport->Enable(enabled);
    m_btnCopyList->Enable(enabled);
}

bool MainFrame::SyncHeightAglIfNeeded() {
    double current_spin = ReadSpinHeightAgl(true);
    if (m_lastLoc.success && std::abs(current_spin - m_lastHeightAgl) > 1e-6) {
        m_lastHeightAgl = current_spin;
        UpdateElevationLabel();
        if (m_lastJma.success && !m_lastJma.hours.empty()) {
            JmaService::RecalculatePressures(m_lastJma, m_lastLoc.elevation + m_lastHeightAgl);
            UpdatePressureSummaryLabel();
            PopulateHourlyList(false);
        }
        wxString prevMain = GetStatusBar() ? GetStatusBar()->GetStatusText(0) : wxString();
        UpdateStatusBarInfo(prevMain, true);
        return true;
    }
    return false;
}

wxString MainFrame::BuildListRowTsv(long row) const {
    wxString line;
    int colCount = m_listHourly->GetColumnCount();
    for (int col = 0; col < colCount; ++col) {
        if (col > 0) line += wxT("\t");
        line += m_listHourly->GetItemText(row, col);
    }
    return line;
}

bool MainFrame::CopyTextToClipboard(const wxString& text, const wxString& statusMessage) {
    if (!wxTheClipboard->Open()) {
        wxMessageBox(wxT("クリップボードを開けませんでした。"), wxT("コピーエラー"), wxOK | wxICON_WARNING, this);
        return false;
    }
    bool ok = wxTheClipboard->SetData(new wxTextDataObject(text));
    wxTheClipboard->Flush();
    wxTheClipboard->Close();
    if (!ok) {
        wxMessageBox(wxT("クリップボードへのコピーに失敗しました。"), wxT("コピーエラー"), wxOK | wxICON_WARNING, this);
        return false;
    }
    UpdateStatusBarInfo(statusMessage, false);
    return true;
}

void MainFrame::OnListContextMenu(wxContextMenuEvent& event) {
    if (!m_listHourly || m_listHourly->GetItemCount() == 0) return;

    wxPoint pt = event.GetPosition();
    if (pt != wxDefaultPosition) {
        wxPoint clientPt = m_listHourly->ScreenToClient(pt);
        int flags = 0;
        long hitItem = m_listHourly->HitTest(clientPt, flags);
        if (hitItem == wxNOT_FOUND || !(flags & wxLIST_HITTEST_ONITEM)) {
            return;
        }
        m_listHourly->SetItemState(hitItem, wxLIST_STATE_SELECTED | wxLIST_STATE_FOCUSED, wxLIST_STATE_SELECTED | wxLIST_STATE_FOCUSED);
    }

    long sel = m_listHourly->GetNextItem(-1, wxLIST_NEXT_ALL, wxLIST_STATE_SELECTED);
    if (sel == -1) return;

    wxString targetPress = m_listHourly->GetItemText(sel, 6);
    wxMenu menu;
    menu.Append(ID_MENU_COPY_ROW, wxT("行データ全体をコピー (Ctrl+C)"));
    menu.Append(ID_MENU_COPY_TARGET_PRESS, wxT("現在地換算気圧をコピー (ダブルクリック)"));
    menu.Enable(ID_MENU_COPY_TARGET_PRESS, !targetPress.IsEmpty() && targetPress != wxT("-"));
    m_listHourly->PopupMenu(&menu);
}

void MainFrame::OnListKeyDown(wxKeyEvent& event) {
    if (event.ControlDown() && !event.AltDown() && (event.GetKeyCode() == 'C' || event.GetKeyCode() == 'c')) {
        if (!m_listHourly || m_listHourly->GetItemCount() == 0) return;
        long sel = m_listHourly->GetNextItem(-1, wxLIST_NEXT_ALL, wxLIST_STATE_SELECTED);
        if (sel != -1) {
            wxString rowText = BuildListRowTsv(sel);
            wxString timeText = m_listHourly->GetItemText(sel, 0);
            CopyTextToClipboard(rowText, wxString::Format(wxT("%s の行データをクリップボードにコピーしました"), timeText));
        } else {
            wxCommandEvent dummy;
            OnCopyList(dummy);
        }
        return;
    }
    event.Skip();
}

void MainFrame::OnListItemActivated(wxListEvent& event) {
    if (!m_listHourly) return;
    long sel = event.GetIndex();
    if (sel < 0 || sel >= m_listHourly->GetItemCount()) return;

    wxString timeText = m_listHourly->GetItemText(sel, 0);
    wxString pressText = m_listHourly->GetItemText(sel, 6);
    if (!pressText.IsEmpty() && pressText != wxT("-")) {
        CopyTextToClipboard(pressText, wxString::Format(wxT("%s の現在地換算気圧 (%s hPa) をクリップボードにコピーしました"), timeText, pressText));
    } else {
        wxString rowText = BuildListRowTsv(sel);
        CopyTextToClipboard(rowText, wxString::Format(wxT("%s の行データをクリップボードにコピーしました"), timeText));
    }
}

void MainFrame::OnCopySelectedRow(wxCommandEvent& WXUNUSED(event)) {
    if (!m_listHourly) return;
    long sel = m_listHourly->GetNextItem(-1, wxLIST_NEXT_ALL, wxLIST_STATE_SELECTED);
    if (sel == -1) return;

    wxString rowText = BuildListRowTsv(sel);
    wxString timeText = m_listHourly->GetItemText(sel, 0);
    CopyTextToClipboard(rowText, wxString::Format(wxT("%s の行データをクリップボードにコピーしました"), timeText));
}

void MainFrame::OnCopySelectedTargetPressure(wxCommandEvent& WXUNUSED(event)) {
    if (!m_listHourly) return;
    long sel = m_listHourly->GetNextItem(-1, wxLIST_NEXT_ALL, wxLIST_STATE_SELECTED);
    if (sel == -1) return;

    wxString pressText = m_listHourly->GetItemText(sel, 6);
    if (pressText.IsEmpty() || pressText == wxT("-")) return;

    wxString timeText = m_listHourly->GetItemText(sel, 0);
    CopyTextToClipboard(pressText, wxString::Format(wxT("%s の現在地換算気圧 (%s hPa) をクリップボードにコピーしました"), timeText, pressText));
}

void MainFrame::OnCopyList(wxCommandEvent& WXUNUSED(event)) {
    if (!m_lastJma.success || m_lastJma.hours.empty() || !m_listHourly) return;

    SyncHeightAglIfNeeded();

    int rowCount = m_listHourly->GetItemCount();
    int colCount = m_listHourly->GetColumnCount();
    if (rowCount == 0 || colCount == 0) return;

    wxString tsv;
    for (int col = 0; col < colCount; ++col) {
        if (col > 0) tsv += wxT("\t");
        wxListItem colInfo;
        colInfo.SetMask(wxLIST_MASK_TEXT);
        if (m_listHourly->GetColumn(col, colInfo)) {
            tsv += colInfo.GetText();
        }
    }
    for (int row = 0; row < rowCount; ++row) {
        tsv += wxT("\r\n");
        tsv += BuildListRowTsv(row);
    }

    CopyTextToClipboard(tsv, wxT("リスト全体をクリップボードにコピーしました"));
}

void MainFrame::OnExportCsv(wxCommandEvent& WXUNUSED(event)) {
    if (!m_lastJma.success || m_lastJma.hours.empty()) return;

    SyncHeightAglIfNeeded();

    wxFileDialog saveFileDialog(
        this,
        wxT("気象・換算気圧データの保存"),
        wxT(""),
        wxString::Format(wxT("weather_elevation_%04d%02d%02d.csv"), m_lastJma.year, m_lastJma.month, m_lastJma.day),
        wxT("CSVファイル (*.csv)|*.csv"),
        wxFD_SAVE | wxFD_OVERWRITE_PROMPT
    );

    if (saveFileDialog.ShowModal() == wxID_CANCEL) return;

    wxFileOutputStream outputStream(saveFileDialog.GetPath());
    if (!outputStream.IsOk()) {
        wxMessageBox(wxT("ファイルの作成に失敗しました。"), wxT("保存エラー"), wxOK | wxICON_ERROR, this);
        return;
    }

    const PresetLocation* preset = GetSelectedPreset();
    std::string preset_name = preset ? preset->name : "-";

    std::string export_addr = m_baseAddress.empty() ? m_lastLoc.resolved_address : m_baseAddress;
    if (m_isFineTuned) {
        export_addr += " (位置微修正済み)";
    }

    std::ostringstream ss;
    // UTF-8 BOM
    ss << "\xEF\xBB\xBF";
    ss << "# 登録地点名," << CsvEscape(preset_name) << "\n";
    ss << "# 対象日," << m_lastJma.year << "/" << m_lastJma.month << "/" << m_lastJma.day << "\n";
    ss << "# 指定場所," << CsvEscape(export_addr) << "\n";
    ss << std::fixed << std::setprecision(6);
    ss << "# 緯度経度," << m_lastLoc.lat << "," << m_lastLoc.lon << "\n";
    ss << std::fixed << std::setprecision(1);
    ss << "# 地盤標高(m)," << m_lastLoc.elevation << "\n";
    ss << "# 地上高(m)," << m_lastHeightAgl << "\n";
    ss << "# 換算標高(海抜m)," << (m_lastLoc.elevation + m_lastHeightAgl) << "\n";
    std::string stn_info = std::string(m_lastJma.station.name) + "(" + m_lastJma.station.pref + ")";
    ss << "# 最寄り官署," << CsvEscape(stn_info) << ",距離(km)," << std::setprecision(2) << m_lastJma.distance_km << "\n";
    const HourlyWeather* latestSp = JmaService::FindLatestValidSpatialHour(m_lastJma);
    if (latestSp && latestSp->spatial.valid) {
        ss << "# 空間補間手法," << SpatialPressureCalculator::MethodToString(latestSp->spatial.method_used)
           << ",使用地点数," << latestSp->spatial.stations_used.size() << "\n";
        ss << "# 空間補間最新対象時刻," << latestSp->FormatTimeHHMM()
           << ",推定海面気圧(hPa)," << std::setprecision(2) << latestSp->spatial.interpolated_sea_level_pressure
           << ",推定現地気温(℃)," << std::setprecision(1) << latestSp->spatial.estimated_temp;
        if (latestSp->has_spatial_correction) {
            ss << ",補正値(hPa)," << std::setprecision(2) << latestSp->spatial_correction;
        }
        ss << "\n";
        if (latestSp->spatial.has_gradient) {
            ss << std::setprecision(4)
               << "# 気圧傾度(東西/南北 hPa/km)," << latestSp->spatial.pressure_gradient_x
               << "," << latestSp->spatial.pressure_gradient_y << "\n";
        }
    }
    ss << "\n";

    ss << "時刻,天候,気温(℃),湿度(％),観測所気圧(hPa),海面気圧(hPa),現在地換算気圧(hPa),気圧差(hPa)\n";
    ss << std::fixed << std::setprecision(1);
    for (const auto& h : m_lastJma.hours) {
        ss << h.FormatTimeHHMM() << "," << CsvEscape(h.weather) << ",";
        if (h.temp > -900.0) ss << h.temp;
        ss << ",";
        if (h.humidity > -900.0) ss << std::setprecision(0) << h.humidity << std::setprecision(1);
        ss << ",";
        if (h.station_press > -900.0) ss << h.station_press;
        ss << ",";
        if (h.sea_press > -900.0) ss << h.sea_press;
        ss << ",";
        if (h.target_press > -900.0) ss << h.target_press;
        ss << ",";
        if (h.has_press_diff) ss << h.press_diff;
        ss << "\n";
    }

    std::string csv_data = ss.str();
    outputStream.Write(csv_data.data(), csv_data.size());
    if (outputStream.LastWrite() != csv_data.size() || !outputStream.Close()) {
        wxMessageBox(wxT("CSVファイルの書き込み中にエラーが発生しました。"), wxT("保存エラー"), wxOK | wxICON_ERROR, this);
        return;
    }

    wxMessageBox(wxT("CSVファイルを出力しました。"), wxT("完了"), wxOK | wxICON_INFORMATION, this);
}
