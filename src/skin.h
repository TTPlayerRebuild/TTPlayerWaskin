#pragma once
#include "archive.h"
#include "metadata.h"
#include "ttp_skin_plugin.h"
#include <array>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>
#include <commctrl.h>
#include "spectrum.h"
#include "playlist_font.h"

namespace waskin {
// Keep provider menus, persisted layouts and the host content callback in sync.
inline constexpr std::array<const wchar_t*,7> visualEffectNames{
    L"无",L"梦幻",L"频谱分析",L"波形",L"专辑封面",L"韵动脉冲",L"水波荡漾"};
enum SkinHit {
    hitShade=500, hitDrag=501, hitResize=502, hitShuffle=503, hitRepeat=504,
    hitSeek=505, hitVolume=506, hitBalance=507, hitScroll=508, hitScale=509,
    hitEqUp=510, hitEqFlat=511, hitEqDown=512, hitAuto=513,
    hitListAdd=520, hitListRem=521, hitListSel=522, hitListMisc=523, hitListList=524,
    hitScrollUp=525, hitScrollDown=526,
    hitVideoFullscreen=530, hitVideoNormal=531, hitVideoDouble=532,
    hitVideoMode=533, hitVideoMenu=534, hitRow=1000
};
struct Image {
    HBITMAP bitmap{};
    int width{}, height{};
    Image() = default;
    explicit Image(const Bytes& bytes);
    ~Image() { if(bitmap) DeleteObject(bitmap); }
    Image(const Image&) = delete;
    Image& operator=(const Image&) = delete;
    Image(Image&& other) noexcept : bitmap(std::exchange(other.bitmap,nullptr)), width(other.width),height(other.height) {}
};
struct CursorHandle {
    HCURSOR value{};
    explicit CursorHandle(HCURSOR cursor):value(cursor) {}
    CursorHandle(CursorHandle&& other) noexcept:value(std::exchange(other.value,nullptr)) {}
    CursorHandle(const CursorHandle&)=delete;
    ~CursorHandle() {if(value) DestroyCursor(value);}
};
struct View {
    ClassicSpectrum spectrum;
    class Skin* skin{};
    HWND window{};
    HWND tooltip{};
    int tip_hit{};
    RECT tip_bounds{};
    std::wstring tip_text;
    DWORD tip_tick{};
    int kind{};
    RECT saved{}, drag_rect{};
    HRGN saved_region{};
    std::vector<std::pair<HWND,bool>> children;
    POINT drag_start{};
    int pressed{}, scroll{}, selected{-1}, wheel{}, grab{}, seek{-1}, initial{};
    int track_value{}, track_coordinate{};
    bool hot{true}, row_drag{}, selection_pending{};
    int drop{-1}, external_drop{-1};
    bool shaded{}, dragging{}, resizing{}, host_drag{};
    int expanded_height{232};
};
Image MakeFallback(const char* name,int width,int height);
HCURSOR ReadCursor(const Bytes& bytes);
class Skin {
public:
    Skin(const wchar_t* path,const TtpSkinHost* host,bool fallback_only=false);
    virtual ~Skin();
    virtual const Metadata& Info() const {return metadata_;}
    virtual HRESULT Attach(const TtpSkinWindows& windows);
    virtual void Detach() noexcept;
    virtual HBITMAP Preview();
    virtual void Paint(HWND window,HDC dc,bool child_background=false);
    virtual void Shade();
    virtual bool Translate(const MSG& message);
    virtual HRESULT Layout(TtpSkinLayout& state,bool restore);
    virtual bool Handles(HWND window) const;
    virtual HMENU Menu(HWND window,uint32_t command);
    virtual bool ContentState(TtpSkinContent& state,bool apply);
    bool ContentMinimum(HWND window,SIZE& size) const;
    bool LyricColors(HWND window,TtpSkinLyricColors& colors) const;
    virtual bool LyricFont(LOGFONTW& font) const;
    virtual bool DefaultPlaylistFont(LOGFONTW& font) const;
    virtual bool VolumeTracking() const;
    virtual bool PlaylistReveal(uint32_t row,int32_t caret);
    virtual bool PlaylistDrop(TtpSkinPlaylistDrop& drop);
    virtual bool PlaylistViewport(HWND window,uint32_t& first,uint32_t& count);
    static LRESULT CALLBACK Subclass(HWND,UINT,WPARAM,LPARAM,UINT_PTR,DWORD_PTR);
private:
    bool fallback_only_{};
    Metadata metadata_;
    struct SavedLayout {
        std::array<RECT,4> bounds{};
        std::array<bool,4> shaded{};
        std::array<int,4> expanded{116,232,116,290};
        int scale{1},scroll{};
        int content_mode{TTP_SKIN_CONTENT_LYRICS},visual_type{1};
    } layout_;
    bool binding_{};
    void CaptureLayout() noexcept;
    std::unordered_map<std::string,Image> images_, fallback_;
    std::unordered_map<std::string,CursorHandle> cursors_;
    TtpSkinVisualColors visual_colors_{RGB(0,0,0),RGB(255,64,32),RGB(240,220,32),RGB(32,190,32),RGB(255,255,255),RGB(0,255,0)};
    std::array<COLORREF,24> visual_palette_{ClassicVisualPalette()};
    int scale_{1}, row_height_{13};
    COLORREF text_color_{RGB(0,255,0)};
    COLORREF text_background_{RGB(0,0,0)};
    COLORREF video_text_{RGB(0,255,0)},video_background_{RGB(0,0,0)};
    unsigned feedback_until_{};
    std::wstring feedback_;
    uint32_t stats_index_{}, stats_count_{};
    int64_t stats_total_{}, stats_selected_{};
    bool stats_unknown_{}, stats_selected_unknown_{};
    std::wstring statistics_;
    void UpdateStatistics();
    void Feedback(const std::wstring& text);
    void Title(HDC dc,RECT bounds,const std::wstring& text) const;
    void Visual(View& view,HDC dc,RECT bounds) const;
    void ToggleScale();
    void BeginTrack(View& view,int hit,POINT point);
    void SelectRow(View& view,int row);
    HCURSOR Cursor(const View& view,POINT point) const;
    std::unordered_map<std::string,std::string> ini_;
    std::array<View,4> views_{};
    TtpSkinHost host_{};
    HFONT font_{};
    PlaylistFontCache playlist_font_;
    void SyncPlaylistFont();
    COLORREF normal_{RGB(0,255,0)}, current_{RGB(255,255,255)}, background_{RGB(0,0,0)}, selection_{RGB(0,0,198)};
    unsigned ticks_{};
    bool Sliding(int hit) const;
    TtpSkinState State() const;
    void Command(uint32_t command,int32_t value=0) const;
    bool Blit(HDC dc,const char* image,int x,int y,int w,int h,int sx=0,int sy=0,int sw=0,int sh=0) const;
    void TextBackground(HDC dc,RECT bounds,bool bitmap) const;
    void Text(HDC dc,RECT bounds,const std::wstring& text,COLORREF color,bool bitmap=false,HFONT font=nullptr) const;
    void Draw(View& view,HDC dc,int width,int height);
    void DrawMain(View& view,HDC dc,const TtpSkinState& state);
    void DrawPlaylist(View& view,HDC dc,int width,int height,const TtpSkinState& state);
    void DrawPlaylistTime(HDC dc,int width,int height,const TtpSkinState& state) const;
    void DrawEqualizer(View& view,HDC dc,const TtpSkinState& state);
    void DrawVideo(View& view,HDC dc,int width,int height);
    void VideoAction(View& view,int hit);
    void VideoContentChanged();
    int Hit(const View& view,POINT point,RECT* bounds=nullptr) const;
    void Activate(View& view,int hit,POINT point);
    void Track(View& view,int hit,POINT point,bool final=false);
    void ToggleShade(View& view);
    void Region(View& view);
    void HideChildren(View& view);
    void UpdateTip(View& view,POINT point);
    void HideTip(View& view) noexcept;
    void RefreshRowTip(View& view);
    std::wstring TipText(const View& view,int hit,const RECT& bounds) const;
    bool HostDrag(View& view, uint32_t phase, POINT point = {}) const;
    void EndDrag(View& view);
    LRESULT Message(View& view,UINT message,WPARAM wp,LPARAM lp);
};
}
