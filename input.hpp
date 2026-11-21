#pragma once
#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

using Text = std::string_view;
using I64 = int64_t;
namespace fs = std::filesystem;
constexpr I64 day_ms = 86400000, week_ms = 7 * day_ms;
constexpr I64 bad = std::numeric_limits<I64>::min();

inline std::string env(const char* key, const char* fallback) {
    const char* value = std::getenv(key);
    return value ? value : fallback;
}

struct MappedFiles {
    std::vector<Text> files;
    std::vector<Text> chunks;
    ~MappedFiles() {
        for (Text file : files) if (!file.empty()) munmap((void*)file.data(), file.size());
    }
    void add(const fs::path& path) {
        int fd = open(path.c_str(), O_RDONLY);
        if (fd < 0) throw std::runtime_error("cannot open input");
        struct stat st{};
        if (fstat(fd, &st)) { close(fd); throw std::runtime_error("stat failed"); }
        if (!st.st_size) { close(fd); return; }
        void* raw = mmap(nullptr, st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
        close(fd);
        if (raw == MAP_FAILED) throw std::runtime_error("mmap failed");
        Text file((const char*)raw, st.st_size);
        files.push_back(file);
        size_t header = file.find('\n');
        if (header == Text::npos) return;
        size_t start = header+1;
        while (start < file.size()) {
            size_t next = file.find('\n',std::min(start+524288, file.size()));
            next = next == Text::npos ? file.size() : next+1;
            chunks.push_back(file.substr(start,next-start));
            start=next;
        }
    }
    void folder(const fs::path& dir) {
        for (auto& f : fs::directory_iterator(dir))
            if (f.path().extension()==".csv" && f.is_regular_file()) add(f.path());
    }
};

template<class F> void lines(Text chunk, F fn) {
    while (!chunk.empty()) {
        size_t n=chunk.find('\n');
        if (n==Text::npos) { fn(chunk); break; }
        fn(chunk.substr(0,n));
        chunk.remove_prefix(n+1);
    }
}

template<size_t N> bool fields(Text line, std::array<Text,N>& out) {
    for (size_t i=0; i<N; ++i) {
        size_t n=line.find(',');
        if ((n==Text::npos) != (i+1==N)) return false;
        Text value=line.substr(0,n);
        if (!value.empty() && value.front()=='"') {
            if (value.size()<2 || value.back()!='"') return false;
            value.remove_prefix(1);
            value.remove_suffix(1);
        }
        if (value.find('"')!=Text::npos) return false;
        out[i]=value;
        if (n!=Text::npos) line.remove_prefix(n+1);
    }
    return true;
}

inline int digits(Text s, int pos, int len) {
    int value=0;
    for (int i=0;i<len;++i) {
        unsigned d=static_cast<unsigned>(s[pos+i]-'0');
        if (d>9) return -1;
        value=value*10+d;
    }
    return value;
}

inline I64 numeric(Text s) {
    if (s.empty()) return bad;
    I64 v=0;
    auto res=std::from_chars(s.data(), s.data()+s.size(), v);
    return s.front()>='0' && s.front()<='9' && res.ec==std::errc{} && res.ptr==s.data()+s.size() ? v : bad;
}

inline I64 timestamp(Text s) {
    if ((s.size()!=20 && s.size()!=25) || s[4]!='-') return numeric(s);
    if (s[7]!='-' || s[10]!='T' || s[13]!=':' || s[16]!=':') return bad;
    int y=digits(s,0,4),m=digits(s,5,2),d=digits(s,8,2);
    int h=digits(s,11,2),min=digits(s,14,2),sec=digits(s,17,2);
    if(y<1 || m<1 || m>12 || d<1 || h<0 || h>23 || min<0 || min>59 || sec<0 || sec>59) return bad;
    constexpr int month_lengths[]={31,28,31,30,31,30,31,31,30,31,30,31};
    bool leap=(y%4==0 && (y%100!=0 || y%400==0));
    if(d>month_lengths[m-1]+(m==2 && leap)) return bad;
    int offset=0;
    if (s.size()==20) { if (s[19]!='Z') return bad; }
    else {
        int oh=digits(s,20,2),om=digits(s,23,2);
        if((s[19]!='+' && s[19]!='-') || s[22]!=':' || oh<0 || oh>23 || om<0 || om>59) return bad;
        offset=(oh*60+om)*(s[19]=='+' ? 1 : -1);
    }
    y -= m<=2;
    int era=y/400, year=y%400, doy=(153*(m+(m>2?-3:9))+2)/5+d-1;
    I64 day=era*146097+year*365+year/4-year/100+doy-719468;
    return day*day_ms+((h*60+min-offset)*60+sec)*1000LL;
}

struct Window {
    I64 start,end;
    explicit Window(fs::path path) {
        std::ifstream in(path);
        std::string json((std::istreambuf_iterator<char>(in)),{});
        auto value=[&](std::string key) {
            size_t at=json.find("\""+key+"\"");
            if(at==std::string::npos) throw std::runtime_error("missing manifest key");
            at=json.find('"',json.find(':',at)+1);
            size_t close=json.find('"',at+1);
            return timestamp(Text(json).substr(at+1,close-at-1));
        };
        start=value("window_start"); end=value("window_end");
        if(start==bad || end<=start) throw std::runtime_error("invalid window");
    }
};

template<class F> void parallel(size_t tasks, int workers, F fn) {
    std::atomic<size_t> next{0};
    std::vector<std::thread> pool;
    std::exception_ptr error;
    std::mutex lock;
    for (int i=0; i<workers && size_t(i)<tasks; ++i) pool.emplace_back([&,i] {
        try {
            for (size_t j; (j=next.fetch_add(1,std::memory_order_relaxed))<tasks;) fn(j,i);
        } catch (...) {
            std::lock_guard<std::mutex> guard(lock);
            if(!error) error=std::current_exception();
            next.store(tasks);
        }
    });
    for(auto& t:pool) t.join();
    if(error) std::rethrow_exception(error);
}

inline uint64_t text_hash(Text s) {
    if(s.size()<8 || s.size()>16) return std::hash<Text>{}(s);
    uint64_t lo,hi;
    std::memcpy(&lo,s.data(),8);
    std::memcpy(&hi,s.data()+s.size()-8,8);
    uint64_t v=(lo*0x9e3779b185ebca87ULL) ^ (hi*0xc2b2ae3d27d4eb4fULL) ^ s.size();
    v^=v>>33;
    v*=0xff51afd7ed558ccdULL;
    return v^(v>>32);
}

// Flat open addressing keeps lookups contiguous. Hash collisions always compare full text.
struct NameIndex {
    struct Entry { Text key; uint64_t hash=0; int value=0; };
    std::vector<Entry> slots=std::vector<Entry>(1024);
    size_t used=0;
    size_t locate(Text key,uint64_t h) const {
        size_t mask=slots.size()-1;
        size_t i=h & mask;
        while(!slots[i].key.empty() && (slots[i].hash!=h || slots[i].key!=key))
            i=(i+1)&mask;
        return i;
    }
    void expand() {
        auto old=std::move(slots);
        slots.assign(old.size()*2,Entry{});
        for(auto& e:old) if(!e.key.empty()) slots[locate(e.key,e.hash)]=e;
    }
    std::pair<int,bool> insert(Text key,int proposed) {
        if(used*10>=slots.size()*7) expand();
        auto h=text_hash(key);
        auto& e=slots[locate(key,h)];
        if(!e.key.empty()) return {e.value,false};
        e={key,h,proposed};
        ++used;
        return {proposed,true};
    }
};

struct Person {
    Text name;
    uint64_t order;
    bool operator==(const Person& other) const {
        return order==other.order && (!(order>>63) || name==other.name);
    }
    bool operator!=(const Person& other) const { return !(*this==other); }
    bool operator<(const Person& other) const {
        if(order!=other.order) return order<other.order;
        return (order>>63) && name<other.name;
    }
};
struct People {
    NameIndex index;
    std::vector<int> parents, sizes;
    std::vector<Text> names;
    int id(Text s) {
        auto [value,added]=index.insert(s,parents.size());
        if(added) { parents.push_back(value); sizes.push_back(1); names.push_back(s); }
        return value;
    }
    int root(int a) {
        while(a!=parents[a]) { parents[a]=parents[parents[a]]; a=parents[a]; }
        return a;
    }
    void join(Text left, Text right) {
        int a=root(id(left)), b=root(id(right));
        if (a==b) return;
        if (sizes[a]<sizes[b]) std::swap(a,b);
        parents[b]=a; sizes[a]+=sizes[b];
        names[a]=std::min(names[a],names[b]);
    }
    void finish() { for(auto& e:index.slots) if(!e.key.empty()) e.value=root(e.value); }
    Person operator()(Text s) const {
        uint64_t h=text_hash(s);
        const auto& e=index.slots[index.locate(s,h)];
        if(e.key.empty()) return {s,h|(uint64_t(1)<<63)};
        return {names[e.value],static_cast<uint64_t>(e.value)};
    }
};
