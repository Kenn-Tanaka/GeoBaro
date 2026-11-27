#include "help_dialog.h"
#include <wx/button.h>
#include <wx/display.h>
#include <wx/listbox.h>
#include <wx/sizer.h>
#include <wx/stattext.h>
#include <wx/textctrl.h>
#include <algorithm>
#include <iterator>

namespace {
struct HelpPage {
    HelpTopic topic;
    const wchar_t* title;
    const wchar_t* body;
};

constexpr HelpPage kPages[] = {
    {HelpTopic::GettingStarted, L"はじめての照会", LR"help(1. 「現在地」に日本国内の住所、または緯度・経度を入力します。座標は緯度が先です。例として 35.6814, 139.7670 を入力できます。

2. 「対象日」と「地上高」を指定します。地上高は地盤からの高さです。地表は 0.0 m、地盤から5 m上なら 5.0、地下5 mなら -5.0 と入力します。範囲は −2000.0〜2000.0 m です。

3. 「照会・換算実行」を押します。一覧の「★ 現在地換算気圧」に、指定地点・指定高度の推定気圧が hPa で表示されます。各行の時刻は日本標準時（JST）です。

照会後に地上高を変え、Enter を押すと、取得済みの観測値で再計算します。たとえば地盤標高22.4 mの場所で地上高を -5.3 m に設定すると、換算標高は海抜17.1 mです。

対象日は日付欄で選べます。「◀」「▶」で1日ずつ移動し、「今日」で当日のデータを照会します。地点の取得後に対象日を変えると、同じ地点の気象データを自動で照会します。

照会中に「中止」を押すと、照会前の入力や表示中の結果に対応する条件に戻ります。)help"},

    {HelpTopic::MapAndPresets, L"地図と登録地点", LR"help(地図上で地点を調整する

地図を左ボタンでドラッグするか、調べたい位置をダブルクリックします。選択地点が変わると標高と気圧が更新されます。「元位置へ」で元の検索地点に戻ります。「コピー」で現在地の座標をコピーできます。

地図種別から、標準地図・淡色地図・色別標高図を選べます。「+」「−」または地図上でのマウスホイールで拡大・縮小できます。色別標高図の Z16〜Z18 では Z15 の画像が拡大表示されます。

よく使う地点を登録する

1. 照会した地点の位置、地上高、地図倍率を調整します。
2. 「現在地を登録...」を押し、名前を入力して保存します。
3. 「登録地点」から名前を選ぶと、保存した位置・地上高・倍率が呼び出され、表示中の対象日で照会します。

登録を消すときは「選択地点を削除」を使います。プリセットはアプリを閉じても保存されます。)help"},

    {HelpTopic::Results, L"結果の見方と保存", LR"help(気圧の列を読む

・観測所気圧は、表示された気象官署が観測地点で測った気圧です。
・海面気圧は、その官署で観測した気圧を気象庁が海面の高さに換算した値です。
・★ 現在地換算気圧は、GeoBaroが指定地点と指定高度に対して求めた推定値です。
・気圧差は、現在地換算気圧から海面気圧の基準値を引いた値です。空間補間時は指定地点の推定海面気圧、単点換算時は官署の海面気圧が基準です。

空間補間補正と対象時刻

「空間補間補正」は、空間補間後の換算気圧から単点換算の値を引いた差です。サマリーには、最後に補間が成立した時刻の手法、使用地点数、推定海面気圧（P0）、推定現地気温（T）が表示されます。欄にマウスを重ねると対象時刻と使用地点を確認できます。平面近似が成立したときは、気圧傾度が hPa/km で表示されます。

結果をコピー・保存する

一覧で行を選んで Ctrl+C を押すと、その行をコピーできます。行をダブルクリックすると換算気圧だけをコピーできます。「クリップボードにコピー」は一覧全体をコピーします。

「CSVファイルとして保存...」で、地点、対象日、地盤標高・地上高・換算標高、時刻別データを保存できます。CSVの文字コードは UTF-8（BOM付き）です。欠測値は空欄になります。)help"},

    {HelpTopic::Calculation, L"計算の仕組み", LR"help(1. 換算標高を求める

国土地理院の地盤標高に入力した地上高を足して、換算標高 H（m）を求めます。地下の場合は地上高に負の値を使います。

2. 代表の気象官署を選ぶ

距離に、官署標高と地盤標高の差50 mあたり1 km相当のペナルティを加えて選びます。官署標高が不明な場合は気圧計高度を近似に使います。画面の距離は、標高差を加える前の直線距離です。

地点履歴のある期間は対象日の正午の位置で主官署を選び、各行の計算には観測時刻の地点情報を使います。過去日の24時には翌日0時の地点情報を使います。

3. 海面気圧と気温を空間補間する

各時刻に海面気圧・気温・官署標高がそろう100 km以内の地点から、近い順に最大6地点を使います。観測気温を標高1000 mあたり6.5 ℃の割合で海面の気温に換算し、距離 d（km）に対する重み 1 / (d + 1)^2 で平均します。近い地点ほど影響が大きくなります。

海面気圧は、4地点以上で地点の配置と外挿の条件を満たす場合に平面近似（PlaneFit）で推定します。条件を満たさない場合は距離に応じた加重平均（IDW）を使い、IDWで最寄りが0.1 km未満ならその地点の海面気圧を使います。1地点だけなら、その地点の海面気圧を使います（SingleNearest）。

4. 指定高度に換算する

推定海面気圧を P0（hPa）、海面基準気温を T0（K）とすると、換算標高 H での気圧 P は次式で求めます。

P = P0 × (1 − 0.0065 × H / T0)^5.257

たとえば P0 が1013.25 hPa、T0 が288.15 K（15 ℃）で H が17.1 mなら、P は約1011.2 hPaです。

空間補間が成立しない場合の単点換算

代表官署の観測所気圧を Ps、気圧計高度を hs とすると、P = Ps × (F(H) / F(hs))^5.257 で換算します。F(h) は 1 − 0.0065 × h / T0 です。観測所気圧や気圧計高度が欠ける場合は、利用できる海面気圧を基準にします。

代表官署の気温が一部欠測の場合は同日の前後または直近の気温を使います。気温を得られない場合や標高と気圧計高度の両方が不明な場合は、海面基準気温を15 ℃とします。

富士山頂付近など、代表官署が近く、海面気圧が欠測している過去観測では、条件に応じて単点換算を優先します。)help"},

    {HelpTopic::Troubleshooting, L"欠測・速報・推定の限界", LR"help(欠測と速報

一覧の「-」は、その項目の観測値または換算値が得られなかったことを示します。AMeDAS速報には天候の項目がないため、天候欄は「-」になります。

過去日は気象庁の「過去の気象データ検索」を使います。取得できなかったとき、気象庁の公開日を基準に直近9日以内なら、利用可能なAMeDAS速報を照会します。速報値は後日の確定値と異なる場合があります。日付が変わった直後など、当日のデータがまだ公開されていない場合は、公開後に再照会してください。

地点や気象データが取得できないとき

通信エラーの場合はネットワーク接続を確認し、「照会・換算実行」を押してください。住所が見つからない場合は、緯度・経度で指定できます。海上など、地盤標高データが得られない地点では換算できません。

1960年以前は、観測所の現在位置を使って対象日の官署を近似選定します。距離順に最大3官署を試すため、近隣で時別気圧を取得できない日もあります。

推定値の意味とデータ出典

現在地換算気圧は、周辺の観測値と地盤標高から求めた推定値です。観測地点が少ない場合や標高差が大きい場合は誤差が大きくなることがあります。地下室の換気・加圧や建物の密閉状態による気圧差は計算に含まれません。

住所・標高・地図は国土地理院、観測値と観測所の地点情報は気象庁のデータを使用しています。換算気圧は、それらをもとにGeoBaroが計算します。)help"},
};
} // namespace

HelpDialog::HelpDialog(wxWindow* parent)
    : wxDialog(parent, wxID_ANY, wxT("GeoBaro ヘルプ"), wxDefaultPosition, wxDefaultSize,
               wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER) {
    auto* layout = new wxBoxSizer(wxVERTICAL);
    auto* columns = new wxBoxSizer(wxHORIZONTAL);
    const int margin = FromDIP(12);

    m_topics = new wxListBox(this, wxID_ANY);
    m_topics->SetName(wxT("章一覧"));
    for (const auto& page : kPages) m_topics->Append(page.title);
    columns->Add(m_topics, 0, wxEXPAND | wxRIGHT, margin);

    auto* readingPane = new wxBoxSizer(wxVERTICAL);
    m_heading = new wxStaticText(this, wxID_ANY, wxEmptyString);
    m_heading->SetFont(m_heading->GetFont().Bold());
    readingPane->Add(m_heading, 0, wxEXPAND | wxBOTTOM, FromDIP(6));
    m_body = new wxTextCtrl(this, wxID_ANY, wxEmptyString, wxDefaultPosition, wxDefaultSize,
                           wxTE_MULTILINE | wxTE_READONLY | wxTE_BESTWRAP);
    m_body->SetName(wxT("本文"));
    auto readingFont = m_body->GetFont();
    readingFont.SetFractionalPointSize(readingFont.GetFractionalPointSize() + 1.0);
    m_body->SetFont(readingFont);
    readingPane->Add(m_body, 1, wxEXPAND);
    columns->Add(readingPane, 1, wxEXPAND);

    layout->Add(columns, 1, wxEXPAND | wxALL, margin);
    auto* closeButton = new wxButton(this, wxID_CLOSE, wxT("閉じる"));
    layout->Add(closeButton, 0, wxALIGN_RIGHT | wxLEFT | wxRIGHT | wxBOTTOM, margin);
    SetSizer(layout);
    SetEscapeId(wxID_CLOSE);

    m_topics->Bind(wxEVT_LISTBOX, [this](wxCommandEvent& event) {
        const int selection = event.GetSelection();
        if (selection != wxNOT_FOUND) SelectTopic(kPages[selection].topic);
    });
    closeButton->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { Close(); });
    Bind(wxEVT_CLOSE_WINDOW, [this](wxCloseEvent& event) {
        if (event.CanVeto()) {
            event.Veto();
            Hide();
        } else {
            event.Skip();
        }
    });

    const wxSize workArea = wxDisplay(parent).GetClientArea().GetSize();
    const wxSize minimum = FromDIP(wxSize(520, 310));
    const wxSize preferred = FromDIP(wxSize(790, 545));
    SetMinSize(wxSize(std::min(minimum.x, workArea.x), std::min(minimum.y, workArea.y)));
    SetSize(wxSize(std::min(preferred.x, workArea.x), std::min(preferred.y, workArea.y)));
    CentreOnParent();
    SelectTopic(HelpTopic::GettingStarted);
}

void HelpDialog::SelectTopic(HelpTopic topic) {
    for (int i = 0; i < static_cast<int>(std::size(kPages)); ++i) {
        const auto& page = kPages[i];
        if (page.topic != topic) continue;
        m_topics->SetSelection(i);
        m_heading->SetLabel(page.title);
        m_body->ChangeValue(page.body);
        m_body->SetInsertionPoint(0);
        Layout();
        return;
    }
}
