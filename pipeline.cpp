#include "output.hpp"
#include <chrono>
#include <future>

namespace {
constexpr int buckets=64;
unsigned bucket(Person who) { return (who*0x9e3779b97f4a7c15ULL)>>(64-6); }
template<class T> using Parts=std::array<std::vector<T>,buckets>;
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
    }
    void load_links(MappedFiles& files,const fs::path& root) {
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

template<class Event> void keep_event(const std::array<Text,7>& f,const Dimensions& dims,const Window& win,Parts<Event>& dst) {
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
    Person who=dims.people.raw(f[1]);
    dst[bucket(who)].push_back(Event::make(who,ts,cents,category,win.start));
}

struct DailyScratch {
    std::vector<int> counts,position,active;
    std::vector<I64> categories;
    std::vector<int> used;
    explicit DailyScratch(const Dimensions& dims):
        counts(dims.campaigns.size()),position(dims.campaigns.size()),categories(dims.categories.size()) {}
    void add_click(int id) {
        if(counts[id]++==0) {
            position[id]=active.size();
            active.push_back(id);
        }
    }
    void expire_click(int id) {
        if(--counts[id]) return;
        int last=active.back();
        active[position[id]]=last;
        position[last]=position[id];
        active.pop_back();
    }
    void end_person() {
        for(int id:active) counts[id]=0;
        active.clear();
    }
};
struct CampaignGrid {
    int base=0,days=0;
    std::vector<Totals> dense;
    Campaigns sparse;
    static int day(I64 ts) {return int(ts/day_ms-(ts%day_ms<0));}
    void init(const Window& win,size_t campaigns) {
        base=day(win.start);
        days=day(win.end-1)-base+1;
        if(campaigns<=1048576u/static_cast<unsigned>(days)) dense.resize(campaigns*days);
    }
    Totals& at(int campaign,int date) {
        if(dense.empty()) return sparse[{campaign,date}];
        return dense[size_t(campaign)*days+date-base];
    }
    void merge(Campaigns& all) const {
        if(dense.empty()) {
            for(auto& item:sparse) all[item.first].merge(item.second);
            return;
        }
        for(size_t i=0;i<dense.size();++i) {
            const auto& item=dense[i];
            if(item.sessions || item.assists) all[{int(i/days),int(i%days)+base}].merge(item);
        }
    }
};
template<class Event> void process_person(Person person,const Event* events,const Event* end,const Click* click,const Click* click_end,
                    const Dimensions& dims,CampaignGrid& totals,std::vector<UserRow>& users,I64 base,DailyScratch& scratch) {

    const Click* expire=click;
    int latest=0;

    UserRow row{person,0,0,0,0,{},false};
    auto flush=[&] {
        I64 best=0;
        for(int category:scratch.used) {
            I64 sum=scratch.categories[category];
            Text name=dims.categories[category];
            if(sum>best || (sum==best && name<row.top)) { row.top=name; row.has_top=true; best=sum; }
            scratch.categories[category]=0;
        }
        users.push_back(row);
        scratch.used.clear();
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
            if(events->cents()) {
                int category=events->category();
                if(!scratch.categories[category]) scratch.used.push_back(category);
                scratch.categories[category]+=events->cents();
                sum+=events->cents();
            }
            prev=events->timestamp(base);
            ++events;
        } while(events!=end && events->timestamp(base)-prev<=1800000);
        row.revenue+=sum;
        while(click!=click_end && click->ts<=start) {
            latest=click->campaign;
            scratch.add_click(click->campaign);
            ++click;
        }
        while(expire!=click && expire->ts<start-week_ms) {
            scratch.expire_click(expire->campaign);
            ++expire;
        }
        auto& credit=totals.at(scratch.active.empty()?0:latest,day);
        ++credit.sessions;
        credit.purchases+=sum>0;
        credit.revenue+=sum;
        for(int campaign:scratch.active) ++totals.at(campaign,day).assists;
    }
    flush();
    scratch.end_person();
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
// Equal canonical owners share a contiguous event span and click span.
struct GroupRange {
    Person person;
    size_t events=0,clicks=0;
    size_t event_end=0,click_end=0;
};
struct OwnerGroups {
    // All three person encodings reserve zero.
    struct Slot { Person key=0; uint32_t group=0; };
    std::vector<Slot> slots=std::vector<Slot>(1024);
    std::vector<GroupRange> ranges;
    static uint64_t hash(Person p) {
        p=(p^(p>>31))*0x9e3779b185ebca87ULL;
        return p^(p>>29);
    }
    Slot& locate(Person p) {
        auto i=hash(p)&(slots.size()-1);
        while(slots[i].key && slots[i].key!=p) i=(i+1)&(slots.size()-1);
        return slots[i];
    }
    unsigned intern(Person p) {
        auto& slot=locate(p);
        if(slot.key) return slot.group;
        unsigned id=ranges.size();
        slot={p,id};
        ranges.push_back({p});
        if(ranges.size()*10>slots.size()*7) {
            auto old=std::move(slots);
            slots.assign(old.size()*2,{});
            for(auto& s:old) if(s.key) locate(s.key)=s;
        }
        return id;
    }
};

void run(const fs::path& data,const fs::path& out,int threads) {
    Profile timer;
    Window win(data/"manifest.json");
    MappedFiles dim_files,link_files,event_files,click_files;
    Dimensions dims;
    auto link_job=std::async(threads>1 ? std::launch::async : std::launch::deferred,
                                  [&] { dims.load_links(link_files,data); });
    dims.load(dim_files,data);
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
    auto finish=[&](auto representation) {
        using Event=decltype(representation);
        std::vector<Parts<Event>> events(threads);
        parallel(event_files.chunks.size(),threads,[&](size_t i,int worker) {
            const auto& skip=discarded[i];
            uint32_t ordinal=0;
            lines(event_files.chunks[i],[&](Text line) {
                uint32_t row=ordinal++;
                if(skip[row/64] & (uint64_t(1)<<(row%64))) return;
                std::array<Text,7> f;
                if(!scan_fields(line,f) || f[0].empty()) return;
                keep_event(f,dims,win,events[worker]);
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
                Person who=dims.people.raw(f[1]);
                clicks[worker][bucket(who)].push_back({who,ts,f[0],it->second});
            });
        });
        timer.lap("parse clicks");
        link_job.get();
        std::vector<Parts<Event>> canonical_events(threads);
        std::vector<Parts<Click>> canonical_clicks(threads);
        parallel(buckets,threads,[&](size_t b,int worker) {
            OwnerGroups raw_groups;
            std::vector<Person> owners;
            auto resolve=[&](Person raw) {
                unsigned group=raw_groups.intern(raw);
                if(group==owners.size()) owners.push_back(dims.people.canonical(raw));
                return owners[group];
            };
            auto route=[&](auto& sources,auto& dest) {
                for(auto& part:sources) {
                    for(auto& row:part[b]) {
                        row.person=resolve(row.person);
                        dest[bucket(row.person)].push_back(row);
                    }
                    using Buffer=std::decay_t<decltype(part[b])>;
                    Buffer().swap(part[b]);
                }
            };
            route(events,canonical_events[worker]);
            route(clicks,canonical_clicks[worker]);
        });
        events.swap(canonical_events);
        clicks.swap(canonical_clicks);
        timer.lap("resolve distinct raw owners and route rows");
        std::vector<CampaignGrid> campaign_parts(threads);
        std::vector<DailyScratch> scratch;
        for(auto& part:campaign_parts) {
            part.init(win,dims.campaigns.size());
            scratch.emplace_back(dims);
        }
        parallel(buckets,threads,[&](size_t b,int worker) {
            OwnerGroups groups;
            for(auto& source:events) for(auto& e:source[b]) {
                auto id=groups.intern(e.person);
                ++groups.ranges[id].events;
                e.person=id;
            }
            for(auto& source:clicks) for(auto& c:source[b]) {
                auto& slot=groups.locate(c.person);
                if(!slot.key) { c.person=UINT64_MAX; continue; }
                c.person=slot.group;
                ++groups.ranges[slot.group].clicks;
            }
            size_t ne=0,nc=0;
            for(auto& g:groups.ranges) {
                g.event_end=ne;
                ne+=g.events;
                g.click_end=nc;
                nc+=g.clicks;
            }
            std::vector<Event> e(ne);
            std::vector<Click> c(nc);
            for(auto& source:events) {
                for(auto& x:source[b]) e[groups.ranges[x.person].event_end++]=x;
                std::vector<Event>().swap(source[b]);
            }
            for(auto& source:clicks) {
                for(auto& x:source[b]) if(x.person!=UINT64_MAX) c[groups.ranges[x.person].click_end++]=x;
                std::vector<Click>().swap(source[b]);
            }
            std::vector<UserRow> users;
            for(auto& g:groups.ranges) {
                auto first=e.begin()+g.event_end-g.events,last=e.begin()+g.event_end;
                if(g.events>1) std::sort(first,last,[&](const Event& a,const Event& b) {
                    return a.timestamp(win.start)<b.timestamp(win.start);
                });
                const Click *first_click=nullptr,*last_click=nullptr;
                if(g.clicks) {
                    Click* lo=c.data()+g.click_end-g.clicks;
                    Click* hi=c.data()+g.click_end;
                    std::sort(lo,hi,[](const Click& a,const Click& b) {
                        return a.ts!=b.ts ? a.ts<b.ts : a.id>b.id;
                    });
                    first_click=lo;
                    last_click=std::unique(lo,hi,[](const Click& a,const Click& b){return a.id==b.id;});
                }
                process_person(g.person,&*first,e.data()+g.event_end,first_click,last_click,dims,
                               campaign_parts[worker],users,win.start,scratch[worker]);
            }
            std::vector<Event>().swap(e);
            std::vector<Click>().swap(c);
            if(b==0 || !users.empty()) write_users(out/"user_daily"/(std::to_string(b)+".parquet"),users,dims.people);
        });
        timer.lap("group owners, aggregate sessions and write user Parquet");
        Campaigns all;
        for(auto& part:campaign_parts) part.merge(all);
        write_campaigns(out/"campaign_daily"/"part.parquet",all,dims.campaigns);
        timer.lap("merge campaigns and write campaign Parquet");
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
