// Windows setup for the Scooby-Doo! Mystery Mayhem recompilation. It installs
// the release payload it carries, verifies the player's own XISO, extracts the
// verified default.xbe, and, copied as Uninstall.exe, removes only the files it
// recorded. Saved games, preferences and session reports are always kept.
#include "xbox/disc.h"
#include "xbox/file.h"
#include "target.h"
#include <Windows.h>
#include <commctrl.h>
#include <commdlg.h>
#include <fdi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <shellapi.h>
#include <uxtheme.h>
#include <wrl/client.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <format>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <sstream>
#include <thread>

using Microsoft::WRL::ComPtr;
using xbox_native::Bytes;
namespace {
constexpr wchar_t title[]=L"Scooby-Doo! Mystery Mayhem";
constexpr wchar_t shortcut_name[]=L"Scooby-Doo! Mystery Mayhem.lnk";
constexpr wchar_t registry[]=L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\ScoobyMMRecomp";
constexpr std::string_view marker="SCOOBYMM-INSTALL-V1",payload_marker="SCOOBYMM-PAYLOAD-V1";
// The palette of the in-game settings menu (src/xbox/settings_menu.cpp).
constexpr COLORREF cream=RGB(232,241,236),gold=RGB(255,207,66),ink=RGB(26,48,56),teal=RGB(13,82,90),border=RGB(30,120,118);
constexpr RECT form_panel{24,116,856,432};
constexpr int status_top=form_panel.bottom+8;
constexpr RECT status_panel{24,status_top,856,status_top+188};
constexpr int buttons_top=status_panel.bottom+10,client_width=880,client_height=buttons_top+54;
constexpr int form_left=form_panel.left+24,form_right=form_panel.right-24;
constexpr int status_left=status_panel.left+16,status_right=status_panel.right-16;
constexpr int checkbox_inset_x=12,checkbox_inset_y=4;
constexpr std::array checkbox_rows{RECT{form_left,330,form_right,364},RECT{form_left,374,form_right,408}};
constexpr int backdrop_width=512,backdrop_height=256;
struct Entry {std::filesystem::path path;std::uint64_t size;std::string digest;};
struct State {
    std::mutex mutex;std::wstring phase=L"Choose your game image and installation folder.",error;
    unsigned percent=0,stage=0;bool running=false,complete=false;
    std::uint64_t revision=1;
    std::atomic_bool cancel=false;
    std::jthread worker;
} state;
HWND window=nullptr,source_edit=nullptr,destination_edit=nullptr,copy_check=nullptr,desktop_check=nullptr,install_button=nullptr,close_button=nullptr,progress_bar=nullptr;
std::vector<HWND> inputs;
std::vector<std::pair<HWND,RECT>> controls;
HFONT font=nullptr,heading=nullptr,font_small=nullptr;
HICON logo_icon=nullptr;
HBRUSH edit_brush=nullptr;
std::vector<std::uint32_t> backdrop,backdrop_inset;
unsigned dpi=96;
unsigned preview_dpi=0;
bool uninstalling=false;
unsigned cancellation_percent=101;
std::filesystem::path uninstall_root;
std::uint64_t started=0;
std::uint64_t status_invalidations=0;

void checked(bool success,std::string_view operation) {
    if(!success) throw std::runtime_error(std::string(operation)+std::format(" (Windows error {}).",GetLastError()));
}
void checked_hr(HRESULT result,std::string_view operation) {if(FAILED(result)) throw std::runtime_error(std::format("{} (0x{:08X}).",operation,unsigned(result)));}
std::filesystem::path utf8_path(std::string_view text) {return std::filesystem::path(std::u8string(text.begin(),text.end()));}
std::wstring wide(std::string_view text) {return utf8_path(text).wstring();}
std::string text_of(const std::filesystem::path& path) {return xbox_native::path_text(path);}
bool same_digest(std::string_view a,std::string_view b) {
    return a.size()==b.size() && std::ranges::equal(a,b,[](char x,char y) {
        return std::toupper(static_cast<unsigned char>(x))==std::toupper(static_cast<unsigned char>(y));
    });
}
std::string file_digest(const std::filesystem::path& path) {xbox_native::File file(path);return xbox_native::sha256(file,0,file.size());}
std::filesystem::path module_path() {
    std::array<wchar_t,32768> path{};const auto count=GetModuleFileNameW(nullptr,path.data(),DWORD(path.size()));
    checked(count && count<path.size(),"Cannot locate Setup");return std::wstring_view(path.data(),count);
}
// A typed trailing separator would break folder comparisons and the quoted
// "root\" UninstallString argument, where \" reads as a literal quote.
std::filesystem::path folder_path(const std::filesystem::path& value) {
    auto path=std::filesystem::absolute(value).lexically_normal();
    while(!path.has_filename() && path!=path.root_path()) path=path.parent_path();
    return path;
}
std::filesystem::path known_folder(REFKNOWNFOLDERID id) {
    PWSTR value=nullptr;checked_hr(SHGetKnownFolderPath(id,0,nullptr,&value),"Cannot locate your user folders");
    std::filesystem::path path(value);CoTaskMemFree(value);return path;
}
Bytes resource(unsigned id) {
    const auto entry=FindResourceW(nullptr,MAKEINTRESOURCEW(id),RT_RCDATA);checked(entry!=nullptr,"Setup data is missing");
    const auto handle=LoadResource(nullptr,entry);checked(handle!=nullptr,"Cannot read Setup data");
    return {static_cast<const std::byte*>(LockResource(handle)),SizeofResource(nullptr,entry)};
}
// Validates a relative path from the payload or an installation manifest. Saves,
// settings and session reports (data\user, build\) never belong to the program;
// only the extracted executable and copied disc image may name data\local.
std::filesystem::path relative_path(std::string_view value,bool data=false) {
    const auto path=utf8_path(value);
    if(path.empty() || path.is_absolute() || path.has_root_name() || path.has_root_directory() || value.find_first_of("\r\n\t:")!=std::string_view::npos)
        throw std::runtime_error("Invalid path in installation manifest.");
    for(const auto& part:path) if(part==".." || part==".") throw std::runtime_error("Invalid installation path.");
    const auto first=*path.begin();
    if(first=="build" || (first=="data" && !(data && (path=="data/local/default.xbe" || path=="data/local/game.iso"))))
        throw std::runtime_error("The installation manifest includes private user data.");
    return path;
}
std::vector<Entry> payload() {
    const auto bytes=resource(3);
    if(bytes.size()>256*1024) throw std::runtime_error("Installation manifest exceeds its size limit.");
    std::istringstream input{std::string(reinterpret_cast<const char*>(bytes.data()),bytes.size())};
    std::string line;std::getline(input,line);
    if(line!=payload_marker) throw std::runtime_error("Invalid installation manifest.");
    std::vector<Entry> entries;
    while(std::getline(input,line)) {
        const auto a=line.find('\t'),b=line.find('\t',a==std::string::npos?0:a+1);
        if(a==std::string::npos || b==std::string::npos || entries.size()==128) throw std::runtime_error("Invalid payload entry.");
        std::uint64_t size=0;const auto parsed=std::from_chars(line.data()+a+1,line.data()+b,size);
        if(parsed.ec!=std::errc{} || parsed.ptr!=line.data()+b || line.size()-b-1!=64) throw std::runtime_error("Invalid payload size or checksum.");
        entries.push_back({relative_path(std::string_view(line).substr(0,a)),size,line.substr(b+1)});
    }
    if(entries.empty()) throw std::runtime_error("Setup does not contain any game files.");
    return entries;
}
void report(unsigned stage,unsigned percent,std::string_view phase) {
    {std::lock_guard lock(state.mutex);state.stage=stage;state.percent=percent;state.phase=wide(phase);++state.revision;}
    if(stage==3 && percent>=cancellation_percent) state.cancel=true;
}
void cancellation() {if(state.cancel.load()) throw std::runtime_error("Installation cancelled. Your original game image and saved games were kept.");}
struct SetupLock {
    HANDLE handle=INVALID_HANDLE_VALUE;
    explicit SetupLock(const std::filesystem::path& root) {
        handle=CreateFileW((root/".setup-lock").c_str(),GENERIC_READ|GENERIC_WRITE,0,nullptr,CREATE_NEW,
            FILE_ATTRIBUTE_TEMPORARY|FILE_FLAG_DELETE_ON_CLOSE,nullptr);
        if(handle==INVALID_HANDLE_VALUE) throw std::runtime_error("Another setup may be using this folder. Close it and try again.");
    }
    ~SetupLock() {CloseHandle(handle);}
};
struct Cabinet {
    std::filesystem::path directory;const std::vector<Entry>& entries;
    Bytes archive;std::map<INT_PTR,std::size_t> cursors;INT_PTR next_archive=-2;
    std::uint64_t completed=0,total=0,current_size=0;std::string error;
    std::set<HANDLE> opened;
    Cabinet(std::filesystem::path path,const std::vector<Entry>& files,Bytes bytes):directory(std::move(path)),entries(files),archive(bytes) {}
};
thread_local Cabinet* reading_cabinet=nullptr;
void* DIAMONDAPI cabinet_allocate(ULONG bytes) {return std::malloc(bytes);}
void DIAMONDAPI cabinet_free(void* data) {std::free(data);}
INT_PTR DIAMONDAPI cabinet_open(char*,int,int) {
    try {const auto handle=reading_cabinet->next_archive--;reading_cabinet->cursors.emplace(handle,0);return handle;} catch(...) {return -1;}
}
UINT DIAMONDAPI cabinet_read(INT_PTR handle,void* output,UINT bytes) {
    auto& cabinet=*reading_cabinet;
    const auto found=cabinet.cursors.find(handle);if(found==cabinet.cursors.end()) return UINT(-1);
    auto& cursor=found->second;const auto count=std::min<std::size_t>(bytes,cabinet.archive.size()-cursor);
    std::memcpy(output,cabinet.archive.data()+cursor,count);cursor+=count;return UINT(count);
}
UINT DIAMONDAPI cabinet_write(INT_PTR handle,void* data,UINT bytes) {
    DWORD written=0;return WriteFile(reinterpret_cast<HANDLE>(handle),data,bytes,&written,nullptr)?written:UINT(-1);
}
int DIAMONDAPI cabinet_close(INT_PTR handle) {
    if(reading_cabinet->cursors.erase(handle)) return 0;
    const auto file=reinterpret_cast<HANDLE>(handle);
    if(reading_cabinet->opened.erase(file)) return CloseHandle(file)?0:-1;
    return 0;
}
long DIAMONDAPI cabinet_seek(INT_PTR handle,long offset,int origin) {
    auto& cabinet=*reading_cabinet;
    const auto found=cabinet.cursors.find(handle);if(found==cabinet.cursors.end()) return -1;
    const auto base=origin==SEEK_SET?0:origin==SEEK_CUR?std::int64_t(found->second):std::int64_t(cabinet.archive.size());
    const auto position=base+offset;
    if(position<0 || std::uint64_t(position)>cabinet.archive.size()) return -1;
    found->second=std::size_t(position);return long(position);
}
INT_PTR DIAMONDAPI cabinet_callback(FDINOTIFICATIONTYPE notification,PFDINOTIFICATION info) {
    auto& cabinet=*static_cast<Cabinet*>(info->pv);
    try {
        cancellation();
        if(notification==fdintCOPY_FILE) {
            const auto name=relative_path(info->psz1);
            const auto found=std::ranges::find(cabinet.entries,name,&Entry::path);
            if(found==cabinet.entries.end() || std::uint64_t(info->cb)!=found->size) throw std::runtime_error("Unexpected file in Setup payload.");
            const auto target=cabinet.directory/name;std::filesystem::create_directories(target.parent_path());
            const auto file=CreateFileW(target.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);
            checked(file!=INVALID_HANDLE_VALUE,"Cannot create an application file");cabinet.opened.insert(file);cabinet.current_size=found->size;
            report(2,unsigned(cabinet.completed*100/cabinet.total),"Installing "+text_of(name));
            return reinterpret_cast<INT_PTR>(file);
        }
        if(notification==fdintCLOSE_FILE_INFO) {
            checked(cabinet_close(info->hf)==0,"Cannot finish writing an application file");cabinet.completed+=cabinet.current_size;
            report(2,unsigned(cabinet.completed*100/cabinet.total),"Installing application files");
            return TRUE;
        }
        if(notification==fdintNEXT_CABINET || notification==fdintPARTIAL_FILE) throw std::runtime_error("Setup payload is incomplete.");
        return 0;
    } catch(const std::exception& error) {cabinet.error=error.what();return -1;}
}
void extract_payload(const std::filesystem::path& directory,const std::vector<Entry>& entries) {
    Cabinet context{directory,entries,resource(2)};for(const auto& entry:entries) context.total+=entry.size;
    reading_cabinet=&context;ERF error{};
    const auto decoder=FDICreate(cabinet_allocate,cabinet_free,cabinet_open,cabinet_read,cabinet_write,cabinet_close,cabinet_seek,cpuUNKNOWN,&error);
    if(!decoder) throw std::runtime_error("Cannot open the installer payload.");
    const auto result=FDICopy(decoder,const_cast<char*>("payload.cab"),const_cast<char*>(""),0,cabinet_callback,nullptr,&context);
    FDIDestroy(decoder);for(const auto file:context.opened) CloseHandle(file);reading_cabinet=nullptr;
    if(!context.error.empty()) throw std::runtime_error(context.error);
    if(!result) throw std::runtime_error(std::format("Cannot unpack the application (cabinet error {}).",error.erfOper));
    for(const auto& entry:entries) {
        cancellation();const auto path=directory/entry.path;
        if(std::filesystem::file_size(path)!=entry.size || !same_digest(file_digest(path),entry.digest))
            throw std::runtime_error("Installed file failed verification: "+text_of(entry.path));
    }
}
std::unique_ptr<xbox_native::Disc> open_disc(const std::filesystem::path& path) {
    if(!std::filesystem::is_regular_file(path)) throw std::runtime_error("Choose your Scooby-Doo! Mystery Mayhem disc image file.");
    try {return std::make_unique<xbox_native::Disc>(path);}
    catch(const std::exception& error) {throw std::runtime_error(std::string("The selected file is not a readable Xbox XISO image (")+error.what()+").");}
}
// knowledge/supported_target.json names the one executable this build translates.
xbox_native::DiscEntry verified_executable(xbox_native::Disc& disc) {
    const auto entry=disc.lookup("default.xbe");
    if(!entry || entry->directory || entry->size!=xbox_native::target::file_size ||
       !same_digest(disc.file_sha256(*entry,0,entry->size),xbox_native::target::sha256))
        throw std::runtime_error("Select the supported Xbox release: Scooby-Doo! Mystery Mayhem (USA, Europe) (En,Fr,De).");
    return *entry;
}
// Prepares data\local\default.xbe, the optional data\local\game.iso and the
// media lines of data\user\settings.cfg in the staging folder. Settings name the
// final installation paths; the player's other preferences are preserved.
void install_game_files(const std::filesystem::path& iso,const std::filesystem::path& staging,const std::filesystem::path& root,bool copy_disc) {
    auto disc=open_disc(iso);const auto entry=verified_executable(*disc);
    const auto local=staging/"data"/"local",user=staging/"data"/"user";
    std::filesystem::create_directories(local);std::filesystem::create_directories(user);
    const auto image=local/"game.iso";
    report(3,0,copy_disc?"Copying your game image":"Preparing game files");
    const bool same_image=std::filesystem::exists(image) && std::filesystem::equivalent(iso,image);
    if(copy_disc && !same_image) {
        if(std::filesystem::space(local).available<disc->size()+entry.size+16*1024*1024)
            throw std::runtime_error("There is not enough free space to copy the game image. Choose another folder or keep the image in its current location.");
        {
            xbox_native::File source(iso);std::ofstream output(image,std::ios::binary|std::ios::noreplace);
            if(!output) throw std::runtime_error("Cannot create the copied game image.");
            output.exceptions(std::ios::failbit|std::ios::badbit);
            std::vector<std::byte> buffer(4*1024*1024);unsigned previous=101;
            for(std::uint64_t offset=0;offset<source.size();) {
                cancellation();const auto count=std::min<std::uint64_t>(buffer.size(),source.size()-offset);
                source.read(offset,std::span(buffer).first(static_cast<std::size_t>(count)));
                output.write(reinterpret_cast<const char*>(buffer.data()),static_cast<std::streamsize>(count));offset+=count;
                const auto percent=static_cast<unsigned>(offset*100/source.size());
                if(percent!=previous) {report(3,percent,"Copying your game image");previous=percent;}
            }
            output.close();
        }
        cancellation();
        auto copied=open_disc(image);verified_executable(*copied);
    }
    cancellation();
    report(3,100,"Extracting the game executable");
    const auto executable=local/"default.xbe";
    {
        std::ofstream output(executable,std::ios::binary|std::ios::noreplace);
        if(!output) throw std::runtime_error("Cannot create the game executable.");
        output.exceptions(std::ios::failbit|std::ios::badbit);
        std::vector<std::byte> buffer(1024*1024);
        for(std::uint64_t offset=0;offset<entry.size;) {
            const auto count=static_cast<std::size_t>(std::min<std::uint64_t>(buffer.size(),entry.size-offset));
            disc->read(entry,offset,std::span(buffer).first(count));
            output.write(reinterpret_cast<const char*>(buffer.data()),static_cast<std::streamsize>(count));offset+=count;
        }
        output.close();
    }
    if(!same_digest(file_digest(executable),xbox_native::target::sha256)) throw std::runtime_error("Extracted game executable failed verification.");
    std::string preferences;
    const auto settings=root/"data"/"user"/"settings.cfg";
    if(std::filesystem::exists(settings)) {
        if(std::filesystem::file_size(settings)>256*1024) throw std::runtime_error("The existing settings.cfg exceeds 256 KiB.");
        std::ifstream input(settings,std::ios::binary);std::string line;
        while(std::getline(input,line)) {
            if(line.ends_with('\r')) line.pop_back();
            if(!line.empty() && !line.starts_with("xbe=") && !line.starts_with("disc=")) preferences+=line+'\n';
        }
    }
    const auto disc_path=copy_disc?root/"data"/"local"/"game.iso":std::filesystem::absolute(iso).lexically_normal();
    const auto xbe=text_of(root/"data"/"local"/"default.xbe"),image_path=text_of(disc_path);
    if((xbe+image_path).find_first_of("\r\n")!=std::string::npos) throw std::runtime_error("The game-image path contains a line break.");
    xbox_native::write_text(user/"settings.cfg","xbe="+xbe+"\ndisc="+image_path+'\n'+preferences);
    report(3,100,"Game files ready");
}
void shortcut(const std::filesystem::path& destination,const std::filesystem::path& root) {
    ComPtr<IShellLinkW> link;checked_hr(CoCreateInstance(CLSID_ShellLink,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&link)),"Cannot create the game shortcut");
    checked_hr(link->SetPath((root/"scoobymm.exe").c_str()),"Cannot set the shortcut target");
    checked_hr(link->SetWorkingDirectory(root.c_str()),"Cannot set the shortcut folder");
    link->SetDescription(L"Play Scooby-Doo! Mystery Mayhem");link->SetIconLocation((root/"scoobymm.exe").c_str(),0);
    ComPtr<IPersistFile> file;checked_hr(link.As(&file),"Cannot save the game shortcut");
    std::filesystem::create_directories(destination.parent_path());checked_hr(file->Save(destination.c_str(),TRUE),"Cannot save the game shortcut");
}
bool shortcut_targets(const std::filesystem::path& path,const std::filesystem::path& root) {
    ComPtr<IShellLinkW> link;
    if(FAILED(CoCreateInstance(CLSID_ShellLink,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&link)))) return false;
    ComPtr<IPersistFile> file;if(FAILED(link.As(&file)) || FAILED(file->Load(path.c_str(),STGM_READ))) return false;
    std::array<wchar_t,32768> target{};
    return SUCCEEDED(link->GetPath(target.data(),int(target.size()),nullptr,SLGP_RAWPATH)) && std::filesystem::path(target.data())==root/"scoobymm.exe";
}
void remove_registered_shortcut(const std::filesystem::path& path,const std::filesystem::path& root) {
    if(shortcut_targets(path,root)) {std::error_code error;std::filesystem::remove(path,error);}
}
void register_install(const std::filesystem::path& root,std::optional<bool> desktop,std::uint64_t bytes) {
    checked_hr(CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED),"Cannot initialize shortcuts");
    struct Com {~Com(){CoUninitialize();}} com;
    shortcut(known_folder(FOLDERID_Programs)/shortcut_name,root);
    const auto desktop_link=known_folder(FOLDERID_Desktop)/shortcut_name;
    if(!desktop) desktop=shortcut_targets(desktop_link,root);
    if(*desktop) shortcut(desktop_link,root);
    else remove_registered_shortcut(desktop_link,root);
    HKEY key=nullptr;const auto status=RegCreateKeyExW(HKEY_CURRENT_USER,registry,0,nullptr,0,KEY_SET_VALUE,nullptr,&key,nullptr);
    if(status!=ERROR_SUCCESS) {SetLastError(status);checked(false,"Cannot register the uninstaller");}
    struct Key {HKEY key;~Key(){RegCloseKey(key);}} guard{key};
    const auto value=[&](const wchar_t* name,std::wstring text) {
        const auto error=RegSetValueExW(key,name,0,REG_SZ,reinterpret_cast<const BYTE*>(text.c_str()),DWORD((text.size()+1)*sizeof(wchar_t)));
        if(error!=ERROR_SUCCESS) {SetLastError(error);checked(false,"Cannot register installation details");}
    };
    const auto number=[&](const wchar_t* name,DWORD data) {RegSetValueExW(key,name,0,REG_DWORD,reinterpret_cast<const BYTE*>(&data),sizeof(data));};
    value(L"DisplayName",title);value(L"DisplayVersion",wide(SCOOBYMM_VERSION));value(L"InstallLocation",root.native());
    value(L"DisplayIcon",(root/"scoobymm.exe").native());value(L"UninstallString",L"\""+(root/"Uninstall.exe").native()+L"\" --uninstall \""+root.native()+L"\"");
    number(L"NoModify",1);number(L"NoRepair",1);number(L"DesktopShortcut",*desktop?1:0);
    number(L"EstimatedSize",DWORD(std::min<std::uint64_t>(bytes/1024,0xFFFFFFFF)));
}
void remove_stale_uninstallers() {
    // Earlier temporary uninstallers cannot delete themselves; remove idle ones.
    // A copy that is still running stays, because Windows refuses the deletion.
    constexpr std::wstring_view prefix=L"scoobymm-uninstall-",suffix=L".exe";
    std::error_code error;const auto directory=std::filesystem::temp_directory_path(error);if(error) return;
    for(std::filesystem::directory_iterator item(directory,error),end;!error && item!=end;item.increment(error)) {
        const std::wstring_view name=item->path().filename().native();
        if(!name.starts_with(prefix) || !name.ends_with(suffix) || name.size()<=prefix.size()+suffix.size()) continue;
        const auto digits=name.substr(prefix.size(),name.size()-prefix.size()-suffix.size());
        if(std::ranges::all_of(digits,[](wchar_t c) {return c>=L'0' && c<=L'9';})) DeleteFileW(item->path().c_str());
    }
}
// Windows paths may differ only in letter case, as typed or as stored on disk.
bool same_path(const std::filesystem::path& a,const std::filesystem::path& b) {
    const auto left=a.lexically_normal().make_preferred(),right=b.lexically_normal().make_preferred();
    return CompareStringOrdinal(left.c_str(),-1,right.c_str(),-1,TRUE)==CSTR_EQUAL;
}
bool linked(const std::filesystem::path& path) {return !same_path(std::filesystem::weakly_canonical(path),path);}
bool empty_installation_folder(const std::filesystem::path& root) {
    for(const auto& item:std::filesystem::directory_iterator(root))
        if(!item.path().filename().native().starts_with(L".installing-")) return false;
    return true;
}
void remove_stale_staging(const std::filesystem::path& root) {
    // The exclusive folder lock proves no other setup owns these directories.
    std::vector<std::filesystem::path> stale;
    for(const auto& item:std::filesystem::directory_iterator(root)) {
        const auto attributes=GetFileAttributesW(item.path().c_str());
        if(item.path().filename().native().starts_with(L".installing-") && attributes!=INVALID_FILE_ATTRIBUTES &&
           (attributes&FILE_ATTRIBUTE_DIRECTORY) && !(attributes&FILE_ATTRIBUTE_REPARSE_POINT)) stale.push_back(item.path());
    }
    for(const auto& path:stale) std::filesystem::remove_all(path);
}
void write_manifest(const std::filesystem::path& path,const std::set<std::filesystem::path>& files) {
    std::string text(marker);text+='\n';for(const auto& file:files) text+=text_of(file)+'\n';
    xbox_native::write_text(path,text);
}
std::set<std::filesystem::path> installed_files(const std::filesystem::path& root) {
    const auto path=root/"install-manifest.txt";
    if(std::filesystem::file_size(path)>256*1024) throw std::runtime_error("Invalid installation manifest.");
    std::ifstream input(path);std::string line;std::getline(input,line);
    if(line!=marker) throw std::runtime_error("This folder belongs to another program. Choose an empty installation folder.");
    std::set<std::filesystem::path> files;
    while(std::getline(input,line)) files.insert(relative_path(line,true));
    return files;
}
// desktop: create or remove the desktop shortcut; empty keeps the current choice.
void install(const std::filesystem::path& iso,const std::filesystem::path& chosen,bool copy_disc,std::optional<bool> desktop,bool registration) {
    auto root=folder_path(chosen);
    if(root==root.root_path() || root.native().size()>160 || linked(root))
        throw std::runtime_error("Choose a shorter installation folder without links or junctions.");
    root=std::filesystem::weakly_canonical(root);
    if(same_path(module_path().parent_path(),root)) throw std::runtime_error("Run the downloaded Setup to update this installation.");
    if(std::filesystem::exists(root) && !empty_installation_folder(root) && !std::filesystem::exists(root/"install-manifest.txt"))
        throw std::runtime_error("Choose an empty folder, or the folder containing an existing installation.");
    const auto previous_files=std::filesystem::exists(root/"install-manifest.txt")?installed_files(root):std::set<std::filesystem::path>{};
    const auto entries=payload();
    std::uint64_t payload_bytes=0;
    for(const auto& entry:entries) {
        payload_bytes+=entry.size;
        if(linked((root/entry.path).parent_path())) throw std::runtime_error("An application folder crosses a link or junction.");
        if(std::filesystem::exists(root/entry.path) && !previous_files.contains(entry.path))
            throw std::runtime_error("The destination contains an unrelated application file. Choose an empty folder.");
    }
    for(const auto& file:previous_files) if(linked((root/file).parent_path())) throw std::runtime_error("An application folder crosses a link or junction.");
    for(const auto folder:{"data/local","data/user"}) if(linked(root/folder)) throw std::runtime_error("The game-data folder crosses a link or junction.");
    report(1,0,"Checking your game image");
    {auto disc=open_disc(iso);verified_executable(*disc);}
    cancellation();report(1,100,"Game image verified");
    std::filesystem::create_directories(root);
    const auto uninstaller_bytes=std::filesystem::file_size(module_path());
    if(std::filesystem::space(root).available<payload_bytes+uninstaller_bytes+xbox_native::target::file_size+64*1024*1024)
        throw std::runtime_error(std::format("Setup needs {:.1f} GB of free space in the installation folder.",
            double(payload_bytes+uninstaller_bytes+64*1024*1024)/1e9));
    SetupLock lock(root);
    remove_stale_staging(root);
    const auto staging=root/std::format(".installing-{}",GetCurrentProcessId());
    if(!std::filesystem::create_directory(staging)) throw std::runtime_error("An installation is already using this folder.");
    struct Staging {
        std::filesystem::path path,root;
        ~Staging() {
            // Recursive cleanup is restricted to this newly-created child of the selected installation folder.
            std::error_code error;
            if(path.parent_path()==root && path.filename().native().starts_with(L".installing-") && same_path(std::filesystem::weakly_canonical(path,error),path))
                std::filesystem::remove_all(path,error);
        }
    } cleanup{staging,root};
    report(2,0,"Installing application files");extract_payload(staging,entries);
    report(3,0,"Preparing your game files");
    bool reuse_disc=false;
    if(copy_disc && std::filesystem::exists(root/"data/local/game.iso") && std::filesystem::equivalent(iso,root/"data/local/game.iso")) {
        std::filesystem::create_directories(staging/"data/local");std::error_code error;
        std::filesystem::create_hard_link(root/"data/local/game.iso",staging/"data/local/game.iso",error);reuse_disc=!error;
    }
    install_game_files(iso,staging,root,copy_disc);
    checked(CopyFileW(module_path().c_str(),(staging/"Uninstall.exe").c_str(),TRUE)!=FALSE,"Cannot prepare the uninstaller");
    std::set<std::filesystem::path> recorded;for(const auto& entry:entries) recorded.insert(entry.path);
    recorded.insert("Uninstall.exe");recorded.insert("data/local/default.xbe");
    if(copy_disc || previous_files.contains("data/local/game.iso")) recorded.insert("data/local/game.iso");
    write_manifest(staging/"install-manifest.txt",recorded);
    // Until every file is in place the folder's manifest also lists the previous
    // files, so an interrupted update is repaired by running Setup again.
    auto pending=recorded;pending.insert(previous_files.begin(),previous_files.end());
    write_manifest(staging/"install-manifest.pending",pending);
    std::vector<std::filesystem::path> files(recorded.begin(),recorded.end());
    files.push_back("data/user/settings.cfg");files.push_back("install-manifest.txt");cancellation();
    // Detect a running game before replacing or removing any of its application files.
    for(const auto& file:pending) if(file!="data/local/game.iso" && std::filesystem::exists(root/file)) {
        const auto handle=CreateFileW((root/file).c_str(),GENERIC_WRITE|DELETE,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
        if(handle==INVALID_HANDLE_VALUE) throw std::runtime_error("Close the game and its diagnostics, then try installing again.");
        CloseHandle(handle);
    }
    report(4,0,"Finishing installation");
    const auto move=[&](const std::filesystem::path& from,const std::filesystem::path& to) {
        checked(MoveFileExW(from.c_str(),to.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)!=FALSE,"Cannot finish installing application files");
    };
    move(staging/"install-manifest.pending",root/"install-manifest.txt");
    unsigned committed=0;
    for(const auto& file:files) {
        if(reuse_disc && file=="data/local/game.iso") continue;
        if(!std::filesystem::exists(staging/file)) continue;
        std::filesystem::create_directories((root/file).parent_path());
        move(staging/file,root/file);
        report(4,unsigned(++committed*100/files.size()),"Finishing installation");
    }
    // Remove application files that this version no longer ships.
    for(const auto& file:previous_files) if(!recorded.contains(file)) {
        std::error_code error;std::filesystem::remove(root/file,error);
        for(auto parent=file.parent_path();!parent.empty();parent=parent.parent_path()) RemoveDirectoryW((root/parent).c_str());
    }
    if(registration) register_install(root,desktop,payload_bytes+uninstaller_bytes+xbox_native::target::file_size);
    report(4,100,"Ready to play. Your installation is complete.");
}
void uninstall(const std::filesystem::path& chosen) {
    auto root=folder_path(chosen);
    if(root==root.root_path() || linked(root)) throw std::runtime_error("Invalid installation folder.");
    root=std::filesystem::weakly_canonical(root);
    const auto path=root/"install-manifest.txt";
    if(!std::filesystem::exists(path)) throw std::runtime_error("This folder is not an installed copy of the game.");
    SetupLock lock(root);
    const auto recorded=installed_files(root);
    const auto entries=payload();std::vector<std::filesystem::path> files;
    for(const auto& file:recorded) {
        if(std::ranges::find(entries,file,&Entry::path)==entries.end() && file!="Uninstall.exe" && file!="data/local/default.xbe" && file!="data/local/game.iso")
            throw std::runtime_error("Uninstall manifest contains an unknown file. No files were removed.");
        if(linked((root/file).parent_path())) throw std::runtime_error("An uninstall path crosses a link or junction.");
        files.push_back(file);
    }
    for(const auto& file:files) if(std::filesystem::exists(root/file)) {
        const auto handle=CreateFileW((root/file).c_str(),GENERIC_WRITE|DELETE,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
        if(handle==INVALID_HANDLE_VALUE) throw std::runtime_error("Close the game and its diagnostics before uninstalling. No files were removed.");
        CloseHandle(handle);
    }
    unsigned count=0;
    for(const auto& file:files) {
        report(1,unsigned(count++*100/files.size()),"Removing "+text_of(file));
        std::error_code error;std::filesystem::remove(root/file,error);
        if(error) throw std::runtime_error("Close the game before uninstalling. Could not remove "+text_of(file));
    }
    std::filesystem::remove(path);
    const auto initialized=CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);
    if(SUCCEEDED(initialized)) {
        remove_registered_shortcut(known_folder(FOLDERID_Programs)/shortcut_name,root);
        remove_registered_shortcut(known_folder(FOLDERID_Desktop)/shortcut_name,root);CoUninitialize();
    }
    HKEY key=nullptr;
    if(RegOpenKeyExW(HKEY_CURRENT_USER,registry,0,KEY_QUERY_VALUE,&key)==ERROR_SUCCESS) {
        std::array<wchar_t,32768> location{};DWORD size=DWORD(sizeof(location));
        const auto status=RegGetValueW(key,nullptr,L"InstallLocation",RRF_RT_REG_SZ,nullptr,location.data(),&size);RegCloseKey(key);
        if(status==ERROR_SUCCESS && same_path(location.data(),root)) RegDeleteTreeW(HKEY_CURRENT_USER,registry);
    }
    // The Upscaled HUD cache beside the preferences is derived data the game
    // rebuilds on demand. Remove its files, never anything else in data\user.
    if(const auto cache=root/"data/user/hud-cache";std::filesystem::is_directory(cache) && !linked(cache)) {
        std::error_code error;
        for(const auto& item:std::filesystem::directory_iterator(cache,error)) {
            const auto extension=item.path().extension();
            if(item.is_regular_file(error) && (extension==".hud" || extension==".tmp")) std::filesystem::remove(item.path(),error);
        }
        RemoveDirectoryW(cache.c_str());
    }
    // Remove now-empty application folders, deepest first; saves keep data\user in place.
    std::set<std::filesystem::path,std::greater<>> folders;
    for(const auto& file:files) for(auto parent=file.parent_path();!parent.empty();parent=parent.parent_path()) folders.insert(parent);
    for(const auto& folder:folders) RemoveDirectoryW((root/folder).c_str());
    RemoveDirectoryW((root/"data").c_str());RemoveDirectoryW(root.c_str());
    report(1,100,"Uninstalled. Your saved games and preferences were kept.");
}
int scale(int value) {return MulDiv(value,int(dpi),96);}
struct Defaults {
    std::filesystem::path folder,iso;
    bool copy=true,desktop=false;
};
Defaults defaults() {
    Defaults result{known_folder(FOLDERID_LocalAppData)/L"Programs"/title,{},true,false};
    std::array<wchar_t,32768> previous{};DWORD size=DWORD(sizeof(previous));
    if(RegGetValueW(HKEY_CURRENT_USER,registry,L"InstallLocation",RRF_RT_REG_SZ,nullptr,previous.data(),&size)==ERROR_SUCCESS) {
        const std::filesystem::path path(previous.data());
        if(path.is_absolute() && std::filesystem::exists(path/"install-manifest.txt")) result.folder=path;
    }
    const auto manifest=result.folder/"install-manifest.txt";
    if(std::filesystem::exists(manifest) && std::filesystem::file_size(manifest)<256*1024) {
        std::ifstream input(manifest);std::string line;std::getline(input,line);
        if(line==marker) {
            result.copy=false;
            while(std::getline(input,line)) if(line=="data/local/game.iso" || line=="data\\local\\game.iso") result.copy=true;
            DWORD desktop=0;size=sizeof(desktop);
            if(RegGetValueW(HKEY_CURRENT_USER,registry,L"DesktopShortcut",RRF_RT_REG_DWORD,nullptr,&desktop,&size)==ERROR_SUCCESS) result.desktop=desktop!=0;
        }
    }
    if(std::filesystem::exists(result.folder/"data/local/game.iso") && result.copy) result.iso=result.folder/"data/local/game.iso";
    else {
        const auto settings=result.folder/"data/user/settings.cfg";
        if(std::filesystem::exists(settings) && std::filesystem::file_size(settings)<256*1024) {
            std::ifstream input(settings,std::ios::binary);std::string line;
            while(std::getline(input,line)) {
                if(line.ends_with('\r')) line.pop_back();
                if(line.starts_with("disc=")) result.iso=utf8_path(line.substr(5));
            }
        }
    }
    if(!result.iso.empty() && (!result.iso.is_absolute() || !std::filesystem::exists(result.iso))) result.iso.clear();
    result.folder.make_preferred();result.iso.make_preferred();return result;
}
void update_fonts() {
    for(const auto old:{font,heading,font_small}) if(old) DeleteObject(old);
    const auto make=[](int pixels,int weight=FW_NORMAL) {return CreateFontW(-scale(pixels),0,0,0,weight,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH,L"Segoe UI");};
    font=make(18);heading=make(26,FW_SEMIBOLD);font_small=make(15);
    HICON icon=nullptr;
    checked_hr(LoadIconWithScaleDown(GetModuleHandleW(nullptr),MAKEINTRESOURCEW(1),scale(56),scale(56),&icon),"Cannot load the installer icon");
    if(logo_icon) DestroyIcon(logo_icon);
    logo_icon=icon;
    for(const auto& entry:controls) SendMessageW(entry.first,WM_SETFONT,reinterpret_cast<WPARAM>(font),TRUE);
}
std::wstring text(HWND control) {
    const auto length=GetWindowTextLengthW(control);std::wstring value(std::size_t(length)+1,L'\0');
    GetWindowTextW(control,value.data(),length+1);value.resize(length);return value;
}
void enable_control(HWND handle,bool enabled) {if((IsWindowEnabled(handle)!=FALSE)!=enabled) EnableWindow(handle,enabled);}
void caption_control(HWND handle,const wchar_t* caption) {if(text(handle)!=caption) SetWindowTextW(handle,caption);}
bool paths_ready() {
    return uninstalling || (text(source_edit).find_first_not_of(L" \t\r\n")!=std::wstring::npos &&
        text(destination_edit).find_first_not_of(L" \t\r\n")!=std::wstring::npos);
}
void update_install_button() {
    if(!install_button) return;
    bool running,complete;
    {std::lock_guard lock(state.mutex);running=state.running;complete=state.complete;}
    enable_control(install_button,!running && (complete?!uninstalling:paths_ready()));
}
void invalidate_status(HWND handle) {
    ++status_invalidations;
    const RECT bounds{scale(status_panel.left),scale(status_panel.top),scale(status_panel.right),scale(status_panel.bottom)};
    InvalidateRect(handle,&bounds,FALSE);
}
HWND control(const wchar_t* type,const wchar_t* caption,DWORD style,int id,RECT rectangle) {
    const auto child=CreateWindowExW(0,type,caption,WS_CHILD|WS_VISIBLE|style,0,0,0,0,window,reinterpret_cast<HMENU>(INT_PTR(id)),GetModuleHandleW(nullptr),nullptr);
    checked(child!=nullptr,"Cannot create installer controls");SendMessageW(child,WM_SETFONT,reinterpret_cast<WPARAM>(font),TRUE);
    controls.emplace_back(child,rectangle);return child;
}
HWND checkbox(const wchar_t* caption,int id,RECT row) {
    InflateRect(&row,-checkbox_inset_x,-checkbox_inset_y);
    const auto child=control(L"BUTTON",caption,BS_AUTOCHECKBOX|WS_TABSTOP,id,row);
    SetWindowTheme(child,L"",L"");return child;
}
void position_controls() {
    for(const auto& [child,r]:controls) MoveWindow(child,scale(r.left),scale(r.top),scale(r.right-r.left),scale(r.bottom-r.top),TRUE);
}
void choose(bool folder) {
    ComPtr<IFileOpenDialog> dialog;checked_hr(CoCreateInstance(CLSID_FileOpenDialog,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&dialog)),"Cannot open the file picker");
    DWORD options=0;dialog->GetOptions(&options);
    dialog->SetOptions(options|FOS_FORCEFILESYSTEM|(folder?FOS_PICKFOLDERS:FOS_FILEMUSTEXIST));
    dialog->SetTitle(folder?L"Choose an installation folder":L"Select your Scooby-Doo! Mystery Mayhem disc image");
    const COMDLG_FILTERSPEC filter[]={{L"Xbox disc images (*.iso; *.xiso)",L"*.iso;*.xiso"},{L"All files",L"*.*"}};
    if(!folder) dialog->SetFileTypes(2,filter);
    const auto result=dialog->Show(window);if(result==HRESULT_FROM_WIN32(ERROR_CANCELLED)) return;checked_hr(result,"Cannot select a path");
    ComPtr<IShellItem> item;checked_hr(dialog->GetResult(&item),"Cannot read the selected path");
    PWSTR value=nullptr;checked_hr(item->GetDisplayName(SIGDN_FILESYSPATH,&value),"Cannot read the selected path");
    SetWindowTextW(folder?destination_edit:source_edit,value);CoTaskMemFree(value);
}
void start() {
    if(state.worker.joinable()) state.worker.join();
    const auto iso=text(source_edit);
    const auto destination=uninstalling?uninstall_root:std::filesystem::path(text(destination_edit));
    const bool copy=SendMessageW(copy_check,BM_GETCHECK,0,0)==BST_CHECKED,desktop=SendMessageW(desktop_check,BM_GETCHECK,0,0)==BST_CHECKED;
    if(!paths_ready()) throw std::runtime_error("Choose your game image and installation folder first.");
    {std::lock_guard lock(state.mutex);state.running=true;state.complete=false;state.error.clear();state.stage=0;state.percent=0;++state.revision;}
    state.cancel=false;started=GetTickCount64();
    for(const auto input:inputs) EnableWindow(input,FALSE);
    EnableWindow(install_button,FALSE);SetWindowTextW(close_button,L"Cancel");
    state.worker=std::jthread([iso,destination,copy,desktop] {
        try {
            if(uninstalling) uninstall(destination);else install(iso,destination,copy,desktop,true);
            std::lock_guard lock(state.mutex);state.complete=true;++state.revision;
        } catch(const std::exception& error) {
            std::lock_guard lock(state.mutex);state.error=wide(error.what());state.phase=L"Setup did not finish. You can correct the problem and try again.";++state.revision;
        }
        std::lock_guard lock(state.mutex);state.running=false;++state.revision;
    });
}
// A night-sky gradient with a faint moon glow; panels show a dimmer copy.
void make_backdrop() {
    backdrop.resize(std::size_t(backdrop_width)*backdrop_height);backdrop_inset.resize(backdrop.size());
    for(int y=0;y<backdrop_height;++y) for(int x=0;x<backdrop_width;++x) {
        const double t=double(y)/(backdrop_height-1),dx=(x-430)/150.0,dy=(y-30)/110.0;
        const double glow=std::pow(std::max(0.0,1-std::sqrt(dx*dx+dy*dy)),2)*0.55;
        const auto channel=[&](double top,double bottom,double moon) {return std::clamp(top+(bottom-top)*t+(moon-top)*glow,0.0,255.0);};
        const double r=channel(18,6,96),g=channel(46,15,132),b=channel(56,21,128);
        const auto index=std::size_t(y)*backdrop_width+x;
        backdrop[index]=(unsigned(r)<<16)|(unsigned(g)<<8)|unsigned(b);
        backdrop_inset[index]=(unsigned(r*0.62+4)<<16)|(unsigned(g*0.66+6)<<8)|unsigned(b*0.68+7);
    }
}
void draw_text(HDC dc,std::wstring value,RECT rectangle,COLORREF color,HFONT selected,UINT flags=DT_LEFT|DT_WORDBREAK) {
    for(auto* p:{&rectangle.left,&rectangle.top,&rectangle.right,&rectangle.bottom}) *p=scale(*p);
    const auto old=SelectObject(dc,selected);SetTextColor(dc,color);SetBkMode(dc,TRANSPARENT);
    DrawTextW(dc,value.c_str(),int(value.size()),&rectangle,flags);SelectObject(dc,old);
}
void paint(HDC dc) {
    SetStretchBltMode(dc,HALFTONE);SetBrushOrgEx(dc,0,0,nullptr);
    BITMAPINFO bitmap{};bitmap.bmiHeader={sizeof(BITMAPINFOHEADER),backdrop_width,-backdrop_height,1,32,BI_RGB,0,0,0,0,0};
    StretchDIBits(dc,0,0,scale(client_width),scale(client_height),0,0,backdrop_width,backdrop_height,backdrop.data(),&bitmap,DIB_RGB_COLORS,SRCCOPY);
    auto pen=CreatePen(PS_SOLID,scale(2),border);const auto old_pen=SelectObject(dc,pen);const auto old_brush=SelectObject(dc,GetStockObject(NULL_BRUSH));
    for(const auto r:{form_panel,status_panel}) {
        const auto saved=SaveDC(dc);const auto clip=CreateRoundRectRgn(scale(r.left),scale(r.top),scale(r.right),scale(r.bottom),scale(14),scale(14));
        ExtSelectClipRgn(dc,clip,RGN_AND);
        StretchDIBits(dc,scale(r.left),scale(r.top),scale(r.right-r.left),scale(r.bottom-r.top),0,0,backdrop_width,backdrop_height,backdrop_inset.data(),&bitmap,DIB_RGB_COLORS,SRCCOPY);
        RestoreDC(dc,saved);DeleteObject(clip);RoundRect(dc,scale(r.left),scale(r.top),scale(r.right),scale(r.bottom),scale(14),scale(14));
    }
    SelectObject(dc,old_brush);SelectObject(dc,old_pen);DeleteObject(pen);
    if(!uninstalling) for(const auto row:checkbox_rows) {
        const RECT background{scale(row.left),scale(row.top),scale(row.right),scale(row.bottom)};
        FillRect(dc,&background,edit_brush);
    }
    DrawIconEx(dc,scale(28),scale(28),logo_icon,scale(56),scale(56),0,nullptr,DI_NORMAL);
    draw_text(dc,uninstalling?L"Uninstallation":L"Installation",{100,28,850,65},gold,heading);
    draw_text(dc,L"v"+wide(SCOOBYMM_VERSION)+L" · Windows x64",{640,34,840,60},gold,font_small,DT_RIGHT|DT_SINGLELINE);
    draw_text(dc,uninstalling?L"Uninstall — your saved games and preferences will be kept.":L"Select your copy of Scooby-Doo! Mystery Mayhem and get ready to solve some mysteries!",{100,70,850,106},cream,font_small);
    if(!uninstalling) {
        draw_text(dc,L"YOUR GAME IMAGE",{form_left,140,form_right,162},gold,font_small);
        draw_text(dc,L"Use the USA/Europe Xbox XISO image. Its game executable is verified before installation.",{form_left,218,form_right,242},cream,font_small);
        draw_text(dc,L"INSTALLATION FOLDER",{form_left,254,form_right,276},gold,font_small);
    } else draw_text(dc,L"Remove the installed program and its copied game files. Your original disc image, saved games, preferences and session reports are kept.",{40,140,824,215},cream,font);
    std::wstring phase,error;unsigned percent,stage;bool running,complete;
    {std::lock_guard lock(state.mutex);phase=state.phase;error=state.error;percent=state.percent;stage=state.stage;running=state.running;complete=state.complete;}
    const auto caption=running?std::format(L"STEP {} OF {}  ·  {}%",stage,uninstalling?1:4,percent):complete?L"COMPLETE":L"INSTALLATION PROGRESS";
    draw_text(dc,caption,{status_left,status_top+12,status_right,status_top+36},gold,font_small);
    draw_text(dc,phase,{status_left,status_top+43,status_right,status_top+77},cream,font);
    if(!error.empty()) draw_text(dc,error,{status_left,status_top+112,status_right,status_top+178},RGB(255,170,140),font_small);
    else if(running) draw_text(dc,std::format(L"Elapsed: {}:{:02}  ·  You can leave this window open while setup works.",(GetTickCount64()-started)/60000,((GetTickCount64()-started)/1000)%60),{status_left,status_top+112,status_right,status_top+152},cream,font_small);
    else draw_text(dc,uninstalling?L"Only files recorded by this installer are removed.":L"Your original game image and saved games are kept.",{status_left,status_top+112,status_right,status_top+152},cream,font_small);
}
LRESULT CALLBACK procedure(HWND handle,UINT message,WPARAM wparam,LPARAM lparam) {
    try {
        if(message==WM_PAINT) {PAINTSTRUCT ps{};const auto dc=BeginPaint(handle,&ps);paint(dc);EndPaint(handle,&ps);return 0;}
        if(message==WM_PRINTCLIENT) {paint(reinterpret_cast<HDC>(wparam));return 0;}
        if(message==WM_ERASEBKGND) return 1;
        if(message==WM_CTLCOLORSTATIC || message==WM_CTLCOLOREDIT) {
            const auto dc=reinterpret_cast<HDC>(wparam);SetTextColor(dc,cream);SetBkColor(dc,ink);return reinterpret_cast<LRESULT>(edit_brush);
        }
        if(message==WM_DRAWITEM) {
            const auto& item=*reinterpret_cast<DRAWITEMSTRUCT*>(lparam);const bool primary=item.hwndItem==install_button;
            const auto saved=SaveDC(item.hDC);
            DrawThemeParentBackground(item.hwndItem,item.hDC,&item.rcItem);
            RestoreDC(item.hDC,saved);
            const bool disabled=(item.itemState&ODS_DISABLED)!=0,pressed=(item.itemState&ODS_SELECTED)!=0;
            const auto brush=CreateSolidBrush(primary?(disabled?RGB(128,110,58):pressed?RGB(226,178,40):gold):pressed?RGB(9,62,68):teal);
            const auto old=SelectObject(item.hDC,brush);const auto pen=CreatePen(PS_SOLID,1,border);const auto old_pen=SelectObject(item.hDC,pen);
            RoundRect(item.hDC,item.rcItem.left,item.rcItem.top,item.rcItem.right,item.rcItem.bottom,scale(8),scale(8));
            SelectObject(item.hDC,old);SelectObject(item.hDC,old_pen);DeleteObject(brush);DeleteObject(pen);
            const auto old_font=SelectObject(item.hDC,font);SetBkMode(item.hDC,TRANSPARENT);SetTextColor(item.hDC,primary?RGB(24,26,20):cream);
            auto bounds=item.rcItem;const auto value=text(item.hwndItem);DrawTextW(item.hDC,value.c_str(),int(value.size()),&bounds,DT_CENTER|DT_VCENTER|DT_SINGLELINE);
            if(item.itemState&ODS_FOCUS) {InflateRect(&bounds,-scale(4),-scale(4));DrawFocusRect(item.hDC,&bounds);}
            SelectObject(item.hDC,old_font);return TRUE;
        }
        if(message==WM_COMMAND) {
            switch(LOWORD(wparam)) {
            case 101:choose(false);break;case 102:choose(true);break;
            case 110:case 111:if(HIWORD(wparam)==EN_CHANGE) update_install_button();break;
            case 103: {
                bool complete;{std::lock_guard lock(state.mutex);complete=state.complete;}
                if(complete && !uninstalling) {
                    const auto root=std::filesystem::weakly_canonical(folder_path(text(destination_edit)));
                    checked(reinterpret_cast<INT_PTR>(ShellExecuteW(window,L"open",(root/"scoobymm.exe").c_str(),nullptr,root.c_str(),SW_SHOWNORMAL))>32,"Cannot launch the game");
                } else start();
                break;
            }
            case 104:SendMessageW(handle,WM_CLOSE,0,0);break;
            }
            return 0;
        }
        if(message==WM_TIMER) {
            bool running,complete;unsigned percent,stage;
            std::uint64_t revision;
            {std::lock_guard lock(state.mutex);running=state.running;complete=state.complete;percent=state.percent;stage=state.stage;revision=state.revision;}
            static std::uint64_t seen_revision=0,seen_second=0;
            const auto second=running?(GetTickCount64()-started)/1000:0;
            if(revision==seen_revision && second==seen_second) return 0;
            seen_revision=revision;seen_second=second;
            if(SendMessageW(progress_bar,PBM_GETPOS,0,0)!=LRESULT(percent)) SendMessageW(progress_bar,PBM_SETPOS,percent,0);
            if(!running) {
                enable_control(close_button,true);
                for(const auto input:inputs) enable_control(input,!complete);
                update_install_button();caption_control(install_button,complete&&!uninstalling?L"Play game":uninstalling?L"Uninstall":L"Install");
                caption_control(close_button,L"Close");
            } else enable_control(close_button,!uninstalling && stage<4 && !state.cancel.load());  // Uninstall cannot be cancelled.
            invalidate_status(handle);return 0;
        }
        if(message==WM_DPICHANGED) {
            dpi=HIWORD(wparam);update_fonts();const auto& rectangle=*reinterpret_cast<RECT*>(lparam);
            SetWindowPos(handle,nullptr,rectangle.left,rectangle.top,rectangle.right-rectangle.left,rectangle.bottom-rectangle.top,SWP_NOZORDER);position_controls();return 0;
        }
        if(message==WM_CLOSE) {
            bool running;{std::lock_guard lock(state.mutex);running=state.running;}
            if(running && uninstalling) return 0;
            if(running) {state.cancel=true;SetWindowTextW(close_button,L"Cancelling…");EnableWindow(close_button,FALSE);return 0;}
            DestroyWindow(handle);return 0;
        }
        if(message==WM_DESTROY) {PostQuitMessage(0);return 0;}
    } catch(const std::exception& error) {std::lock_guard lock(state.mutex);state.error=wide(error.what());++state.revision;invalidate_status(handle);return 0;}
    return DefWindowProcW(handle,message,wparam,lparam);
}
// --preview writes the composed window to a bitmap and a JSON summary, so the
// layout can be checked at any DPI without showing a window.
void capture_preview(const std::filesystem::path& path) {
    bool idle_repaint=false;std::uint64_t idle_invalidations=0;
    if(!state.running) {
        SendMessageW(window,WM_TIMER,1,0);
        RedrawWindow(window,nullptr,nullptr,RDW_VALIDATE|RDW_ALLCHILDREN|RDW_NOERASE|RDW_NOINTERNALPAINT);
        const auto before=status_invalidations;
        for(unsigned i=0;i<10;++i) SendMessageW(window,WM_TIMER,1,0);
        idle_invalidations=status_invalidations-before;
        idle_repaint=GetUpdateRect(window,nullptr,FALSE)!=FALSE;
        if(idle_invalidations) throw std::runtime_error("Idle installer requested an unnecessary repaint");
    }
    const int width=scale(client_width),height=scale(client_height);const auto dc=GetDC(window),memory=CreateCompatibleDC(dc);const auto bitmap=CreateCompatibleBitmap(dc,width,height);const auto old=SelectObject(memory,bitmap);
    paint(memory);
    for(const auto& [child,r]:controls) if(IsWindowVisible(child) || !uninstalling) {
        const auto saved=SaveDC(memory);SetViewportOrgEx(memory,scale(r.left),scale(r.top),nullptr);
        IntersectClipRect(memory,0,0,scale(r.right-r.left),scale(r.bottom-r.top));
        SendMessageW(child,WM_PRINT,reinterpret_cast<WPARAM>(memory),PRF_CLIENT|PRF_NONCLIENT|PRF_ERASEBKGND);
        RestoreDC(memory,saved);
    }
    SelectObject(memory,old);
    BITMAPINFO info{};info.bmiHeader={sizeof(BITMAPINFOHEADER),width,height,1,32,BI_RGB,0,0,0,0,0};std::vector<std::byte> pixels(std::size_t(width)*height*4);
    checked(GetDIBits(memory,bitmap,0,height,pixels.data(),&info,DIB_RGB_COLORS)!=0,"Cannot capture the installer preview");
    BITMAPFILEHEADER file{0x4D42,DWORD(sizeof(BITMAPFILEHEADER)+sizeof(BITMAPINFOHEADER)+pixels.size()),0,0,DWORD(sizeof(BITMAPFILEHEADER)+sizeof(BITMAPINFOHEADER))};
    std::ofstream output(path,std::ios::binary);output.exceptions(std::ios::failbit|std::ios::badbit);
    output.write(reinterpret_cast<const char*>(&file),sizeof(file));output.write(reinterpret_cast<const char*>(&info.bmiHeader),sizeof(info.bmiHeader));output.write(reinterpret_cast<const char*>(pixels.data()),std::streamsize(pixels.size()));
    DeleteObject(bitmap);DeleteDC(memory);ReleaseDC(window,dc);
    ICONINFO icon{};checked(GetIconInfo(logo_icon,&icon)!=FALSE,"Cannot inspect the installer icon");
    BITMAP image{};checked(GetObjectW(icon.hbmColor,sizeof(image),&image)!=0,"Cannot inspect icon dimensions");
    DeleteObject(icon.hbmColor);DeleteObject(icon.hbmMask);
    std::ofstream summary(path.native()+L".json",std::ios::binary);summary.exceptions(std::ios::failbit|std::ios::badbit);
    summary<<std::format("{{\"format\":\"scoobymm-installer-preview-v1\",\"render_dpi\":{},\"size\":[{},{}],\"icon_size\":[{},{}],\"composited\":{},\"clips_children\":{},\"idle_timer_invalidations\":{},\"pending_window_paint\":{},\"install_enabled\":{}}}\n",
        dpi,width,height,image.bmWidth,image.bmHeight,(GetWindowLongPtrW(window,GWL_EXSTYLE)&WS_EX_COMPOSITED)!=0,
        (GetWindowLongPtrW(window,GWL_STYLE)&WS_CLIPCHILDREN)!=0,idle_invalidations,idle_repaint,IsWindowEnabled(install_button)!=FALSE);
}
int wizard(const std::filesystem::path& preview={}) {
    checked_hr(CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED),"Cannot initialize Setup");
    const INITCOMMONCONTROLSEX common{sizeof(common),ICC_PROGRESS_CLASS|ICC_STANDARD_CLASSES};InitCommonControlsEx(&common);
    dpi=preview_dpi?preview_dpi:GetDpiForSystem();update_fonts();
    edit_brush=CreateSolidBrush(ink);make_backdrop();
    WNDCLASSW cls{};cls.lpfnWndProc=procedure;cls.hInstance=GetModuleHandleW(nullptr);cls.hCursor=LoadCursorW(nullptr,IDC_ARROW);cls.hIcon=LoadIconW(cls.hInstance,MAKEINTRESOURCEW(1));cls.lpszClassName=L"ScoobyMMInstaller";
    checked(RegisterClassW(&cls)!=0,"Cannot register the installer window");
    RECT bounds{0,0,scale(client_width),scale(client_height)};AdjustWindowRectExForDpi(&bounds,WS_OVERLAPPED|WS_CAPTION|WS_SYSMENU|WS_MINIMIZEBOX,FALSE,0,dpi);
    window=CreateWindowExW(WS_EX_CONTROLPARENT|WS_EX_COMPOSITED,cls.lpszClassName,uninstalling?L"Scooby-Doo! Mystery Mayhem — Uninstall":L"Scooby-Doo! Mystery Mayhem — Setup",WS_OVERLAPPED|WS_CAPTION|WS_SYSMENU|WS_MINIMIZEBOX|WS_CLIPCHILDREN,CW_USEDEFAULT,CW_USEDEFAULT,bounds.right-bounds.left,bounds.bottom-bounds.top,nullptr,nullptr,cls.hInstance,nullptr);
    checked(window!=nullptr,"Cannot open the installer");
    const auto initial=uninstalling?Defaults{uninstall_root,{},true,false}:defaults();
    source_edit=control(L"EDIT",initial.iso.c_str(),ES_AUTOHSCROLL|WS_BORDER|WS_TABSTOP,110,{form_left,172,form_right-116,206});
    const auto browse_iso=control(L"BUTTON",L"Browse…",BS_OWNERDRAW|WS_TABSTOP,101,{form_right-104,172,form_right,206});
    destination_edit=control(L"EDIT",initial.folder.c_str(),ES_AUTOHSCROLL|WS_BORDER|WS_TABSTOP,111,{form_left,286,form_right-116,320});
    const auto browse_folder=control(L"BUTTON",L"Browse…",BS_OWNERDRAW|WS_TABSTOP,102,{form_right-104,286,form_right,320});
    copy_check=checkbox(L"Keep a copy of my disc image with the game (uses about 3.6 GB more disk space)",112,checkbox_rows[0]);SendMessageW(copy_check,BM_SETCHECK,initial.copy?BST_CHECKED:BST_UNCHECKED,0);
    desktop_check=checkbox(L"Create a desktop shortcut",113,checkbox_rows[1]);SendMessageW(desktop_check,BM_SETCHECK,initial.desktop?BST_CHECKED:BST_UNCHECKED,0);
    progress_bar=control(PROGRESS_CLASSW,L"",PBS_SMOOTH,114,{status_left,status_top+84,status_right,status_top+104});SetWindowTheme(progress_bar,L"",L"");
    SendMessageW(progress_bar,PBM_SETRANGE32,0,100);SendMessageW(progress_bar,PBM_SETBARCOLOR,0,gold);SendMessageW(progress_bar,PBM_SETBKCOLOR,0,ink);
    install_button=control(L"BUTTON",uninstalling?L"Uninstall":L"Install",BS_OWNERDRAW|WS_TABSTOP|WS_DISABLED,103,{620,buttons_top,748,buttons_top+38});
    close_button=control(L"BUTTON",L"Close",BS_OWNERDRAW|WS_TABSTOP,104,{760,buttons_top,848,buttons_top+38});
    inputs={source_edit,browse_iso,destination_edit,browse_folder,copy_check,desktop_check};position_controls();update_install_button();
    if(uninstalling) for(const auto input:inputs) ShowWindow(input,SW_HIDE);
    SetTimer(window,1,100,nullptr);
    if(!preview.empty()) {SendMessageW(progress_bar,PBM_SETPOS,state.percent,0);capture_preview(preview);DestroyWindow(window);return 0;}
    ShowWindow(window,SW_SHOWNORMAL);UpdateWindow(window);SetFocus(uninstalling?install_button:source_edit);
    MSG message{};while(GetMessageW(&message,nullptr,0,0)>0) if(!IsDialogMessageW(window,&message)) {TranslateMessage(&message);DispatchMessageW(&message);}
    if(state.worker.joinable()) state.worker.join();
    DestroyIcon(logo_icon);DeleteObject(edit_brush);DeleteObject(font);DeleteObject(heading);DeleteObject(font_small);CoUninitialize();return 0;
}
}
int wmain(int argc,wchar_t** argv) {
    bool interactive=argc==1;
    try {
        if((argc==3 || (argc==5 && std::wstring_view(argv[3])==L"--dpi")) && (std::wstring_view(argv[1])==L"--preview" || std::wstring_view(argv[1])==L"--preview-progress")) {
            if(argc==5) {preview_dpi=unsigned(std::stoul(argv[4]));if(preview_dpi<96 || preview_dpi>384) throw std::runtime_error("Preview DPI must be 96..384");}
            if(std::wstring_view(argv[1])==L"--preview-progress") {state.running=true;state.stage=3;state.percent=47;state.phase=L"Copying your game image";started=GetTickCount64();}
            return wizard(argv[2]);
        }
        if(argc>=4 && std::wstring_view(argv[1])==L"--silent-install") {
            bool copy=true,registration=true;
            for(int i=4;i<argc;++i) {
                const auto option=std::wstring_view(argv[i]);
                if(option==L"--reference-disc") copy=false;
                else if(option==L"--no-register") registration=false;
                else if(option==L"--cancel-after-percent" && i+1<argc) {
                    cancellation_percent=unsigned(std::stoul(argv[++i]));
                    if(cancellation_percent>100) throw std::runtime_error("Cancellation percentage must be 0..100");
                } else throw std::runtime_error("Unknown installer option");
            }
            install(argv[2],argv[3],copy,std::nullopt,registration);return 0;
        }
        std::vector<std::wstring> arguments(argv+1,argv+argc);
        // Opening the installed Uninstall.exe directly starts uninstall, not a second installation.
        const auto self=module_path();
        if(arguments.empty() && same_path(self.filename(),L"Uninstall.exe") && std::filesystem::exists(self.parent_path()/"install-manifest.txt"))
            arguments={L"--uninstall",self.parent_path().native()};
        if(arguments.size()>=2 && arguments[0]==L"--uninstall") {
            uninstall_root=folder_path(arguments[1]);
            bool silent=false;DWORD parent_id=0;
            for(std::size_t i=2;i<arguments.size();++i) {
                if(arguments[i]==L"--silent") silent=true;
                else if(arguments[i]==L"--wait" && i+1<arguments.size()) parent_id=DWORD(std::stoul(arguments[++i]));
                else throw std::runtime_error("Unknown uninstaller option");
            }
            interactive=!silent;
            remove_stale_uninstallers();
            if(same_path(self.parent_path(),uninstall_root)) {
                // Windows cannot delete a running program: a temporary copy removes this one.
                // A silent request therefore returns once that copy has started.
                const auto temporary=std::filesystem::temp_directory_path()/std::format("scoobymm-uninstall-{}.exe",GetCurrentProcessId());
                checked(CopyFileW(self.c_str(),temporary.c_str(),FALSE)!=FALSE,"Cannot start the uninstaller");
                auto parameters=L"--uninstall \""+uninstall_root.native()+L"\" --wait "+std::to_wstring(GetCurrentProcessId());
                if(silent) parameters+=L" --silent";
                checked(reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr,L"open",temporary.c_str(),parameters.c_str(),nullptr,silent?SW_HIDE:SW_SHOWNORMAL))>32,"Cannot start the uninstaller");
                return 0;
            }
            if(parent_id) {
                const auto parent=OpenProcess(SYNCHRONIZE,FALSE,parent_id);if(parent) {WaitForSingleObject(parent,10000);CloseHandle(parent);}
            }
            if(silent) {uninstall(uninstall_root);return 0;}
            uninstalling=true;
        } else if(!arguments.empty()) throw std::runtime_error("Unknown installer arguments");
        return wizard();
    } catch(const std::exception& error) {
        if(interactive) MessageBoxW(nullptr,wide(error.what()).c_str(),title,MB_OK|MB_ICONERROR);
        else std::fputws((wide(error.what())+L"\n").c_str(),stderr);
        return 1;
    }
}
