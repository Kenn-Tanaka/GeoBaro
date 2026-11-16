#include "map_panel.h"
#include <wx/mstream.h>
#include <wx/dcclient.h>
#include <wx/dcbuffer.h>
#include <wx/dcmemory.h>
#include <algorithm>
#include <cmath>

wxBEGIN_EVENT_TABLE(MapPanel, wxPanel)
    EVT_PAINT(MapPanel::OnPaint)
    EVT_LEFT_DOWN(MapPanel::OnLeftDown)
    EVT_MOTION(MapPanel::OnMotion)
    EVT_LEFT_UP(MapPanel::OnLeftUp)
    EVT_LEFT_DCLICK(MapPanel::OnLeftDClick)
    EVT_MOUSE_CAPTURE_LOST(MapPanel::OnMouseCaptureLost)
wxEND_EVENT_TABLE()

MapPanel::MapPanel(wxWindow* parent, wxWindowID id)
    : wxPanel(parent, id, wxDefaultPosition, wxDefaultSize, wxBORDER_SUNKEN) {
    wxSize panelSz = FromDIP(wxSize(320, 320));
    SetMinClientSize(panelSz);
    SetClientSize(panelSz);
    SetBackgroundStyle(wxBG_STYLE_PAINT);
    SetCursor(wxCursor(wxCURSOR_CROSS));
    SetToolTip(wxT("左ドラッグ: 地図を動かして位置を微修正\nダブルクリック: クリック位置へ移動\nマウスホイール: 拡大・縮小 (Z10〜Z18)"));
}

void MapPanel::SetLocationMovedCallback(std::function<void(double lat, double lon)> cb) {
    m_onLocationMoved = std::move(cb);
}

void MapPanel::SetWaitingMove(bool waiting) {
    m_waitingMove = waiting;
    Refresh(false);
}

void MapPanel::ResetDragOffset() {
    if (HasCapture()) {
        ReleaseMouse();
    }
    m_isDragging = false;
    m_waitingMove = false;
    m_dragDx = 0;
    m_dragDy = 0;
    SetCursor(wxCursor(wxCURSOR_CROSS));
    Refresh(false);
}

void MapPanel::GetDrawOffsets(int& out_x_offset, int& out_y_offset) const {
    wxSize sz = GetClientSize();
    if (m_bitmap.IsOk()) {
        out_x_offset = (sz.GetWidth() - m_bitmap.GetWidth()) / 2;
        out_y_offset = (sz.GetHeight() - m_bitmap.GetHeight()) / 2;
    } else {
        out_x_offset = 0;
        out_y_offset = 0;
    }
}

void MapPanel::SetMapTile(const TileInfo& tile) {
    if (HasCapture()) {
        ReleaseMouse();
    }
    m_isDragging = false;
    m_waitingMove = false;
    m_dragDx = 0;
    m_dragDy = 0;
    SetCursor(wxCursor(wxCURSOR_CROSS));

    m_tileInfo = tile;
    m_tileInfo.pieces.clear(); // 描画後はメモリ節約のため生データ保持を外す
    m_marker_x = tile.pixel_x;
    m_marker_y = tile.pixel_y;
    m_has_map = false;

    if (!tile.pieces.empty() && tile.view_width > 0 && tile.view_height > 0 && tile.src_w > 0 && tile.src_h > 0) {
        if (tile.digital_scale <= 1) {
            wxBitmap composite(tile.view_width, tile.view_height, 24);
            wxMemoryDC memDC(composite);
            memDC.SetBackground(wxBrush(wxColor(240, 240, 240)));
            memDC.Clear();

            int loaded_count = 0;
            for (const auto& piece : tile.pieces) {
                if (piece.png_data.empty()) continue;
                wxMemoryInputStream stream(piece.png_data.data(), piece.png_data.size());
                wxImage img;
                if (img.LoadFile(stream, wxBITMAP_TYPE_ANY)) {
                    wxBitmap bmp(img);
                    memDC.DrawBitmap(bmp, piece.draw_x, piece.draw_y, true);
                    loaded_count++;
                }
            }
            memDC.SelectObject(wxNullBitmap);

            if (loaded_count > 0) {
                m_bitmap = composite;
                m_has_map = true;
            }
        } else {
            wxBitmap srcBmp(tile.src_w, tile.src_h, 24);
            wxMemoryDC srcDC(srcBmp);
            srcDC.SetBackground(wxBrush(wxColor(240, 240, 240)));
            srcDC.Clear();

            int loaded_count = 0;
            for (const auto& piece : tile.pieces) {
                if (piece.png_data.empty()) continue;
                wxMemoryInputStream stream(piece.png_data.data(), piece.png_data.size());
                wxImage img;
                if (img.LoadFile(stream, wxBITMAP_TYPE_ANY)) {
                    wxBitmap bmp(img);
                    srcDC.DrawBitmap(bmp, piece.draw_x, piece.draw_y, true);
                    loaded_count++;
                }
            }
            srcDC.SelectObject(wxNullBitmap);

            if (loaded_count > 0) {
                wxImage scaledImg = srcBmp.ConvertToImage();
                scaledImg.Rescale(
                    tile.src_w * tile.digital_scale,
                    tile.src_h * tile.digital_scale,
                    wxIMAGE_QUALITY_BILINEAR
                );

                wxBitmap composite(tile.view_width, tile.view_height, 24);
                wxMemoryDC compDC(composite);
                compDC.SetBackground(wxBrush(wxColor(240, 240, 240)));
                compDC.Clear();

                int blit_x = tile.src_px_left * tile.digital_scale - tile.view_left_z;
                int blit_y = tile.src_px_top * tile.digital_scale - tile.view_top_z;
                compDC.DrawBitmap(wxBitmap(scaledImg), blit_x, blit_y, false);
                compDC.SelectObject(wxNullBitmap);

                m_bitmap = composite;
                m_has_map = true;
            }
        }
    }
    Refresh(false);
}

void MapPanel::ClearMap() {
    if (HasCapture()) {
        ReleaseMouse();
    }
    m_isDragging = false;
    m_waitingMove = false;
    m_dragDx = 0;
    m_dragDy = 0;
    m_has_map = false;
    m_marker_x = -1;
    m_marker_y = -1;
    SetCursor(wxCursor(wxCURSOR_CROSS));
    Refresh(false);
}

void MapPanel::OnLeftDown(wxMouseEvent& event) {
    if (!m_has_map || m_waitingMove) {
        event.Skip();
        return;
    }
    SetFocus();
    m_dragStart = event.GetPosition();
    m_dragDx = 0;
    m_dragDy = 0;
    m_isDragging = true;
    SetCursor(wxCursor(wxCURSOR_SIZING));
    if (!HasCapture()) {
        CaptureMouse();
    }
}

void MapPanel::OnMotion(wxMouseEvent& event) {
    if (!m_isDragging || !event.Dragging() || !event.LeftIsDown()) {
        event.Skip();
        return;
    }
    wxPoint pos = event.GetPosition();
    m_dragDx = pos.x - m_dragStart.x;
    m_dragDy = pos.y - m_dragStart.y;
    Refresh(false);
}

void MapPanel::OnLeftUp(wxMouseEvent& event) {
    if (!m_isDragging) {
        event.Skip();
        return;
    }
    if (HasCapture()) {
        ReleaseMouse();
    }
    m_isDragging = false;
    SetCursor(wxCursor(wxCURSOR_CROSS));

    wxPoint pos = event.GetPosition();
    m_dragDx = pos.x - m_dragStart.x;
    m_dragDy = pos.y - m_dragStart.y;

    if (std::abs(m_dragDx) >= 2 || std::abs(m_dragDy) >= 2) {
        double new_world_px = m_tileInfo.world_px_z - static_cast<double>(m_dragDx);
        double new_world_py = m_tileInfo.world_py_z - static_cast<double>(m_dragDy);
        double new_lat = 0.0, new_lon = 0.0;
        GsiService::WorldPxToLatLon(new_world_px, new_world_py, m_tileInfo.z, new_lat, new_lon);

        if (m_onLocationMoved) {
            m_pendingLat = new_lat;
            m_pendingLon = new_lon;
            m_waitingMove = true;
            SetCursor(wxCursor(wxCURSOR_WAIT));
            Refresh(false);
            m_onLocationMoved(new_lat, new_lon);
            return;
        }
    }

    m_dragDx = 0;
    m_dragDy = 0;
    Refresh(false);
}

void MapPanel::OnLeftDClick(wxMouseEvent& event) {
    if (!m_has_map || m_waitingMove) return;

    if (HasCapture()) {
        ReleaseMouse();
    }
    m_isDragging = false;
    m_dragDx = 0;
    m_dragDy = 0;

    wxPoint pos = event.GetPosition();
    int x_offset = 0, y_offset = 0;
    GetDrawOffsets(x_offset, y_offset);

    double new_world_px = static_cast<double>(m_tileInfo.view_left_z) + (pos.x - x_offset);
    double new_world_py = static_cast<double>(m_tileInfo.view_top_z) + (pos.y - y_offset);
    double new_lat = 0.0, new_lon = 0.0;
    GsiService::WorldPxToLatLon(new_world_px, new_world_py, m_tileInfo.z, new_lat, new_lon);

    if (m_onLocationMoved) {
        m_pendingLat = new_lat;
        m_pendingLon = new_lon;
        m_waitingMove = true;
        SetCursor(wxCursor(wxCURSOR_WAIT));
        Refresh(false);
        m_onLocationMoved(new_lat, new_lon);
    } else {
        SetCursor(wxCursor(wxCURSOR_CROSS));
    }
}

void MapPanel::OnMouseCaptureLost(wxMouseCaptureLostEvent& WXUNUSED(event)) {
    m_isDragging = false;
    m_dragDx = 0;
    m_dragDy = 0;
    SetCursor(wxCursor(wxCURSOR_CROSS));
    Refresh(false);
}

void MapPanel::OnPaint(wxPaintEvent& WXUNUSED(event)) {
    wxAutoBufferedPaintDC dc(this);
    dc.SetBackground(wxBrush(wxColor(232, 232, 232)));
    dc.Clear();

    wxSize sz = GetClientSize();

    if (m_has_map && m_bitmap.IsOk()) {
        int x_offset = 0, y_offset = 0;
        GetDrawOffsets(x_offset, y_offset);

        // ドラッグ中は地図画像をオフセットして移動、中央クロスヘアは固定
        dc.DrawBitmap(m_bitmap, x_offset + m_dragDx, y_offset + m_dragDy, false);

        // 精密位置合わせ用オープン・クロスヘア (白縁取り＋細赤線)
        if (m_marker_x >= 0 && m_marker_y >= 0) {
            int cx = x_offset + m_marker_x;
            int cy = y_offset + m_marker_y;
            int r = FromDIP(7);
            int inner = FromDIP(3);
            int outer = FromDIP(15);

            dc.SetBrush(*wxTRANSPARENT_BRUSH);

            dc.SetPen(wxPen(*wxWHITE, std::max(3, FromDIP(3))));
            dc.DrawCircle(cx, cy, r);
            dc.DrawLine(cx - outer, cy, cx - inner, cy);
            dc.DrawLine(cx + inner, cy, cx + outer, cy);
            dc.DrawLine(cx, cy - outer, cx, cy - inner);
            dc.DrawLine(cx, cy + inner, cx, cy + outer);

            dc.SetPen(wxPen(wxColor(220, 20, 20), std::max(1, FromDIP(1))));
            dc.DrawCircle(cx, cy, r);
            dc.DrawLine(cx - outer, cy, cx - inner, cy);
            dc.DrawLine(cx + inner, cy, cx + outer, cy);
            dc.DrawLine(cx, cy - outer, cx, cy - inner);
            dc.DrawLine(cx, cy + inner, cx, cy + outer);
        }

        // 左下ステータスオーバーレイ (ドラッグ中座標 / 標高取得中 / デジタル拡大通知)
        wxString badge;
        if (m_isDragging) {
            double plat = 0.0, plon = 0.0;
            GsiService::WorldPxToLatLon(
                m_tileInfo.world_px_z - m_dragDx,
                m_tileInfo.world_py_z - m_dragDy,
                m_tileInfo.z, plat, plon
            );
            badge = wxString::Format(wxT("移動先: %.6f, %.6f"), plat, plon);
        } else if (m_waitingMove) {
            badge = wxString::Format(wxT("取得中: %.6f, %.6f"), m_pendingLat, m_pendingLon);
        } else if (m_tileInfo.digital_scale > 1) {
            badge = wxString::Format(wxT("色別標高図 Z%d (Z%dから拡大表示)"), m_tileInfo.z, m_tileInfo.fetch_z);
        }

        int badgeRight = 0;
        if (!badge.IsEmpty()) {
            wxFont badgeFont = dc.GetFont();
            badgeFont.SetPointSize(8);
            dc.SetFont(badgeFont);
            wxSize bsz = dc.GetTextExtent(badge);
            int pad = FromDIP(4);
            int margin = FromDIP(6);
            int bx = margin;
            int by = sz.GetHeight() - bsz.GetHeight() - pad * 2 - margin;
            int bw = bsz.GetWidth() + pad * 2;
            dc.SetBrush(wxBrush(wxColor(255, 255, 255)));
            dc.SetPen(wxPen(wxColor(160, 160, 160), 1));
            dc.DrawRectangle(bx, by, bw, bsz.GetHeight() + pad * 2);
            dc.SetTextForeground(wxColor(30, 30, 30));
            dc.DrawText(badge, bx + pad, by + pad);
            badgeRight = bx + bw + FromDIP(6);
        }

        // 右下簡易スケールバー (Webメルカトル解像度に基づく物理px基準)
        constexpr double kPi = 3.14159265358979323846;
        double lat_rad = m_tileInfo.center_lat * kPi / 180.0;
        double meters_per_px = 156543.03392 * std::cos(lat_rad) / std::pow(2.0, m_tileInfo.z);
        if (meters_per_px > 0.0) {
            const int candidates[] = {10, 20, 50, 100, 200, 500, 1000, 2000, 5000, 10000, 20000, 50000};
            const int max_px = FromDIP(105);
            int chosen_m = candidates[0];
            for (int i = static_cast<int>(sizeof(candidates) / sizeof(candidates[0])) - 1; i >= 0; --i) {
                int px = static_cast<int>(std::round(candidates[i] / meters_per_px));
                if (px <= max_px) {
                    chosen_m = candidates[i];
                    break;
                }
            }
            int bar_px = std::max(12, static_cast<int>(std::round(chosen_m / meters_per_px)));
            wxString scaleLabel = (chosen_m >= 1000)
                ? wxString::Format(wxT("%d km"), chosen_m / 1000)
                : wxString::Format(wxT("%d m"), chosen_m);

            wxFont scaleFont = dc.GetFont();
            scaleFont.SetPointSize(8);
            dc.SetFont(scaleFont);
            wxSize lsz = dc.GetTextExtent(scaleLabel);

            int padX = FromDIP(6);
            int padY = FromDIP(3);
            int margin = FromDIP(6);
            int contentW = std::max(bar_px, lsz.GetWidth());
            int boxW = contentW + padX * 2;
            int tickH = FromDIP(4);
            int boxH = lsz.GetHeight() + tickH + FromDIP(3) + padY * 2;
            int bx = sz.GetWidth() - boxW - margin;
            int by = sz.GetHeight() - boxH - margin;

            if (bx > badgeRight) {
                dc.SetBrush(wxBrush(wxColor(255, 255, 255)));
                dc.SetPen(wxPen(wxColor(160, 160, 160), 1));
                dc.DrawRectangle(bx, by, boxW, boxH);

                dc.SetTextForeground(wxColor(30, 30, 30));
                int tx = bx + (boxW - lsz.GetWidth()) / 2;
                int ty = by + padY;
                dc.DrawText(scaleLabel, tx, ty);

                int lineX1 = bx + (boxW - bar_px) / 2;
                int lineX2 = lineX1 + bar_px;
                int lineY = ty + lsz.GetHeight() + FromDIP(4);
                dc.SetPen(wxPen(wxColor(40, 40, 40), std::max(1, FromDIP(2))));
                dc.DrawLine(lineX1, lineY, lineX2, lineY);
                dc.DrawLine(lineX1, lineY - tickH, lineX1, lineY + 1);
                dc.DrawLine(lineX2, lineY - tickH, lineX2, lineY + 1);
            }
        }
    } else {
        dc.SetTextForeground(wxColor(120, 120, 120));
        wxString text = wxT("地図プレビュー (照会実行後に表示)");
        wxSize tsz = dc.GetTextExtent(text);
        dc.DrawText(text, (sz.GetWidth() - tsz.GetWidth()) / 2, (sz.GetHeight() - tsz.GetHeight()) / 2);
    }
}
