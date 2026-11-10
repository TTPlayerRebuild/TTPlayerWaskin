#include "skin.h"
#include <algorithm>
#include <sstream>

namespace waskin {
void Skin::CaptureLayout() noexcept {
    if(binding_ || (!fallback_only_ && !IsWindow(views_[0].window))) return;
    layout_.scroll=std::max(0,views_[1].scroll);
    if(IsIconic(views_[0].window)) return;
    layout_.scale=scale_;
    for(size_t i=0;i<views_.size();++i) {
        const auto& v=views_[i];RECT bounds{};
        if(!IsWindow(v.window) || !GetWindowRect(v.window,&bounds)) continue;
        layout_.bounds[i]=bounds;layout_.shaded[i]=v.shaded;
        layout_.expanded[i]=v.shaded?v.expanded_height:int(bounds.bottom-bounds.top);
    }
}
HRESULT Skin::Layout(TtpSkinLayout& state,bool restore) {
    if(!restore) {
        CaptureLayout();
        for(size_t i=0;i<3;++i) state.windows[i]=layout_.bounds[i];
        if(state.size>=sizeof(state)) state.lyric_window=layout_.bounds[3];
        const auto& video=layout_.bounds[3];
        swprintf_s(state.state,L"2 %d %d %d %d %d %d %d %d %ld %ld %ld %ld %d %d",layout_.scale,
            int(layout_.shaded[0]),int(layout_.shaded[1]),int(layout_.shaded[2]),
            layout_.expanded[0],layout_.expanded[1],layout_.expanded[2],layout_.scroll,
            video.left,video.top,video.right,video.bottom,layout_.content_mode,layout_.visual_type);
        return S_OK;
    }
    if(IsWindow(views_[0].window)) return E_UNEXPECTED;
    SavedLayout next;
    if(state.state[0]) {
        const auto length=wcsnlen_s(state.state,std::size(state.state));
        if(length==std::size(state.state)) return E_INVALIDARG;
        std::wistringstream input(std::wstring(state.state,length));
        int version{},shade[3]{};
        if(!(input>>version>>next.scale>>shade[0]>>shade[1]>>shade[2]
            >>next.expanded[0]>>next.expanded[1]>>next.expanded[2]>>next.scroll) || (version!=1 && version!=2) ||
            (next.scale!=1 && next.scale!=2) || next.scroll<0) return E_INVALIDARG;
        if(version==2) {
            auto& video=next.bounds[3];
            if(!(input>>video.left>>video.top>>video.right>>video.bottom>>next.content_mode>>next.visual_type) ||
               next.content_mode<1 || next.content_mode>3 || next.visual_type<0 ||
               static_cast<size_t>(next.visual_type)>=visualEffectNames.size())
                return E_INVALIDARG;
            if(!IsRectEmpty(&video)) {
                const int64_t width=int64_t(video.right)-video.left,height=int64_t(video.bottom)-video.top;
                if(width<275 || width>32767 || height<116 || height>32767 ||
                   video.left < -1000000 || video.left>1000000 || video.top < -1000000 || video.top>1000000)
                    return E_INVALIDARG;
                next.expanded[3]=int(height);
            } else video={};
        }
        input>>std::ws;if(!input.eof()) return E_INVALIDARG;
        for(size_t i=0;i<3;++i) {
            if(shade[i]!=0 && shade[i]!=1) return E_INVALIDARG;
            next.shaded[i]=shade[i]!=0;
        }
        if(next.expanded[0]!=116*next.scale || next.expanded[2]!=116*next.scale ||
            next.expanded[1]<116 || next.expanded[1]>32767) return E_INVALIDARG;
    } else {
        // Previously saved sidecars already contain rectangles. Recover their
        // scale/shade where possible; only the folded playlist's old expanded
        // height was never recorded and must use the default once.
        const auto& main=state.windows[0];
        next.scale=int64_t(main.right)-main.left==550?2:1;
        next.expanded[0]=next.expanded[2]=116*next.scale;
        for(size_t i=0;i<3;++i) {
            const int64_t height=int64_t(state.windows[i].bottom)-state.windows[i].top;
            next.shaded[i]=height==14*(i==1?1:next.scale);
            if(i==1 && !next.shaded[i] && height>=116 && height<=32767) next.expanded[i]=int(height);
        }
    }
    // The explicit rectangles are authoritative. A root XML saved after an
    // older sidecar may contain a new scale, fold state or playlist height.
    const auto& main=state.windows[0];
    const auto main_width=int64_t(main.right)-main.left;
    if(!fallback_only_ && (main_width==275 || main_width==550)) {
        next.scale=static_cast<int>(main_width/275);
        next.expanded[0]=next.expanded[2]=116*next.scale;
    }
    for(size_t i=0;i<3;++i) {
        const auto& r=state.windows[i];
        const int64_t width=int64_t(r.right)-r.left,height=int64_t(r.bottom)-r.top;
        if(width<=0 || height<=0 || r.left < -1000000 || r.left>1000000 || r.top < -1000000 || r.top>1000000) continue;
        next.shaded[i]=height==14*(i==1?1:next.scale);
        if(i==1 && !next.shaded[i]) next.expanded[i]=static_cast<int>(std::clamp<int64_t>(height,116,32767));
        const int restored_width=i==1?int(std::clamp<int64_t>(width,275,32767)):275*next.scale;
        const int restored_height=next.shaded[i]?14*(i==1?1:next.scale):next.expanded[i];
        next.bounds[i]={r.left,r.top,r.left+restored_width,r.top+restored_height};
    }
    if(state.size>=sizeof(state) && !IsRectEmpty(&state.lyric_window)) {
        const auto& r=state.lyric_window;
        const int64_t width=int64_t(r.right)-r.left,height=int64_t(r.bottom)-r.top;
        if(r.left>=-1000000 && r.left<=1000000 && r.top>=-1000000 && r.top<=1000000) {
            next.bounds[3]={r.left,r.top,r.left+static_cast<LONG>(std::clamp<int64_t>(width,275,32767)),
                r.top+static_cast<LONG>(std::clamp<int64_t>(height,116,32767))};
            next.expanded[3]=next.bounds[3].bottom-next.bounds[3].top;
        }
    }
    layout_=next;return S_OK;
}
}
