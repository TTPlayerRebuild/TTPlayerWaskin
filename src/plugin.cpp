#include "skin.h"
#include "builtin_skin.h"
#include "modern.h"
#include "maki_client.h"
#include <new>
#include <memory>
#include <string>
#include <algorithm>
#include <cstring>

namespace {
bool IsModern(const wchar_t* path) {
    const auto length=wcslen(path);
    return length>=4 && _wcsicmp(path+length-4,L".wal")==0;
}
std::unique_ptr<waskin::Skin> Open(const wchar_t* path,const TtpSkinHost* host) {
    if(IsModern(path)) return std::make_unique<waskin::Modern>(path,host);
    return std::make_unique<waskin::Skin>(path,host);
}
template<size_t N> void CopyField(wchar_t (&target)[N],const std::wstring& value) {
    const auto count=std::min(value.size(),N-1);
    // Do not cut a non-BMP character in half at an ABI field boundary.
    const auto length=count && value[count-1]>=0xd800 && value[count-1]<=0xdbff?count-1:count;
    std::copy_n(value.data(),length,target);target[length]=0;
}
HRESULT WINAPI Probe(const wchar_t* path,TtpSkinInfo* info) {
    if(!path || !info || info->size<TTP_SKIN_INFO_V1_SIZE) return E_INVALIDARG;
    const auto capacity=info->size;
    TtpSkinInfo result{};result.size=capacity;
    const auto publish=[&] {
        std::memcpy(info,&result,TTP_SKIN_INFO_V1_SIZE);
        if(capacity>=offsetof(TtpSkinInfo,website)) std::memcpy(info->email,result.email,sizeof(result.email));
        if(capacity>=sizeof(TtpSkinInfo)) std::memcpy(info->website,result.website,sizeof(result.website));
    };
    publish();
    try {
        // Listing a WAL must not execute MAKI or require every optional
        // component. Creation/check perform the full compatibility validation.
        auto validated=IsModern(path)?std::unique_ptr<waskin::Skin>{}:Open(path,nullptr);
        if(waskin::IsBuiltinPackage(path)) {
            wcscpy_s(result.name,L"<默认皮肤>");
        } else {
            const auto metadata=validated?validated->Info():waskin::Modern::Inspect(path);
            CopyField(result.name,metadata.name);CopyField(result.author,metadata.author);
            CopyField(result.email,metadata.email);CopyField(result.website,metadata.website);
        }
        publish();
        return S_OK;
    } catch(...) {return HRESULT_FROM_WIN32(ERROR_BAD_FORMAT);}
}
HRESULT WINAPI Create(const wchar_t* path,const TtpSkinHost* host,void** output) {
    if(!path || !output || (host && (host->size<TTP_SKIN_HOST_V1_SIZE || host->version!=TTP_SKIN_ABI))) return E_INVALIDARG;
    *output=nullptr;
    try {*output=Open(path,host).release();return S_OK;}
    catch(const std::bad_alloc&) {return E_OUTOFMEMORY;} catch(...) {return HRESULT_FROM_WIN32(ERROR_BAD_FORMAT);}
}
HRESULT WINAPI Attach(void* instance,const TtpSkinWindows* windows) {
    if(!instance || !windows || windows->size<TTP_SKIN_WINDOWS_V1_SIZE) return E_INVALIDARG;
    try {return static_cast<waskin::Skin*>(instance)->Attach(*windows);} catch(...) {static_cast<waskin::Skin*>(instance)->Detach();return E_FAIL;}
}
void WINAPI Detach(void* instance) {if(instance) static_cast<waskin::Skin*>(instance)->Detach();}
void WINAPI Destroy(void* instance) {delete static_cast<waskin::Skin*>(instance);}
HBITMAP WINAPI Preview(void* instance) {try {return instance?static_cast<waskin::Skin*>(instance)->Preview():nullptr;} catch(...) {return nullptr;}}
void WINAPI Shade(void* instance) {try {if(instance) static_cast<waskin::Skin*>(instance)->Shade();} catch(...) {}}
void WINAPI Paint(void* instance,HWND window,HDC dc) {try {if(instance && dc) static_cast<waskin::Skin*>(instance)->Paint(window,dc);} catch(...) {}}
BOOL WINAPI Translate(void* instance,const MSG* message) {try {return instance && message && static_cast<waskin::Skin*>(instance)->Translate(*message);} catch(...) {return FALSE;}}
BOOL WINAPI Handles(void* instance,HWND window) {return instance && static_cast<waskin::Skin*>(instance)->Handles(window);}
HMENU WINAPI Menu(void* instance,HWND window,uint32_t command) {try {return instance?static_cast<waskin::Skin*>(instance)->Menu(window,command):nullptr;} catch(...) {return nullptr;}}
BOOL WINAPI ContentState(void* instance,TtpSkinContent* state,BOOL apply) {try {return instance && state && static_cast<waskin::Skin*>(instance)->ContentState(*state,apply!=FALSE);} catch(...) {return FALSE;}}
BOOL WINAPI LyricColors(void* instance,HWND window,TtpSkinLyricColors* colors) {return instance && colors && static_cast<waskin::Skin*>(instance)->LyricColors(window,*colors);}
int32_t WINAPI LyricFontHeight(void* instance) {return instance?-11:0;}
BOOL WINAPI LyricFont(void* instance,LOGFONTW* font) {
    return instance && font && static_cast<waskin::Skin*>(instance)->LyricFont(*font);
}
BOOL WINAPI VolumeTracking(void* instance) {return instance && static_cast<waskin::Skin*>(instance)->VolumeTracking();}
BOOL WINAPI PlaylistReveal(void* instance,uint32_t row,int32_t caret) {
    try {return instance && static_cast<waskin::Skin*>(instance)->PlaylistReveal(row,caret);} catch(...) {return FALSE;}
}
BOOL WINAPI ContentMinimum(void* instance,HWND window,SIZE* size) {
    return instance && size && static_cast<waskin::Skin*>(instance)->ContentMinimum(window,*size);
}
HRESULT WINAPI Check(const wchar_t* path,wchar_t* message,uint32_t count) {
    if(message && count)message[0]=0;
    if(!path || (!message && count))return E_INVALIDARG;
    try {auto validated=Open(path,nullptr);
        if(IsModern(path)) {
            const auto notes=static_cast<waskin::Modern*>(validated.get())->Diagnostic();
            if(!notes.empty()){if(message && count)wcsncpy_s(message,count,notes.c_str(),_TRUNCATE);return S_FALSE;}
        }
        return S_OK;
    }
    catch(const std::exception& error) {
        if(message && count) {
            const char* text=error.what();const int size=MultiByteToWideChar(CP_UTF8,0,text,-1,nullptr,0);
            if(size>0) {std::wstring wide(size,L'\0');MultiByteToWideChar(CP_UTF8,0,text,-1,wide.data(),size);wcsncpy_s(message,count,wide.c_str(),_TRUNCATE);}
        }
        return HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);
    }catch(...) {return E_FAIL;}
}
BOOL WINAPI PlaylistDrop(void* instance,TtpSkinPlaylistDrop* drop) {
    try {return instance && drop && static_cast<waskin::Skin*>(instance)->PlaylistDrop(*drop);} catch(...) {return FALSE;}
}
BOOL WINAPI DefaultPlaylistFont(void* instance,LOGFONTW* font) {
    return instance && font && static_cast<waskin::Skin*>(instance)->DefaultPlaylistFont(*font);
}
BOOL WINAPI PlaylistViewport(void* instance,HWND window,uint32_t* first,uint32_t* count) {
    try {return instance && first && count && static_cast<waskin::Skin*>(instance)->PlaylistViewport(window,*first,*count);} catch(...) {return FALSE;}
}
HRESULT WINAPI Layout(void* instance,TtpSkinLayout* state,BOOL restore) {
    if(!instance || !state || state->size<TTP_SKIN_LAYOUT_V1_SIZE) return E_INVALIDARG;
    try {return static_cast<waskin::Skin*>(instance)->Layout(*state,restore!=FALSE);} catch(...) {return E_FAIL;}
}
}
extern "C" HRESULT WINAPI ttpGetSkinPlugin(uint32_t version,TtpSkinPlugin* output) {
    if(version!=TTP_SKIN_ABI || !output || output->size<TTP_SKIN_PLUGIN_V1_SIZE) return E_INVALIDARG;
    const auto size=static_cast<uint32_t>(std::min<size_t>(output->size,sizeof(*output)));
    const TtpSkinPlugin api{size,TTP_SKIN_ABI,L"Winamp",Probe,Create,Attach,Detach,Destroy,Preview,Shade,Paint,Translate,
        L"waskin",waskin::MakiLibrary::Available()?L".wsz;.wal":L".wsz",Layout,Handles,Menu,ContentState,LyricColors,LyricFontHeight,PlaylistDrop,ContentMinimum,waskin::kBuiltinPackage,
        L"https://skins.webamp.org/",Check,LyricFont,VolumeTracking,PlaylistReveal,DefaultPlaylistFont,PlaylistViewport};
    std::memcpy(output,&api,size);
    return S_OK;
}
