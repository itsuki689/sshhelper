#include "credential_store.hpp"
#include "ssh_client.hpp"
#include <winsock2.h>
#include <windows.h>
#include <libssh2.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <chrono>
#include <algorithm>

namespace fs=std::filesystem;
std::string encoded(const fs::path& p){auto t=p.generic_u8string();return {t.begin(),t.end()};}
std::string read(const fs::path& p){std::ifstream f(p,std::ios::binary);return {std::istreambuf_iterator<char>(f),{}};}
void require(bool good,const std::string& message){if(!good)throw std::runtime_error(message);}
int run(int argc,char** argv) {
    require(argc==4,"Usage: live_transfer_test HOST CREDENTIAL_FILE LOCAL_TEST_ROOT");
    const std::string host=argv[1];
    ServerSettings saved;std::string error;
    require(loadServerSettings(fs::u8path(argv[2]),saved,error),error);
    auto found=std::find_if(saved.servers.begin(),saved.servers.end(),[&](auto& p){return p.connection.host==host;});
    require(found!=saved.servers.end(),"No saved profile for the specified host");
    const auto config=found->connection;
    auto root=fs::absolute(fs::u8path(argv[3])).lexically_normal();
    require(!fs::exists(root),"Test directory must not already exist");
    fs::create_directories(root/"source/empty");
    std::string payload(16*1024*1024,'\0');std::uint32_t rng=1234567;
    for(char& c:payload){rng^=rng<<13;rng^=rng>>17;rng^=rng<<5;c=char(rng&255);}
    const auto source=root/"source/blob.bin";
    std::ofstream(source,std::ios::binary).write(payload.data(),payload.size());
    for(int i=0;i<12;++i)std::ofstream(root/"source"/("small-"+std::to_string(i)+".txt"))<<"test "<<i;
    std::ofstream(root/"source"/fs::path(L"中文.txt"))<<"unicode";
    // Use the same pinned host keys as the application before authenticating.
    const auto pins = hostKeyPathForCredentials(fs::u8path(argv[2]));
    require(fs::exists(pins), "Existing application host-key pins required");
    SshSession session({}, 15000, pins);
    require(session.connect(config,error),"Connect: "+error);
    std::string output;double latency=0;
    require(session.executeScript("umask 077\nmktemp -d /tmp/airsftp-test.XXXXXXXXXX\n",output,latency,error),error);
    while(!output.empty()&&(output.back()=='\n'||output.back()=='\r'))output.pop_back();
    const auto remote=output;
    require(remote.starts_with("/tmp/airsftp-test.") && remote.size()==28 &&
        std::all_of(remote.begin()+18,remote.end(),[](unsigned char c){return std::isalnum(c);}),"Unexpected temporary directory: "+remote);
    std::cout<<"Remote isolated test directory: "<<remote<<std::endl;
    std::ofstream(root/"remote-directory.txt")<<remote;
    auto cleanup=[&]{
        std::string out;
        const bool ok=session.executeScript("rm -rf -- '"+remote+"'\n",out,latency,error);
        if(!ok) std::cerr<<"Remote test cleanup failed: "<<error<<std::endl;
        return ok;
    };
    try {
        TransferProgress final;
        auto progress=[&](const TransferProgress& p){final=p;};
        auto approve=[](std::size_t,std::uint64_t){return true;};
        auto batch=[&](TransferDirection d,const fs::path& local,const std::string& target){
#ifdef AIRSFTP_BASELINE
            return session.transferFolder(d,encoded(local),target,std::nullopt,progress,error);
#else
            return session.transferFolder(d,encoded(local),target,std::nullopt,progress,error,approve);
#endif
        };
        auto begin=std::chrono::steady_clock::now();
        require(batch(TransferDirection::Upload,root/"source",remote+"/batch"),"Upload: "+error);
        const auto up=std::chrono::duration<double>(std::chrono::steady_clock::now()-begin).count();
        std::cout<<"Upload: "<<final.filesCompleted<<" files, "<<final.total<<" bytes, "<<up<<" s, "<<(final.total/up/1048576)<<" MiB/s"<<std::endl;
        begin=std::chrono::steady_clock::now();
        require(batch(TransferDirection::Download,root/"download",remote+"/batch"),"Download: "+error);
        const auto down=std::chrono::duration<double>(std::chrono::steady_clock::now()-begin).count();
        std::cout<<"Download: "<<final.filesCompleted<<" files, "<<final.total<<" bytes, "<<down<<" s, "<<(final.total/down/1048576)<<" MiB/s"<<std::endl;
        require(read(root/"download/blob.bin")==payload,"Roundtrip content mismatch");
        require(fs::is_directory(root/"download/empty"),"Empty directory missing");
        require(read(root/"download"/fs::path(L"中文.txt"))=="unicode","Unicode file mismatch");
#ifndef AIRSFTP_BASELINE
        int confirmations=0;
        require(!session.transferFolder(TransferDirection::Upload,encoded(root/"source"),remote+"/denied",std::nullopt,
            progress,error,[&](std::size_t n,std::uint64_t){++confirmations;return false;}),"Denied batch unexpectedly started");
        require(confirmations==1,"Expected a single confirmation");
        std::vector<TransferEntry> entries;
        require(session.listFolder(TransferDirection::Download,remote,entries,error),error);
        require(std::none_of(entries.begin(),entries.end(),[](auto& e){return e.relativePath.starts_with("denied");}),"Denied transfer created files");
        const auto old=root/"old.bin";std::ofstream(old)<<"original";
        require(session.transfer(TransferDirection::Upload,encoded(old),remote+"/resume.bin",progress,error),error);
        std::stop_source cancelUpload;
        {
            SshSession interrupted(cancelUpload.get_token(), 15000, pins);
            require(interrupted.connect(config,error),error);
            require(!interrupted.transfer(TransferDirection::Upload,encoded(source),remote+"/resume.bin",
                [&](const TransferProgress& p){final=p;if(p.active&&p.transferred>=1048576)cancelUpload.request_stop();},error),"Upload did not cancel");
        }
        require(session.transfer(TransferDirection::Download,encoded(root/"old-check.bin"),remote+"/resume.bin",progress,error),error);
        require(read(root/"old-check.bin")=="original","Cancelled upload replaced old remote target");
        require(session.transfer(TransferDirection::Upload,encoded(source),remote+"/resume.bin",progress,error),error);
        require(final.resumedBytes>0,"Upload did not resume");
        std::cout<<"Upload resume verified: "<<final.resumedBytes<<" bytes reused"<<std::endl;
        const auto target=root/"resume-download.bin";std::ofstream(target)<<"original-local";
        std::stop_source cancelDownload;
        {
            SshSession interrupted(cancelDownload.get_token(), 15000, pins);
            require(interrupted.connect(config,error),error);
            require(!interrupted.transfer(TransferDirection::Download,encoded(target),remote+"/resume.bin",
                [&](const TransferProgress& p){final=p;if(p.active&&p.transferred>=1048576)cancelDownload.request_stop();},error),"Download did not cancel");
        }
        require(read(target)=="original-local","Cancelled download replaced old local target");
        require(session.transfer(TransferDirection::Download,encoded(target),remote+"/resume.bin",progress,error),error);
        require(final.resumedBytes>0 && read(target)==payload,"Download resume/content mismatch");
        std::cout<<"Download resume verified: "<<final.resumedBytes<<" bytes reused"<<std::endl;
        require(session.listFolder(TransferDirection::Download,remote,entries,error),error);
        require(std::none_of(entries.begin(),entries.end(),[](auto& e){return e.relativePath.ends_with(".part");}),"Unexpected temporary entry");
#endif
        require(session.executeScript("find '" + remote + "' -name '*.part' -print\n",output,latency,error),error);
        require(output.empty(),"Completed tests left unexpected temporary files");
        require(cleanup(),"Remote test cleanup failed");
        std::cout<<"PASS: byte-for-byte roundtrip, isolated test cleanup complete"<<std::endl;
        return 0;
    } catch(...) { cleanup();throw; }
}
int main(int argc,char** argv){WSADATA data{};WSAStartup(MAKEWORD(2,2),&data);libssh2_init(0);
int result=1;try{result=run(argc,argv);}catch(const std::exception& e){std::cerr<<e.what()<<std::endl;}
libssh2_exit();WSACleanup();return result;}
