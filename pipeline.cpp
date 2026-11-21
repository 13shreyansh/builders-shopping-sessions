#include "output.hpp"
#include <chrono>
#include <future>

namespace {
constexpr int buckets=64;
unsigned bucket(Person who) { return (who*0x9e3779b97f4a7c15ULL)>>(64-6); }
template<class T> using Parts=std::array<std::vector<T>,buckets>;
template<class T> std::vector<T> collect(std::vector<Parts<T>>& parts, int index) {
    size_t n=0;
    for(auto& p:parts) n+=p[index].size();
    std::vector<T> out;
    out.reserve(n);
    for(auto& p:parts) {
        auto& v=p[index];
        out.insert(out.end(),v.begin(),v.end());
        std::vector<T>().swap(v);
    }
    return out;
}

struct Dimensions {
    People people;
    std::unordered_map<Text,int> products,campaign_ids;
    std::vector<Text> categories{"unknown"};
    std::vector<Campaign> campaigns{{"organic","organic"}};
    void load(MappedFiles& files, const fs::path& root) {
        files.add(root/"products.csv");
        std::unordered_map<Text,int> cat{{"unknown",0}};
        for(Text chunk:files.chunks) lines(chunk,[&](Text line) {
            std::array<Text,2> f;
            if(!scan_fields(line,f)) return;
            auto [it,added]=cat.emplace(f[1],categories.size());
            if(added) categories.push_back(f[1]);
            products.emplace(f[0],it->second);
        });
        files.chunks.clear();
        files.add(root/"campaigns.csv");
        for(Text chunk:files.chunks) lines(chunk,[&](Text line) {
            std::array<Text,2> f;
            if(!scan_fields(line,f)) return;
            campaign_ids.emplace(f[0],campaigns.size());
            campaigns.push_back({f[0],f[1]});
        });
        files.chunks.clear();
        files.folder(root/"links");
        for(Text chunk:files.chunks) lines(chunk,[&](Text line) {
            std::array<Text,2> f;
            if(scan_fields(line,f) && !f[0].empty() && !f[1].empty()) people.join(f[0],f[1]);
        });
        people.finish();
    }
};
// A Bloom filter only routes potential repeats to the exact winner map.
struct Fingerprints {
    std::vector<uint64_t> words;
    void size_for(size_t n) {
        size_t blocks=1;
        while(blocks<n/2) blocks*=2;
        words.assign(blocks,0);
    }
    size_t index(uint64_t hash) const { return (hash>>6)&(words.size()-1); }
    static uint64_t mask(uint64_t hash) {
        return (uint64_t(1)<<(hash>>58)) | (uint64_t(1)<<((hash>>50)&63));
    }
    bool has(uint64_t hash) const { return (words[index(hash)]&mask(hash))==mask(hash); }
    void add(uint64_t hash) { words[index(hash)]|=mask(hash); }
};
struct Raw {
    uint64_t hash;
    const char* row;
    uint32_t chunk, ordinal;
    Text line(const std::vector<Text>& chunks) const {
        Text source=chunks[chunk];
        Text tail=source.substr(row-source.data());
        return tail.substr(0,tail.find('\n'));
    }
};
struct Winner { Text ingest; uint32_t chunk,ordinal; };
struct Repeats {
    Fingerprints possible;
    std::unordered_map<Text,Winner> winners;
};
Text first_field(Text line) {
    Text id=line.substr(0,line.find(','));
    if(id.size()>=2 && id.front()=='"' && id.back()=='"') return id.substr(1,id.size()-2);
    return id;
}
struct WideEvent {
    Person person;
    I64 time;
    int price,cat;
    I64 timestamp(I64) const { return time; }
    int cents() const { return price; }
    int category() const { return cat; }
    static WideEvent make(Person p,I64 ts,int price,int cat,I64) {return {p,ts,price,cat};}
};
struct PackedEvent {
    Person person;
    uint64_t payload;
    I64 timestamp(I64 base) const { return base+I64(payload>>32); }
    int cents() const { return int(payload&0xffffffU); }
    int category() const { return int(uint32_t(payload)>>24); }
    static PackedEvent make(Person p,I64 ts,int price,int cat,I64 base) {
        return {p,(uint64_t(ts-base)<<32)|(uint64_t(cat)<<24)|unsigned(price)};
    }
};
struct Click { Person person; I64 ts; Text id; int campaign; };

template<class Event> void keep_event(const std::array<Text,7>& f,const Dimensions& dims,const Window& win,Parts<Event>& dst,AliasCache& cache) {
    if(f[1].empty() || f[5].empty()) return;
    I64 ts=timestamp(f[2]);
    if(ts<win.start || ts>=win.end || timestamp(f[3])-ts>day_ms) return;
    int cents=0,category=0;
    if(f[4]=="purchase") {
        I64 price=numeric(f[6]);
        if(price<1 || price>10000000) return;
        cents=static_cast<int>(price);
        auto it=dims.products.find(f[5]);
        if(it!=dims.products.end()) category=it->second;
    } else if(f[4]!="view" && f[4]!="cart") return;
    Person who=dims.people.lookup(f[1],cache);
    dst[bucket(who)].push_back(Event::make(who,ts,cents,category,win.start));
}

template<class Event> void process_person(const Event* events,const Event* end,const Click* click,const Click* click_end,
                    const Dimensions& dims,Campaigns& totals,std::vector<UserRow>& users,I64 base) {
    const Person person=events->person;
    std::unordered_map<int,int> active;
    const Click* expire=click;
    int latest=0;
    std::unordered_map<int,I64> cat_totals;
    UserRow row{person,0,0,0,0,{},false};
    auto flush=[&] {
        I64 best=0;
        for(auto [category,sum]:cat_totals) {
            Text name=dims.categories[category];
            if(sum>best || (sum==best && name<row.top)) { row.top=name; row.has_top=true; best=sum; }
        }
        users.push_back(row);
        cat_totals.clear();
    };
    while(events!=end) {
        I64 start=events->timestamp(base);
        int day=static_cast<int>(start/day_ms-(start%day_ms<0));
        if(!row.sessions || row.day!=day) {
            if(row.sessions) flush();
            row={person,day,0,0,0,{},false};
        }
        ++row.sessions;
        I64 prev=start,sum=0;
        do {
            ++row.events;
            if(events->cents()) { sum+=events->cents(); cat_totals[events->category()]+=events->cents(); }
            prev=events->timestamp(base);
            ++events;
        } while(events!=end && events->timestamp(base)-prev<=1800000);
        row.revenue+=sum;
        while(click!=click_end && click->ts<=start) {
            latest=click->campaign;
            ++active[click->campaign];
            ++click;
        }
        while(expire!=click && expire->ts<start-week_ms) {
            auto it=active.find(expire->campaign);
            if(--it->second==0) active.erase(it);
            ++expire;
        }
        auto& credit=totals[{active.empty()?0:latest,day}];
        ++credit.sessions;
        credit.purchases+=sum>0;
        credit.revenue+=sum;
        for(auto [campaign,n]:active) ++totals[{campaign,day}].assists;
    }
    flush();
}

struct Profile {
    using Clock=std::chrono::steady_clock;
    Clock::time_point mark=Clock::now();
    void lap(const char* stage) {
        auto now=Clock::now();
        if(std::getenv("PROFILE")) std::cerr<<stage<<" "<<std::chrono::duration<double>(now-mark).count()<<" s\n";
        mark=now;
    }
};
// Group equal owner codes in place. Bytes shared by the entire range need no pass.
template<class E> void group_owners(E* first,E* last) {
    size_t n=last-first;
    if(n<64) {
        std::sort(first,last,[](const E& a,const E& b){return a.person<b.person;});
        return;
    }
    uint64_t different=0, base=first->person;
    for(auto* p=first+1;p!=last;++p) different|=p->person^base;
    if(!different) return;
    unsigned shift=(63-__builtin_clzll(different))&~7u;
    auto lane=[shift](const E& row){return unsigned((row.person>>shift)&255);};
    std::array<size_t,256> counts{},next{},end{};
    for(auto* p=first;p!=last;++p) ++counts[lane(*p)];
    size_t off=0;
    for(unsigned b=0;b<256;++b) {
        next[b]=off;
        off+=counts[b];
        end[b]=off;
    }
    for(unsigned b=0;b<256;++b) {
        while(next[b]<end[b]) {
            auto& row=first[next[b]];
            unsigned target=lane(row);
            if(target==b) ++next[b];
            else std::swap(row,first[next[target]++]);
        }
    }
    if(shift) for(unsigned b=0;b<256;++b) if(counts[b]>1)
        group_owners(first+end[b]-counts[b], first+end[b]);
}

void run(const fs::path& data,const fs::path& out,int threads) {
    Profile timer;
    Window win(data/"manifest.json");
    MappedFiles dim_files,event_files,click_files;
    Dimensions dims;
    auto dimension_job=std::async(threads>1 ? std::launch::async : std::launch::deferred,
                                  [&] { dims.load(dim_files,data); });
    event_files.folder(data/"events");
    click_files.folder(data/"clicks");
    fs::create_directories(out/"user_daily");
    fs::create_directories(out/"campaign_daily");
    timer.lap("dimensions and mapping");
    if(event_files.chunks.size()>UINT32_MAX) throw std::length_error("too many event chunks");
    std::vector<Parts<Raw>> raw(threads);
    // A chunk ends at the first LF after 512 KiB, so even an oversized final
    // record cannot put more than 524289 physical rows in one chunk.
    std::vector<std::vector<uint64_t>> discarded(event_files.chunks.size());
    parallel(event_files.chunks.size(),threads,[&](size_t i,int worker) {
        uint32_t ordinal=0;
        lines(event_files.chunks[i],[&](Text line) {
            uint32_t row=ordinal++;
            Text id=first_field(line);
            if(id.empty()) return;
            auto hash=text_hash(id);
            raw[worker][hash%buckets].push_back({hash,line.data(),static_cast<uint32_t>(i),row});
        });
        discarded[i].resize((ordinal+63)/64);
    });
    timer.lap("partition event IDs");

    parallel(buckets,threads,[&](size_t b,int) {
        size_t n=0;
        for(auto& src:raw) n+=src[b].size();
        Fingerprints seen;
        seen.size_for(n);
        Repeats r;
        r.possible.size_for(n);
        for(auto& src:raw) for(const auto& row:src[b]) {
            if(seen.has(row.hash)) r.possible.add(row.hash);
            seen.add(row.hash);
        }
        for(auto& src:raw) {
            for(const auto& row:src[b]) {
                if(!r.possible.has(row.hash)) continue;
                std::array<Text,7> f;
                if(!scan_fields(row.line(event_files.chunks),f) || f[0].empty()) continue;
                Winner current{f[3],row.chunk,row.ordinal};
                auto [it,added]=r.winners.emplace(f[0],current);
                if(!added) {
                    if(current.ingest<it->second.ingest) std::swap(current,it->second);
                    // Separate event-ID buckets can reject rows in the same word.
                    auto& word=discarded[current.chunk][current.ordinal/64];
                    __atomic_fetch_or(&word,uint64_t(1)<<(current.ordinal%64),__ATOMIC_RELAXED);
                }
            }
            std::vector<Raw>().swap(src[b]);
        }
    });
    timer.lap("resolve potential duplicate IDs");
    dimension_job.get();
    timer.lap("await dimensions");
    auto finish=[&](auto representation) {
        using Event=decltype(representation);
        std::vector<AliasCache> caches(threads);
        std::vector<Parts<Event>> events(threads);
        parallel(event_files.chunks.size(),threads,[&](size_t i,int worker) {
            const auto& skip=discarded[i];
            uint32_t ordinal=0;
            lines(event_files.chunks[i],[&](Text line) {
                uint32_t row=ordinal++;
                if(skip[row/64] & (uint64_t(1)<<(row%64))) return;
                std::array<Text,7> f;
                if(!scan_fields(line,f) || f[0].empty()) return;
                keep_event(f,dims,win,events[worker],caches[worker]);
            });
        });
        timer.lap("parse and validate winners");
        std::vector<Parts<Click>> clicks(threads);
        parallel(click_files.chunks.size(),threads,[&](size_t i,int worker) {
            lines(click_files.chunks[i],[&](Text line) {
                std::array<Text,4> f;
                if(!scan_fields(line,f) || f[0].empty() || f[1].empty()) return;
                I64 ts=timestamp(f[3]);
                auto it=dims.campaign_ids.find(f[2]);
                if(ts<win.start-week_ms || ts>=win.end || it==dims.campaign_ids.end()) return;
                Person who=dims.people.lookup(f[1],caches[worker]);
                clicks[worker][bucket(who)].push_back({who,ts,f[0],it->second});
            });
        });
        timer.lap("parse clicks");
        std::array<Campaigns,buckets> campaign_parts;
        std::array<std::vector<UserRow>,buckets> user_parts;
        struct Partition { std::vector<Event> events; std::vector<Click> clicks; };
        std::array<Partition,buckets> sorted;
        parallel(buckets,threads,[&](size_t b,int) {
            auto& p=sorted[b];
            p.events=collect(events,b);
            p.clicks=collect(clicks,b);
            if(p.events.size()>1) group_owners(p.events.data(),p.events.data()+p.events.size());
            std::sort(p.clicks.begin(),p.clicks.end(),[](const Click& a,const Click& b) {
                if(a.person!=b.person) return a.person<b.person;
                if(a.ts!=b.ts) return a.ts<b.ts;
                return a.id>b.id;
            });
            p.clicks.erase(std::unique(p.clicks.begin(),p.clicks.end(),[](const Click& a,const Click& b) {
                return a.id==b.id;
            }),p.clicks.end());
        });
        timer.lap("sort people and dedup clicks");
        parallel(buckets,threads,[&](size_t b,int) {
            auto& e=sorted[b].events;
            auto& c=sorted[b].clicks;
            size_t ci=0;
            for(size_t i=0,j;i<e.size();i=j) {
                for(j=i+1;j<e.size() && e[j].person==e[i].person;++j) {}
                if(j-i>1) std::sort(e.begin()+i,e.begin()+j,[&](const Event& a,const Event& b) {
                    return a.timestamp(win.start)<b.timestamp(win.start);
                });
                while(ci<c.size() && c[ci].person<e[i].person) ++ci;
                size_t begin=ci;
                while(ci<c.size() && c[ci].person==e[i].person) ++ci;
                const Click* first=begin==ci ? nullptr : c.data()+begin;
                const Click* last=begin==ci ? nullptr : c.data()+ci;
                process_person(e.data()+i,e.data()+j,first,last,dims,campaign_parts[b],user_parts[b],win.start);
            }
            std::vector<Event>().swap(e);
            std::vector<Click>().swap(c);
        });
        timer.lap("sessions, attribution and assists");
        Campaigns all;
        for(auto& part:campaign_parts) for(auto& [key,t]:part) all[key].merge(t);
        parallel(buckets,threads,[&](size_t b,int) {
            if(b==0 || !user_parts[b].empty()) write_users(out/"user_daily"/(std::to_string(b)+".parquet"),user_parts[b],dims.people);
        });
        write_campaigns(out/"campaign_daily"/"part.parquet",all,dims.campaigns);
        timer.lap("Parquet and campaign merge");
    };
    if(uint64_t(win.end-win.start)<=uint64_t(UINT32_MAX)+1 && dims.categories.size()<=256)
        finish(PackedEvent{});
    else
        finish(WideEvent{});
}
}
int main(int argc,char** argv) {
    try {
        run(argc>1?argv[1]:env("DATA_DIR","/data"),argc>2?argv[2]:env("OUTPUT_DIR","/output"),
            std::clamp(std::stoi(env("NCPU","8")),1,64));
    } catch(const std::exception& e) { std::cerr<<e.what()<<"\n"; return 1; }
    return 0;
}
