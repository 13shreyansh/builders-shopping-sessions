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
#if defined(__aarch64__)
#include <arm_neon.h>
#elif defined(__SSE2__)
#include <immintrin.h>
#endif
#if defined(__BMI2__)
#include <immintrin.h>
#endif
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

#if defined(__aarch64__) || defined(__SSE2__)
struct Delimiters { unsigned commas, quotes; };
#if defined(__aarch64__)
inline unsigned mask_lanes(uint8x16_t lanes) {
    const uint8_t bits[]={1,2,4,8,16,32,64,128,1,2,4,8,16,32,64,128};
    auto m=vandq_u8(lanes,vld1q_u8(bits));
    return vaddv_u8(vget_low_u8(m)) | (unsigned(vaddv_u8(vget_high_u8(m)))<<8);
}
#endif
inline Delimiters delimiters(const char* p) {
#if defined(__aarch64__)
    auto v=vld1q_u8(reinterpret_cast<const uint8_t*>(p));
    return {mask_lanes(vceqq_u8(v,vdupq_n_u8(','))),mask_lanes(vceqq_u8(v,vdupq_n_u8('"')))};
#else
    auto v=_mm_loadu_si128(reinterpret_cast<const __m128i*>(p));
    return {unsigned(_mm_movemask_epi8(_mm_cmpeq_epi8(v,_mm_set1_epi8(',')))),
            unsigned(_mm_movemask_epi8(_mm_cmpeq_epi8(v,_mm_set1_epi8('"'))))};
#endif
}
#if defined(__AVX2__)
struct Masks64 { uint64_t commas,quotes; };
inline Masks64 masks32(const char* p) {
    auto v=_mm256_loadu_si256(reinterpret_cast<const __m256i*>(p));
    return {uint32_t(_mm256_movemask_epi8(_mm256_cmpeq_epi8(v,_mm256_set1_epi8(',')))),
            uint32_t(_mm256_movemask_epi8(_mm256_cmpeq_epi8(v,_mm256_set1_epi8('"'))))};
}
inline Masks64 masks64(const char* p) {
    auto left=masks32(p),right=masks32(p+32);
    return {left.commas|(right.commas<<32),left.quotes|(right.quotes<<32)};
}
template<size_t N> bool decode_masks(Text line,std::array<Text,N>& out,Masks64 lo,Masks64 hi) {
    if(__builtin_popcountll(lo.commas)+__builtin_popcountll(hi.commas)!=N-1) return false;
    std::array<unsigned,N> ends;
    unsigned col=0;
    while(lo.commas) { ends[col++]=__builtin_ctzll(lo.commas); lo.commas&=lo.commas-1; }
    while(hi.commas) { ends[col++]=64+__builtin_ctzll(hi.commas); hi.commas&=hi.commas-1; }
    ends[N-1]=line.size();
    unsigned start=0,wrapped=0;
    for(size_t i=0;i<N;++i) {
        unsigned end=ends[i];
        unsigned len=end-start;
        unsigned quoted=len>=2 && line[start]=='"' && line[end-1]=='"';
        out[i]=Text(line.data()+start+quoted,len-2*quoted);
        wrapped+=quoted;
        start=end+1;
    }
    return 2*wrapped==unsigned(__builtin_popcountll(lo.quotes)+__builtin_popcountll(hi.quotes));
}
template<size_t N> bool read_avx2(Text line,std::array<Text,N>& out) {
    const char* p=line.data();
    Masks64 lo,hi{};
    if(line.size()<=64) {
        lo=masks32(p);
        if(line.size()>32) {
            unsigned skip=64-line.size();
            auto tail=masks32(p+line.size()-32);
            lo.commas|=(tail.commas>>skip)<<32;
            lo.quotes|=(tail.quotes>>skip)<<32;
        }
    } else {
        lo=masks64(p);
        unsigned skip=128-line.size();
        auto tail=masks64(p+line.size()-64);
        hi={tail.commas>>skip,tail.quotes>>skip};
    }
    return decode_masks(line,out,lo,hi);
}
#endif
template<size_t N> bool scan_fields(Text line,std::array<Text,N>& out) {
#if defined(__AVX2__)
    if(line.size()<=128) {
        if(line.size()>=32) return read_avx2(line,out);
    }
#endif
    // Long and short lines retain exactly the same scalar dialect.
    if(line.size()<16 || line.size()>192) return fields(line,out);
    std::array<unsigned,N> ends;
    unsigned count=0,quotes=0;
    auto scan=[&](unsigned i,unsigned skip) {
        auto mask=delimiters(line.data()+i);
        mask.commas &= (0xffffu<<skip);
        quotes+=__builtin_popcount(mask.quotes & (0xffffu<<skip));
        while(mask.commas) {
            if(count>=N-1) return false;
            ends[count++]=i+__builtin_ctz(mask.commas);
            mask.commas&=mask.commas-1;
        }
        return true;
    };
    unsigned pos=0;
    for(;pos+16<=line.size();pos+=16) if(!scan(pos,0)) return false;
    if(pos<line.size() && !scan(line.size()-16,16-(line.size()-pos))) return false;
    if(count!=N-1) return false;
    ends[N-1]=line.size();
    unsigned begin=0,wrapped=0;
    for(size_t i=0;i<N;++i) {
        unsigned end=ends[i];
        Text value=line.substr(begin,end-begin);
        if(!value.empty() && value.front()=='"') {
            if(value.size()<2 || value.back()!='"') return false;
            value.remove_prefix(1);
            value.remove_suffix(1);
            wrapped+=2;
        }
        out[i]=value;
        begin=end+1;
    }
    return wrapped==quotes;
}
#else
template<size_t N> bool scan_fields(Text line,std::array<Text,N>& out) { return fields(line,out); }
#endif

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

// Validate the calendar prefix on cache misses, including leap days.
inline I64 civil_midnight(Text s,int day) {
    if(s[4]!='-' || s[7]!='-') return bad;
    int year=digits(s,0,4),month=digits(s,5,2);
    if(year<1 || month<1 || month>12) return bad;
    constexpr int month_days[]={31,28,31,30,31,30,31,31,30,31,30,31};
    bool leap=year%4==0 && (year%100!=0 || year%400==0);
    if(day<1 || day>month_days[month-1]+(month==2 && leap)) return bad;
    year-=month<=2;
    int era=year/400,within=year%400;
    int doy=(153*(month+(month>2?-3:9))+2)/5+day-1;
    I64 days=era*146097+within*365+within/4-within/100+doy-719468;
    return days*day_ms;
}

inline uint64_t read_u64(const char* p) {
    uint64_t v;
    std::memcpy(&v,p,sizeof(v));
    return v;
}
inline int read_pair(const char* p) {
    static constexpr auto pairs=[] {
        std::array<uint8_t,65536> result{};
        for(unsigned i=0;i<100;++i) result[('0'+i/10)|(('0'+i%10)<<8)]=i+1;
        return result;
    }();
    uint16_t key;
    std::memcpy(&key,p,sizeof(key));
    return int(pairs[key])-1;
}
inline bool eight_digits(uint64_t bytes) {
    constexpr uint64_t high=0x8080808080808080ULL;
    return !(((bytes+0x4646464646464646ULL)|(bytes-0x3030303030303030ULL))&high);
}
inline uint32_t parse_eight(uint64_t bytes) {
    uint64_t digits=bytes&0x0f0f0f0f0f0f0f0fULL;
    uint64_t pairs=(digits*10+(digits>>8))&0x00ff00ff00ff00ffULL;
    uint64_t quads=(pairs*100+(pairs>>16))&0x0000ffff0000ffffULL;
    return uint32_t(quads)*10000+uint32_t(quads>>32);
}
inline I64 timestamp(Text s) {
    if(s.size()==13) {
        uint64_t first=read_u64(s.data()),last=read_u64(s.data()+5);
        if(!eight_digits(first) || !eight_digits(last)) return bad;
        return I64(parse_eight(first)/1000)*100000000+parse_eight(last);
    }
    if((s.size()!=20 && s.size()!=25) || s[4]!='-') return numeric(s);
    if(s[10]!='T' || s[13]!=':' || s[16]!=':') return bad;
    int day=read_pair(s.data()+8);
    if(unsigned(day-1)>=31) return bad;
    struct Date {uint64_t prefix=0; I64 midnight=0; bool valid=false;};
    static thread_local std::array<Date,32> cache{};
    auto& d=cache[day];
    uint64_t prefix=read_u64(s.data());
    if(!d.valid || d.prefix!=prefix) {
        auto t=civil_midnight(s,day);
        if(t==bad) return bad;
        d={prefix,t,true};
    }
    int h=read_pair(s.data()+11),m=read_pair(s.data()+14),sec=read_pair(s.data()+17);
    if(unsigned(h)>=24 || unsigned(m)>=60 || unsigned(sec)>=60) return bad;
    int zone=0;
    if(s.size()==20) { if(s[19]!='Z') return bad; }
    else {
        int zh=read_pair(s.data()+20),zm=read_pair(s.data()+23);
        if((s[19]!='+' && s[19]!='-') || s[22]!=':' || unsigned(zh)>=24 || unsigned(zm)>=60) return bad;
        zone=(zh*60+zm)*(s[19]=='+' ? 1 : -1);
    }
    return d.midnight+((h*60+m-zone)*60+sec)*1000LL;
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

// Nine nonzero ASCII bytes fit in 63 bits. Zero padding recovers the length.
using Person=uint64_t;
constexpr Person linked_tag=uint64_t(1)<<63, extended_tag=linked_tag|(linked_tag>>1);
inline uint64_t pack_ascii(Text s) {
    if(s.empty() || s.size()>9) return 0;
    uint64_t raw=0;
    if(s.size()>=8) {
        std::memcpy(&raw,s.data(),8);
        constexpr uint64_t high=0x8080808080808080ULL;
        if((raw&high) || ((raw-0x0101010101010101ULL)&~raw&high)) return 0;
    } else {
        for(size_t i=0;i<s.size();++i) {
            unsigned byte=static_cast<unsigned char>(s[i]);
            if(!byte || byte>127) return 0;
            raw|=uint64_t(byte)<<(8*i);
        }
    }
    unsigned last=s.size()==9 ? static_cast<unsigned char>(s[8]) : 0;
    if(s.size()==9 && (!last || last>127)) return 0;
#if defined(__BMI2__)
    uint64_t packed=_pext_u64(raw,0x7f7f7f7f7f7f7f7fULL);
#else
    uint64_t packed=0;
    for(unsigned i=0;i<8;++i) packed|=((raw>>(8*i))&127)<<(7*i);
#endif
    return packed|(uint64_t(last)<<56);
}
struct ExtendedNames {
    struct Shard {
        std::mutex lock;
        NameIndex index;
        std::vector<Text> names;
    };
    std::array<Shard,64> shards;
    Person intern(Text s,uint64_t hash) {
        unsigned shard=hash%shards.size();
        auto& storage=shards[shard];
        std::lock_guard<std::mutex> guard(storage.lock);
        auto [id,added]=storage.index.insert(s,storage.names.size());
        if(added) storage.names.push_back(s);
        return extended_tag|(uint64_t(shard)<<32)|static_cast<unsigned>(id);
    }
    Text lookup(Person p) const { return shards[(p>>32)&63].names[uint32_t(p)]; }
};
struct People {
    mutable ExtendedNames extended;
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
    // Each slot in the directory marks 64 hash buckets. A clear bit proves absence.
    std::vector<uint64_t> membership;
    void finish() {
        size_t words=1;
        while(words<index.used/4) words*=2;
        membership.assign(words,0);
        for(auto& e:index.slots) if(!e.key.empty()) {
            e.value=root(e.value);
            membership[(e.hash>>6)&(words-1)] |= uint64_t(1)<<(e.hash&63);
        }
    }
    Person raw(Text key) const {
        if(Person code=pack_ascii(key)) return code;
        return extended.intern(key,text_hash(key));
    }
    Person canonical(Person raw_code) const {
        std::array<char,9> temporary;
        Text key=name(raw_code,temporary);
        uint64_t hash=text_hash(key);
        if(!(membership[(hash>>6)&(membership.size()-1)] & (uint64_t(1)<<(hash&63))))
            return raw_code;
        const auto& e=index.slots[index.locate(key,hash)];
        return e.key.empty()?raw_code:linked_tag|static_cast<unsigned>(e.value);
    }
    Text name(Person p,std::array<char,9>& buf) const {
        if((p&extended_tag)==extended_tag) return extended.lookup(p);
        if(p&linked_tag) return names[uint32_t(p)];
        size_t len=0;
        while(p) { buf[len++]=char(p&127); p>>=7; }
        return Text(buf.data(),len);
    }
};
