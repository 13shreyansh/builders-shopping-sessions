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
struct WidePayload {
    I64 time;
    int price,cat;
    I64 timestamp(I64) const { return time; }
    int cents() const { return price; }
    int category() const { return cat; }
};
struct WideEvent:WidePayload {
    using Payload=WidePayload;
    Person person;
    static WideEvent make(Person p,I64 ts,int price,int cat,I64) {return {{ts,price,cat},p};}
};
struct PackedPayload {
    uint64_t payload;
    I64 timestamp(I64 base) const { return base+I64(payload>>32); }
    int cents() const { return int(payload&0xffffffU); }
    int category() const { return int(uint32_t(payload)>>24); }
};
struct PackedEvent:PackedPayload {
    using Payload=PackedPayload;
    Person person;
    static PackedEvent make(Person p,I64 ts,int price,int cat,I64 base) {
        return {{(uint64_t(ts-base)<<32)|(uint64_t(cat)<<24)|unsigned(price)},p};
    }
};
// Radix-sort only sufficiently large compact timelines. Equal times share a session.
void sort_times(PackedPayload* first,size_t n,std::vector<PackedPayload>& scratch,I64) {
    if(n<512) {
        std::sort(first,first+n,[](auto a,auto b){return a.payload<b.payload;});
        return;
    }
    size_t counts[4][256]{};
    uint32_t varying=0,base=uint32_t(first->payload>>32);
    for(size_t i=0;i<n;++i) {
        uint32_t t=uint32_t(first[i].payload>>32);
        varying|=t^base;
        for(unsigned j=0;j<4;++j) ++counts[j][(t>>(j*8))&255];
    }
    scratch.resize(n);
    auto in=first,out=scratch.data();
    for(unsigned j=0;j<4;++j) {
        unsigned shift=j*8;
        if(!((varying>>shift)&255)) continue;
        size_t total=0;
        for(auto& z:counts[j]) {size_t count=z;z=total;total+=count;}
        for(size_t i=0;i<n;++i) out[counts[j][(in[i].payload>>(32+shift))&255]++]=in[i];
        std::swap(in,out);
    }
    if(in!=first) std::copy(in,in+n,first);
}
void sort_times(WidePayload* first,size_t n,std::vector<WidePayload>&,I64) {
    std::sort(first,first+n,[](auto a,auto b){return a.time<b.time;});
}
struct Click {
    Person person; I64 ts; const char* line; int campaign;
    Text id() const {
        const char* end=line;
        while(*end!=',') ++end;
        bool quote=*line=='"';
        return Text(line+quote,size_t(end-line)-2*quote);
    }
};

template<class Event> Event decode_event(const std::array<Text,7>& f,const Dimensions& dims,const Window& win,I64 ingest) {
    if(f[1].empty() || f[5].empty()) return Event::make(UINT64_MAX,win.start,0,0,win.start);
    I64 ts=timestamp(f[2]);
    if(ts<win.start || ts>=win.end || ingest-ts>day_ms) return Event::make(UINT64_MAX,win.start,0,0,win.start);
    int cents=0,category=0;
    if(f[4]=="purchase") {
        I64 price=numeric(f[6]);
        if(price<1 || price>10000000) return Event::make(UINT64_MAX,win.start,0,0,win.start);
        cents=static_cast<int>(price);
        auto it=dims.products.find(f[5]);
        if(it!=dims.products.end()) category=it->second;
    } else if(f[4]!="view" && f[4]!="cart") return Event::make(UINT64_MAX,win.start,0,0,win.start);
    Person who=dims.people.raw(f[1]);
    return Event::make(who,ts,cents,category,win.start);
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
    const Click* latest=nullptr;

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
            if(!latest || latest->ts!=click->ts || click->id()<latest->id()) latest=click;
            scratch.add_click(click->campaign);
            ++click;
        }
        while(expire!=click && expire->ts<start-week_ms) {
            scratch.expire_click(expire->campaign);
            ++expire;
        }
        auto& credit=totals.at(scratch.active.empty()?0:latest->campaign,day);
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
    People event_ids;
    auto finish=[&](auto representation) {
        using Event=decltype(representation);
        struct Version { Person id; I64 ingest; Event event; };
        constexpr size_t shards=256;
        std::vector<std::array<std::vector<Version>,shards>> versions(threads);
        parallel(event_files.chunks.size(),threads,[&](size_t i,int worker) {
            lines(event_files.chunks[i],[&](Text line) {
                std::array<Text,7> f;
                if(!scan_fields(line,f) || f[0].empty()) return;
                Person id=event_ids.raw(f[0]);
                I64 ingest=timestamp(f[3]);
                uint64_t h=OwnerGroups::hash(id);
                versions[worker][h>>(64-8)].push_back({id,ingest,decode_event<Event>(f,dims,win,ingest)});
            });
        });
        timer.lap("decode versions and partition exact event IDs");
        std::vector<Parts<Event>> events(threads);
        parallel(shards,threads,[&](size_t b,int worker) {
            size_t n=0;
            for(auto& src:versions) n+=src[b].size();
            auto keep=[&](const Version& row) {
                if(row.event.person!=UINT64_MAX) events[worker][bucket(row.event.person)].push_back(row.event);
            };
            Fingerprints once,possible;
            once.size_for(n);
            possible.size_for(n);
            for(auto& src:versions) for(auto& row:src[b]) {
                uint64_t hash=OwnerGroups::hash(row.id)*0x9e3779b185ebca87ULL;
                if(once.has(hash)) possible.add(hash);
                once.add(hash);
            }
            std::unordered_map<Person,const Version*> winners;
            for(auto& src:versions) for(auto& row:src[b]) {
                if(!possible.has(OwnerGroups::hash(row.id)*0x9e3779b185ebca87ULL)) { keep(row); continue; }
                auto [it,added]=winners.emplace(row.id,&row);
                if(!added && row.ingest<it->second->ingest) it->second=&row;
            }
            for(const auto& item:winners) keep(*item.second);
            for(auto& src:versions) std::vector<Version>().swap(src[b]);
        });
        timer.lap("select earliest exact versions");
        std::vector<Parts<Click>> clicks(threads);
        parallel(click_files.chunks.size(),threads,[&](size_t i,int worker) {
            lines(click_files.chunks[i],[&](Text line) {
                std::array<Text,4> f;
                if(!scan_fields(line,f) || f[0].empty() || f[1].empty()) return;
                I64 ts=timestamp(f[3]);
                auto it=dims.campaign_ids.find(f[2]);
                if(ts<win.start-week_ms || ts>=win.end || it==dims.campaign_ids.end()) return;
                Person who=dims.people.raw(f[1]);
                clicks[worker][bucket(who)].push_back({who,ts,line.data(),it->second});
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
            using Payload=typename Event::Payload;
            std::vector<Payload> e(ne);
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
            std::vector<Payload> sort_scratch;
            for(auto& g:groups.ranges) {
                auto first=e.begin()+g.event_end-g.events;
                if(g.events>1) sort_times(&*first,g.events,sort_scratch,win.start);
                const Click *first_click=nullptr,*last_click=nullptr;
                if(g.clicks) {
                    Click* lo=c.data()+g.click_end-g.clicks;
                    Click* hi=c.data()+g.click_end;
                    std::sort(lo,hi,[](const Click& a,const Click& b) {
                        return a.ts<b.ts;
                    });
                    first_click=lo;
                    last_click=hi;
                }
                process_person(g.person,&*first,e.data()+g.event_end,first_click,last_click,dims,
                               campaign_parts[worker],users,win.start,scratch[worker]);
            }
            std::vector<Payload>().swap(e);
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
