// Real feature_log queue and drain against controllable offline sinks. No GPU/XR.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "feature_log.h"
#include <atomic>
#include <cstdlib>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>

namespace feature_log_fixture {
namespace fl=acz::feature_log;
inline void require(bool ok,const char* message) {
    if(!ok){std::cerr<<"FAIL "<<message<<'\n';std::exit(1);}
}
struct Sink {
    HANDLE entered=CreateEventW(nullptr,TRUE,FALSE,nullptr);
    HANDLE release=CreateEventW(nullptr,TRUE,FALSE,nullptr);
    std::atomic<bool> blocked{false},fail{false},waiting{false};
    std::atomic<DWORD> piece{MAXDWORD};
    std::atomic<unsigned> calls{0};
    std::mutex mutex;std::string content;
    ~Sink(){CloseHandle(entered);CloseHandle(release);}
    static bool put(void* context,HANDLE,const char* bytes,DWORD count,DWORD* written) noexcept {
        auto& s=*static_cast<Sink*>(context);++s.calls;
        if(s.blocked.load()) {
            s.waiting=true;SetEvent(s.entered);
            const DWORD result=WaitForSingleObject(s.release,10000);s.waiting=false;
            if(result!=WAIT_OBJECT_0){*written=0;return false;}
        } else SetEvent(s.entered);
        if(s.fail.load()){*written=0;return false;}
        const DWORD piece=s.piece.load();*written=count<piece?count:piece;
        std::lock_guard<std::mutex> lock(s.mutex);s.content.append(bytes,*written);return true;
    }
    std::string text(){std::lock_guard<std::mutex> lock(mutex);return content;}
};
inline void setup() {
    wchar_t tmp[MAX_PATH]{};GetTempPathW(MAX_PATH,tmp);
    const std::wstring dir=std::wstring(tmp)+L"acz-feature-log-"+std::to_wstring(GetCurrentProcessId());
    require(CreateDirectoryW(dir.c_str(),nullptr)||GetLastError()==ERROR_ALREADY_EXISTS,"fixture directory");
    fl::set_directory(dir.c_str());
}
inline void slow() {
    setup();const auto h=fl::open("slow");require(h>=0,"open slow fixture");
    Sink sink;sink.blocked=true;fl::set_sink(&Sink::put,&sink);
    fl::write(h,"first");std::thread drain([]{fl::flush();});
    const bool entered=WaitForSingleObject(sink.entered,2000)==WAIT_OBJECT_0;
    HANDLE progressed=CreateEventW(nullptr,TRUE,FALSE,nullptr);
    std::thread producer([&]{for(int i=0;i<32;++i)fl::write(h,"while-blocked");
        (void)fl::stats(h);SetEvent(progressed);});
    const bool progress=WaitForSingleObject(progressed,500)==WAIT_OBJECT_0;
    const bool stillBlocked=sink.waiting.load()&&WaitForSingleObject(sink.release,0)==WAIT_TIMEOUT;
    SetEvent(sink.release);producer.join();drain.join();CloseHandle(progressed);
    require(entered&&progress&&stillBlocked,"producer and stats finish while sink is still blocked");
    require(sink.text().find("while-blocked")==std::string::npos,"detached batch excludes later arrivals");
    fl::flush();require(sink.text().find("while-blocked")!=std::string::npos,"next drain writes later arrivals");
    require(!fl::stats(h).incomplete,"successful slow drain complete");fl::set_sink(nullptr);
}
inline void failing() {
    setup();const auto h=fl::open("failing");require(h>=0,"open failing fixture");
    Sink sink;sink.fail=true;fl::set_sink(&Sink::put,&sink);fl::write(h,"lost");fl::flush();
    const auto st=fl::stats(h);require(st.incomplete&&st.write_errors>0&&st.written_lines==0,"failed write is observable");
    sink.fail=false;fl::write(h,"recovery");fl::flush();
    require(sink.text().find("event=incomplete-evidence")!=std::string::npos,"error marker emitted after sink recovery");
    require(fl::stats(h).incomplete,"error latch persists after recovery");fl::set_sink(nullptr);
}
inline void partial() {
    setup();const auto h=fl::open("partial");require(h>=0,"open partial fixture");
    Sink sink;sink.piece=3;fl::set_sink(&Sink::put,&sink);fl::write(h,"complete-payload");fl::flush();
    const auto text=sink.text();const auto st=fl::stats(h);
    require(text.find("|complete-payload\n")!=std::string::npos,"partial writes reconstruct exact payload");
    require(st.written_lines==1&&st.written_bytes==text.size()&&st.short_writes>0&&st.incomplete,"partial counts and latch");
    require(text.find("event=incomplete-evidence")!=std::string::npos,"partial-write marker");fl::set_sink(nullptr);
}
} // namespace feature_log_fixture
#ifndef ACZ_FEATURE_LOG_FIXTURE_ONLY
int main(int argc,char** argv) {
    using namespace feature_log_fixture;
    require(argc==2,"choose slow, failing or partial");const std::string mode=argv[1];
    if(mode=="slow")slow();else if(mode=="failing")failing();else if(mode=="partial")partial();else require(false,"unknown case");
    std::cout<<"feature_log "<<mode<<" passed\n";
}
#endif
