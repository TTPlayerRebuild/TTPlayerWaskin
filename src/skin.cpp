#include "skin.h"
#include "host_interaction.h"
#include "playlist_interaction.h"
#include "builtin_skin.h"
#include <windowsx.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <cwctype>
#include <sstream>
#include <stdexcept>
#include <optional>

namespace waskin {
namespace {
constexpr UINT_PTR subclassId=0x77534b49, timerId=0x77534b49;
std::string Trim(std::string s) {
    const auto first=s.find_first_not_of(" \t\r\n");
    if(first==std::string::npos) return {};
    return s.substr(first,s.find_last_not_of(" \t\r\n")-first+1);
}
std::string Lower(std::string s) { for(char& c:s) if(c>='A'&&c<='Z') c=char(c+32); return s; }
void ReadIni(const Bytes& bytes,const std::string& prefix,std::unordered_map<std::string,std::string>& values) {
    std::istringstream stream(std::string(bytes.begin(),bytes.end())); std::string line, section;
    while(std::getline(stream,line)) {
        line=Trim(line); if(line.empty() || line[0]==';' || line[0]=='#') continue;
        if(line[0]=='[') { const auto end=line.find(']'); if(end!=std::string::npos) section=Lower(Trim(line.substr(1,end-1))); }
        else { const auto eq=line.find('='); if(eq!=std::string::npos) values[prefix+section+"/"+Lower(Trim(line.substr(0,eq)))]=Trim(line.substr(eq+1)); }
    }
}
COLORREF Color(const std::string& text,COLORREF fallback) {
    auto s=Trim(text); if(!s.empty() && s[0]=='#') s.erase(0,1);
    if(s.size()!=6) return fallback;
    for(char c:s) if(!std::isxdigit(static_cast<unsigned char>(c))) return fallback;
    const auto n=std::stoul(s,nullptr,16); return RGB((n>>16)&255,(n>>8)&255,n&255);
}
void Fill(HDC dc,RECT rect,COLORREF color) {
    SetDCBrushColor(dc,color); FillRect(dc,&rect,static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
}
bool Inside(POINT p,int x,int y,int w,int h) { return p.x>=x && p.x<x+w && p.y>=y && p.y<y+h; }
std::wstring Time(int64_t milliseconds) {
    const auto seconds=std::max<int64_t>(0,milliseconds/1000);
    wchar_t text[48]{}; swprintf_s(text,L"%lld:%02lld",seconds/60,seconds%60); return text;
}
std::wstring DurationSummary(int64_t seconds,bool unknown) {
    if(!seconds && unknown) return L"?";
    wchar_t text[64]{};
    if(seconds<3600) swprintf_s(text,L"%lld:%02lld",seconds/60,seconds%60);
    else swprintf_s(text,L"%lld:%02lld:%02lld",seconds/3600,seconds/60%60,seconds%60);
    return std::wstring(text)+(unknown?L"+":L"");
}
}

Image::Image(const Bytes& bytes) {
    if(bytes.empty()) return;
    if(bytes.size()<sizeof(BITMAPFILEHEADER)+sizeof(BITMAPINFOHEADER)) throw std::runtime_error("Invalid BMP");
    BITMAPFILEHEADER file{}; BITMAPINFOHEADER info{};
    memcpy(&file,bytes.data(),sizeof(file)); memcpy(&info,bytes.data()+sizeof(file),sizeof(info));
    if(file.bfType!=0x4d42 || info.biSize<40 || info.biSize>124 || info.biWidth<=0 || info.biWidth>4096 ||
       info.biHeight==0 || info.biHeight < -4096 || info.biHeight>4096 || info.biPlanes!=1 ||
       (info.biBitCount!=1 && info.biBitCount!=4 && info.biBitCount!=8 && info.biBitCount!=16 && info.biBitCount!=24 && info.biBitCount!=32) ||
       (info.biCompression!=BI_RGB && info.biCompression!=BI_BITFIELDS && info.biCompression!=BI_RLE8 && info.biCompression!=BI_RLE4)) throw std::runtime_error("Unsupported BMP");
    width=info.biWidth; height=std::abs(info.biHeight);
    const size_t palette=info.biBitCount<=8 ? (info.biClrUsed?info.biClrUsed:1u<<info.biBitCount) : 0;
    if(palette>256) throw std::runtime_error("Invalid BMP palette");
    const size_t header=sizeof(file)+info.biSize+palette*4+((info.biCompression==BI_BITFIELDS && info.biSize==40)?12:0);
    const size_t stride=((size_t(width)*info.biBitCount+31)/32)*4;
    const bool rle=info.biCompression==BI_RLE8 || info.biCompression==BI_RLE4;
    if(header>bytes.size() || file.bfOffBits<header || file.bfOffBits>bytes.size() || (!rle && stride*height>bytes.size()-file.bfOffBits))
        throw std::runtime_error("Truncated BMP");
    Bytes decoded;
    if(rle) {
        if(info.biHeight<0 || (info.biCompression==BI_RLE8 && info.biBitCount!=8) ||
           (info.biCompression==BI_RLE4 && info.biBitCount!=4)) throw std::runtime_error("Invalid BMP RLE format");
        decoded.resize(stride*height);
        size_t input=file.bfOffBits; int x=0,y=0;bool ended=false;
        const auto byte=[&]() {if(input==bytes.size()) throw std::runtime_error("Truncated BMP RLE");return bytes[input++];};
        const auto pixel=[&](unsigned value) {
            if(x>=width || y>=height || value>=palette) throw std::runtime_error("BMP RLE outside image");
            auto& target=decoded[size_t(y)*stride+(info.biBitCount==8?x:x/2)];
            if(info.biBitCount==8) target=uint8_t(value);
            else target=uint8_t((x%2)?(target&0xf0)|value:(target&0x0f)|(value<<4));
            ++x;
        };
        while(!ended) {
            const unsigned count=byte(),value=byte();
            if(count) for(unsigned i=0;i<count;++i) pixel(info.biBitCount==8?value:(i%2?value&15:value>>4));
            else if(value==0) {x=0;if(++y>height) throw std::runtime_error("BMP RLE excess rows");}
            else if(value==1) ended=true;
            else if(value==2) {x+=byte();y+=byte();if(x>width || y>=height) throw std::runtime_error("BMP RLE delta outside image");}
            else {
                unsigned pair=0;
                for(unsigned i=0;i<value;++i) {
                    if(info.biBitCount==8) pixel(byte());
                    else {if(i%2==0) pair=byte();pixel(i%2?pair&15:pair>>4);}
                }
                const unsigned length=info.biBitCount==8?value:(value+1)/2;
                if(length%2) byte();
            }
        }
    }
    // Aligned BITMAPINFO storage also supports palette entries and bit masks.
    std::vector<DWORD> metadata((header-sizeof(file)+3)/4);
    memcpy(metadata.data(),bytes.data()+sizeof(file),header-sizeof(file));
    if(rle) {
        auto* uncompressed=reinterpret_cast<BITMAPINFOHEADER*>(metadata.data());
        uncompressed->biCompression=BI_RGB;uncompressed->biSizeImage=DWORD(decoded.size());
    }
    HDC screen=GetDC(nullptr);
    bitmap=CreateDIBitmap(screen,reinterpret_cast<const BITMAPINFOHEADER*>(metadata.data()),CBM_INIT,
        rle?decoded.data():bytes.data()+file.bfOffBits,reinterpret_cast<const BITMAPINFO*>(metadata.data()),DIB_RGB_COLORS);
    ReleaseDC(nullptr,screen);
    if(!bitmap) throw std::runtime_error("Cannot create BMP");
}
Skin::Skin(const wchar_t* path,const TtpSkinHost* host,bool fallback_only):fallback_only_(fallback_only) {
    if(host) {
        // Old v1 hosts end at command. Never read a partially supplied tail.
        std::memcpy(&host_,host,TTP_SKIN_HOST_V1_SIZE);
        if(host->size>=offsetof(TtpSkinHost,selection)) host_.drag=host->drag;
        if(host->size>=offsetof(TtpSkinHost,visual)) host_.selection=host->selection;
        if(host->size>=offsetof(TtpSkinHost,tip)) host_.visual=host->visual;
        if(host->size>=offsetof(TtpSkinHost,resize)) host_.tip=host->tip;
        if(host->size>=offsetof(TtpSkinHost,spectrum)) host_.resize=host->resize;
        if(host->size>=offsetof(TtpSkinHost,content)) host_.spectrum=host->spectrum;
        if(host->size>=offsetof(TtpSkinHost,content_input)) host_.content=host->content;
        if(host->size>=offsetof(TtpSkinHost,option)) host_.content_input=host->content_input;
        if(host->size>=offsetof(TtpSkinHost,option)+sizeof(host->option)) host_.option=host->option;
        if(host->size>=offsetof(TtpSkinHost,playlist_context)+sizeof(host->playlist_context)) host_.playlist_context=host->playlist_context;
        if(host->size>=offsetof(TtpSkinHost,playlist_font)+sizeof(host->playlist_font)) host_.playlist_font=host->playlist_font;
    }
    const auto& builtin=BuiltinArchive();
    std::optional<Archive> package;
    if(!IsBuiltinPackage(path)) package.emplace(path);
    const auto& archive=package?*package:builtin;
    struct Spec {const char* name;int w,h;};
    const Spec specs[]={
        {"main.bmp",275,116},{"cbuttons.bmp",136,36},
        {"titlebar.bmp",344,87},{"shufrep.bmp",92,85},
        {"posbar.bmp",307,10},{"volume.bmp",68,433},
        {"balance.bmp",68,433},{"numbers.bmp",99,13},
        {"nums_ex.bmp",108,13},{"text.bmp",155,74},
        {"playpaus.bmp",42,9},{"monoster.bmp",58,24},
        {"eqmain.bmp",275,315},{"eq_ex.bmp",275,82},
        {"pledit.bmp",280,186},{"video.bmp",234,119}};
    bool recognized=false;
    for(const auto& spec:specs) {
        if(archive.Has(spec.name)) {
            // ZIP/CRC errors are fatal. Bad optional BMPs fall back locally.
            const auto bytes=archive.Read(spec.name);
            try {
                Image decoded(bytes);
                // draw_LBitmap accepts any decodable bitmap. Skins deliberately
                // omit atlas rows to leave the background artwork unobscured.
                if(decoded.bitmap) {recognized=true;images_.emplace(spec.name,std::move(decoded));}
            } catch(const std::runtime_error&) {}
        }
        const auto defaults=builtin.Read(spec.name);
        fallback_.emplace(spec.name,defaults.empty()?MakeFallback(spec.name,spec.w,spec.h):Image(defaults));
    }
    if(!recognized) throw std::runtime_error("No valid classic skin resources");
    metadata_=ReadMetadata(archive.Read("skininfo.xml"));
    ReadIni(builtin.Read("pledit.txt"),"playlist/",ini_);
    ReadIni(builtin.Read("region.txt"),"region/",ini_);
    if(package) {
        ReadIni(archive.Read("pledit.txt"),"playlist/",ini_);
        ReadIni(archive.Read("region.txt"),"region/",ini_);
    }
    normal_=Color(ini_["playlist/text/normal"],normal_);
    current_=Color(ini_["playlist/text/current"],current_);
    background_=Color(ini_["playlist/text/normalbg"],background_);
    selection_=Color(ini_["playlist/text/selectedbg"],selection_);
    video_text_=Color(ini_["playlist/text/mbfg"],video_text_);
    video_background_=Color(ini_["playlist/text/mbbg"],video_background_);
    const auto font_name=ini_["playlist/text/font"];
    std::wstring face=L"Arial";
    if(!font_name.empty()) {
        const int count=MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,font_name.data(),int(font_name.size()),nullptr,0);
        if(count>0 && count<LF_FACESIZE) {face.resize(count);MultiByteToWideChar(CP_UTF8,0,font_name.data(),int(font_name.size()),face.data(),count);}
    }
    // Font is the classic key; accept TextFont as an additional alias.
    const auto text_font=ini_["playlist/text/textfont"];
    if(!text_font.empty()) {
        const int count=MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,text_font.data(),int(text_font.size()),nullptr,0);
        if(count>0 && count<LF_FACESIZE) {face.resize(count);MultiByteToWideChar(CP_UTF8,0,text_font.data(),int(text_font.size()),face.data(),count);}
    }
    std::istringstream colors(std::string{});
    const auto palette_bytes=(archive.Has("viscolor.txt")?archive:builtin).Read("viscolor.txt");
    colors.str(std::string(palette_bytes.begin(),palette_bytes.end()));
    auto& palette=visual_palette_;size_t at=0;std::string line;
    while(at<palette.size() && std::getline(colors,line)) {
        std::replace(line.begin(),line.end(),',',' ');std::istringstream values(line);int r,g,b;
        if(!(values>>r>>g>>b) || r<0 || r>255 || g<0 || g>255 || b<0 || b>255) break;
        palette[at++]=RGB(r,g,b);
    }
    visual_colors_={palette[0],palette[2],palette[10],palette[17],palette[23],palette[20]};
    for(const char* name:{"volbal","posbar","winbut","min","close","mainmenu","titlebar","songname","normal",
        "wsposbar","mmenu","wsnormal","pwinbut","pclose","ptbar","pvscroll","psize","pnormal","pwssize","pwsnorm",
        "eqslid","eqclose","eqtitle","eqnormal","vnormal","vclose","vsize","vtbar","vmbuts"}) {
        const auto resource=std::string(name)+".cur";
        auto cursor=ReadCursor(archive.Read(resource));
        if(!cursor && package) cursor=ReadCursor(builtin.Read(resource));
        if(cursor) cursors_.emplace(name,CursorHandle(cursor));
    }
    font_=CreateFontW(-11,0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,
        NONANTIALIASED_QUALITY,DEFAULT_PITCH,face.c_str());
    HDC dc=CreateCompatibleDC(nullptr);const auto previous=SelectObject(dc,font_?font_:GetStockObject(DEFAULT_GUI_FONT));
    TEXTMETRICW metric{};if(GetTextMetricsW(dc,&metric)) row_height_=std::max(1L,metric.tmHeight);
    if(const auto text=images_.find("text.bmp");text!=images_.end()) {
        HDC sample=CreateCompatibleDC(dc);const auto old=SelectObject(sample,text->second.bitmap);
        const auto bg=GetPixel(sample,150,4);int best=-1;
        if(bg!=CLR_INVALID) text_background_=bg;
        for(int y=0;y<6;++y) for(int x=0;x<20;++x) {
            const auto c=GetPixel(sample,x,y);const int d=std::abs(int(GetRValue(c))-GetRValue(bg))+std::abs(int(GetGValue(c))-GetGValue(bg))+std::abs(int(GetBValue(c))-GetBValue(bg));
            if(d>best && c!=CLR_INVALID) {best=d;text_color_=c;}
        }
        SelectObject(sample,old);DeleteDC(sample);
    }
    SelectObject(dc,previous);DeleteDC(dc);
    for(int i=0;i<4;++i) { views_[i].skin=this; views_[i].kind=i; }
}
Skin::~Skin() { Detach(); if(font_) DeleteObject(font_); }
bool Skin::DefaultPlaylistFont(LOGFONTW& font) const {
    return font_ && GetObjectW(font_,sizeof(font),&font)==sizeof(font);
}
void Skin::SyncPlaylistFont() {
    LOGFONTW fallback{};
    if(DefaultPlaylistFont(fallback) && playlist_font_.Update(host_,fallback,views_[1].window!=nullptr)) {
        row_height_=playlist_font_.Height();HideTip(views_[1]);
        auto& view=views_[1];RECT bounds{};
        if(view.window && GetClientRect(view.window,&bounds)) {
            const int height=view.shaded?view.expanded_height:int(bounds.bottom);
            view.scroll=std::clamp(view.scroll,0,std::max(0,int(State().track_count)-std::max(1,(height-60)/row_height_)));
        }
    }
}
TtpSkinState Skin::State() const {
    TtpSkinState state{}; state.size=sizeof(state); state.volume=100; state.elapsed=1; state.playing_row=-1;
    wcscpy_s(state.title,L"TTPlayer");
    if(host_.query) host_.query(host_.context,&state);
    return state;
}
void Skin::Command(uint32_t command,int32_t value) const { if(host_.command) host_.command(host_.context,command,value); }
bool Skin::Blit(HDC dc,const char* name,int x,int y,int w,int h,int sx,int sy,int sw,int sh) const {
    if(!sw) sw=w;if(!sh) sh=h;
    if(sx<0 || sy<0 || w<=0 || h<=0 || sw<=0 || sh<=0) return false;
    const Image* chosen=nullptr;
    if(const auto found=images_.find(name);found!=images_.end()) chosen=&found->second;
    if(!chosen) if(const auto found=fallback_.find(name);found!=fallback_.end()) chosen=&found->second;
    if(!chosen) return false;
    const auto& image=*chosen;
    // Like Winamp's BitBlt, an existing short atlas clips to its source. Never
    // substitute another image or text for a deliberately absent sprite.
    const bool stretch=sw!=w || sh!=h;
    if(sx>=image.width || sy>=image.height) return true;
    if(!stretch) {w=std::min(w,image.width-sx);h=std::min(h,image.height-sy);}
    HDC source=CreateCompatibleDC(dc); if(!source) return false;
    const auto old=SelectObject(source,image.bitmap);
    SetStretchBltMode(dc,COLORONCOLOR);
    const BOOL result=stretch?StretchBlt(dc,x,y,w,h,source,sx,sy,sw,sh,SRCCOPY):BitBlt(dc,x,y,w,h,source,sx,sy,SRCCOPY);
    SelectObject(source,old); DeleteDC(source); return result!=FALSE;
}
void Skin::TextBackground(HDC dc,RECT bounds,bool bitmap) const {
    if(!bitmap) {Fill(dc,bounds,text_background_);return;}
    const auto found=images_.find("text.bmp");
    const auto& text=found==images_.end()?fallback_.at("text.bmp"):found->second;
    if(text.width<=4) return;
    HDC source=CreateCompatibleDC(dc);if(!source) return;
    const auto old=SelectObject(source,text.bitmap);
    // _draw_songname uses column 4 of the first glyph for trailing blanks;
    // preserve its six individual pixels instead of guessing a flat colour.
    for(int y=0;y<std::min(6,text.height) && bounds.top+y<bounds.bottom;++y)
        Fill(dc,{bounds.left,bounds.top+y,bounds.right,bounds.top+y+1},GetPixel(source,4,y));
    SelectObject(source,old);DeleteDC(source);
}
void Skin::Text(HDC dc,RECT bounds,const std::wstring& text,COLORREF color,bool bitmap,HFONT font) const {
    const bool ascii=std::all_of(text.begin(),text.end(),[](wchar_t c){return c>=32 && c<127;});
    if(bitmap && ascii) {
        const int saved=SaveDC(dc); IntersectClipRect(dc,bounds.left,bounds.top,bounds.right,bounds.bottom);
        int x=bounds.left;
        // Classic text.bmp: alphabet in row 0; digits and punctuation in row 1.
        const std::wstring row1=L"0123456789\1.:()-'!_+\\/[]^&%,=$#";
        for(wchar_t c:text) {
            c=wchar_t(towupper(c)); int sx=142,sy=0;
            if(c==L'{' || c==L'<') c=L'[';
            if(c==L'}' || c==L'>') c=L']';
            if(c==L'~') c=L'^';
            if(c>=L'A'&&c<=L'Z') sx=(c-L'A')*5;
            else { const auto at=row1.find(c); if(at!=std::wstring::npos) {sx=int(at)*5;sy=6;} }
            if(c==L'?') {sx=15;sy=12;} else if(c==L'*') {sx=20;sy=12;}
            if(!Blit(dc,"text.bmp",x,bounds.top,5,6,sx,sy)) {
                const auto previous=SelectObject(dc,font_);SetTextColor(dc,color);SetBkMode(dc,TRANSPARENT);
                TextOutW(dc,x,bounds.top,&c,1);SelectObject(dc,previous);
            } x+=5;
            if(x>=bounds.right) break;
        }
        RestoreDC(dc,saved); return;
    }
    const auto old=SelectObject(dc,font?font:font_?font_:GetStockObject(DEFAULT_GUI_FONT));
    SetBkMode(dc,TRANSPARENT); SetTextColor(dc,color);
    DrawTextW(dc,text.c_str(),int(text.size()),&bounds,DT_LEFT|DT_SINGLELINE|DT_VCENTER|DT_END_ELLIPSIS|DT_NOPREFIX);
    SelectObject(dc,old);
}
void Skin::Title(HDC dc,RECT bounds,const std::wstring& title) const {
    const bool bitmap=std::all_of(title.begin(),title.end(),[](wchar_t c){return c>=32 && c<127;});
    const auto old=SelectObject(dc,font_);SIZE size{};GetTextExtentPoint32W(dc,title.data(),int(title.size()),&size);
    const int width=bitmap?int(title.size())*5:size.cx;
    const int offset=width>bounds.right-bounds.left?int(ticks_*2%unsigned(width+30)):0;
    const RECT clip{bounds.left,bounds.top-(bitmap?0:3),bounds.right,bounds.top+(bitmap?6:9)};
    const int save=SaveDC(dc);IntersectClipRect(dc,clip.left,clip.top,clip.right,clip.bottom);
    TextBackground(dc,clip,bitmap);
    if(bitmap) {
        Text(dc,{bounds.left-offset,bounds.top,bounds.left-offset+width,bounds.bottom},title,text_color_,true);
        if(offset) Text(dc,{bounds.left-offset+width+30,bounds.top,bounds.left-offset+2*width+30,bounds.bottom},title,text_color_,true);
    } else {
        SetTextColor(dc,text_color_);SetBkMode(dc,TRANSPARENT);
        const int y=bounds.top+(6-size.cy)/2;
        TextOutW(dc,bounds.left-offset,y,title.data(),int(title.size()));
        if(offset) TextOutW(dc,bounds.left-offset+width+30,y,title.data(),int(title.size()));
    }
    RestoreDC(dc,save);SelectObject(dc,old);
}
void Skin::DrawMain(View& view,HDC dc,const TtpSkinState& s) {
    Blit(dc,"main.bmp",0,0,275,116);
    const bool active=GetActiveWindow()==view.window;
    Blit(dc,"titlebar.bmp",0,0,275,14,27,view.shaded?(active?29:42):(active?0:15));
    const int pressed=view.hot?view.pressed:0;
    const auto time=s.elapsed?s.position_ms:std::max<int64_t>(0,s.duration_ms-s.position_ms);
    const bool playing=s.playback==2 || s.playback==3;
    const bool digits_visible=playing && (s.playback!=3 || (ticks_/5)%2==0);
    Blit(dc,"titlebar.bmp",6,3,9,9,0,pressed==TTP_SKIN_MENU?9:0);
    Blit(dc,"titlebar.bmp",244,3,9,9,9,pressed==TTP_SKIN_MINIMIZE?9:0);
    Blit(dc,"titlebar.bmp",254,3,9,9,pressed==hitShade?9:0,view.shaded?27:18);
    Blit(dc,"titlebar.bmp",264,3,9,9,18,pressed==TTP_SKIN_CLOSE?9:0);
    if(view.shaded) {
        Visual(view,dc,{79,5,117,10});
        if(digits_visible) Text(dc,{126,4,158,10},(s.elapsed?L"":L"-")+Time(time),text_color_,true);
        const int pos=view.seek>=0?view.seek:int(s.duration_ms>0?std::clamp<int64_t>(s.position_ms,0,s.duration_ms)*10000/s.duration_ms:0);
        Blit(dc,"titlebar.bmp",226,4,17,7,0,36);
        if(s.duration_ms>0) Blit(dc,"titlebar.bmp",227+pos*12/10000,4,3,7,17+(pos<4167?0:pos<6667?3:6),36);
        if(pressed>=TTP_SKIN_PLAY && pressed<=TTP_SKIN_OPEN) {
            static constexpr int commands[]={TTP_SKIN_PREVIOUS,TTP_SKIN_PLAY,TTP_SKIN_PAUSE,TTP_SKIN_STOP,TTP_SKIN_NEXT,TTP_SKIN_OPEN};
            static constexpr int edges[]={168,176,186,195,204,215,224};
            for(int i=0;i<6;++i) if(pressed==commands[i]) {RECT r{edges[i],2,edges[i+1],11};DrawEdge(dc,&r,EDGE_SUNKEN,BF_RECT);}
        }
        return;
    }
    for(int i=0;i<5;++i) {
        static constexpr int commands[]={TTP_SKIN_PREVIOUS,TTP_SKIN_PLAY,TTP_SKIN_PAUSE,TTP_SKIN_STOP,TTP_SKIN_NEXT};
        if(!Blit(dc,"cbuttons.bmp",16+i*23,88,i==4?22:23,18,i*23,pressed==commands[i]?18:0)) {
            static const wchar_t* labels[]={L"|<",L">",L"||",L"[]",L">|"};
            Text(dc,{16+i*23,88,38+i*23,106},labels[i],current_);
        }
    }
    if(!Blit(dc,"cbuttons.bmp",136,89,22,16,114,pressed==TTP_SKIN_OPEN?16:0)) Text(dc,{136,89,158,106},L"+",current_);
    Blit(dc,"shufrep.bmp",164,89,47,15,28,(s.mode==4?30:0)+(pressed==hitShuffle?15:0));
    Blit(dc,"shufrep.bmp",210,89,28,15,0,(s.mode==1||s.mode==3?30:0)+(pressed==hitRepeat?15:0));
    Blit(dc,"shufrep.bmp",219,58,23,12,pressed==TTP_SKIN_EQUALIZER?46:0,61+(s.equalizer_visible?12:0));
    Blit(dc,"shufrep.bmp",242,58,23,12,23+(pressed==TTP_SKIN_PLAYLIST?46:0),61+(s.playlist_visible?12:0));
    const int volume=std::clamp(s.volume,0,100), balance=std::clamp(s.balance,-100,100);
    Blit(dc,"volume.bmp",107,57,68,13,0,(volume*27/100)*15);
    Blit(dc,"volume.bmp",107+volume*51/100,58,14,11,pressed==hitVolume?0:15,422);
    const char* pan=images_.contains("balance.bmp")?"balance.bmp":"volume.bmp";
    Blit(dc,pan,177,57,38,13,9,(std::abs(balance)*27/100)*15);
    Blit(dc,pan,177+(balance+100)*24/200,58,14,11,pressed==hitBalance?0:15,422);
    Blit(dc,"posbar.bmp",16,72,248,10);
    if(s.duration_ms>0) {
        const int pos=view.seek>=0?view.seek*219/10000:int(std::clamp<int64_t>(s.position_ms,0,s.duration_ms)*219/s.duration_ms);
        Blit(dc,"posbar.bmp",16+pos,72,29,10,pressed==hitSeek?278:248,0);
    }
    int seconds=int(std::max<int64_t>(0,time/1000));
    const char* numbers=images_.contains("nums_ex.bmp")?"nums_ex.bmp":"numbers.bmp";
    const int digits[]={seconds/600%10,seconds/60%10,seconds/10%6,seconds%10};
    const int places[]={48,60,78,90};
    for(int i=0;i<4;++i) if(!Blit(dc,numbers,places[i],26,9,13,(digits_visible?digits[i]:10)*9,0)) {
        Text(dc,{places[i],26,places[i]+9,39},std::to_wstring(digits[i]),current_);
    }
    if(!digits_visible || seconds<6000) Blit(dc,numbers,36,26,9,13,90,0);
    if(digits_visible && seconds>=6000) Blit(dc,numbers,36,26,9,13,(seconds/6000%10)*9,0);
    if(digits_visible && !s.elapsed) {
        if(images_.contains("nums_ex.bmp")) Blit(dc,numbers,seconds>=6000?25:36,26,9,13,99,0);
        else Blit(dc,numbers,seconds>=6000?25:36,32,5,1,20,6);
    }
    const int icon=s.playback==2?0:s.playback==3?9:18;
    Blit(dc,"playpaus.bmp",26,28,9,9,icon,0);
    Blit(dc,"playpaus.bmp",24,28,3,9,s.playback==1?39:36,0);
    Visual(view,dc,{24,43,100,59});
    Blit(dc,"titlebar.bmp",10,22,8,43,304,0);
    Blit(dc,"monoster.bmp",212,41,28,12,29,s.channels==1?0:12);
    Blit(dc,"monoster.bmp",239,41,29,12,0,s.channels>=2?0:12);
    const auto status=HostStatus(host_);
    if(!status.empty()) Title(dc,{111,27,265,33},status);
    else if(ticks_<feedback_until_) {
        TextBackground(dc,{111,27,265,33},true);
        Text(dc,{111,27,265,33},feedback_,text_color_,true);
    } else Title(dc,{111,27,265,33},s.title);
    Text(dc,{111,43,131,49},s.bitrate>0?std::to_wstring(s.bitrate/1000):L"",current_,true);
    Text(dc,{156,43,171,49},s.sample_rate>0?std::to_wstring(s.sample_rate/1000):L"",current_,true);
}
void Skin::DrawPlaylist(View& view,HDC dc,int width,int height,const TtpSkinState& s) {
    SyncPlaylistFont();
    Fill(dc,{0,0,width,height},background_);
    const int state=GetActiveWindow()==view.window?0:21;
    const int pressed=view.hot?view.pressed:0;
    const auto tile=[&](int x,int y,int w,int h,int sx,int sy,int tw,int th) {
        for(int yy=0;yy<h;yy+=th) for(int xx=0;xx<w;xx+=tw)
            Blit(dc,"pledit.bmp",x+xx,y+yy,std::min(tw,w-xx),std::min(th,h-yy),sx,sy,tw,th);
    };
    if(view.shaded) {
        Blit(dc,"pledit.bmp",0,0,25,14,72,42);
        tile(25,0,width-75,14,72,57,25,14);
        Blit(dc,"pledit.bmp",width-50,0,50,14,99,state?57:42);
        Text(dc,{6,0,width-87,14},s.title,normal_);
        Text(dc,{width-85,0,width-35,14},(s.elapsed?L"":L"-")+Time(s.elapsed?s.position_ms:std::max<int64_t>(0,s.duration_ms-s.position_ms)),normal_);
    } else {
        Blit(dc,"pledit.bmp",0,0,25,20,0,state);
        // draw_pe_tbar splits an odd tile into 12/13 pixels before the full
        // tiles on each side. A partial trailing tile alone is stretched.
        const int tiles=(width-150)/25,half=tiles/2;
        int title_x=25;
        if(tiles&1) {Blit(dc,"pledit.bmp",title_x,0,12,20,127,state);title_x+=12;}
        tile(title_x,0,half*25,20,127,state,25,20);title_x+=half*25;
        Blit(dc,"pledit.bmp",title_x,0,100,20,26,state);title_x+=100;
        if(tiles&1) {Blit(dc,"pledit.bmp",title_x,0,13,20,127,state);title_x+=13;}
        tile(title_x,0,half*25,20,127,state,25,20);title_x+=half*25;
        tile(title_x,0,(width-150)%25,20,127,state,25,20);
        Blit(dc,"pledit.bmp",width-25,0,25,20,153,state);
        tile(0,20,12,height-58,0,42,12,29);
        tile(width-20,20,20,height-58,31,42,20,29);
        Blit(dc,"pledit.bmp",0,height-38,125,38,0,72);
        const int visual=width>=350?75:0;
        tile(125,height-38,width-275-visual,38,179,0,25,38);
        if(visual) {Blit(dc,"pledit.bmp",width-225,height-38,75,38,205,0);Visual(view,dc,{width-223,height-26,width-151,height-10});}
        Blit(dc,"pledit.bmp",width-150,height-38,150,38,126,72);
        const int rows=std::max(1,(height-60)/row_height_);
        const int maximum=std::max(0,int(s.track_count)-rows);
        view.scroll=std::clamp(view.scroll,0,maximum);
        const int row_clip=SaveDC(dc);IntersectClipRect(dc,12,22,width-20,height-38);
        const int drawn_rows=std::max(1,(height-60+row_height_-1)/row_height_);
        for(int i=0;i<drawn_rows && uint32_t(view.scroll+i)<s.track_count;++i) {
            const int index=view.scroll+i,y=22+i*row_height_;
            const uint32_t flags=host_.selection?host_.selection(host_.context,uint32_t(index)):(index==view.selected?1u:0u);
            if(flags&2) view.selected=index;
            if(flags&1) Fill(dc,{12,y,width-20,y+row_height_},selection_);
            TtpSkinTrack track{};track.size=sizeof(track);
            if(host_.track && host_.track(host_.context,uint32_t(index),&track)) {
                const auto color=index==s.playing_row?current_:normal_;
                const auto duration=track.duration_ms>=0?Time(track.duration_ms):L"";
                const auto old=SelectObject(dc,playlist_font_.Font());SIZE size{};GetTextExtentPoint32W(dc,duration.data(),int(duration.size()),&size);SelectObject(dc,old);
                Text(dc,{13,y,width-24-size.cx,y+row_height_},std::to_wstring(index+1)+L". "+track.title,color,false,playlist_font_.Font());
                Text(dc,{width-22-size.cx,y,width-20,y+row_height_},duration,color,false,playlist_font_.Font());
            }
        }
        RestoreDC(dc,row_clip);
        const int drop=view.external_drop>=0?view.external_drop:view.row_drag?view.drop:-1;
        if(drop>=view.scroll && drop<=view.scroll+rows) {
            const int y=std::min(height-39,22+(drop-view.scroll)*row_height_);
            Fill(dc,{12,y,width-20,y+1},current_);
        }
        Blit(dc,"pledit.bmp",width-15,20+(maximum?view.scroll*(height-76)/maximum:0),8,18,pressed==hitScroll?61:52,53);
        // draw_pl's bottom strips already contain the five toolbar icons.
        // The y=111/130/149 cells belong to Winamp's expanded flyout menus;
        // painting them here replaces custom icons with menu-item labels.
        // This provider opens the host's native menus instead of that flyout.
        auto summary=statistics_.empty()?(s.track_count?L"?/?":L"0:00/0:00"):statistics_;
        summary.resize(18,L' '); // draw_pe_infostr overwrites all 18 cells.
        Text(dc,{width-143,height-28,width-53,height-22},summary,normal_,true);
        DrawPlaylistTime(dc,width,height,s);
    }
    Blit(dc,"pledit.bmp",width-11,3,9,9,pressed==TTP_SKIN_PLAYLIST?52:167,pressed==TTP_SKIN_PLAYLIST?42:3);
    if(pressed==hitShade) Blit(dc,"pledit.bmp",width-20,3,9,9,view.shaded?150:62,42);
    else Blit(dc,"pledit.bmp",width-20,3,9,9,view.shaded?128:158,view.shaded?45:3);
}
void Skin::DrawPlaylistTime(HDC dc,int width,int height,const TtpSkinState& s) const {
    // draw_pe_timedisp uses fixed digit slots. The colon and the gaps belong
    // to pledit.bmp; writing a normal time string overwrites that artwork.
    const int x=width-86,y=height-15;
    const bool clear=s.playback!=2 && (s.playback!=3 || (ticks_/5)%2!=0);
    const int offsets[]={0,4,9,14,22,27};
    if(clear) {
        for(int offset:offsets) Blit(dc,"text.bmp",x+offset,y,offset?5:4,6,142,0);
        return;
    }
    const auto seconds=std::max<int64_t>(0,(s.elapsed?s.position_ms:s.duration_ms-s.position_ms)/1000);
    const auto minutes=seconds/60;
    const bool hundreds=minutes>=100;
    if(!hundreds) Blit(dc,"text.bmp",x,y,4,6,142,0);
    Blit(dc,"text.bmp",x+(hundreds?0:4),y,3,6,s.elapsed?142:75,s.elapsed?0:6);
    if(hundreds) Blit(dc,"text.bmp",x+4,y,5,6,int(minutes/100%10)*5,6);
    Blit(dc,"text.bmp",x+9,y,5,6,int(minutes/10%10)*5,6);
    Blit(dc,"text.bmp",x+14,y,5,6,int(minutes%10)*5,6);
    Blit(dc,"text.bmp",x+22,y,5,6,int(seconds/10%6)*5,6);
    Blit(dc,"text.bmp",x+27,y,5,6,int(seconds%10)*5,6);
}
void Skin::DrawEqualizer(View& view,HDC dc,const TtpSkinState& s) {
    const bool active=GetActiveWindow()==view.window;
    const int pressed=view.hot?view.pressed:0;
    if(view.shaded) {
        Blit(dc,"eq_ex.bmp",0,0,275,14,0,active?0:15);
        const int vol=std::clamp(s.volume,0,100),pan=std::clamp(s.balance,-100,100);
        Blit(dc,"eq_ex.bmp",61+vol*94/100,4,3,7,1+(vol<33?0:vol<66?3:6),30);
        Blit(dc,"eq_ex.bmp",164+(pan+100)*39/200,4,3,7,11+(pan<-33?0:pan<33?3:6),30);
        if(pressed==hitShade) Blit(dc,"eq_ex.bmp",254,3,9,9,1,47);
        Blit(dc,"eq_ex.bmp",264,3,9,9,11,pressed==TTP_SKIN_EQUALIZER?47:38);
        return;
    }
    Blit(dc,"eqmain.bmp",0,0,275,116);
    Blit(dc,"eqmain.bmp",0,0,275,14,0,active?134:149);
    Blit(dc,"eqmain.bmp",264,3,9,9,0,pressed==TTP_SKIN_EQUALIZER?125:116);
    if(pressed==hitShade) Blit(dc,"eq_ex.bmp",254,3,9,9,1,38);
    Blit(dc,"eqmain.bmp",14,18,25,12,10+(s.eq_enabled?59:0)+(pressed==TTP_SKIN_EQ_ENABLE?118:0),119);
    Blit(dc,"eqmain.bmp",39,18,33,12,35,119);
    // No per-track autoload: use the skin's off-state artwork unchanged.
    // Tooltip/cursor feedback describes the unavailable action.
    Blit(dc,"eqmain.bmp",217,18,44,12,224,pressed==TTP_SKIN_EQ_PRESETS?176:164);
    for(int i=0;i<11;++i) {
        const int x=i?78+(i-1)*18:21;
        const int pos=(12-std::clamp(s.eq[i],-12,12))*63/24,frame=27-pos*28/64;
        Blit(dc,"eqmain.bmp",x,38,14,63,13+(frame%14)*15,frame<14?164:229);
        Blit(dc,"eqmain.bmp",x+1,89-(63-pos)*52/64,11,11,0,pressed==TTP_SKIN_EQ_VALUE+i?176:164);
    }
    Blit(dc,"eqmain.bmp",86,17,113,19,0,294);
    Blit(dc,"eqmain.bmp",86,35-(12-std::clamp(s.eq[0],-12,12))*63/24*19/64,113,1,0,314);
    // Cubic Hermite interpolation with classic tension 0.1 and zero bias /
    // continuity; repeated end keys avoid endpoint discontinuities.
    std::array<float,12> keys{};
    for(int i=1;i<=10;++i) keys[i]=float((12-std::clamp(s.eq[i],-12,12))*63/24)*19/64;
    keys[0]=keys[1];keys[11]=keys[10];int last=-1;
    for(int x=0;x<109;++x) {
        const float frame=1.0f+x/12.0f;const int i=int(frame);const float t=frame-i;
        const float p0=keys[i-1],p1=keys[i],p2=keys[std::min(i+1,11)],p3=keys[std::min(i+2,11)];
        const float m1=.45f*(p2-p0),m2=.45f*(p3-p1);
        const int y=std::clamp(int((2*t*t*t-3*t*t+1)*p1+(t*t*t-2*t*t+t)*m1+(-2*t*t*t+3*t*t)*p2+(t*t*t-t*t)*m2),0,18);
        const int top=last<0?y:std::min(y,last),bottom=last<0?y:std::max(y,last);
        Blit(dc,"eqmain.bmp",88+x,17+top,1,bottom-top+1,115,294+top);last=y;
    }
}
void Skin::Draw(View& view,HDC dc,int width,int height) {
    const int saved=SaveDC(dc); IntersectClipRect(dc,0,0,width,height);
    Fill(dc,{0,0,width,height},background_);
    const auto state=State();
    if(view.kind==0) DrawMain(view,dc,state);
    else if(view.kind==1) DrawPlaylist(view,dc,width,height,state);
    else if(view.kind==2) DrawEqualizer(view,dc,state);
    else DrawVideo(view,dc,width,height);
    RestoreDC(dc,saved);
}
void Skin::Paint(HWND window,HDC dc,bool child_background) {
    for(auto& view:views_) if(view.window==window) {
        RECT rect{}; GetClientRect(window,&rect);
        if(IsIconic(window)) GetClipBox(dc,&rect);
        HDC memory=CreateCompatibleDC(dc); HBITMAP bitmap=CreateCompatibleBitmap(dc,std::max(1L,rect.right),std::max(1L,rect.bottom));
        if(!memory || !bitmap) { if(memory) DeleteDC(memory); if(bitmap) DeleteObject(bitmap); return; }
        const auto old=SelectObject(memory,bitmap);
        const int scale=(view.kind==1 || view.kind==3)?1:scale_;
        if(scale!=1) {SetMapMode(memory,MM_ANISOTROPIC);SetWindowExtEx(memory,1,1,nullptr);SetViewportExtEx(memory,scale,scale,nullptr);}
        Draw(view,memory,rect.right/scale,rect.bottom/scale);
        SetMapMode(memory,MM_TEXT);
        // The supplied DC can come from GetDC/WM_PRINTCLIENT as well as
        // BeginPaint. Protect live host widgets; the lyric parent does not
        // necessarily have WS_CLIPCHILDREN. A transparent child's erase DC
        // needs its clean backing, while opaque RichEdit pixels stay excluded.
        const int saved=SaveDC(dc);
        if(!saved) {SelectObject(memory,old);DeleteObject(bitmap);DeleteDC(memory);return;}
        if(view.kind==3) for(HWND child=GetWindow(window,GW_CHILD);child;child=GetWindow(child,GW_HWNDNEXT)) {
            const auto role=reinterpret_cast<UINT_PTR>(GetPropW(child,TTP_SKIN_CONTENT_CHILD));
            if(role && (GetWindowLongPtrW(child,GWL_STYLE)&WS_VISIBLE) &&
               !(child_background && role==TTP_SKIN_CONTENT_CHILD_TRANSPARENT)) {
                RECT bounds{};GetWindowRect(child,&bounds);
                MapWindowPoints(nullptr,window,reinterpret_cast<POINT*>(&bounds),2);
                ExcludeClipRect(dc,bounds.left,bounds.top,bounds.right,bounds.bottom);
            }
        }
        BitBlt(dc,0,0,rect.right,rect.bottom,memory,0,0,SRCCOPY);
        RestoreDC(dc,saved);
        SelectObject(memory,old); DeleteObject(bitmap); DeleteDC(memory); return;
    }
}
HBITMAP Skin::Preview() {
    HDC screen=GetDC(nullptr), dc=CreateCompatibleDC(screen);
    HBITMAP result=CreateCompatibleBitmap(screen,275,116); ReleaseDC(nullptr,screen);
    if(!dc || !result) { if(dc) DeleteDC(dc); if(result) DeleteObject(result); return nullptr; }
    const auto old=SelectObject(dc,result); View view{}; Draw(view,dc,275,116);
    SelectObject(dc,old); DeleteDC(dc); return result;
}
void Skin::Region(View& view) {
    const char* names[]={"normal","playlist","equalizer","video"};
    std::string name=names[view.kind];
    if(view.shaded) name=view.kind==0?"windowshade":name+"ws";
    const auto key="region/"+name+"/";
    const auto counts=ini_.find(key+"numpoints"), points=ini_.find(key+"pointlist");
    HRGN region=nullptr;
    if(view.kind!=1 && view.kind!=3 && counts!=ini_.end() && points!=ini_.end()) {
        auto c=counts->second,p=points->second; std::replace(c.begin(),c.end(),',',' '); std::replace(p.begin(),p.end(),',',' ');
        std::istringstream cs(c),ps(p); std::vector<int> sizes; std::vector<POINT> coords; int n,total=0;
        while(cs>>n) { if(n<3 || n>8192-total) {sizes.clear();break;} sizes.push_back(n);total+=n; }
        POINT point{};
        while(ps>>point.x>>point.y) { if(std::abs(int64_t(point.x))>16384 || std::abs(int64_t(point.y))>16384 || coords.size()>=8192) {coords.clear();break;} coords.push_back(point); }
        for(auto& coordinate:coords) {coordinate.x*=scale_;coordinate.y*=scale_;}
        if(!sizes.empty() && coords.size()==size_t(total)) region=CreatePolyPolygonRgn(coords.data(),sizes.data(),int(sizes.size()),WINDING);
    }
    if(!region) { RECT r{};GetClientRect(view.window,&r);region=CreateRectRgn(0,0,r.right,r.bottom); }
    if(!SetWindowRgn(view.window,region,TRUE) && region) DeleteObject(region);
}
void Skin::HideChildren(View& view) {
    for(HWND child=GetWindow(view.window,GW_CHILD);child;child=GetWindow(child,GW_HWNDNEXT)) {
        if(view.kind==3 && GetPropW(child,TTP_SKIN_CONTENT_CHILD)) continue;
        if(std::none_of(view.children.begin(),view.children.end(),[child](const auto& p){return p.first==child;}))
            view.children.emplace_back(child,(GetWindowLongPtrW(child,GWL_STYLE)&WS_VISIBLE)!=0);
        if(GetWindowLongPtrW(child,GWL_STYLE)&WS_VISIBLE) ShowWindow(child,SW_HIDE);
    }
}
HRESULT Skin::Attach(const TtpSkinWindows& windows) {
    Detach();binding_=true;scale_=layout_.scale;
    const HWND handles[]={windows.player,windows.playlist,windows.equalizer,
        windows.size>=sizeof(windows) && host_.content?windows.lyrics:nullptr};
    RECT origin{}; GetWindowRect(windows.player,&origin);
    if(layout_.bounds[0].right>layout_.bounds[0].left && layout_.bounds[0].bottom>layout_.bounds[0].top)
        origin=layout_.bounds[0];
    for(int i=0;i<4;++i) {
        if(i==0 && fallback_only_) continue;
        if(!IsWindow(handles[i])) { if(i==0) {Detach();binding_=false;return E_INVALIDARG;} continue; }
        auto& v=views_[i];v.shaded=layout_.shaded[i];v.seek=-1;v.scroll=i==1?layout_.scroll:0;v.selected=-1;v.external_drop=-1;v.window=handles[i]; GetWindowRect(v.window,&v.saved);
        v.saved_region=CreateRectRgn(0,0,0,0);
        if(GetWindowRgn(v.window,v.saved_region)==ERROR) {DeleteObject(v.saved_region);v.saved_region=nullptr;}
        if(!SetWindowSubclass(v.window,Subclass,subclassId,reinterpret_cast<DWORD_PTR>(&v))) {Detach();binding_=false;return E_FAIL;}
        HideChildren(v);
        v.expanded_height=layout_.expanded[i];
        const auto& saved=layout_.bounds[i];
        const bool have_saved=saved.right>saved.left && saved.bottom>saved.top;
        const int factor=(i==1 || i==3)?1:scale_;
        SetWindowPos(v.window,nullptr,have_saved?saved.left:i==3?v.saved.left:origin.left,
            have_saved?saved.top:i==3?v.saved.top:origin.top+(i==1?232:i==2?116:0)*scale_,
            have_saved?saved.right-saved.left:275*factor,
            v.shaded?14*factor:v.expanded_height,SWP_NOACTIVATE|SWP_NOZORDER);
        Region(v);
        SetTimer(v.window,timerId,i==3?33:100,nullptr);
        InvalidateRect(v.window,nullptr,FALSE);
    }
    binding_=false;CaptureLayout();return S_OK;
}
bool Skin::Handles(HWND window) const {
    return window && std::any_of(views_.begin(),views_.end(),[window](const View& view){return view.window==window;});
}
bool Skin::VolumeTracking() const {
    return std::any_of(views_.begin(),views_.end(),[](const View& v){
        return v.window && v.pressed==hitVolume && GetCapture()==v.window;
    });
}
bool Skin::PlaylistReveal(uint32_t row,int32_t caret) {
    SyncPlaylistFont();
    auto& v=views_[1];if(!v.window || row>INT_MAX || row>=State().track_count)return false;
    RECT bounds{};GetClientRect(v.window,&bounds);
    const int rows=std::max(1,((v.shaded?v.expanded_height:int(bounds.bottom))-60)/row_height_);
    v.selected=caret;
    if(int(row)<v.scroll)v.scroll=int(row);
    else if(int(row)>=v.scroll+rows)v.scroll=int(row)-rows+1;
    HideTip(v);InvalidateRect(v.window,nullptr,FALSE);return true;
}
bool Skin::PlaylistDrop(TtpSkinPlaylistDrop& drop) {
    SyncPlaylistFont();
    auto& view=views_[1];
    if(drop.size<sizeof(drop) || !view.window || drop.window!=view.window || drop.phase>TTP_SKIN_DROP_LEAVE) return false;
    drop.insertion=-1;
    RECT client{};
    if(drop.phase!=TTP_SKIN_DROP_LEAVE && GetClientRect(view.window,&client) && PtInRect(&client,drop.point)) {
        const int count=int(State().track_count);
        if(view.shaded) drop.insertion=count; // Winamp pledit.cpp: folded playlist appends.
        else {
            int top=view.scroll;
            drop.insertion=PlaylistInsertion({12,22,client.right-20,client.bottom-38},row_height_,drop.point,top,count,
                drop.phase==TTP_SKIN_DROP_PREVIEW);
            if(drop.phase==TTP_SKIN_DROP_PREVIEW && view.scroll!=top) {
                view.scroll=top;InvalidateRect(view.window,nullptr,FALSE);
            }
        }
    }
    if(drop.phase!=TTP_SKIN_DROP_QUERY && view.external_drop!=drop.insertion) {
        view.external_drop=drop.insertion;
        InvalidateRect(view.window,nullptr,FALSE);
    }
    return true;
}
bool Skin::PlaylistViewport(HWND window,uint32_t& first,uint32_t& count) {
    auto& view=views_[1];
    if(!window || view.window!=window)return false;
    first=count=0;
    if(view.shaded || !IsWindowVisible(window) || IsIconic(window))return true;
    SyncPlaylistFont();RECT client{};GetClientRect(window,&client);
    const int height=std::max(0,int(client.bottom)-60),tracks=int(State().track_count);
    view.scroll=std::clamp(view.scroll,0,std::max(0,tracks-std::max(1,height/row_height_)));
    first=uint32_t(view.scroll);
    count=uint32_t(std::min(tracks-view.scroll,(height+row_height_-1)/row_height_));
    return true;
}
void Skin::Detach() noexcept {
    CaptureLayout();
    for(auto& v:views_) {
        HideTip(v);
        if(IsWindow(v.tooltip)) DestroyWindow(v.tooltip);
        v.tooltip=nullptr;
        if(IsWindow(v.window)) {
            KillTimer(v.window,timerId);
            EndDrag(v);
            if(GetCapture()==v.window) ReleaseCapture();
            RemoveWindowSubclass(v.window,Subclass,subclassId);
            for(const auto& [child,visible]:v.children) if(IsWindow(child) && visible) ShowWindow(child,SW_SHOWNA);
            SetWindowPos(v.window,nullptr,0,0,v.saved.right-v.saved.left,v.saved.bottom-v.saved.top,SWP_NOMOVE|SWP_NOACTIVATE|SWP_NOZORDER);
            if(SetWindowRgn(v.window,v.saved_region,TRUE)) v.saved_region=nullptr;
            InvalidateRect(v.window,nullptr,TRUE);
        }
        if(v.saved_region) DeleteObject(v.saved_region);
        v.saved_region=nullptr;v.window=nullptr;v.children.clear();v.pressed=0;v.external_drop=-1;v.dragging=v.resizing=v.host_drag=false;
    }
}
void Skin::ToggleShade(View& v) {
    if(v.kind==3 || !IsWindow(v.window) || IsIconic(v.window)) return;
    RECT r{};if(!GetClientRect(v.window,&r)) return;
    if(!v.shaded) v.expanded_height=r.bottom;
    v.shaded=!v.shaded;
    const SIZE size{r.right,v.shaded?14*(v.kind==1?1:scale_):v.expanded_height};
    // The host includes native lyrics in the same geometry transaction. This
    // must finish here, before repaint or persistence observes the new layout.
    if(!(host_.resize && host_.resize(host_.context,v.window,size)) &&
       !SetWindowPos(v.window,nullptr,0,0,size.cx,size.cy,SWP_NOMOVE|SWP_NOACTIVATE|SWP_NOZORDER))
        v.shaded=!v.shaded;
    Region(v);InvalidateRect(v.window,nullptr,FALSE);
    CaptureLayout();
}
void Skin::Shade() { if(views_[0].window) ToggleShade(views_[0]); }
bool Skin::Translate(const MSG& message) {
    if(message.message!=WM_MOUSEWHEEL || !message.hwnd || GetCapture() ||
       !IsWindowEnabled(views_[0].window) || GetWindowThreadProcessId(message.hwnd,nullptr)!=GetCurrentThreadId()) return false;
    const POINT point{GET_X_LPARAM(message.lParam),GET_Y_LPARAM(message.lParam)};
    const HWND hit=WindowFromPoint(point);
    for(const auto& view:views_) {
        if(view.window && hit==view.window && message.hwnd!=view.window && IsWindowVisible(view.window) && IsWindowEnabled(view.window)) {
            SendMessageW(view.window,message.message,message.wParam,message.lParam);return true;
        }
    }
    return false;
}
void Skin::Feedback(const std::wstring& text) {feedback_=text;feedback_until_=ticks_+15;}
void Skin::UpdateStatistics() {
    const auto count=State().track_count;
    if(!stats_index_ || stats_count_!=count) {
        stats_index_=0;stats_count_=count;stats_total_=stats_selected_=0;stats_unknown_=stats_selected_unknown_=false;
    }
    // Bounded UI-thread work even for huge libraries; never scan every track
    // from WM_PAINT. Metadata/selection changes converge on the next pass.
    for(unsigned work=0;work<128 && stats_index_<count;++work,++stats_index_) {
        TtpSkinTrack track{};track.size=sizeof(track);
        const bool known=host_.track && host_.track(host_.context,stats_index_,&track) && track.duration_ms>=0;
        if(known) stats_total_+=track.duration_ms/1000;else stats_unknown_=true;
        if(host_.selection && (host_.selection(host_.context,stats_index_)&1)) {
            if(known) stats_selected_+=track.duration_ms/1000;else stats_selected_unknown_=true;
        }
    }
    if(stats_index_==count) {
        statistics_=DurationSummary(stats_selected_,stats_selected_unknown_)+L"/"+DurationSummary(stats_total_,stats_unknown_);stats_index_=0;
    }
}
void Skin::ToggleScale() {
    const int old=scale_;scale_=scale_==1?2:1;
    RECT origin{};GetWindowRect(views_[0].window,&origin);
    for(auto& v:views_) if(v.window) {
        EndDrag(v);RECT r{};GetWindowRect(v.window,&r);
        const int factor=(v.kind==1 || v.kind==3)?old:scale_;
        SetWindowPos(v.window,nullptr,origin.left+(r.left-origin.left)*scale_/old,
            origin.top+(r.top-origin.top)*scale_/old,(r.right-r.left)*factor/old,
            (r.bottom-r.top)*factor/old,SWP_NOZORDER|SWP_NOACTIVATE);
        if(v.kind!=1 && v.kind!=3) v.expanded_height=v.expanded_height*scale_/old;
        Region(v);InvalidateRect(v.window,nullptr,FALSE);
    }
    CaptureLayout();
}
int Skin::Hit(const View& v,POINT p,RECT* bounds) const {
    RECT r{};GetClientRect(v.window,&r);const int scale=(v.kind==1 || v.kind==3)?1:scale_;
    r.right/=scale;r.bottom/=scale;
    if(!Inside(p,0,0,r.right,r.bottom)) return 0;
    const auto inside=[&](int x,int y,int w,int h) {
        if(!Inside(p,x,y,w,h)) return false;
        if(bounds) *bounds={x,y,x+w,y+h};
        return true;
    };
    if(v.kind==3) {
        if(inside(r.right-11,3,9,9)) return TTP_SKIN_LYRICS;
        if(inside(r.right-20,r.bottom-20,20,20)) return hitResize;
        for(int i=0;i<5;++i)
            if(inside(9+i*15,r.bottom-29,15,18)) return hitVideoFullscreen+i;
        if(inside(0,0,r.right,20)) return hitDrag;
        // Content clicks do not cycle modes or fall through to the native
        // lyric/visual controls. Only explicit buttons and menus switch it.
        return 0;
    }
    // Precise controls precede the title drag surface, including windowshade.
    if(inside(r.right-11,3,9,9)) return v.kind==0?TTP_SKIN_CLOSE:v.kind==1?TTP_SKIN_PLAYLIST:TTP_SKIN_EQUALIZER;
    if(inside(r.right-(v.kind==1?20:21),3,9,9)) return hitShade;
    if(v.kind==0) {
        if(inside(244,3,9,9)) return TTP_SKIN_MINIMIZE;
        if(inside(6,3,9,9)) return TTP_SKIN_MENU;
    }
    static constexpr int buttons[]={TTP_SKIN_PREVIOUS,TTP_SKIN_PLAY,TTP_SKIN_PAUSE,TTP_SKIN_STOP,TTP_SKIN_NEXT,TTP_SKIN_OPEN};
    if(v.shaded) {
        if(v.kind==0) {
            constexpr int edges[]={168,176,186,195,204,215,224};
            for(int i=0;i<6;++i) if(inside(edges[i],2,edges[i+1]-edges[i],9)) return buttons[i];
            if(inside(226,3,17,9)) return hitSeek;
            if(inside(126,3,32,9)) return TTP_SKIN_TIME_MODE;
            if(inside(79,5,38,5)) return TTP_SKIN_VISUAL_NEXT;
        } else if(v.kind==2) {
            if(inside(61,3,102,9)) return hitVolume;
            if(inside(166,3,44,9)) return hitBalance;
        } else {
            if(inside(r.right-29,0,8,14)) return hitResize;
            if(inside(r.right-85,0,50,14)) return TTP_SKIN_TIME_MODE;
        }
        return hitDrag;
    }
    if(p.y<14) return hitDrag;
    if(v.kind==0) {
        struct Control {int x,y,w,h,hit;};
        static constexpr Control controls[]={
            {136,89,22,16,TTP_SKIN_OPEN},{164,89,46,15,hitShuffle},{210,89,28,15,hitRepeat},
            {219,58,23,12,TTP_SKIN_EQUALIZER},{242,58,23,12,TTP_SKIN_PLAYLIST},
            {107,57,68,13,hitVolume},{177,57,38,13,hitBalance},{16,72,248,10,hitSeek},
            {36,26,64,13,TTP_SKIN_TIME_MODE},{24,43,76,16,TTP_SKIN_VISUAL_NEXT},
            {11,24,8,8,TTP_SKIN_OPTIONS},{11,32,8,8,TTP_SKIN_ALWAYS_ON_TOP},
            {11,40,8,8,TTP_SKIN_PROPERTIES},{11,48,8,8,hitScale},{11,56,8,7,TTP_SKIN_VISUAL_MENU},
            {111,24,154,11,TTP_SKIN_PROPERTIES}};
        for(int i=0;i<5;++i) if(inside(16+i*23,88,i==4?22:23,18)) return buttons[i];
        for(const auto& c:controls) if(inside(c.x,c.y,c.w,c.h)) return c.hit;
    } else if(v.kind==1) {
        if(inside(r.right-18,r.bottom-18,18,18)) return hitResize;
        if(inside(r.right-15,r.bottom-36,8,5)) return hitScrollUp;
        if(inside(r.right-15,r.bottom-31,8,5)) return hitScrollDown;
        if(inside(r.right-15,20,8,r.bottom-58)) return hitScroll;
        if(inside(12,22,r.right-32,r.bottom-60)) {
            const int row=(p.y-22)/row_height_;
            if(bounds) *bounds={12,22+row*row_height_,r.right-20,22+(row+1)*row_height_};
            return hitRow+v.scroll+row;
        }
        const int xs[]={14,43,72,101,int(r.right)-44};
        for(int i=0;i<5;++i) if(inside(xs[i],r.bottom-30,22,18)) return hitListAdd+i;
        for(int i=0;i<6;++i) if(inside(r.right-144+i*9,r.bottom-15,9,8)) return buttons[i];
        if(inside(r.right-87,r.bottom-18,34,10)) return TTP_SKIN_TIME_MODE;
        if(r.right>=350 && inside(r.right-223,r.bottom-26,72,16)) return TTP_SKIN_VISUAL_NEXT;
    } else {
        if(inside(14,18,25,12)) return TTP_SKIN_EQ_ENABLE;
        if(inside(39,18,33,12)) return hitAuto;
        if(inside(217,18,44,12)) return TTP_SKIN_EQ_PRESETS;
        for(int i=0;i<11;++i) if(inside(i?78+(i-1)*18:21,38,14,63)) return TTP_SKIN_EQ_VALUE+i;
        if(inside(42,33,25,10)) return hitEqUp;
        if(inside(42,65,25,10)) return hitEqFlat;
        if(inside(42,92,25,10)) return hitEqDown;
    }
    return hitDrag;
}
bool Skin::Sliding(int hit) const {
    return hit==hitVolume || hit==hitBalance || hit==hitSeek || hit==hitScroll ||
        (hit>=TTP_SKIN_EQ_VALUE && hit<TTP_SKIN_EQ_VALUE+11);
}
void Skin::BeginTrack(View& v,int hit,POINT p) {
    const auto s=State();int start=0,travel=1,thumb=1,position=0,coordinate=p.x;
    if(hit==hitSeek) {v.initial=0;v.seek=s.duration_ms>0?int(std::clamp<int64_t>(s.position_ms,0,s.duration_ms)*10000/s.duration_ms):-1;
        start=v.shaded?227:16;travel=v.shaded?12:219;thumb=v.shaded?3:29;position=std::max(0,v.seek)*travel/10000;}
    else if(hit==hitVolume) {v.initial=s.volume;start=v.shaded?61:107;travel=v.shaded?94:51;thumb=v.shaded?3:14;position=std::clamp(s.volume,0,100)*travel/100;}
    else if(hit==hitBalance) {v.initial=s.balance;start=v.shaded?164:177;travel=v.shaded?39:24;thumb=v.shaded?3:14;position=(std::clamp(s.balance,-100,100)+100)*travel/200;}
    else if(hit==hitScroll) {RECT r{};GetClientRect(v.window,&r);start=20;travel=std::max(1,int(r.bottom)-76);thumb=18;coordinate=p.y;
        const int maximum=std::max(0,int(s.track_count)-std::max(1,(int(r.bottom)-60)/row_height_));position=maximum?v.scroll*travel/maximum:0;}
    else {v.initial=s.eq[hit-TTP_SKIN_EQ_VALUE];start=38;travel=51;thumb=11;coordinate=p.y;position=(12-std::clamp(v.initial,-12,12))*travel/24;}
    const bool onThumb=coordinate>=start+position && coordinate<start+position+thumb;
    v.grab=onThumb?coordinate-start-position:thumb/2;
    v.track_value=v.initial;v.track_coordinate=coordinate;
    // Pressing the thumb is not an EQ change. Keep a local queued value because
    // host commands are asynchronous and State() may still report the old one.
    if(hit==hitBalance || (hit>=TTP_SKIN_EQ_VALUE && hit<TTP_SKIN_EQ_VALUE+11)) {
        if(onThumb)return;
        v.track_coordinate=INT_MIN;
    }
    Track(v,hit,p);
}
void Skin::Track(View& v,int hit,POINT p,bool final) {
    if(hit==hitVolume) {
        const int value=std::clamp((int(p.x)-(v.shaded?61:107)-v.grab)*100/(v.shaded?94:51),0,100);
        Command(TTP_SKIN_VOLUME,value);
        if(!NativeVolume(host_))Feedback(L"Volume: "+std::to_wstring(value)+L"%");
    } else if(hit==hitBalance) {
        int value=std::clamp((int(p.x)-(v.shaded?164:177)-v.grab)*200/(v.shaded?39:24)-100,-100,100);
        if(std::abs(value)<10) value=0;
        if(p.x==v.track_coordinate)value=v.track_value;
        if(final || value!=v.track_value)Command(TTP_SKIN_BALANCE,value);
        v.track_value=value;v.track_coordinate=p.x;Feedback(L"Balance: "+std::to_wstring(value));
    } else if(hit==hitSeek) {
        if(State().duration_ms<=0) return;
        v.seek=std::clamp((int(p.x)-(v.shaded?227:16)-v.grab)*10000/(v.shaded?12:219),0,10000);
        Feedback(L"Seek: "+Time(State().duration_ms*v.seek/10000));
    } else if(hit>=TTP_SKIN_EQ_VALUE && hit<TTP_SKIN_EQ_VALUE+11) {
        if(!State().eq_enabled) return;
        const int value=p.y==v.track_coordinate?v.track_value:std::clamp(12-MulDiv(int(p.y)-38-v.grab,24,51),-12,12);
        if(final || value!=v.track_value)Command(uint32_t(hit),value);
        v.track_value=value;v.track_coordinate=p.y;
        Feedback((hit==TTP_SKIN_EQ_VALUE?L"Preamp: ":L"EQ: ")+std::to_wstring(value)+L" dB");
    } else if(hit==hitScroll) {
        RECT r{};GetClientRect(v.window,&r);const auto s=State();
        const int maximum=std::max(0,int(s.track_count)-std::max(1,(int(r.bottom)-60)/row_height_));
        v.scroll=std::clamp((int(p.y)-20-v.grab)*maximum/std::max(1,int(r.bottom)-76),0,maximum);
    }
    InvalidateRect(v.window,nullptr,FALSE);
}
void Skin::SelectRow(View& v,int row) {
    if(row<0 || uint32_t(row)>=State().track_count) return;
    v.selected=row;stats_index_=0;
    const bool control=(GetKeyState(VK_CONTROL)&0x8000)!=0,shift=(GetKeyState(VK_SHIFT)&0x8000)!=0;
    Command(shift?(control?TTP_SKIN_EXTEND_TOGGLE_ROW:TTP_SKIN_EXTEND_ROW):(control?TTP_SKIN_TOGGLE_ROW:TTP_SKIN_SELECT_ROW),row);
    RECT r{};GetClientRect(v.window,&r);const int rows=std::max(1,(int(r.bottom)-60)/row_height_);
    if(row<v.scroll) v.scroll=row;else if(row>=v.scroll+rows) v.scroll=row-rows+1;
    InvalidateRect(v.window,nullptr,FALSE);
}
void Skin::Activate(View& v,int hit,POINT p) {
    if(hit>=hitVideoFullscreen && hit<=hitVideoMenu) VideoAction(v,hit);
    else if(hit==hitShade) ToggleShade(v);
    else if(hit==hitScale) ToggleScale();
    else if(hit==hitAuto) Feedback(L"Auto EQ: unavailable");
    else if(hit==hitEqUp || hit==hitEqFlat || hit==hitEqDown) Command(TTP_SKIN_EQ_BANDS,hit==hitEqUp?12:hit==hitEqDown?-12:0);
    else if(hit==hitShuffle) Command(TTP_SKIN_MODE,State().mode==4?2:4);
    else if(hit==hitRepeat) Command(TTP_SKIN_MODE,State().mode==1||State().mode==3?2:3);
    else if(hit>=hitListAdd && hit<=hitListList) {static constexpr int native[]={0,1,5,3,2};Command(TTP_SKIN_LIST_TOOLBAR,native[hit-hitListAdd]);}
    else if(hit==hitScrollUp || hit==hitScrollDown) v.scroll=std::max(0,v.scroll+(hit==hitScrollUp?-1:1));
    else if(hit>0 && hit<hitShade && !Sliding(hit)) Command(uint32_t(hit));
    else if(Sliding(hit)) Track(v,hit,p);
}
HCURSOR Skin::Cursor(const View& v,POINT p) const {
    const int hit=Hit(v,p);const char* name=nullptr;
    if(v.kind==0) {
        name=v.shaded?"wsnormal":"normal";
        if(hit==hitVolume || hit==hitBalance) name="volbal";
        else if(hit==hitSeek) name=v.shaded?"wsposbar":"posbar";
        else if(hit==hitDrag) name=v.shaded?"wsnormal":"titlebar";
        else if(hit==TTP_SKIN_MINIMIZE) name="min";
        else if(hit==TTP_SKIN_CLOSE) name="close";
        else if(hit==TTP_SKIN_MENU) name=v.shaded?"mmenu":"mainmenu";
        else if(hit==TTP_SKIN_PROPERTIES) name="songname";
        else if(hit==hitShade) name="winbut";
    } else if(v.kind==1) {
        name=v.shaded?"pwsnorm":"pnormal";
        if(hit==hitResize) name=v.shaded?"pwssize":"psize";
        else if(hit==hitScroll) name="pvscroll";
        else if(hit==TTP_SKIN_PLAYLIST) name="pclose";
        else if(hit==hitShade) name="pwinbut";
        else if(hit==hitDrag) name="ptbar";
    } else if(v.kind==3) {
        name=hit==TTP_SKIN_LYRICS?"vclose":hit==hitDrag?"vtbar":hit==hitResize?"vsize":
            hit>=hitVideoFullscreen && hit<=hitVideoMenu?"vmbuts":"vnormal";
    } else {
        name="eqnormal";
        if(Sliding(hit)) name="eqslid";
        else if(hit==TTP_SKIN_EQUALIZER) name="eqclose";
        else if(hit==hitDrag || hit==hitShade) name="eqtitle";
    }
    if(const auto found=cursors_.find(name);found!=cursors_.end()) return found->second.value;
    return LoadCursorW(nullptr,hit==hitResize?(v.shaded?IDC_SIZEWE:IDC_SIZENWSE):hit==hitAuto?IDC_NO:IDC_ARROW);
}
LRESULT CALLBACK Skin::Subclass(HWND window,UINT message,WPARAM wp,LPARAM lp,UINT_PTR,DWORD_PTR data) {
    auto& v=*reinterpret_cast<View*>(data);
    if(message==WM_NCDESTROY) {
        v.skin->HideTip(v);
        if(IsWindow(v.tooltip)) DestroyWindow(v.tooltip);
        v.tooltip=nullptr;
        v.skin->EndDrag(v);RemoveWindowSubclass(window,Subclass,subclassId);
        v.window=nullptr;return DefSubclassProc(window,message,wp,lp);
    }
    try { return v.skin->Message(v,message,wp,lp); }
    catch(...) { return DefSubclassProc(window,message,wp,lp); }
}
bool Skin::HostDrag(View& v,uint32_t phase,POINT point) const {
    if(!host_.drag) return false;
    const TtpSkinDrag event{sizeof(event),phase,v.window,point,
        static_cast<uint32_t>(v.resizing ? TTP_SKIN_DRAG_RIGHT|(v.shaded?0:TTP_SKIN_DRAG_BOTTOM) : TTP_SKIN_DRAG_WINDOW),
        {275,v.shaded?14:116}};
    return host_.drag(host_.context,&event)!=FALSE;
}
void Skin::EndDrag(View& v) {
    // Clear first: the host releases capture synchronously, reentering our
    // WM_CAPTURECHANGED handler before this call returns.
    const bool delegated=std::exchange(v.host_drag,false);
    v.dragging=v.resizing=false;
    if(delegated) HostDrag(v,TTP_SKIN_DRAG_END);
}
LRESULT Skin::Message(View& v,UINT message,WPARAM wp,LPARAM lp) {
    if(v.kind==1)SyncPlaylistFont();
    if(v.kind==3 && host_.content_input &&
       (message==WM_SIZE || (!v.pressed && !v.dragging && !v.resizing))) {
        TtpSkinContent content{sizeof(content),v.window};
        if(ContentState(content,false)) {
            const MSG event{v.window,message,wp,lp};
            LRESULT result{};
            if(host_.content_input(host_.context,&content,&event,&result)) {
                HideTip(v);return result;
            }
        }
    }
    const POINT raw{GET_X_LPARAM(lp),GET_Y_LPARAM(lp)};
    const int scale=(v.kind==1 || v.kind==3)?1:scale_;
    const POINT point{raw.x/scale,raw.y/scale};
    switch(message) {
    case WM_NOTIFYFORMAT: return NFR_UNICODE;
    case WM_WINDOWPOSCHANGED: CaptureLayout();break;
    case WM_NOTIFY: {
        const auto header=reinterpret_cast<NMHDR*>(lp);
        if(header && header->hwndFrom==v.tooltip && header->code==TTN_GETDISPINFOW) {
            v.tip_text=TipText(v,v.tip_hit,v.tip_bounds);
            reinterpret_cast<NMTTDISPINFOW*>(lp)->lpszText=v.tip_text.data();return 0;
        }
        break;
    }
    case WM_PAINT: { PAINTSTRUCT p{};HDC dc=BeginPaint(v.window,&p);Paint(v.window,dc);EndPaint(v.window,&p);return 0; }
    case WM_PRINTCLIENT: Paint(v.window,reinterpret_cast<HDC>(wp));return 0;
    case WM_ERASEBKGND:
        if(v.kind==3 && wp) {
            const auto dc=reinterpret_cast<HDC>(wp);
            // TBSTYLE_TRANSPARENT forwards erasing with a translated child
            // DC (or themed memory DC). Returning success without drawing
            // leaves hover/pressed pixels behind. Parent screen DCs must
            // still preserve the live toolbar as well as the editor.
            Paint(v.window,dc,WindowFromDC(dc)!=v.window);
        }
        return 1;
    case WM_NCHITTEST: return HTCLIENT;
    case WM_SIZE:
        HideTip(v);if(wp!=SIZE_MINIMIZED) {HideChildren(v);Region(v);InvalidateRect(v.window,nullptr,FALSE);}
        // Main WM_SIZE also drives tray state, desktop lyric restoration and
        // taskbar previews. Only auxiliary native layouts are provider-owned.
        if(v.kind==0)break;
        return 0;
    case WM_TIMER:
        if(wp==timerId) { if(v.kind==0) {++ticks_;UpdateStatistics();} RefreshRowTip(v);HideChildren(v);InvalidateRect(v.window,nullptr,FALSE);return 0; } break;
    case WM_ACTIVATE: if(LOWORD(wp)==WA_INACTIVE) HideTip(v);InvalidateRect(v.window,nullptr,FALSE);break;
    case WM_ENABLE: if(!wp) HideTip(v);break;
    case WM_SETCURSOR: {POINT cursor{};GetCursorPos(&cursor);ScreenToClient(v.window,&cursor);cursor.x/=scale;cursor.y/=scale;SetCursor(Cursor(v,cursor));return TRUE;}
    case WM_GETMINMAXINFO: {auto* info=reinterpret_cast<MINMAXINFO*>(lp);info->ptMinTrackSize={275*scale,(v.shaded?14:116)*scale};return 0;}
    case WM_CONTEXTMENU:
        HideTip(v);
        if(v.kind==3) {
            Command(TTP_SKIN_CONTENT_MENU,lp==LPARAM(-1)?1:0);return 0;
        }
        if(lp==LPARAM(-1)) {
            if(v.kind==1) {
                RECT r{};GetClientRect(v.window,&r);const RECT list{12,22,r.right-20,r.bottom-38};
                PlaylistContext(host_,v.window,v.selected,PlaylistMenuPoint(list,row_height_,v.selected,v.scroll),0,true);
            }
            else Command(v.kind==2?TTP_SKIN_EQ_PRESETS:TTP_SKIN_MENU);
        }
        return 0;
    case WM_RBUTTONDOWN: HideTip(v);if(v.kind==1)SetFocus(v.window);return 0;
    case WM_RBUTTONUP: {
        HideTip(v);
        const int hit=Hit(v,point);
        if(v.kind==3) Command(TTP_SKIN_CONTENT_MENU);
        else if(v.kind==1) PlaylistContext(host_,v.window,hit>=hitRow && uint32_t(hit-hitRow)<State().track_count?hit-hitRow:-1,raw,wp);
        else if(v.kind==2) Command(TTP_SKIN_EQ_PRESETS);
        else Command(hit==TTP_SKIN_VISUAL_NEXT?TTP_SKIN_VISUAL_MENU:TTP_SKIN_MENU);
        return 0;
    }
    case WM_LBUTTONDBLCLK: {
        HideTip(v);
        const int hit=Hit(v,point);
        if(v.kind==1 && hit>=hitRow && uint32_t(hit-hitRow)<State().track_count) Command(TTP_SKIN_PLAY_ROW,hit-hitRow);
        else if(hit==hitDrag && point.y<14) ToggleShade(v);
        return 0;
    }
    case WM_LBUTTONDOWN: {
        HideTip(v);
        SetFocus(v.window);v.pressed=Hit(v,point);v.hot=true;
        if(v.pressed>=hitRow) {
            const int row=v.pressed-hitRow;
            if(uint32_t(row)>=State().track_count) {v.pressed=0;return 0;}
            v.row_drag=false;v.drop=-1;v.drag_start=raw;
            v.selection_pending=host_.selection && (host_.selection(host_.context,row)&1) && !(wp&MK_SHIFT);
            if(!v.selection_pending) SelectRow(v,row);
            SetCapture(v.window);InvalidateRect(v.window,nullptr,FALSE);return 0;
        }
        if(v.pressed==hitDrag || v.pressed==hitResize) {
            v.dragging=v.pressed==hitDrag;v.resizing=v.pressed==hitResize;
            v.host_drag=HostDrag(v,TTP_SKIN_DRAG_BEGIN,raw);
            // The native begin callback already captures. Capturing twice can
            // synchronously cancel this gesture through WM_CAPTURECHANGED.
            if(!v.host_drag) SetCapture(v.window);
            v.drag_start=raw;ClientToScreen(v.window,&v.drag_start);GetWindowRect(v.window,&v.drag_rect);
        } else {
            SetCapture(v.window);
            if(Sliding(v.pressed)) BeginTrack(v,v.pressed,point);
        }
        InvalidateRect(v.window,nullptr,FALSE);return 0;
    }
    case WM_MOUSEMOVE:
        UpdateTip(v,point);
        if(GetCapture()==v.window) {
            if(v.pressed>=hitRow) {
                if(!PlaylistDragEnabled(host_))return 0;
                if(std::abs(raw.x-v.drag_start.x)>=GetSystemMetrics(SM_CXDRAG) || std::abs(raw.y-v.drag_start.y)>=GetSystemMetrics(SM_CYDRAG)) v.row_drag=true;
                if(v.row_drag) {
                    RECT r{};GetClientRect(v.window,&r);v.drop=-1;
                    if(!PtInRect(&r,raw)) {
                        // DoDragDrop pumps messages and may unload this skin.
                        // Finish this gesture first, then let the queued host
                        // command enter OLE after the DLL has left the stack.
                        v.pressed=0;v.row_drag=false;v.selection_pending=false;
                        HideTip(v);ReleaseCapture();InvalidateRect(v.window,nullptr,FALSE);
                        Command(TTP_SKIN_DRAG_SELECTION);return 0;
                    }
                    v.drop=PlaylistInsertion({12,22,r.right-20,r.bottom-38},row_height_,point,v.scroll,int(State().track_count),true,true);
                    InvalidateRect(v.window,nullptr,FALSE);
                }
            } else if(v.dragging || v.resizing) {
                if(v.host_drag) {HostDrag(v,TTP_SKIN_DRAG_MOVE,raw);return 0;}
                // Compatibility with older v1 hosts: move this window alone.
                // Full TTPlayer docking requires the optional host callback.
                POINT cursor=raw;ClientToScreen(v.window,&cursor);const int dx=cursor.x-v.drag_start.x,dy=cursor.y-v.drag_start.y;
                if(v.resizing) SetWindowPos(v.window,nullptr,0,0,std::clamp(int(v.drag_rect.right-v.drag_rect.left)+dx,275,2000),(v.shaded?14:std::clamp(int(v.drag_rect.bottom-v.drag_rect.top)+dy,116,1600)),SWP_NOMOVE|SWP_NOZORDER|SWP_NOACTIVATE);
                else SetWindowPos(v.window,nullptr,v.drag_rect.left+dx,v.drag_rect.top+dy,0,0,SWP_NOSIZE|SWP_NOZORDER|SWP_NOACTIVATE);
            } else if(Sliding(v.pressed)) Track(v,v.pressed,point);
            else {v.hot=Hit(v,point)==v.pressed;InvalidateRect(v.window,nullptr,FALSE);}
        }
        return 0;
    case WM_LBUTTONUP: {
        const int hit=v.pressed;
        if(hit>=hitRow) {
            RECT r{};GetClientRect(v.window,&r);
            if(v.row_drag && PlaylistDragEnabled(host_)) {
                // Validate the release too: the final point need not have a preceding mouse move.
                v.drop=PlaylistInsertion({12,22,r.right-20,r.bottom-38},row_height_,point,v.scroll,int(State().track_count),false,true);
                if(v.drop>=0)Command((wp&MK_CONTROL)?TTP_SKIN_COPY_SELECTION:TTP_SKIN_MOVE_SELECTION,v.drop);
            }
            else if(!v.row_drag && v.selection_pending) SelectRow(v,hit-hitRow);
            v.pressed=0;v.row_drag=false;v.selection_pending=false;v.drop=-1;
            if(GetCapture()==v.window) ReleaseCapture();InvalidateRect(v.window,nullptr,FALSE);return 0;
        }
        if(GetCapture()==v.window && Sliding(hit)) {Track(v,hit,point,true);if(hit==hitSeek && v.seek>=0) Command(TTP_SKIN_SEEK,v.seek);}
        const bool resize=v.resizing && !v.host_drag;
        v.pressed=0;v.seek=-1;
        if(hit==hitVolume)EndVolume(host_);
        const bool moving=v.dragging||v.resizing;EndDrag(v);
        if(GetCapture()==v.window) ReleaseCapture();
        if(resize && v.kind==1) {
            RECT r{};GetClientRect(v.window,&r);
            SetWindowPos(v.window,nullptr,0,0,275+std::max(0,int(r.right)-275)/25*25,
                v.shaded?14:116+std::max(0,int(r.bottom)-116)/29*29,SWP_NOMOVE|SWP_NOZORDER|SWP_NOACTIVATE);
        }
        if(!moving && !Sliding(hit) && hit==Hit(v,point)) Activate(v,hit,point);
        InvalidateRect(v.window,nullptr,FALSE);return 0;
    }
    case WM_MOUSELEAVE: HideTip(v);return 0;
    case WM_SHOWWINDOW: if(!wp) HideTip(v);break;
    case WM_CAPTURECHANGED: case WM_CANCELMODE:
        HideTip(v);
        if(v.pressed==hitVolume)EndVolume(host_);
        v.pressed=0;v.seek=-1;v.row_drag=false;v.selection_pending=false;v.drop=-1;EndDrag(v);
        if(message==WM_CANCELMODE && GetCapture()==v.window) ReleaseCapture();
        InvalidateRect(v.window,nullptr,FALSE);return 0;
    case WM_MOUSEWHEEL:
        HideTip(v);
        if(v.kind==1) {
            RECT r{};GetClientRect(v.window,&r);
            PlaylistWheel(wp,v.wheel,v.scroll,int(State().track_count),std::max(1,(int(r.bottom)-60)/row_height_));
            InvalidateRect(v.window,nullptr,FALSE);
        }
        else VolumeWheel(host_,GET_WHEEL_DELTA_WPARAM(wp),State().volume);
        return 0;
    case WM_KEYDOWN:
        HideTip(v);
        if(wp==VK_ESCAPE && GetCapture()==v.window) {
            const int hit=v.pressed;
            if(hit==hitVolume) Command(TTP_SKIN_VOLUME,v.initial);
            else if(hit==hitBalance) Command(TTP_SKIN_BALANCE,v.initial);
            else if(hit>=TTP_SKIN_EQ_VALUE && hit<TTP_SKIN_EQ_VALUE+11) Command(uint32_t(hit),v.initial);
            SendMessageW(v.window,WM_CANCELMODE,0,0);return 0;
        }
        if(v.kind==1) {
            if(wp==VK_RETURN && v.selected>=0) {Command(TTP_SKIN_PLAY_ROW,v.selected);return 0;}
            if(wp==VK_DELETE) {if(host_.selection) Command(TTP_SKIN_DELETE_SELECTED);else if(v.selected>=0) Command(TTP_SKIN_REMOVE_ROW,v.selected);return 0;}
            if(wp=='A' && (GetKeyState(VK_CONTROL)&0x8000)) {Command(TTP_SKIN_SELECT_ALL);return 0;}
            RECT r{};GetClientRect(v.window,&r);int row=std::max(0,v.selected),page=std::max(1,(int(r.bottom)-60)/row_height_);
            if(wp==VK_UP) --row;else if(wp==VK_DOWN) ++row;else if(wp==VK_PRIOR) row-=page;else if(wp==VK_NEXT) row+=page;
            else if(wp==VK_HOME) row=0;else if(wp==VK_END) row=int(State().track_count)-1;else break;
            SelectRow(v,std::clamp(row,0,std::max(0,int(State().track_count)-1)));return 0;
        }
        break;
    }
    return DefSubclassProc(v.window,message,wp,lp);
}
}
