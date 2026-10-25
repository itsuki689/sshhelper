#include "mock_sftp.hpp"
#define CHECK(c) do { if (!(c)) { std::cerr << "Line " << __LINE__ << ": " #c "\n" << error << "\n"; return __LINE__; } } while(false)
int runTests() {
    namespace fs = std::filesystem;
    std::string error;
    const auto root = fs::temp_directory_path() / ("airsftp-resume-" + std::to_string(GetCurrentProcessId()));
    CHECK(!fs::exists(root)); fs::create_directory(root);
    struct Cleanup { fs::path p; ~Cleanup() { std::error_code e; fs::remove_all(p,e); } } cleanup{root};
    auto encoded = [](const fs::path& p) { auto text=p.generic_u8string(); return std::string(text.begin(),text.end()); };
    auto read = [](const fs::path& p) { std::ifstream file(p,std::ios::binary); return std::string(std::istreambuf_iterator<char>(file),{}); };
    auto write = [](const fs::path& p,const std::string& data) { std::ofstream(p,std::ios::binary|std::ios::trunc)<<data; };
    std::string payload(2*1024*1024+19,'x');
    for (std::size_t i=0;i<payload.size();++i) payload[i]=char((i*17)%251);
    const auto source=root/"source.bin", destination=root/"target.bin";
    write(source,payload); write(destination,"old-local");
    remoteFiles["/target"]={LIBSSH2_SFTP_S_IFREG,"old-remote"};
    SOCKET listener=socket(AF_INET,SOCK_STREAM,IPPROTO_TCP);
    sockaddr_in address{};address.sin_family=AF_INET;address.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
    CHECK(bind(listener,reinterpret_cast<sockaddr*>(&address),sizeof(address))==0);
    CHECK(listen(listener,4)==0);
    int length=sizeof(address);getsockname(listener,reinterpret_cast<sockaddr*>(&address),&length);
    ConnectionConfig config;config.host="127.0.0.1";config.port=ntohs(address.sin_port);config.username="test";
    // Monitoring may use only existing pins, and must never create a trust file.
    const auto readOnlyPins = root / "readonly.known_hosts";
    {
        SshSession readOnly({}, 15000, readOnlyPins, false);
        CHECK(!readOnly.connect(config, error));
        CHECK(error.find("not pinned") != std::string::npos && !fs::exists(readOnlyPins));
    }
    std::ofstream(readOnlyPins) << config.host << ':' << config.port << " wrong-fingerprint\n";
    {
        SshSession changed({}, 15000, readOnlyPins, false);
        CHECK(!changed.connect(config, error));
        CHECK(error.find("host key changed") != std::string::npos);
        CHECK(read(readOnlyPins).find("wrong-fingerprint") != std::string::npos);
    }
    std::stop_source stop;
    SshSession session(stop.get_token());
    CHECK(session.connect(config,error));
    TransferProgress final;
    auto progress=[&](const TransferProgress& p){final=p;};
    auto upload=[&]{return session.transfer(TransferDirection::Upload,encoded(source),"/target",progress,error);};
    auto download=[&]{return session.transfer(TransferDirection::Download,encoded(destination),"/target",progress,error);};
    failWriteAfter=100003;bytesWritten=0;
    CHECK(!upload());
    CHECK(remoteFiles.at("/target").data=="old-remote" && final.failed);
    std::string remotePart;
    for(auto& [path,node]:remoteFiles) if(path.ends_with(".part")) remotePart=path;
    CHECK(!remotePart.empty() && remoteFiles.at(remotePart).data.size()==100003);
    failWriteAfter=SIZE_MAX;bytesWritten=0;
    CHECK(upload());
    CHECK(final.resumedBytes==100003 && bytesWritten==payload.size()-100003);
    CHECK(remoteFiles.at("/target").data==payload && !remoteFiles.contains(remotePart));
    failReadAfter=100003;bytesRead=0;
    CHECK(!download());
    CHECK(read(destination)=="old-local" && final.failed);
    fs::path localPart;
    for(auto& entry:fs::directory_iterator(root)) if(entry.path().extension()==".part") localPart=entry.path();
    CHECK(!localPart.empty() && fs::file_size(localPart)==100003);
    failReadAfter=SIZE_MAX;bytesRead=0;
    CHECK(download());
    CHECK(final.resumedBytes==100003 && read(destination)==payload && !fs::exists(localPart));
    // Corrupted partial data must restart, not contaminate the final target.
    remoteFiles[remotePart]={LIBSSH2_SFTP_S_IFREG,std::string(500,'z')};
    CHECK(upload() && final.resumedBytes==0 && remoteFiles.at("/target").data==payload);
    write(localPart,std::string(500,'z'));
    CHECK(download() && final.resumedBytes==0 && read(destination)==payload);
    // Atomic rename failure preserves both the original final and complete temporary.
    remoteFiles["/target"].data="old-remote";failRename=true;
    CHECK(!upload() && remoteFiles.at("/target").data=="old-remote");
    CHECK(remoteFiles.at(remotePart).data==payload);
    failRename=false;
    CHECK(upload() && final.resumedBytes==payload.size());
    for (auto* fault : {&failInit,&failOpen,&failWrite,&zeroWrite,&failFlush,&failClose}) {
        remoteFiles.erase(remotePart);remoteFiles["/target"].data="old-remote";
        *fault=true;
        CHECK(!upload());
        CHECK(final.failed && remoteFiles.at("/target").data=="old-remote");
        *fault=false;
    }
    remoteFiles["/target"].data=payload;
    for(auto* fault : {&failInit,&failOpen,&failRead,&failClose}) {
        fs::remove(localPart);write(destination,"old-local");*fault=true;
        CHECK(!download() && read(destination)=="old-local");
        *fault=false;
    }
    // Replacement blocked by a reader: final survives and complete .part remains retryable.
    fs::remove(localPart);write(destination,"old-local");
    HANDLE held=CreateFileW(destination.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,0,nullptr);
    CHECK(held!=INVALID_HANDLE_VALUE);
    CHECK(!download() && read(destination)=="old-local" && fs::file_size(localPart)==payload.size());
    CloseHandle(held);
    CHECK(download() && final.resumedBytes==payload.size());
    CHECK(!session.transfer(TransferDirection::Upload,encoded(root/"missing"),"/missing",progress,error));
    CHECK(!remoteFiles.contains("/missing"));
    remoteFiles.erase(remotePart);remoteFiles["/target"].data="old-remote";
    CHECK(!session.transfer(TransferDirection::Upload,encoded(source),"/target",[&](const TransferProgress& p) {
        final=p;if(p.active && p.transferred>0) stop.request_stop();
    },error));
    CHECK(final.failed && error=="Transfer cancelled" && remoteFiles.at("/target").data=="old-remote");
    closesocket(listener);
    std::cout<<"Atomic replacement, upload/download resume, corrupt-prefix restart, cancellation, IO/close/rename failures: OK\n";
    return 0;
}
int main(){WSADATA data{};if(WSAStartup(MAKEWORD(2,2),&data)!=0)return 1;
if(libssh2_init(0)!=0)return 1;int result=runTests();libssh2_exit();WSACleanup();return result;}
