#pragma once
#include <wx/wx.h>
#include <functional>
#include "gsi_service.h"

class MapPanel : public wxPanel {
public:
    MapPanel(wxWindow* parent, wxWindowID id = wxID_ANY);

    void SetMapTile(const TileInfo& tile);
    void ClearMap();
    void ResetDragOffset();
    void SetWaitingMove(bool waiting);

    void SetLocationMovedCallback(std::function<void(double lat, double lon)> cb);

    bool HasMap() const { return m_has_map; }
    int GetCurrentTileZoom() const { return m_tileInfo.z; }
    const std::string& GetCurrentTileLayer() const { return m_tileInfo.layer; }

private:
    void OnPaint(wxPaintEvent& event);
    void OnLeftDown(wxMouseEvent& event);
    void OnMotion(wxMouseEvent& event);
    void OnLeftUp(wxMouseEvent& event);
    void OnLeftDClick(wxMouseEvent& event);
    void OnMouseCaptureLost(wxMouseCaptureLostEvent& event);
    void GetDrawOffsets(int& out_x_offset, int& out_y_offset) const;

    wxBitmap m_bitmap;
    TileInfo m_tileInfo;
    int m_marker_x = -1;
    int m_marker_y = -1;
    bool m_has_map = false;

    bool m_isDragging = false;
    bool m_waitingMove = false;
    wxPoint m_dragStart;
    int m_dragDx = 0;
    int m_dragDy = 0;
    double m_pendingLat = 0.0;
    double m_pendingLon = 0.0;

    std::function<void(double lat, double lon)> m_onLocationMoved;

    wxDECLARE_EVENT_TABLE();
};
