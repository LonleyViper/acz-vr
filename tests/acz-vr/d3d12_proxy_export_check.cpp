// J-3b4 build-time check: the proxy d3d12.dll export table must equal the
// system d3d12.dll export table (ordinal + name, NONAME included). Every proxy
// export except D3D12CreateDevice must be a forwarder to the same name (or
// ordinal) in C:\Windows\System32\d3d12; D3D12CreateDevice must be code.
// Reads both files as bytes (no LoadLibrary, nothing executes).
// Usage: d3d12_proxy_export_check <system d3d12.dll> <proxy d3d12.dll>
#include <windows.h>
#include <algorithm>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <map>
#include <string>
#include <vector>

namespace {
struct Export {std::string name;std::string forwarder;bool code=false;};
using Table=std::map<unsigned,Export>; // ordinal -> export
bool fail(const char* why,const char* path){std::fprintf(stderr,"%s: %s\n",path,why);return false;}
bool read(const char* path,Table& out) {
    std::ifstream f(path,std::ios::binary);
    if(!f)return fail("cannot open",path);
    std::vector<char> b((std::istreambuf_iterator<char>(f)),std::istreambuf_iterator<char>());
    auto at=[&](size_t off,size_t n){return off<=b.size()&&n<=b.size()-off;};
    if(!at(0,sizeof(IMAGE_DOS_HEADER)))return fail("short file",path);
    const auto* dos=reinterpret_cast<const IMAGE_DOS_HEADER*>(b.data());
    if(dos->e_magic!=IMAGE_DOS_SIGNATURE||!at(size_t(dos->e_lfanew),sizeof(IMAGE_NT_HEADERS64)))return fail("not PE",path);
    const auto* nt=reinterpret_cast<const IMAGE_NT_HEADERS64*>(b.data()+dos->e_lfanew);
    if(nt->Signature!=IMAGE_NT_SIGNATURE||nt->OptionalHeader.Magic!=IMAGE_NT_OPTIONAL_HDR64_MAGIC)return fail("not PE32+",path);
    const auto dir=nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
    if(!dir.VirtualAddress)return fail("no export directory",path);
    const auto* sec=IMAGE_FIRST_SECTION(nt);
    const unsigned sections=nt->FileHeader.NumberOfSections;
    auto file=[&](DWORD rva,size_t n)->const char* {
        for(unsigned i=0;i<sections;++i) {
            const auto& s=sec[i];const DWORD size=(std::max)(s.Misc.VirtualSize,s.SizeOfRawData);
            if(rva>=s.VirtualAddress&&rva<s.VirtualAddress+size) {
                const size_t off=size_t(s.PointerToRawData)+(rva-s.VirtualAddress);
                return at(off,n)?b.data()+off:nullptr;
            }
        }
        return nullptr;
    };
    auto str=[&](DWORD rva)->std::string {const char* p=file(rva,1);if(!p)return {};
        const char* end=b.data()+b.size();const char* q=p;while(q<end&&*q)++q;return std::string(p,q);};
    const auto* ed=reinterpret_cast<const IMAGE_EXPORT_DIRECTORY*>(file(dir.VirtualAddress,sizeof(IMAGE_EXPORT_DIRECTORY)));
    if(!ed)return fail("bad export directory",path);
    const auto* functions=reinterpret_cast<const DWORD*>(file(ed->AddressOfFunctions,ed->NumberOfFunctions*4ull));
    const auto* names=reinterpret_cast<const DWORD*>(file(ed->AddressOfNames,ed->NumberOfNames*4ull));
    const auto* ordinals=reinterpret_cast<const WORD*>(file(ed->AddressOfNameOrdinals,ed->NumberOfNames*2ull));
    if(!functions||(ed->NumberOfNames&&(!names||!ordinals)))return fail("bad export arrays",path);
    for(DWORD i=0;i<ed->NumberOfFunctions;++i) {
        if(!functions[i])continue; // unused ordinal slot
        Export e;
        if(functions[i]>=dir.VirtualAddress&&functions[i]<dir.VirtualAddress+dir.Size)e.forwarder=str(functions[i]);
        else e.code=true;
        out[ed->Base+i]=e;
    }
    for(DWORD i=0;i<ed->NumberOfNames;++i) {
        const unsigned ordinal=ed->Base+ordinals[i];
        auto it=out.find(ordinal);if(it==out.end())return fail("name without function",path);
        if(!it->second.name.empty())return fail("ordinal with two names (unsupported)",path);
        it->second.name=str(names[i]);
    }
    return true;
}
std::string lower(std::string s){for(auto& c:s)c=char(tolower(static_cast<unsigned char>(c)));return s;}
}
int main(int argc,char** argv) {
    if(argc!=3){std::fprintf(stderr,"usage: d3d12_proxy_export_check <system d3d12.dll> <proxy d3d12.dll>\n");return 2;}
    Table system,proxy;
    if(!read(argv[1],system)||!read(argv[2],proxy))return 1;
    bool ok=system.size()==proxy.size();
    if(!ok)std::fprintf(stderr,"export count differs: system %zu proxy %zu\n",system.size(),proxy.size());
    bool sawCreate=false;
    for(const auto& [ordinal,s]:system) {
        auto it=proxy.find(ordinal);
        const char* label=s.name.empty()?"[NONAME]":s.name.c_str();
        if(it==proxy.end()){std::fprintf(stderr,"missing ordinal %u %s\n",ordinal,label);ok=false;continue;}
        const auto& p=it->second;
        if(p.name!=s.name){std::fprintf(stderr,"ordinal %u name differs: system '%s' proxy '%s'\n",ordinal,s.name.c_str(),p.name.c_str());ok=false;}
        if(s.name=="D3D12CreateDevice") {
            sawCreate=true;
            if(!p.code){std::fprintf(stderr,"D3D12CreateDevice must be the proxy wrapper, not a forwarder\n");ok=false;}
        } else {
            const std::string expected="c:\\windows\\system32\\d3d12."+(s.name.empty()?"#"+std::to_string(ordinal):s.name);
            if(p.code||lower(p.forwarder)!=lower(expected)) {
                std::fprintf(stderr,"ordinal %u %s: forwarder '%s', expected '%s'\n",ordinal,label,p.forwarder.c_str(),expected.c_str());ok=false;
            }
        }
        std::printf("%u %s -> %s\n",ordinal,label,p.code?"proxy code":p.forwarder.c_str());
    }
    for(const auto& [ordinal,p]:proxy)if(!system.count(ordinal)){std::fprintf(stderr,"extra proxy ordinal %u %s\n",ordinal,p.name.c_str());ok=false;}
    if(!sawCreate){std::fprintf(stderr,"system table has no D3D12CreateDevice\n");ok=false;}
    std::printf("d3d12 proxy export check: %s (%zu system exports)\n",ok?"MATCH":"MISMATCH",system.size());
    return ok?0:1;
}
