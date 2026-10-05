#include "modern.h"
#include "host_interaction.h"
#include "playlist_interaction.h"
#include "modern_objects.h"
#include "modern_document.h"
#include "modern_capabilities.h"
#include "maki_client.h"
#include "builtin_skin.h"
#include "modern_vis.h"
#include <windowsx.h>
#include <sstream>
#include <iomanip>
namespace waskin {
using namespace modern;
namespace {
constexpr UINT_PTR modernId=0x57414c31,modernTimer=0x57414c32;
struct Services {
 HRESULT com=CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);ULONG_PTR token{};
 Services(){Gdiplus::GdiplusStartupInput input;Require(SUCCEEDED(com) || com==RPC_E_CHANGED_MODE,"COM initialization");Require(Gdiplus::GdiplusStartup(&token,&input,nullptr)==Gdiplus::Ok,"GDI+ startup");}
 ~Services(){if(token)Gdiplus::GdiplusShutdown(token);if(SUCCEEDED(com))CoUninitialize();}
};
struct Png {Com<IStream> stream;std::unique_ptr<Gdiplus::Bitmap> image;};
TtpMakiValue Encode(const Value& v){
 TtpMakiValue out{};
 if(auto number=std::get_if<double>(&v)){out.type=TTP_MAKI_NUMBER;out.number=*number;}
 else if(auto text=std::get_if<std::wstring>(&v)){out.type=TTP_MAKI_STRING;out.text=text->c_str();}
 else if(auto object=std::get_if<Node*>(&v)){out.type=TTP_MAKI_OBJECT;out.object=*object;}
 return out;
}
Value Decode(const TtpMakiValue& v){
 switch(v.type){case TTP_MAKI_NUMBER:return v.number;case TTP_MAKI_STRING:return std::wstring(v.text?v.text:L"");case TTP_MAKI_OBJECT:return static_cast<Node*>(v.object);default:return {};}
}
}
struct Modern::Impl {
 Services services;MakiLibrary vm;Archive archive;Document document;TtpSkinHost host{};Metadata metadata;
 struct Script {
  Impl* app{};Node* group{};Node system;void* handle{};Value result;std::string diagnostic;std::wstring parameter,file;bool disabled{};std::vector<std::unique_ptr<Node>> owned;
  void Disable(const std::string& why) {disabled=true;app->document.notes.Add(Narrow(file)+": script disabled: "+why);for(auto& n:owned){n->active=false;n->animating=false;}}
  ~Script(){if(handle)app->vm.api.destroy(handle);}
  static HRESULT WINAPI Construct(void* context,const GUID* type,void** out) {
   *out=nullptr;auto& s=*static_cast<Script*>(context);try {
    const int index=ClassIndex(*type);Require(index>=0 && (std::wstring(Classes()[index].kind)==L"timer" || std::wstring(Classes()[index].kind)==L"map"),"unsupported script-owned object class");
    Require(s.app->nodes.size()<2048,"script object limit");auto node=std::make_unique<Node>();node->kind=Classes()[index].kind;node->source=s.group->source;node->attrs[L"delay"]=L"1000";
    auto* result=node.get();s.owned.push_back(std::move(node));s.app->nodes.push_back(result);*out=result;return S_OK;
   }catch(const std::exception& e){s.diagnostic=e.what();return E_FAIL;}
  }
  static void WINAPI ReleaseObject(void* context,void* object) {
   auto& s=*static_cast<Script*>(context);auto* n=static_cast<Node*>(object);n->active=false;
   auto& nodes=s.app->nodes;nodes.erase(std::remove(nodes.begin(),nodes.end(),n),nodes.end());
   // Keep a tombstone until the script is destroyed so queued event snapshots
   // cannot become dangling pointers if a callback deletes another timer.
  }
  static int WINAPI Resolve(void* context,const GUID* type,const wchar_t* name) {
   auto& s=*static_cast<Script*>(context);try{return s.app->Resolve(*type,name);}
   catch(const std::exception& e){wchar_t guid[40]{};StringFromGUID2(*type,guid,40);s.diagnostic=Narrow(std::wstring(guid)+L"."+name)+": "+e.what();return -1;}
  }
  static HRESULT WINAPI Invoke(void* context,const GUID* type,void* receiver,const wchar_t* name,const TtpMakiValue* args,uint32_t count,TtpMakiValue* out){
   auto& s=*static_cast<Script*>(context);try{
    auto* n=static_cast<Node*>(receiver);
    Require(n==&s.system || std::find(s.app->nodes.begin(),s.app->nodes.end(),n)!=s.app->nodes.end(),"invalid XML object");
    Require(!n->unsupported,"script depends on an omitted control");
    if(!SupportsClass(n,*type) && n->attrs.contains(L"embed_xui"))n=n->Find(n->attrs[L"embed_xui"]);
    Require(SupportsClass(n,*type),"MAKI receiver does not implement declared class");
    std::vector<Value> values;for(uint32_t i=0;i<count;++i)values.push_back(Decode(args[i]));
    s.result=s.app->Call(s,n,name,values);*out=Encode(s.result);return S_OK;
   }catch(const std::exception& e){s.diagnostic=Narrow(name)+": "+e.what();return E_FAIL;}
  }
  Value EventOn(Node* n,const std::wstring& name,const std::vector<Value>& args){
   if(disabled || !handle)return {};
   std::vector<TtpMakiValue> values;for(const auto& v:args)values.push_back(Encode(v));TtpMakiValue out{};BOOL done{};
   if(FAILED(app->vm.api.event(handle,n,name.c_str(),values.data(),uint32_t(values.size()),&out,&done)))throw std::runtime_error(Narrow(group->source+L": "+name)+": "+(diagnostic.empty()?"MAKI event failed":diagnostic));
   app->completed|=done!=FALSE;return Decode(out);
  }
 };
 std::map<std::wstring,Xml*>& bitmaps=document.bitmaps;
 std::unique_ptr<Node> root;Node* layout{};std::vector<Node*> nodes,layouts;struct ScriptSource {std::wstring name;Node* group;std::wstring parameter;};std::vector<ScriptSource> scripts;
 std::vector<std::unique_ptr<Script>> programs;std::vector<Script*> live;
 std::map<std::wstring,Png> images;std::map<std::wstring,int> privateInts;std::map<std::wstring,COLORREF> colors;
 size_t depth{},instantiateDepth{},imageBytes{};int observedVolume{-1};int playback{},scroll{},selected{-1},drop{-1};
 HWND window{},tooltip{};std::wstring tipText;Node *hover{},*pressed{};POINT down{};
 bool ready{},completed{},fault{},moving{},hostMoving{},rowDragging{},selectionPending{},volumeGesture{};RECT saved{},restored{},playlistRect{},dragOrigin{};POINT dragAnchor{};HRGN savedRegion{};
 int revealRow{-1},wheel{};bool playlistFocus{};
 int sliderQueued{},sliderInitial{},sliderGrab{},sliderCoordinate{};bool profileSliding{};
 PlaylistFontCache playlistFont;std::unique_ptr<Gdiplus::Font> rowFont;int rowHeight{14};LOGFONTW defaultPlaylistFont{};
 std::vector<std::pair<HWND,bool>> children;std::unique_ptr<Gdiplus::Bitmap> frame;
 bool restoreLeft{},restoreRight{},restoreVis{};
 RECT contentRect{},notifiedContent{};
 uint32_t contentMode{TTP_SKIN_CONTENT_LYRICS},contentVisual{1};
 std::unordered_map<Node*,ModernVis> visRenderers;
 explicit Impl(const wchar_t* path,const TtpSkinHost* h):archive(path,true),document(archive,true) {
  if(h){std::memcpy(&host,h,TTP_SKIN_HOST_V1_SIZE);
#define COPY_HOST(field) if(h->size>=offsetof(TtpSkinHost,field)+sizeof(h->field))host.field=h->field
   COPY_HOST(drag);COPY_HOST(selection);COPY_HOST(visual);COPY_HOST(tip);COPY_HOST(resize);COPY_HOST(spectrum);COPY_HOST(content);COPY_HOST(content_input);COPY_HOST(option);COPY_HOST(playlist_context);COPY_HOST(playlist_font);
#undef COPY_HOST
  }
  metadata=ReadMetadata(archive.Read("skin.xml"));
  Xml* container{};
  for(auto* e:document.elements){
   if(e->kind==L"container" && Lower(e->attrs[L"id"])==L"main"){Require(!container,"duplicate main container");container=e;}
   if(e->kind==L"color"){int r{},g{},b{};if(swscanf_s(e->attrs[L"value"].c_str(),L"%d,%d,%d",&r,&g,&b)==3)colors[Lower(e->attrs[L"id"])]=RGB(std::clamp(r,0,255),std::clamp(g,0,255),std::clamp(b,0,255));}
  }
  Require(container,"missing main container");root=Instantiate(container);
  for(auto& n:root->children)if(n->kind==L"layout")layouts.push_back(n.get());
  Require(!layouts.empty() && layouts.size()<=16,"main layout count");layout=layouts.front();
  for(auto* item:layouts) {
   Gdiplus::Bitmap* bg=item->attrs[L"background"].empty()?nullptr:Bitmap(item->attrs[L"background"]);
   for(const auto* key:{L"w",L"h"}) {
    const auto def=std::wstring(L"default_")+key,minimum=std::wstring(L"minimum_")+key,maximum=std::wstring(L"maximum_")+key;
    int value=item->Get(key,item->Get(def.c_str(),bg?int(key[0]==L'w'?bg->GetWidth():bg->GetHeight()):item->Get(minimum.c_str())));
    value=std::clamp(value,item->Get(minimum.c_str(),0),std::max(item->Get(minimum.c_str(),0),item->Get(maximum.c_str(),2048)));
    Require(value>0 && value<=2048,"main layout bounds (w/default_w/minimum_w and h/default_h/minimum_h required)");item->attrs[key]=std::to_wstring(value);
   }
  }
  Validate();
  if(scripts.size()>128){document.notes.Add("script candidate limit: excess scripts omitted");scripts.resize(128);}
  for(const auto& [name,group,parameter]:scripts){
   if(programs.size()>=32){document.notes.Add("script instance limit: excess scripts omitted");break;}auto p=std::make_unique<Script>();p->app=this;p->group=group;p->parameter=parameter;p->file=name;p->system.kind=L"system";
   const auto data=archive.Read(Narrow(Lower(name)));TtpMakiHost callbacks{sizeof(callbacks),p.get(),Script::Resolve,Script::Invoke,Script::Construct,Script::ReleaseObject};
   wchar_t error[1024]{};
   const auto result=vm.api.create_checked?vm.api.create_checked(data.data(),uint32_t(data.size()),&callbacks,&p->system,&p->handle,error,1024):
     vm.api.create(data.data(),uint32_t(data.size()),&callbacks,&p->system,&p->handle);
   if(result==E_OUTOFMEMORY)throw std::bad_alloc();
   if(FAILED(result)){document.notes.Add(Narrow(name)+": script skipped: "+(p->diagnostic.empty()?error[0]?Narrow(error):"unsupported MAKI bytecode/class":p->diagnostic));continue;}
   live.push_back(p.get());programs.push_back(std::move(p));
  }
  frame=std::make_unique<Gdiplus::Bitmap>(layout->Get(L"w"),layout->Get(L"h"),PixelFormat32bppARGB);
  Render();Load();Render(); // Geometry is available to onScriptLoaded; commands stay suppressed.
  Require(HasVisiblePixels(),"WAL main layout has no visible content");
 }
 ~Impl(){ready=false;try{SystemEvent(L"onScriptUnloading");}catch(...){}programs.clear();if(savedRegion)DeleteObject(savedRegion);}
 int Resolve(const GUID& type,const std::wstring& name) {return MethodArity(type,name);}
 void Validate(){
  static constexpr const wchar_t* kinds[]={L"container",L"layout",L"group",L"layer",L"button",L"togglebutton",L"text",L"slider",L"vis",L"component",L"guiobject",L"animatedlayer"};
  for(auto* n:nodes){
   const auto note=[&](const std::string& text){document.notes.Add(Narrow(n->source+L": <"+n->kind+L" id='"+n->Id()+L"'> ")+text);};
   for(const auto* key:{L"relatx",L"relaty",L"relatw",L"relath"})Require(n->Get(key)>=0 && n->Get(key)<=2,"relative layout mode must be 0, 1 or 2");
   for(const auto* key:{L"x",L"y",L"w",L"h"})Require(std::abs(int64_t(n->Get(key)))<=8192,"XML coordinate bounds");
   if(std::find(std::begin(kinds),std::end(kinds),n->kind)==std::end(kinds)){note("unsupported control omitted");n->unsupported=true;continue;}
   if(n->Get(L"sysregion")<0 || n->Get(L"sysregion")>1){note("region operation omitted");n->attrs[L"sysregion"]=L"0";}
   for(const auto* key:{L"anchor",L"regionop",L"region",L"resize",L"scale",L"cfg_group"})
    if(n->attrs.contains(key) && !n->attrs[key].empty() && n->attrs[key]!=L"0"){note("attribute omitted: "+Narrow(key));n->attrs.erase(key);}
   for(const auto* key:{L"image",L"downimage",L"hoverimage",L"activeimage",L"inactiveimage",L"background",L"thumb",L"downthumb",L"hoverthumb"})
    if(n->attrs.contains(key) && !n->attrs[key].empty()) {
     if(n->kind==L"animatedlayer" && std::wstring(key)==L"image" && n->Get(L"elementframes")) {
      Require(n->Get(L"elementframes")>0 && n->Get(L"elementframes")<=512,"animation element limit");
      for(int i=0;i<n->Get(L"elementframes");++i){const auto id=n->attrs[key]+std::to_wstring(i);Bitmap(id);if(missingImages.contains(document.Alias(id)))n->unsupported=true;}
     }else {
      Bitmap(n->attrs[key]);
      if(missingImages.contains(document.Alias(n->attrs[key]))){
       const std::wstring attribute=key;
       if(attribute==L"image" || attribute==L"thumb")n->unsupported=true;
       else if(attribute!=L"background")n->attrs[key].clear(); // use the normal-state image
      }
     }
    }
   if(n->kind==L"animatedlayer" && !n->unsupported) {
    const auto a=Animation(n);n->attrs[L"default_w"]=std::to_wstring(a.source.right-a.source.left);n->attrs[L"default_h"]=std::to_wstring(a.source.bottom-a.source.top);
    if(n->Get(L"start",-1)<0)n->attrs[L"start"]=L"0";
    if(n->Get(L"end",-1)<0)n->attrs[L"end"]=std::to_wstring(a.count-1);
   }
   if(n->kind==L"component" && n->attrs[L"param"]!=L"guid:pl" && n->attrs[L"param"]!=L"guid:avs"){note("unsupported embedded component omitted");n->unsupported=true;}
   const auto a=Lower(n->attrs[L"action"]);
   if(n->kind==L"slider" && !(a.empty() || a==L"volume" || a==L"pan" || a==L"eq_band" || a==L"eq_preamp" || a==L"seek")){note("slider action disabled: "+Narrow(a));n->disabledAction=true;}
   if(IsButton(n) && !(a.empty() || a==L"play" || a==L"pause" || a==L"stop" || a==L"prev" || a==L"next" || a==L"close" || a==L"minimize" || a==L"sysmenu" || a==L"eject" || a==L"eq_toggle" || a==L"toggle" || a==L"menu" || a==L"switch")){note("button action disabled: "+Narrow(a));n->disabledAction=true;}
   if(IsButton(n) && a==L"toggle" && !ToggleCommand(n->attrs[L"param"])) {note("window action disabled: "+Narrow(n->attrs[L"param"]));n->disabledAction=true;}
   if(IsButton(n) && a==L"switch" && std::none_of(layouts.begin(),layouts.end(),[&](Node* l){return Lower(l->Id())==Lower(n->attrs[L"param"]);})){note("missing layout switch disabled");n->disabledAction=true;}
  }
 }
 static uint32_t ToggleCommand(std::wstring param) {
  param=Lower(param);
  if(param==L"guid:pl" || param==L"guid:playlist" || param==L"pledit" || param==L"guid:{45f3f7c1-a6f3-4ee6-a15e-125e92fc3f8d}")return TTP_SKIN_PLAYLIST;
  if(param==L"guid:eq" || param==L"eq")return TTP_SKIN_EQUALIZER;
  if(param==L"guid:ml" || param==L"guid:library" || param==L"guid:musiclibrary")return TTP_SKIN_MEDIA_LIBRARY;
  if(param==L"guid:avs" || param==L"guid:{f0816d7b-fffc-4343-80f2-e8199aa15cc3}")return TTP_SKIN_LYRICS;
  return 0;
 }
    void Definition(Xml* def,std::map<std::wstring,std::wstring>& attrs,std::vector<Xml*>& content,std::set<Xml*>& stack) {
        Require(stack.size()<64 && stack.insert(def).second,"cyclic group inheritance");
        Xml* base{};
        if(auto it=def->attrs.find(L"inherit_group");it!=def->attrs.end()) {
            const auto found=document.definitions.find(Lower(it->second));if(found==document.definitions.end())document.notes.Add(Narrow(def->source+L": missing inherited group "+it->second));else base=found->second;
            if(base==def)base=document.ancestors.contains(def)?document.ancestors.at(def):nullptr;
        }else if(document.ancestors.contains(def))base=document.ancestors.at(def);
        if(base) {
            std::map<std::wstring,std::wstring> inherited;std::vector<Xml*> inheritedContent;
            Definition(base,inherited,inheritedContent,stack);
            const auto inherit=def->attrs.contains(L"inherit_content")?Lower(def->attrs.at(L"inherit_content")):
                def->attrs.contains(L"inherit_group")?L"1":L"0";
            if(!def->attrs.contains(L"inherit_params") || def->attrs.at(L"inherit_params")!=L"0")attrs=std::move(inherited);
            for(auto* c:inheritedContent)if(inherit==L"1" || (c->kind==L"script"?inherit.find(L"scripts"):inherit.find(L"xui"))!=std::wstring::npos)content.push_back(c);
        }
        for(const auto& [key,value]:def->attrs)attrs[key]=value;
        for(auto& child:def->children)content.push_back(child.get());stack.erase(def);
    }
    std::unique_ptr<Node> Instantiate(Xml* xml,Node* parent=nullptr,size_t definitionPosition=SIZE_MAX) {
        if(xml->kind==L"layout")definitionPosition=document.order.at(xml);
        Require(nodes.size()<2048 && instantiateDepth<64,"XML object limit");++instantiateDepth;
        auto node=std::make_unique<Node>();node->kind=xml->kind;node->source=xml->source;node->parent=parent;
        Xml* definition{};std::vector<Xml*> content;
        if(xml->kind==L"group" || xml->kind==L"cfggroup") {
            const auto id=xml->attrs.find(L"id");Require(id!=xml->attrs.end(),"group missing id");
            definition=document.DefinitionAt(id->second,definitionPosition);
            if(!definition)document.notes.Add(Narrow(xml->source+L": missing group "+id->second));
        }else if(auto found=document.xui.find(xml->kind);found!=document.xui.end()) {definition=found->second;node->kind=L"group";}
        if(definition){std::set<Xml*> stack;Definition(definition,node->attrs,content,stack);}
        else for(auto& child:xml->children)content.push_back(child.get());
        for(const auto& [key,value]:xml->attrs)node->attrs[key]=value;
        node->visible=node->Get(L"visible",1)!=0;nodes.push_back(node.get());
        if(node->kind==L"windowholder")node->kind=L"component";
        if(node->kind==L"slider"){const auto action=Lower(node->attrs[L"action"]);node->position=action==L"seek"?0:action==L"pan"?127:action.starts_with(L"eq_")?0:128;}
        std::function<void(Xml*)> add=[&](Xml* child) {
            if(child->kind==L"fragment" || child->kind==L"wal") {for(auto& c:child->children)add(c.get());}
            else if(child->kind==L"script") {try {scripts.push_back({ResourcePath(archive,child->source,child->attrs.at(L"file")),node.get(),child->attrs[L"param"]});}catch(const MissingResource& e){document.notes.Add(e.what());}}
            else if(child->kind!=L"groupdef" && child->kind!=L"elements")node->children.push_back(Instantiate(child,node.get(),definitionPosition));
        };
        for(auto* child:content)add(child);
        --instantiateDepth;return node;
    }
    Value Emit(Node* node,const std::wstring& name,const std::vector<Value>& args={}){
        Require(depth<32,"MAKI event recursion");++depth;Value out;
        try{for(auto* p:live)if(!p->disabled){const bool previouslyCompleted=completed;try{auto value=p->EventOn(node,name,args);if(std::holds_alternative<Node*>(value))out=value;}catch(const std::bad_alloc&){throw;}catch(const std::exception& e){completed=previouslyCompleted;p->Disable(e.what());}}}catch(...){--depth;throw;}--depth;return out;}
    Value SystemEvent(const std::wstring& name,const std::vector<Value>& args={}){Value out;for(auto* p:live){auto value=Emit(&p->system,name,args);if(std::holds_alternative<Node*>(value))out=value;}return out;}
    TtpSkinState State(){TtpSkinState s{};s.size=sizeof(s);Require(!host.query || host.query(host.context,&s)!=FALSE,"host state callback");return s;}
    void Command(uint32_t action,int value=0){if(ready && host.command)host.command(host.context,action,value);}
    void Flush(){const auto now=State();const int previousVolume=observedVolume;observedVolume=now.volume;
        if(previousVolume>=0 && previousVolume!=now.volume)SystemEvent(L"onVolumeChanged",{double(MulDiv(now.volume,255,100))});
        if(now.playback!=playback){
        const auto event=now.playback==2?(playback==3?L"onResume":L"onPlay"):now.playback==3?L"onPause":L"onStop";
        playback=now.playback;SystemEvent(event);}
        const auto controls=nodes;for(auto* n:controls)if(n->kind==L"slider"){auto action=Lower(n->attrs[L"action"]);
            if(n==pressed)continue;const int oldPosition=n->position;if(action==L"volume")n->position=MulDiv(now.volume,255,100);
            else if(action==L"pan")n->position=std::clamp(MulDiv(now.balance,127,100)+127,0,255);
            else if(action==L"eq_band" || action==L"eq_preamp")n->position=MulDiv(now.eq[EqIndex(n)],127,12);
            else if(action==L"seek")n->position=now.duration_ms>0?int(now.position_ms*65535/now.duration_ms):0;
            if(n->position!=oldPosition){const double value=n->position/(action==L"seek"?256:1);Emit(n,L"onSetPosition",{value});Emit(n,L"onPostedPosition",{value});}}}
    #include "modern_animation.inc"
    #include "modern_map.inc"
    Value Call(Script& p,Node* n,const std::wstring& raw,const std::vector<Value>& args){const auto method=Lower(raw);
        // MAKI may explicitly call an event, e.g. Layer.onMouseMove(x,y)
        // from onLeftButtonDown. Dispatch it just like the native script API.
        if(method.starts_with(L"on"))return Emit(n,raw,args);
        if(n->kind==L"map")return MapCall(n,method,args);
        if(method==L"setregionfrommap"){RegionFromMap(n,Object(args.at(0)),int(Number(args.at(1)))&255,Number(args.at(2))!=0);return 0.0;}
        if(n->kind==L"animatedlayer" && (method==L"play" || method==L"pause" || method==L"togglepause" || method==L"stop" || method==L"gotoframe" || method==L"setspeed" || method==L"setrealtime" || method.find(L"frame")!=std::wstring::npos || method.find(L"replay")!=std::wstring::npos || method==L"getlength" || method==L"getdirection" || method==L"isplaying" || method==L"ispaused" || method==L"isstopped"))return AnimationCall(n,method,args);
        if(n->kind==L"timer") {
            if(method==L"setdelay")n->attrs[L"delay"]=std::to_wstring(std::clamp(int(Number(args.at(0))),1,600000));
            else if(method==L"getdelay")return double(n->Get(L"delay",1000));
            else if(method==L"start"){n->active=true;n->start=GetTickCount();}
            else if(method==L"stop")n->active=false;
            else if(method==L"isrunning")return double(n->active);
            else throw std::runtime_error("unsupported Timer method");return 0.0;
        }
        if(n->kind==L"system") {
         const auto state=State();
         if(method==L"getplayitemstring") {
          const auto count=host.tip?host.tip(host.context,TTP_SKIN_CURRENT_SOURCE,0,nullptr,0):0;
          if(count<=0 || count>32768)return std::wstring{};std::wstring value(count,0);
          if(!host.tip(host.context,TTP_SKIN_CURRENT_SOURCE,0,value.data(),count))return std::wstring{};
          value.resize(wcsnlen_s(value.data(),value.size()));return value;
         }
         if(method==L"strleft")return String(args.at(0)).substr(0,size_t(std::max(0.0,Number(args.at(1)))));
         if(method==L"getparam")return p.parameter;
         if(method==L"getsonginfotext")return state.sample_rate?std::to_wstring(state.bitrate/1000)+L"kbps "+std::to_wstring(state.sample_rate/1000)+L"kHz "+(state.channels==1?L"Mono":L"Stereo"):L"";
         if(method==L"getposition")return double(state.position_ms);
         if(method==L"getplayitemlength")return double(state.duration_ms);
         if(method==L"seekto"){if(state.duration_ms>0)Command(TTP_SKIN_SEEK,int(std::clamp(Number(args.at(0))/double(state.duration_ms)*10000,0.0,10000.0)));return 0.0;}
         if(method==L"integertotime")return Time(int64_t(Number(args.at(0))));
         if(method==L"strsearch"){const auto at=String(args.at(0)).find(String(args.at(1)));return at==std::wstring::npos?-1.0:double(at);}
         if(method==L"gettoken") {
          const auto value=String(args.at(0)),sep=String(args.at(1));const int index=int(Number(args.at(2)));if(index<0)return std::wstring{};
          size_t start=0;for(int i=0;i<index;++i){const auto at=sep.empty()?std::wstring::npos:value.find(sep.front(),start);if(at==std::wstring::npos)return std::wstring{};start=at+1;}
          return value.substr(start,sep.empty()?std::wstring::npos:value.find(sep.front(),start)-start);
         }
        }
        if(method==L"getruntimeversion")return 5.666; // Compiler guard; capabilities are validated separately.
        if(method==L"integertostring")return std::to_wstring(int(Number(args.at(0))));
        if(method==L"getvolume")return double(MulDiv(State().volume,255,100));
        if(method==L"getskinname")return metadata.name;if(method==L"getscriptgroup")return p.group;
        if(method==L"getstatus"){auto s=State();return double(s.playback==2?1:s.playback==3?-1:0);}
        if(method==L"gettimeofday")return double(GetTickCount());if(method==L"getprivateint"){auto key=String(args.at(0))+L"/"+String(args.at(1));auto i=privateInts.find(key);return i==privateInts.end()?args.at(2):Value(double(i->second));}
        if(method==L"setprivateint"){privateInts[String(args.at(0))+L"/"+String(args.at(1))]=int(Number(args.at(2)));return 0.0;}
        // Imported by HeadAMP's compiler version guard, never taken by the
        // supported scripts. Do not silently acknowledge a required dialog.
        if(method==L"messagebox")throw std::runtime_error("script requested an unsupported modal dialog");
        if(method==L"findobject" || method==L"getobject" || method==L"getlayout"){auto* found=n->Find(String(args.at(0)));Require(found!=nullptr,"missing XML object");return found;}
        if(method==L"getcontainer"){Require(Lower(root->Id())==Lower(String(args.at(0))),"unknown container");return root.get();}
        if(method==L"getxmlparam")return n->attrs[Lower(String(args.at(0)))];
        if(method==L"stringtointeger")return double(_wtoi(String(args.at(0)).c_str()));
        if(method==L"getalpha")return double(n->Get(L"alpha",255));
        if(method==L"getleft")return double(n->bounds.left-(n->parent?n->parent->bounds.left:0));
        if(method==L"gettop")return double(n->bounds.top-(n->parent?n->parent->bounds.top:0));
        if(method==L"getwidth")return double(n->bounds.right-n->bounds.left);
        if(method==L"getheight")return double(n->bounds.bottom-n->bounds.top);
        if(method==L"getposition")return double(n->position/(Lower(n->attrs[L"action"])==L"seek"?256:1));
        if(method==L"gettext")return TextValue(n);
        if(method==L"getactivated" || method==L"getcurcfgval")return double(Active(n));
        if(method==L"show") {n->visible=true;n->attrs[L"visible"]=L"1";}else if(method==L"hide"){n->visible=false;n->attrs[L"visible"]=L"0";}
        else if(method==L"setalpha")n->attrs[L"alpha"]=std::to_wstring(std::clamp(int(Number(args.at(0))),0,255));
        else if(method==L"settext" || method==L"setalternatetext")n->attrs[method==L"settext"?L"text":L"alternatetext"]=String(args.at(0));
        else if(method==L"setactivated"){const bool value=Number(args.at(0))!=0;if(n->active!=value){n->active=value;Emit(n,L"onActivate",{double(value)});}}
        else if(method==L"lock")n->locked=true;
        else if(method==L"unlock")n->locked=false;
        else if(method==L"setxmlparam"){
            const auto key=Lower(String(args.at(0))),value=String(args.at(1));
            Require(value.size()<=4096,"XML parameter limit");
            if(key==L"x" || key==L"y")Require(std::abs(int64_t(_wtoi(value.c_str())))<=8192,"XML position");
            if(key==L"w" || key==L"h")Require(std::abs(int64_t(_wtoi(value.c_str())))<=2048,"XML size");
            n->attrs[key]=value;if(key==L"visible")n->visible=_wtoi(value.c_str())!=0;}
        else if(method==L"settargetspeed")n->speed=int(std::clamp(Number(args.at(0)),0.0,60.0)*4)/4.0;
        else if(method==L"settargetx")n->target=int(std::clamp(Number(args.at(0)),-8192.0,8192.0));
        else if(method==L"gototarget"){n->origin=n->Get(L"x");n->animationStarted=false;n->animating=true;}
        else if(method==L"leftclick")Click(n);
        else if(method==L"setvolume"){if(pressed)volumeGesture=true;Command(TTP_SKIN_VOLUME,std::clamp(MulDiv(int(Number(args.at(0))),100,255),0,100));}
        else if(method==L"seteqband")Command(TTP_SKIN_EQ_VALUE+1+std::clamp(int(Number(args.at(0))),0,9),std::clamp(MulDiv(int(Number(args.at(1))),12,127),-12,12));
        else if(method==L"setposition")Slider(n,int(std::clamp(Number(args.at(0)),-65535.0,65535.0))*(Lower(n->attrs[L"action"])==L"seek"?256:1),true);
        else throw std::runtime_error("unsupported host implementation");return 0.0;}
    void Load(){
     const auto layers=nodes;for(auto* n:layers)if(!n->unsupported && n->kind==L"animatedlayer" && n->Get(L"autoplay"))AnimationCall(n,L"play",{});
     // A script can hide buttons before discovering an unsupported dependency.
     // Roll its initialization back so the static/native controls stay usable.
     struct Saved {Node* node;decltype(Node{}.Save()) state;std::unique_ptr<Gdiplus::Region> clip;};
     for(auto* p:live)if(!p->disabled){
      const auto integers=privateInts;
      std::vector<Saved> snapshot;for(auto* n:nodes)snapshot.push_back({n,n->Save(),std::unique_ptr<Gdiplus::Region>(n->clipRegion?n->clipRegion->Clone():nullptr)});
      try{p->EventOn(&p->system,L"onScriptLoaded",{});}catch(const std::bad_alloc&){throw;}catch(const std::exception& e){p->Disable(e.what());}
       if(p->disabled){privateInts=integers;for(auto& old:snapshot){old.node->Restore(old.state);old.node->clipRegion=std::move(old.clip);}for(auto& owned:p->owned){owned->active=false;owned->animating=false;}}
     }
     Flush();
    }
    void SwitchLayout(const std::wstring& id) {
        const auto found=std::find_if(layouts.begin(),layouts.end(),[&](Node* n){return Lower(n->Id())==Lower(id);});
        Require(found!=layouts.end(),"unknown layout");if(layout==*found)return;
        layout=*found;pressed=hover=nullptr;
        frame=std::make_unique<Gdiplus::Bitmap>(layout->Get(L"w"),layout->Get(L"h"),PixelFormat32bppARGB);
        if(window) {
            if(GetCapture()==window)ReleaseCapture();
            const SIZE size{layout->Get(L"w"),layout->Get(L"h")};
            if(!host.resize || !host.resize(host.context,window,size))SetWindowPos(window,nullptr,0,0,size.cx,size.cy,SWP_NOMOVE|SWP_NOACTIVATE|SWP_NOZORDER);
            Render();Region();InvalidateRect(window,nullptr,FALSE);
        }
    }
    static bool IsButton(const Node* n){return n && (n->kind==L"button" || n->kind==L"togglebutton");}
    static int EqIndex(Node* n){return Lower(n->attrs[L"action"])==L"eq_preamp" || Lower(n->attrs[L"param"])==L"preamp"?0:std::clamp(n->Get(L"param"),1,10);}
    static bool Vertical(Node* n){const auto v=Lower(n->attrs[L"orientation"]);return v==L"v" || v==L"vertical";}
    std::wstring Config(Node* n){const auto cfg=Lower(n->attrs[L"cfgattrib"]);const auto at=cfg.find(L';');return at==std::wstring::npos?L"":cfg.substr(at+1);}
    bool Active(Node* n) {
        const auto cfg=Config(n);const auto state=State();
        if(cfg==L"shuffle")return state.mode==4;
        if(cfg==L"repeat")return state.mode==1 || state.mode==3;
        if(cfg==L"enable crossfading")return host.option && host.option(host.context,TTP_SKIN_CROSSFADE)>0;
        if(Lower(n->attrs[L"action"])==L"eq_toggle")return state.eq_enabled!=0;
        return n->active;
    }
    void Click(Node* n){Require(n!=nullptr,"click target");if(n->unsupported || n->disabledAction || !n->Get(L"enabled",1))return;completed=false;Emit(n,L"onLeftClick");if(completed)return;const auto action=Lower(n->attrs[L"action"]);
        if(action==L"switch"){SwitchLayout(n->attrs[L"param"]);return;}
        if(n->kind==L"togglebutton") {
            const bool next=!Active(n);const auto cfg=Config(n);
            if(cfg==L"shuffle")Command(TTP_SKIN_MODE,next?4:2);
            else if(cfg==L"repeat")Command(TTP_SKIN_MODE,next?3:2);
            else if(cfg==L"enable crossfading")Command(TTP_SKIN_CROSSFADE,next?1:0);
            n->active=next;Emit(n,L"onToggle",{double(next)});
        }
        if(action==L"toggle") {if(const auto command=ToggleCommand(n->attrs[L"param"]))Command(command);return;}
        if(action==L"eq_toggle"){Command(TTP_SKIN_EQ_ENABLE,!State().eq_enabled);return;}
        if(action==L"menu"){Command(Lower(n->attrs[L"param"])==L"presets"?TTP_SKIN_EQ_PRESETS:TTP_SKIN_MENU);return;}
        static constexpr std::pair<const wchar_t*,uint32_t> actions[]={{L"play",TTP_SKIN_PLAY},{L"pause",TTP_SKIN_PAUSE},{L"stop",TTP_SKIN_STOP},
            {L"prev",TTP_SKIN_PREVIOUS},{L"next",TTP_SKIN_NEXT},{L"close",TTP_SKIN_CLOSE},{L"minimize",TTP_SKIN_MINIMIZE},{L"sysmenu",TTP_SKIN_MENU},{L"eject",TTP_SKIN_OPEN}};
        if(!action.empty()){const auto i=std::find_if(std::begin(actions),std::end(actions),[&](const auto& a){return action==a.first;});Require(i!=std::end(actions),"unsupported XML action");Command(i->second);}}
    void Click(const wchar_t* id){Click(root->Find(id));Flush();}
    void Slider(Node* n,int value,bool final,bool physical=false){if(n->unsupported || n->disabledAction)return;auto action=Lower(n->attrs[L"action"]);
        const auto notify=[&](uint32_t command,int mapped) {
            // Do not use host State() to deduplicate posted commands: a burst
            // of mouse moves may arrive before the host dispatches any of them.
            if(!physical || final || mapped!=sliderQueued)Command(command,mapped);
            if(physical)sliderQueued=mapped;
        };
        if(action==L"volume"){n->position=std::clamp(value,0,255);Command(TTP_SKIN_VOLUME,MulDiv(n->position,100,255));}
        else if(action==L"pan"){n->position=std::clamp(value,0,255);notify(TTP_SKIN_BALANCE,std::clamp(MulDiv(n->position-127,100,127),-100,100));}
        else if(action==L"eq_band" || action==L"eq_preamp"){if(!State().eq_enabled)return;n->position=std::clamp(value,-127,127);notify(TTP_SKIN_EQ_VALUE+EqIndex(n),MulDiv(n->position,12,127));}
        else if(action==L"seek"){n->position=std::clamp(value,0,65535);if(final)Command(TTP_SKIN_SEEK,MulDiv(n->position,10000,65535));}
        else if(action.empty())n->position=std::clamp(value,n->Get(L"low",0),std::max(n->Get(L"low",0),n->Get(L"high",255)));
        else throw std::runtime_error("unsupported slider action");
        const double position=n->position/(action==L"seek"?256:1);Emit(n,L"onSetPosition",{position});Emit(n,L"onPostedPosition",{position});if(final)Emit(n,L"onSetFinalPosition",{position});}
    void Advance(DWORD now,bool finish=false){std::vector<Node*> done;for(auto* n:nodes)if(n->animating){
        // GuiObjectI starts timing at the first timer callback. WM_TIMER can
        // coalesce under load, so count elapsed milliseconds, not callbacks.
        if(!n->animationStarted){n->start=now;n->animationStarted=true;}
        const DWORD duration=DWORD(n->speed*1000),elapsed=now-n->start;
        const int progress=finish || !duration || elapsed>=duration?255:
            std::min(255,int((uint64_t(elapsed)*256+duration/2)/duration));
        const float smooth=float((1-std::cos(double(progress)/255*3.14159265358979323846))/2);
        n->attrs[L"x"]=std::to_wstring(int(float(n->origin)+float(n->target-n->origin)*smooth));
        if(progress==255){n->animating=false;done.push_back(n);}}
        for(auto* n:done)Emit(n,L"onTargetReached");
        const auto timers=nodes;for(auto* n:timers)if(n->kind==L"timer" && n->active && ready && now-n->start>=DWORD(n->Get(L"delay",1000))) {
            n->start=now;Emit(n,L"onTimer");
        }
        AdvanceFrames(now);Flush();}
    std::set<std::wstring> missingImages;
    Gdiplus::Bitmap* Bitmap(const std::wstring& id) {
     try{return ReadBitmap(id);}catch(const MissingResource& e){
      const auto key=document.Alias(id);document.notes.Add(e.what());missingImages.insert(key);
      Png empty;empty.image=std::make_unique<Gdiplus::Bitmap>(1,1,PixelFormat32bppARGB);empty.image->SetPixel(0,0,Gdiplus::Color(0,0,0,0));
      auto* result=empty.image.get();images.emplace(key,std::move(empty));return result;
     }
    }
    bool HasVisiblePixels() {
     Gdiplus::BitmapData pixels{};Gdiplus::Rect bounds(0,0,frame->GetWidth(),frame->GetHeight());
     Require(frame->LockBits(&bounds,Gdiplus::ImageLockModeRead,PixelFormat32bppARGB,&pixels)==Gdiplus::Ok,"frame pixels");
     bool visible=false;for(int y=0;y<bounds.Height && !visible;++y){auto* row=reinterpret_cast<const uint32_t*>(static_cast<const uint8_t*>(pixels.Scan0)+y*pixels.Stride);for(int x=0;x<bounds.Width;++x)if(row[x]>>24){visible=true;break;}}
     frame->UnlockBits(&pixels);return visible;
    }
    Gdiplus::Bitmap* ReadBitmap(const std::wstring& id){const auto key=document.Alias(id);if(images.contains(key))return images.at(key).image.get();
        Xml* resource{};
        if(bitmaps.contains(key))resource=bitmaps.at(key);
        else if(document.fonts.contains(key) && document.fonts.at(key)->kind==L"bitmapfont")resource=document.fonts.at(key);
        if(!resource)throw MissingResource(Narrow(L"missing bitmap: "+id));
        const auto& a=resource->attrs;const auto& data=archive.Read(Narrow(ResourcePath(archive,resource->source,a.at(L"file"))));HGLOBAL memory=GlobalAlloc(GMEM_MOVEABLE,data.size());Require(memory!=nullptr,"PNG stream memory");
        auto* bytes=GlobalLock(memory);if(!bytes){GlobalFree(memory);throw std::bad_alloc();}std::memcpy(bytes,data.data(),data.size());GlobalUnlock(memory);IStream* stream{};
        const HRESULT hr=CreateStreamOnHGlobal(memory,TRUE,&stream);if(FAILED(hr))GlobalFree(memory);Require(SUCCEEDED(hr),"PNG stream");
        Png entry;entry.stream.reset(stream);Gdiplus::Bitmap source(stream);Require(source.GetLastStatus()==Gdiplus::Ok && source.GetWidth()<=32768 && source.GetHeight()<=32768 && uint64_t(source.GetWidth())*source.GetHeight()<=32*1024*1024,"PNG decode/bounds");
        const auto attr=[&](const wchar_t* name,int fallback){auto i=a.find(name);return i==a.end()?fallback:_wtoi(i->second.c_str());};
        const int x=attr(L"x",0),y=attr(L"y",0),w=attr(L"w",source.GetWidth()),h=attr(L"h",source.GetHeight());
        Require(x>=0 && y>=0 && w>0 && h>0 && w<=32768 && h<=32768 && uint64_t(w)*h<=32*1024*1024,"PNG sprite bounds");
        // HeadAMP's XML contains overruns in some sprite rectangles.
        // Clip the image to a transparent destination instead of rejecting the
        // whole skin or reading beyond the decoded PNG.
        imageBytes+=size_t(w)*h*4;Require(imageBytes<=128*1024*1024,"PNG memory limit");entry.image=std::make_unique<Gdiplus::Bitmap>(w,h,PixelFormat32bppARGB);
        {Gdiplus::Graphics crop(entry.image.get());crop.Clear(Gdiplus::Color(0,0,0,0));
            crop.SetCompositingMode(Gdiplus::CompositingModeSourceCopy);
            crop.DrawImage(&source,Gdiplus::Rect(-x,-y,source.GetWidth(),source.GetHeight()),0,0,source.GetWidth(),source.GetHeight(),Gdiplus::UnitPixel);}
        if(x+w/2<int(source.GetWidth()) && y+h/2<int(source.GetHeight())){Gdiplus::Color original,cropped;source.GetPixel(x+w/2,y+h/2,&original);entry.image->GetPixel(w/2,h/2,&cropped);
            if(original.GetA()==255)Require(original.GetValue()==cropped.GetValue(),"sprite crop must preserve source pixels independent of PNG DPI");}
        Require(entry.image->GetLastStatus()==Gdiplus::Ok,"PNG crop");auto* result=entry.image.get();images.emplace(key,std::move(entry));return result;}

 struct Hit {Node* node;RECT bounds;Gdiplus::Bitmap* mask;RECT source{};};std::vector<Hit> hits;std::vector<RECT> regionRects;
 COLORREF Color(const wchar_t* id,COLORREF fallback)const{auto i=colors.find(id);return i==colors.end()?fallback:i->second;}
  static Gdiplus::Color GColor(COLORREF c){return Gdiplus::Color(255,GetRValue(c),GetGValue(c),GetBValue(c));}
  static Gdiplus::Font PlaylistFont(){return Gdiplus::Font(L"Tahoma",11,Gdiplus::FontStyleRegular,Gdiplus::UnitPixel);}
 static LOGFONTW DefaultPlaylistFont() {
  HDC dc=CreateCompatibleDC(nullptr);LOGFONTW font{};
  {Gdiplus::Graphics graphics(dc);auto original=PlaylistFont();original.GetLogFontW(&graphics,&font);}
  DeleteDC(dc);return font;
 }
 bool SyncPlaylistFont() {
  if(!defaultPlaylistFont.lfHeight)defaultPlaylistFont=DefaultPlaylistFont();
  if(!playlistFont.Update(host,defaultPlaylistFont,ready) && rowFont)return false;
  HDC dc=CreateCompatibleDC(nullptr);
  rowFont=std::make_unique<Gdiplus::Font>(dc,&playlistFont.Descriptor());DeleteDC(dc);
  rowHeight=playlistFont.Height();
  if(tooltip)SendMessageW(tooltip,TTM_POP,0,0);
  return true;
 }
 void Playlist(Gdiplus::Graphics& g,RECT r){
  SyncPlaylistFont();playlistRect=r;auto state=State();const int rows=std::max(1,int(r.bottom-r.top)/rowHeight);
  if(revealRow>=0){if(revealRow<scroll)scroll=revealRow;else if(revealRow>=scroll+rows)scroll=revealRow-rows+1;revealRow=-1;}
  scroll=std::clamp(scroll,0,std::max(0,int(state.track_count)-rows));
  Gdiplus::SolidBrush bg(GColor(Color(L"wasabi.list.background",RGB(0,0,0))));
  g.FillRectangle(&bg,int(r.left),int(r.top),int(r.right-r.left),int(r.bottom-r.top));
  auto clip=g.Save();g.SetClip(Gdiplus::Rect(r.left,r.top,r.right-r.left,r.bottom-r.top));
   auto& textFont=*rowFont;
  Gdiplus::StringFormat format;format.SetFormatFlags(Gdiplus::StringFormatFlagsNoWrap);format.SetTrimming(Gdiplus::StringTrimmingEllipsisCharacter);
  for(int row=scroll;row<std::min(int(state.track_count),scroll+(int(r.bottom-r.top)+rowHeight-1)/rowHeight);++row){
   TtpSkinTrack track{};track.size=sizeof(track);if(!host.track || !host.track(host.context,uint32_t(row),&track))continue;
   const auto flags=host.selection?host.selection(host.context,uint32_t(row)):uint32_t(row==selected?3:0);
   if(flags&2)selected=row;
   const bool selectedRow=(flags&1)!=0;
   COLORREF c=Color(row==state.playing_row?L"wasabi.list.text.current":L"wasabi.list.text",RGB(255,255,255));
   if(selectedRow){Gdiplus::SolidBrush selection(GColor(Color(L"wasabi.list.text.selected.background",RGB(0,120,215))));
    g.FillRectangle(&selection,r.left,r.top+(row-scroll)*rowHeight,r.right-r.left,rowHeight);c=Color(L"wasabi.list.text.selected",RGB(255,255,255));}
   Gdiplus::SolidBrush fg(GColor(c));std::wstring label=std::to_wstring(row+1)+L". "+track.title;
   const float y=float(r.top+(row-scroll)*rowHeight),width=float(r.right-r.left-4);
   const auto duration=track.duration_ms>=0?Time(track.duration_ms):L"";
   float timeWidth=0;
   if(!duration.empty()) {
    Gdiplus::RectF measured;g.MeasureString(duration.c_str(),int(duration.size()),&textFont,Gdiplus::PointF(0,0),&measured);
    timeWidth=std::min(width,std::ceil(measured.Width));
    Gdiplus::StringFormat right;right.SetFormatFlags(Gdiplus::StringFormatFlagsNoWrap);
    right.SetAlignment(Gdiplus::StringAlignmentFar);
    g.DrawString(duration.c_str(),int(duration.size()),&textFont,Gdiplus::RectF(float(r.right-2)-timeWidth,y,timeWidth,float(rowHeight)),&right,&fg);
   }
   const float titleWidth=std::max(0.0f,width-timeWidth-(timeWidth?4.0f:0.0f));
   if(titleWidth>0)g.DrawString(label.c_str(),int(label.size()),&textFont,Gdiplus::RectF(float(r.left+2),y,titleWidth,float(rowHeight)),&format,&fg);
  }
  if(drop>=scroll && drop<=scroll+rows){Gdiplus::Pen pen(Gdiplus::Color(255,255,255,255));int y=r.top+(drop-scroll)*rowHeight;g.DrawLine(&pen,r.left,y,r.right-1,y);}
  g.Restore(clip);
 }
 #include "modern_content.inc"
 void Visual(Gdiplus::Graphics& g,RECT r,Node* n) {
  HDC screen=GetDC(nullptr),dc=CreateCompatibleDC(screen);HBITMAP bmp=CreateCompatibleBitmap(screen,r.right-r.left,r.bottom-r.top);ReleaseDC(nullptr,screen);
  if(!dc || !bmp){if(dc)DeleteDC(dc);if(bmp)DeleteObject(bmp);return;}
  auto old=SelectObject(dc,bmp);RECT local{0,0,r.right-r.left,r.bottom-r.top};FillRect(dc,&local,static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
  if(n->kind==L"vis") {
   TtpSkinSpectrumFrame f{};f.size=sizeof(f);const auto style=VisStyle(n);
   if(host.spectrum && host.spectrum(host.context,&f) && (f.type==2 || (f.type==3 && f.size>=sizeof(f)))) {
    auto pixels=visRenderers[n].Render(f,style,GetTickCount());
    BITMAPINFO info{};info.bmiHeader={sizeof(BITMAPINFOHEADER),72,-16,1,32,BI_RGB};SetStretchBltMode(dc,COLORONCOLOR);
    StretchDIBits(dc,0,0,local.right,local.bottom,0,0,72,16,pixels.data(),&info,DIB_RGB_COLORS,SRCCOPY);
   }else {
    visRenderers.erase(n);
    if(host.visual){TtpSkinVisualColors c{RGB(0,0,0),style.palette[2],style.palette[10],style.palette[17],style.palette[23],style.palette[18]};host.visual(host.context,dc,&local,&c);}
   }
  }else if(host.content)host.content(host.context,dc,&local,contentMode,contentVisual);
  SelectObject(dc,old);
  {Gdiplus::Bitmap canvas(bmp,nullptr);g.DrawImage(&canvas,Gdiplus::Rect(r.left,r.top,local.right,local.bottom),0,0,local.right,local.bottom,Gdiplus::UnitPixel);}
  DeleteObject(bmp);DeleteDC(dc);
 }
 #include "modern_render.inc"
 void Render(){
  hits.clear();playlistRect={};contentRect={};Gdiplus::Graphics g(frame.get());g.Clear(Gdiplus::Color(0,0,0,0));
  g.SetInterpolationMode(Gdiplus::InterpolationModeNearestNeighbor);g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);
  std::function<void(Node*,RECT,int,bool)> draw=[&](Node* n,RECT parent,int inheritedAlpha,bool parentEnabled){
   if(!n->visible)return;
   if(n->unsupported){n->bounds=Geometry(n,parent,nullptr);for(auto& child:n->children)draw(child.get(),n->bounds,inheritedAlpha,parentEnabled);return;}
   const int alpha=MulDiv(inheritedAlpha,std::clamp(n->Get(L"alpha",255),0,255),255);if(!alpha)return;
   const bool enabled=parentEnabled && n->Get(L"enabled",1)!=0 && !n->disabledAction;
   std::wstring id=n->attrs[n->kind==L"slider"?L"thumb":n->kind==L"layout" || n->kind==L"group"?L"background":L"image"];
   if(IsButton(n) && Active(n) && !n->attrs[L"activeimage"].empty())id=n->attrs[L"activeimage"];
   const wchar_t* state=n==pressed?(n->kind==L"slider"?L"downthumb":L"downimage"):n==hover?(n->kind==L"slider"?L"hoverthumb":L"hoverimage"):L"";
   if(n->attrs.contains(state) && !n->attrs[state].empty())id=n->attrs[state];
   if(!enabled && !n->attrs[L"inactiveimage"].empty())id=n->attrs[L"inactiveimage"];
   Gdiplus::Bitmap* bitmap{};RECT source{};
   if(n->kind==L"animatedlayer"){const auto a=Animation(n);bitmap=a.bitmap;source=a.source;}
   else {bitmap=id.empty()?nullptr:Bitmap(id);if(bitmap)source={0,0,LONG(bitmap->GetWidth()),LONG(bitmap->GetHeight())};}
   RECT r=Geometry(n,parent,n->kind==L"animatedlayer"?nullptr:bitmap);n->bounds=r;
   if(bitmap) {
    RECT imageRect=r;
    if(n->kind==L"slider") {
     const auto action=Lower(n->attrs[L"action"]);const int low=action.starts_with(L"eq_")?-127:action.empty()?n->Get(L"low"):0;
     const int high=action==L"seek"?65535:action.starts_with(L"eq_")?127:action.empty()?n->Get(L"high",255):255;
     const double fraction=high>low?std::clamp(double(n->position-low)/(high-low),0.0,1.0):0;
     if(Vertical(n))imageRect.top+=LONG((1-fraction)*std::max(0L,r.bottom-r.top-LONG(bitmap->GetHeight())));
     else imageRect.left+=LONG(fraction*std::max(0L,r.right-r.left-LONG(bitmap->GetWidth())));
     imageRect.right=imageRect.left+bitmap->GetWidth();imageRect.bottom=imageRect.top+bitmap->GetHeight();
    }
    const auto clip=g.Save();
    if(n->clipRegion){std::unique_ptr<Gdiplus::Region> region(n->clipRegion->Clone());Gdiplus::Matrix transform(float(imageRect.right-imageRect.left)/(source.right-source.left),0,0,float(imageRect.bottom-imageRect.top)/(source.bottom-source.top),float(imageRect.left),float(imageRect.top));region->Transform(&transform);g.SetClip(region.get(),Gdiplus::CombineModeIntersect);}
    Image(g,bitmap,imageRect,alpha,&source);g.Restore(clip);
    if(enabled && !n->Get(L"ghost") && !n->locked)hits.push_back({n,r,n->kind==L"slider" || n->Get(L"rectrgn",n->kind==L"layer" || n->kind==L"animatedlayer"?1:0)?nullptr:bitmap,source});
   }else if(n->kind==L"text") {
    Text(g,n,r,alpha);if(enabled && !n->Get(L"ghost"))hits.push_back({n,r,nullptr});
   }else if(n->kind==L"component" || n->kind==L"vis") {
    if(!IsRectEmpty(&r)) {
     Require(r.right-r.left<=2048 && r.bottom-r.top<=2048,"visual dimensions");
     if(n->attrs[L"param"]==L"guid:pl")Playlist(g,r);else {if(n->kind==L"component")contentRect=r;Visual(g,r,n);}
     if(enabled && !n->Get(L"ghost"))hits.push_back({n,r,nullptr});
    }
   }else if(n->kind==L"layer" && n->attrs.contains(L"move") && enabled && !n->Get(L"ghost") && !n->locked && !IsRectEmpty(&r)) {
    // Skin title/mouse-trap layers may intentionally have no bitmap.
    hits.push_back({n,r,nullptr});
   }
   for(auto& child:n->children)draw(child.get(),r,alpha,enabled);
  };draw(layout,{0,0,layout->Get(L"w"),layout->Get(L"h")},255,true);
 }
 Node* HitTest(POINT p)const{
  for(auto i=hits.rbegin();i!=hits.rend();++i)if(PtInRect(&i->bounds,p)){
   if(i->mask){Gdiplus::Color c;const int x=i->source.left+MulDiv(p.x-i->bounds.left,i->source.right-i->source.left,i->bounds.right-i->bounds.left),y=i->source.top+MulDiv(p.y-i->bounds.top,i->source.bottom-i->source.top,i->bounds.bottom-i->bounds.top);
    if(x<0 || y<0 || x>=int(i->mask->GetWidth()) || y>=int(i->mask->GetHeight()))continue;
    // setRegionFromMap supplies Layer::secrgn, a paint clip, not its hit region.
    i->mask->GetPixel(x,y,&c);if(c.GetA()==0)continue;}return i->node;}return nullptr;
 }
 void Region(){
  if(!window)return;Gdiplus::BitmapData data;Gdiplus::Rect r(0,0,frame->GetWidth(),frame->GetHeight());
  Require(frame->LockBits(&r,Gdiplus::ImageLockModeRead,PixelFormat32bppARGB,&data)==Gdiplus::Ok,"frame lock");
  std::vector<RECT> spans;
  for(int y=0;y<r.Height;++y){const auto* row=reinterpret_cast<const uint32_t*>(static_cast<const uint8_t*>(data.Scan0)+y*data.Stride);
   for(int x=0;x<r.Width;){while(x<r.Width && !(row[x]>>24))++x;const int start=x;while(x<r.Width && (row[x]>>24))++x;if(x>start)spans.push_back({start,y,x,y+1});}}
  frame->UnlockBits(&data);
  if(spans.size()==regionRects.size() && (spans.empty() || !std::memcmp(spans.data(),regionRects.data(),spans.size()*sizeof(RECT))))return;
  std::vector<uint8_t> storage(sizeof(RGNDATAHEADER)+spans.size()*sizeof(RECT));auto* region=reinterpret_cast<RGNDATA*>(storage.data());
  region->rdh={sizeof(RGNDATAHEADER),RDH_RECTANGLES,DWORD(spans.size()),DWORD(spans.size()*sizeof(RECT)),{0,0,r.Width,r.Height}};
  std::memcpy(region->Buffer,spans.data(),spans.size()*sizeof(RECT));HRGN handle=ExtCreateRegion(nullptr,DWORD(storage.size()),region);
  if(handle){if(SetWindowRgn(window,handle,FALSE))regionRects=std::move(spans);else DeleteObject(handle);}
 }
 void Draw(HDC dc,bool background=false){
  Render();const int savedDC=SaveDC(dc);
  if(window)for(HWND child=GetWindow(window,GW_CHILD);child;child=GetWindow(child,GW_HWNDNEXT)) {
   const auto role=reinterpret_cast<UINT_PTR>(GetPropW(child,TTP_SKIN_CONTENT_CHILD));
   if(role && IsWindowVisible(child) && !(background && role==TTP_SKIN_CONTENT_CHILD_TRANSPARENT)) {
    RECT r{};GetWindowRect(child,&r);MapWindowPoints(nullptr,window,reinterpret_cast<POINT*>(&r),2);ExcludeClipRect(dc,r.left,r.top,r.right,r.bottom);
   }
  }
  {Gdiplus::Graphics g(dc);g.DrawImage(frame.get(),Gdiplus::Rect(0,0,frame->GetWidth(),frame->GetHeight()),0,0,frame->GetWidth(),frame->GetHeight(),Gdiplus::UnitPixel);}
  if(savedDC)RestoreDC(dc,savedDC);
 }
 bool Drag(uint32_t phase,POINT p){if(!host.drag)return false;TtpSkinDrag d{sizeof(d),phase,window,p,TTP_SKIN_DRAG_WINDOW,{}};return host.drag(host.context,&d)!=FALSE;}
 void BeginMove(POINT p){
  dragAnchor=p;ClientToScreen(window,&dragAnchor);GetWindowRect(window,&dragOrigin);
  // The host captures synchronously. Pre-capturing here makes its SetCapture
  // reenter WM_CAPTURECHANGED and cancel the same gesture before it begins.
  hostMoving=Drag(TTP_SKIN_DRAG_BEGIN,p);
  if(!hostMoving && GetCapture()!=window)SetCapture(window);
  moving=GetCapture()==window;
 }
 void Move(POINT p){
  if(hostMoving){Drag(TTP_SKIN_DRAG_MOVE,p);return;}
  ClientToScreen(window,&p);
  SetWindowPos(window,nullptr,dragOrigin.left+p.x-dragAnchor.x,dragOrigin.top+p.y-dragAnchor.y,0,0,SWP_NOSIZE|SWP_NOZORDER|SWP_NOACTIVATE);
 }
 void EndMove(POINT p={}){
  const bool delegated=hostMoving;moving=hostMoving=false;
  // Clear before calling the host: ReleaseCapture sends WM_CAPTURECHANGED.
  if(delegated)Drag(TTP_SKIN_DRAG_END,p);
 }
 int Row(POINT p)const{const int row=scroll+int(p.y-playlistRect.top)/rowHeight;return PtInRect(&playlistRect,p) && row>=0 && row<int(StateConst().track_count)?row:-1;}
 bool PlaylistHit(POINT p,bool edge=false)const {
  if(edge) {
   if(!PlaylistDropContains(playlistRect,p))return false;
   p.x=std::clamp(p.x,playlistRect.left,playlistRect.right-1);
   p.y=std::clamp(p.y,playlistRect.top,playlistRect.bottom-1);
  } else if(!PtInRect(&playlistRect,p))return false;
  // A closed drawer may still paint its list underneath the player body.
  // Only the topmost component can accept input or the two-pixel edge strip.
  auto* n=HitTest(p);return n && n->attrs[L"param"]==L"guid:pl";
 }
 TtpSkinState StateConst()const{TtpSkinState s{};s.size=sizeof(s);if(host.query)host.query(host.context,&s);return s;}
 void Select(int row,WPARAM keys){if(row<0)return;selected=row;Command(keys&MK_SHIFT?(keys&MK_CONTROL?TTP_SKIN_EXTEND_TOGGLE_ROW:TTP_SKIN_EXTEND_ROW):(keys&MK_CONTROL?TTP_SKIN_TOGGLE_ROW:TTP_SKIN_SELECT_ROW),row);}
 void Tip(POINT p){
  auto* n=HitTest(p);std::wstring text;
  if(n && n->attrs[L"param"]==L"guid:pl" && host.tip){const int row=Row(p);const auto count=row<0?0:host.tip(host.context,TTP_SKIN_TRACK_TIP,row,nullptr,0);
   if(count>0 && count<32768){text.resize(count);if(host.tip(host.context,TTP_SKIN_TRACK_TIP,row,text.data(),count))text.resize(wcsnlen_s(text.data(),text.size()));else text.clear();}}
  else if(n && n->attrs.contains(L"tooltip"))text=n->attrs[L"tooltip"];
  if(text==tipText)return;tipText=std::move(text);
  if(!tooltip){tooltip=CreateWindowExW(WS_EX_TOPMOST,TOOLTIPS_CLASSW,nullptr,WS_POPUP|TTS_ALWAYSTIP|TTS_NOPREFIX,CW_USEDEFAULT,CW_USEDEFAULT,CW_USEDEFAULT,CW_USEDEFAULT,window,nullptr,GetModuleHandleW(nullptr),nullptr);
   if(tooltip){TOOLINFOW info{sizeof(info)};info.uFlags=TTF_IDISHWND|TTF_SUBCLASS;info.hwnd=window;info.uId=reinterpret_cast<UINT_PTR>(window);info.lpszText=tipText.data();SendMessageW(tooltip,TTM_ADDTOOLW,0,reinterpret_cast<LPARAM>(&info));SendMessageW(tooltip,TTM_SETMAXTIPWIDTH,0,600);}}
  if(tooltip){SendMessageW(tooltip,TTM_POP,0,0);TOOLINFOW info{sizeof(info)};info.hwnd=window;info.uId=reinterpret_cast<UINT_PTR>(window);info.lpszText=tipText.data();SendMessageW(tooltip,TTM_UPDATETIPTEXTW,0,reinterpret_cast<LPARAM>(&info));}
 }
 void Slide(Node* n,POINT p,bool final,bool begin=false){
  const auto hit=std::find_if(hits.begin(),hits.end(),[&](const auto& h){return h.node==n;});if(hit==hits.end())return;
  auto* thumb=Bitmap(n->attrs[L"thumb"]);auto r=hit->bounds;const bool vertical=Vertical(n);
  // Preserve the grab offset for EQ/pan thumbs. A press without movement must
  // not notify the preset state machine; release sends its final notification.
  const int thumbSize=int(vertical?thumb->GetHeight():thumb->GetWidth());
  const int extent=std::max(1,int(vertical?r.bottom-r.top:r.right-r.left)-thumbSize);
  const auto action=Lower(n->attrs[L"action"]);const bool eq=action==L"eq_band" || action==L"eq_preamp",profile=eq || action==L"pan";
  const int coordinate=vertical?p.y-r.top:p.x-r.left;
  if(begin && profile) {
   profileSliding=true;
   const auto state=State();sliderInitial=sliderQueued=eq?state.eq[EqIndex(n)]:state.balance;
   n->position=eq?MulDiv(sliderInitial,127,12):std::clamp(MulDiv(sliderInitial,127,100)+127,0,255);
   const double fraction=eq?double(n->position+127)/254:double(n->position)/255;
   const int offset=int((vertical?1-fraction:fraction)*extent);
   const bool onThumb=coordinate>=offset && coordinate<offset+thumbSize;
   sliderGrab=onThumb?coordinate-offset:thumbSize/2;sliderCoordinate=coordinate;
   if(onThumb)return;
   sliderCoordinate=INT_MIN;
  }
  if(profile && coordinate==sliderCoordinate) {Slider(n,n->position,final,true);return;}
  double fraction=double(coordinate-(profile?sliderGrab:thumbSize/2))/extent;if(vertical)fraction=1-fraction;
  fraction=std::clamp(fraction,0.0,1.0);
  Slider(n,action==L"seek"?int(fraction*65535):eq?int(std::lround(fraction*254))-127:action.empty()?n->Get(L"low")+int(fraction*(n->Get(L"high",255)-n->Get(L"low"))):int(fraction*255),final,true);
  if(profile)sliderCoordinate=coordinate;
 }
 LRESULT Message(UINT message,WPARAM wp,LPARAM lp){
  if(ready && SyncPlaylistFont()){Render();InvalidateRect(window,nullptr,FALSE);}
  POINT p{GET_X_LPARAM(lp),GET_Y_LPARAM(lp)};
  if(message==WM_LBUTTONDOWN || message==WM_RBUTTONDOWN)playlistFocus=PlaylistHit(p);
  if(message==WM_CONTEXTMENU && lp==LPARAM(-1) && playlistFocus && !IsRectEmpty(&playlistRect)) {
   PlaylistContext(host,window,selected,PlaylistMenuPoint(playlistRect,rowHeight,selected,scroll),0,true);return 0;
  }
  LRESULT contentResult{};if(ContentInput(message,wp,lp,contentResult))return contentResult;
  switch(message){
  case WM_ERASEBKGND:if(wp)Draw(reinterpret_cast<HDC>(wp),WindowFromDC(reinterpret_cast<HDC>(wp))!=window);return 1;
  case WM_PAINT:{PAINTSTRUCT ps{};auto dc=BeginPaint(window,&ps);try{Draw(dc);}catch(...){EndPaint(window,&ps);throw;}EndPaint(window,&ps);return 0;}
  case WM_PRINTCLIENT:Draw(reinterpret_cast<HDC>(wp));return 0;
  case WM_TIMER:if(wp==modernTimer){if(!fault){Advance(GetTickCount());Render();SyncContent();Region();InvalidateRect(window,nullptr,FALSE);}return 0;}break;
  case WM_LBUTTONDOWN:
   SetFocus(window);pressed=HitTest(p);down=p;rowDragging=false;selectionPending=false;profileSliding=false;
   volumeGesture=pressed && Lower(pressed->attrs[L"action"])==L"volume";
   if(pressed && MouseEvent(pressed,L"onLeftButtonDown",p)){if(GetCapture()!=window)SetCapture(window);InvalidateRect(window,nullptr,FALSE);return 0;}
   if((!pressed || (!IsButton(pressed) && pressed->kind!=L"slider" && pressed->attrs[L"param"]!=L"guid:pl" && pressed->Get(L"move",1))) && layout->Get(L"move",1)) {
    BeginMove(p);InvalidateRect(window,nullptr,FALSE);return 0;
   }
   if(pressed && GetCapture()!=window)SetCapture(window);
   if(pressed && pressed->attrs[L"param"]==L"guid:pl"){int row=Row(p);
    selectionPending=row>=0 && host.selection && (host.selection(host.context,row)&1) && !(wp&(MK_CONTROL|MK_SHIFT));
    if(!selectionPending)Select(row,wp);}
   else if(pressed && pressed->kind==L"slider")Slide(pressed,p,false,true);
   else if(IsButton(pressed)){}
   InvalidateRect(window,nullptr,FALSE);return 0;
  case WM_MOUSEMOVE:
   hover=HitTest(p);{const bool consumed=MouseEvent(pressed?pressed:hover,L"onMouseMove",p);
   if(moving){if(GetCapture()==window)Move(p);else EndMove(p);InvalidateRect(window,nullptr,FALSE);return 0;}
   if(consumed){InvalidateRect(window,nullptr,FALSE);return 0;}}
   if(pressed && pressed->kind==L"slider")Slide(pressed,p,false);
   else if(pressed && pressed->attrs[L"param"]==L"guid:pl" && !rowDragging && PlaylistDragEnabled(host) &&
       (std::abs(p.x-down.x)>=GetSystemMetrics(SM_CXDRAG) || std::abs(p.y-down.y)>=GetSystemMetrics(SM_CYDRAG))){
    rowDragging=true;pressed=nullptr;if(GetCapture()==window)ReleaseCapture();Command(TTP_SKIN_DRAG_SELECTION,1);
   }else if(!pressed)Tip(p);
   InvalidateRect(window,nullptr,FALSE);return 0;
  case WM_LBUTTONUP:{
   Node* n=pressed;const bool consumed=MouseEvent(n,L"onLeftButtonUp",p);const bool volume=std::exchange(volumeGesture,false);pressed=nullptr;EndMove(p);
   if(GetCapture()==window)ReleaseCapture();
   if(consumed){if(volume)EndVolume(host);InvalidateRect(window,nullptr,FALSE);return 0;}
   if(n && n->kind==L"slider")Slide(n,p,true);
   else if(n && n==HitTest(p) && IsButton(n))Click(n);
   else if(n && n->attrs[L"param"]==L"guid:pl" && !rowDragging && selectionPending)Select(Row(p),wp);
   if(volume)EndVolume(host);
   InvalidateRect(window,nullptr,FALSE);return 0;}
  case WM_LBUTTONDBLCLK:if(auto* n=HitTest(p);n && n->attrs[L"param"]==L"guid:pl"){int row=Row(p);if(row>=0)Command(TTP_SKIN_PLAY_ROW,row);}return 0;
  case WM_RBUTTONDOWN:if(playlistFocus){SetFocus(window);return 0;}break;
  case WM_RBUTTONUP:{auto* n=HitTest(p);if(n && n->attrs[L"param"]==L"guid:pl")PlaylistContext(host,window,Row(p),p,wp);
   else Command(n && n->kind==L"vis"?TTP_SKIN_VISUAL_MENU:TTP_SKIN_MENU);return 0;}
  case WM_MOUSEWHEEL:ScreenToClient(window,&p);if(PlaylistHit(p)){PlaylistWheel(wp,wheel,scroll,int(State().track_count),PlaylistPage(playlistRect,rowHeight));InvalidateRect(window,nullptr,FALSE);}
   else VolumeWheel(host,GET_WHEEL_DELTA_WPARAM(wp),State().volume);return 0;
  case WM_KEYDOWN:
   if(wp==VK_ESCAPE && profileSliding && pressed && pressed->kind==L"slider") {
    const auto action=Lower(pressed->attrs[L"action"]);
    if(action==L"pan")Command(TTP_SKIN_BALANCE,sliderInitial);
    else if(action==L"eq_band" || action==L"eq_preamp")Command(TTP_SKIN_EQ_VALUE+EqIndex(pressed),sliderInitial);
    else break;
    SendMessageW(window,WM_CANCELMODE,0,0);return 0;
   }
   if(wp==VK_DELETE && selected>=0){Command(TTP_SKIN_DELETE_SELECTED);return 0;}
   if(wp==VK_RETURN && selected>=0){Command(TTP_SKIN_PLAY_ROW,selected);return 0;}
   if(wp=='A' && (GetKeyState(VK_CONTROL)&0x8000)){Command(TTP_SKIN_SELECT_ALL);return 0;}
   break;
  case WM_CAPTURECHANGED:case WM_CANCELMODE:
   if(std::exchange(volumeGesture,false) && ready)EndVolume(host);
   pressed=nullptr;rowDragging=selectionPending=profileSliding=false;EndMove();if(message==WM_CANCELMODE && GetCapture()==window)ReleaseCapture();return 0;
  case WM_SETCURSOR:if(LOWORD(lp)==HTCLIENT){SetCursor(LoadCursorW(nullptr,IDC_ARROW));return TRUE;}break;
  case WM_GETMINMAXINFO:{auto* info=reinterpret_cast<MINMAXINFO*>(lp);info->ptMinTrackSize=info->ptMaxTrackSize={layout->Get(L"w"),layout->Get(L"h")};return 0;}
  }
  return DefSubclassProc(window,message,wp,lp);
 }
 static LRESULT CALLBACK Subclass(HWND w,UINT m,WPARAM wp,LPARAM lp,UINT_PTR,DWORD_PTR data){
  auto* self=reinterpret_cast<Impl*>(data);
  if(m==WM_NCDESTROY){RemoveWindowSubclass(w,Subclass,modernId);self->window=nullptr;return DefSubclassProc(w,m,wp,lp);}
  try{return self->Message(m,wp,lp);}catch(const std::exception& e){self->fault=true;self->pressed=nullptr;if(GetCapture()==w)ReleaseCapture();OutputDebugStringA(e.what());OutputDebugStringW(L"\nttp_waskin: WAL runtime failed; script events disabled.\n");return 0;}
 }
 void Attach(HWND w){
  Require(IsWindow(w),"invalid player window");window=w;GetWindowRect(w,&saved);savedRegion=CreateRectRgn(0,0,0,0);
  if(GetWindowRgn(w,savedRegion)==ERROR){DeleteObject(savedRegion);savedRegion=nullptr;}
  Require(SetWindowSubclass(w,Subclass,modernId,reinterpret_cast<DWORD_PTR>(this))!=FALSE,"modern subclass");
  for(HWND child=GetWindow(w,GW_CHILD);child;child=GetWindow(child,GW_HWNDNEXT)){bool visible=(GetWindowLongPtrW(child,GWL_STYLE)&WS_VISIBLE)!=0;children.emplace_back(child,visible);if(visible)ShowWindow(child,SW_HIDE);}
  ready=true;Flush();
  if(restoreLeft && root->Find(L"eqToggle"))Click(root->Find(L"eqToggle"));
  if(restoreRight && root->Find(L"plToggle"))Click(root->Find(L"plToggle"));
  if(restoreVis && root->Find(L"avsToggle"))Click(root->Find(L"avsToggle"));
  Advance(GetTickCount(),true);
  const auto& pos=IsRectEmpty(&restored)?saved:restored;
  SetWindowPos(w,nullptr,pos.left,pos.top,layout->Get(L"w"),layout->Get(L"h"),SWP_NOACTIVATE|SWP_NOZORDER);
  Render();SyncContent(true);Region();Require(SetTimer(w,modernTimer,20,nullptr)!=0,"modern timer");InvalidateRect(w,nullptr,FALSE);
 }
 void Detach()noexcept {
  ready=false;if(!window)return;
  if(IsWindow(window)){
   KillTimer(window,modernTimer);try{EndMove();contentRect={};SyncContent(true);}catch(...){}
   if(GetCapture()==window)ReleaseCapture();
   RemoveWindowSubclass(window,Subclass,modernId);
   if(tooltip)DestroyWindow(tooltip);tooltip=nullptr;
   for(auto [child,visible]:children)if(visible && IsWindow(child))ShowWindow(child,SW_SHOWNA);
   SetWindowPos(window,nullptr,0,0,saved.right-saved.left,saved.bottom-saved.top,SWP_NOMOVE|SWP_NOACTIVATE|SWP_NOZORDER);
   if(SetWindowRgn(window,savedRegion,TRUE))savedRegion=nullptr;
   InvalidateRect(window,nullptr,TRUE);
  }
  window=nullptr;children.clear();regionRects.clear();
 }
};
Modern::Modern(const wchar_t* path,const TtpSkinHost* host):Skin(kBuiltinPackage,host,true),impl_(std::make_unique<Impl>(path,host)){}
Metadata Modern::Inspect(const wchar_t* path) {
 Services services;MakiLibrary vm;Archive archive(path,true);const auto data=archive.Read("skin.xml");auto xml=ParseXml(data);
 Require(xml->children.size()==1 && (xml->children.front()->kind==L"winampabstractionlayer" || xml->children.front()->kind==L"wasabixml"),"invalid WAL root");
 return ReadMetadata(data);
}
Modern::~Modern(){Detach();}
const Metadata& Modern::Info()const{return impl_->metadata;}
std::wstring Modern::Diagnostic()const{return impl_->document.notes.Text();}
HRESULT Modern::Attach(const TtpSkinWindows& windows){
 auto result=Skin::Attach(windows);if(FAILED(result))return result;
 try{impl_->Attach(windows.player);return S_OK;}catch(...){Detach();return E_FAIL;}
}
void Modern::Detach()noexcept {if(impl_)impl_->Detach();Skin::Detach();}
HBITMAP Modern::Preview(){
 impl_->Render();
 // The options preview uses COLOR_WINDOW, just like the native skin preview.
 // Flatten alpha on that same matte before scaling, so translucent edges do
 // not retain the old hard-coded dark gray rectangle.
 const COLORREF background=GetSysColor(COLOR_WINDOW);
 Gdiplus::Bitmap preview(impl_->frame->GetWidth(),impl_->frame->GetHeight(),PixelFormat32bppARGB);
 {Gdiplus::Graphics graphics(&preview);graphics.Clear(Impl::GColor(background));graphics.DrawImage(impl_->frame.get(),0,0);}
 HBITMAP bitmap{};preview.GetHBITMAP(Impl::GColor(background),&bitmap);return bitmap;
}
void Modern::Paint(HWND w,HDC dc,bool child){if(w==impl_->window)impl_->Draw(dc,child);else Skin::Paint(w,dc,child);}
bool Modern::Handles(HWND w)const{return (w && w==impl_->window)||Skin::Handles(w);}
bool Modern::Translate(const MSG& msg){
 if(msg.message==WM_MOUSEWHEEL && impl_->window && !GetCapture() &&
    IsWindowEnabled(impl_->window) && IsWindowVisible(impl_->window) &&
    GetWindowThreadProcessId(msg.hwnd,nullptr)==GetCurrentThreadId()){
  POINT p{GET_X_LPARAM(msg.lParam),GET_Y_LPARAM(msg.lParam)};
  if(WindowFromPoint(p)==impl_->window){SendMessageW(impl_->window,msg.message,msg.wParam,msg.lParam);return true;}
 }
 return Skin::Translate(msg);
}
HRESULT Modern::Layout(TtpSkinLayout& state,bool restore){
 if(restore){
  if(impl_->window)return E_UNEXPECTED;
  std::wstring text(state.state,wcsnlen_s(state.state,std::size(state.state))),layoutId;int version{},left{},right{},vis{},scroll{};uint32_t contentMode=1,contentVisual=1;
  const auto separator=text.find(L'|');
  if(!text.empty()){
   if(separator==std::wstring::npos)return E_INVALIDARG;
   std::wistringstream input(text.substr(0,separator));
   if(!(input>>version>>left>>right>>vis>>scroll) || (version<1 || version>3) || left<0 || left>1 || right<0 || right>1 || vis<0 || vis>1 || scroll<0)return E_INVALIDARG;
   if(version>=2 && !(input>>std::quoted(layoutId)))return E_INVALIDARG;
   if(!layoutId.empty() && std::none_of(impl_->layouts.begin(),impl_->layouts.end(),[&](Node* n){return Lower(n->Id())==Lower(layoutId);}))return E_INVALIDARG;
   if(version>=3 && (!(input>>contentMode>>contentVisual) || contentMode<1 || contentMode>3 || contentVisual>=visualEffectNames.size()))return E_INVALIDARG;
   input>>std::ws;if(!input.eof())return E_INVALIDARG;
  }
  auto fallback=state;wcscpy_s(fallback.state,text.empty()?L"":text.substr(separator+1).c_str());
  auto result=Skin::Layout(fallback,true);if(FAILED(result))return result;
  if(!layoutId.empty())impl_->SwitchLayout(layoutId);
  impl_->contentMode=contentMode;impl_->contentVisual=contentVisual;
  impl_->restored=state.windows[0];impl_->restoreLeft=left!=0;impl_->restoreRight=right!=0;impl_->restoreVis=vis!=0;impl_->scroll=scroll;return S_OK;
 }
 auto result=Skin::Layout(state,false);if(FAILED(result))return result;
 std::wstring tail=state.state;auto* left=impl_->root->Find(L"LeftDrawerStatus");auto* right=impl_->root->Find(L"RightDrawerStatus");auto* vis=impl_->root->Find(L"vis");
 std::wostringstream payload;payload<<3<<L' '<<(left?left->Get(L"x"):0)<<L' '<<(right?right->Get(L"x"):0)<<L' '<<(vis && vis->visible?1:0)<<L' '<<impl_->scroll<<L' '<<std::quoted(impl_->layout->Id())<<L' '<<impl_->contentMode<<L' '<<impl_->contentVisual<<L'|'<<tail;
 if(payload.str().size()>=std::size(state.state))return E_FAIL;wcscpy_s(state.state,payload.str().c_str());
 if(impl_->window)GetWindowRect(impl_->window,&state.windows[0]);else state.windows[0]=impl_->restored;return S_OK;
}
bool Modern::ContentState(TtpSkinContent& state,bool apply){
 if(state.window!=impl_->window)return Skin::ContentState(state,apply);
 if(state.size<sizeof(state) || !impl_->window)return false;
 if(apply){
   if(state.mode<1 || state.mode>3 || state.visual_type>=visualEffectNames.size())return false;
  SendMessageW(impl_->window,WM_CANCELMODE,0,0);
  impl_->contentMode=state.mode;impl_->contentVisual=state.visual_type;
  impl_->SyncContent(true);InvalidateRect(impl_->window,nullptr,FALSE);
 }
 return impl_->Content(state);
}
HMENU Modern::Menu(HWND window,uint32_t command){
 if(window!=impl_->window)return Skin::Menu(window,command);
 if(command){
  TtpSkinContent c{sizeof(c),window};if(!ContentState(c,false))return nullptr;
  if(command>=700 && command<703)c.mode=command-699;
  else if(command>=710 && command<710+visualEffectNames.size())c.visual_type=command-710;
  else if(command==720)impl_->Command(TTP_SKIN_CONTENT_FULLSCREEN,c.mode|(c.visual_type<<8));
  ContentState(c,true);return nullptr;
 }
 HMENU menu=CreatePopupMenu(),effects=CreatePopupMenu();
 if(!menu || !effects){if(menu)DestroyMenu(menu);if(effects)DestroyMenu(effects);return nullptr;}
 const wchar_t* modes[]={L"歌词",L"视觉效果",L"歌词与视觉同屏"};
 for(UINT i=0;i<3;++i)AppendMenuW(menu,MF_STRING|(impl_->contentMode==i+1?MF_CHECKED:0),700+i,modes[i]);
 for(UINT i=0;i<visualEffectNames.size();++i)AppendMenuW(effects,MF_STRING|(impl_->contentVisual==i?MF_CHECKED:0),710+i,visualEffectNames[i]);
 AppendMenuW(menu,MF_POPUP,reinterpret_cast<UINT_PTR>(effects),L"视觉效果类型");
 AppendMenuW(menu,MF_STRING,720,L"全屏显示当前内容");return menu;
}
bool Modern::DefaultPlaylistFont(LOGFONTW& font)const{font=Impl::DefaultPlaylistFont();return font.lfHeight!=0;}
bool Modern::PlaylistViewport(HWND window,uint32_t& first,uint32_t& count){
 if(!window || window!=impl_->window)return Skin::PlaylistViewport(window,first,count);
 first=count=0;
 if(!impl_->ready || !IsWindowVisible(window) || IsIconic(window))return true;
 if(impl_->SyncPlaylistFont() || impl_->revealRow>=0)impl_->Render();
 RECT client{},visible{};GetClientRect(window,&client);
 if(!IntersectRect(&visible,&client,&impl_->playlistRect))return true;
 const int total=int(impl_->State().track_count),height=impl_->rowHeight;
 const auto& r=impl_->playlistRect;
 // A sliding drawer can render its list under another skin control.
 // Use the same hit ordering as mouse input to exclude covered rows.
 for(int y=visible.top;y<visible.bottom;) {
  const int offset=(y-r.top)/height,row=impl_->scroll+offset;
  if(row>=total)break;
  const int end=std::min(int(visible.bottom),int(r.top)+(offset+1)*height);
  const int middle=(y+end-1)/2;
  if(impl_->PlaylistHit({visible.left,middle}) ||
     impl_->PlaylistHit({(visible.left+visible.right)/2,middle}) ||
     impl_->PlaylistHit({visible.right-1,middle})) {
   if(!count)first=uint32_t(row);
   count=uint32_t(row)-first+1;
  }
  y=end;
 }
 return true;
}
bool Modern::LyricFont(LOGFONTW& font)const{
 // Derive the host's GDI font from the very same pixel font as the embedded
 // playlist. XML text/bitmap fonts belong to other skin controls.
 Gdiplus::Bitmap bitmap(1,1,PixelFormat32bppARGB);Gdiplus::Graphics graphics(&bitmap);
 auto playlistFont=Impl::PlaylistFont();
 return playlistFont.GetLogFontW(&graphics,&font)==Gdiplus::Ok;
}
bool Modern::VolumeTracking()const{
 return (impl_->window && GetCapture()==impl_->window && impl_->volumeGesture) || Skin::VolumeTracking();
}
bool Modern::PlaylistReveal(uint32_t row,int32_t caret){
 const bool classic=Skin::PlaylistReveal(row,caret);
 if(row>=impl_->State().track_count || row>INT_MAX)return classic;
 impl_->selected=caret;impl_->revealRow=int(row);
 if(impl_->window)InvalidateRect(impl_->window,nullptr,FALSE);
 return true;
}
bool Modern::PlaylistDrop(TtpSkinPlaylistDrop& drop){
 if(drop.size<sizeof(drop) || drop.window!=impl_->window)return Skin::PlaylistDrop(drop);
 if(drop.phase>TTP_SKIN_DROP_LEAVE)return false;
 if(impl_->SyncPlaylistFont())impl_->Render();
 drop.insertion=-1;
 const bool inside=impl_->PlaylistHit(drop.point,true);
 int top=impl_->scroll;
 if(drop.phase!=TTP_SKIN_DROP_LEAVE && inside)
  drop.insertion=PlaylistInsertion(impl_->playlistRect,impl_->rowHeight,drop.point,top,int(impl_->State().track_count),drop.phase==TTP_SKIN_DROP_PREVIEW);
 if(drop.phase!=TTP_SKIN_DROP_QUERY){impl_->scroll=top;impl_->drop=drop.insertion;impl_->Render();InvalidateRect(impl_->window,nullptr,FALSE);}
 return inside || drop.phase==TTP_SKIN_DROP_LEAVE;
}
}
