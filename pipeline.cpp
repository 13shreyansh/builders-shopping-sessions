#include "output.hpp"
#include <chrono>

namespace {
constexpr int buckets=64;
unsigned bucket(Text key) { return std::hash<Text>{}(key) % buckets; }
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
            if(!fields(line,f)) return;
            auto [it,added]=cat.emplace(f[1],categories.size());
            if(added) categories.push_back(f[1]);
            products.emplace(f[0],it->second);
        });
        files.chunks.clear();
        files.add(root/"campaigns.csv");
        for(Text chunk:files.chunks) lines(chunk,[&](Text line) {
            std::array<Text,2> f;
            if(!fields(line,f)) return;
            campaign_ids.emplace(f[0],campaigns.size());
            campaigns.push_back({f[0],f[1]});
        });
        files.chunks.clear();
        files.folder(root/"links");
        for(Text chunk:files.chunks) lines(chunk,[&](Text line) {
            std::array<Text,2> f;
            if(fields(line,f) && !f[0].empty() && !f[1].empty()) people.join(f[0],f[1]);
        });
        people.finish();
    }
};
struct Raw { Text id,ingest,line; };
struct Event { Text person; I64 ts; int cents,category; };
struct Click { Text person; I64 ts; Text id; int campaign; };

void keep_event(const Raw& row,const Dimensions& dims,const Window& win,Parts<Event>& dst) {
    std::array<Text,7> f;
    if(!fields(row.line,f)) throw std::runtime_error("invalid accepted CSV row");
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
    Text who=dims.people(f[1]);
    dst[bucket(who)].push_back({who,ts,cents,category});
}

void process_person(const Event* events,const Event* end,const Click* click,const Click* click_end,
                    const Dimensions& dims,Campaigns& totals,std::vector<UserRow>& users) {
    const Text person=events->person;
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
        I64 start=events->ts;
        int day=static_cast<int>(start/day_ms-(start%day_ms<0));
        if(!row.sessions || row.day!=day) {
            if(row.sessions) flush();
            row={person,day,0,0,0,{},false};
        }
        ++row.sessions;
        I64 prev=start,sum=0;
        do {
            ++row.events;
            if(events->cents) { sum+=events->cents; cat_totals[events->category]+=events->cents; }
            prev=events->ts;
            ++events;
        } while(events!=end && events->ts-prev<=1800000);
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
void run(const fs::path& data,const fs::path& out,int threads) {
    Profile timer;
    Window win(data/"manifest.json");
    MappedFiles dim_files,event_files,click_files;
    Dimensions dims;
    dims.load(dim_files,data);
    event_files.folder(data/"events");
    click_files.folder(data/"clicks");
    fs::create_directories(out/"user_daily");
    fs::create_directories(out/"campaign_daily");
    timer.lap("dimensions and mapping");
    std::vector<Parts<Raw>> raw(threads);
    parallel(event_files.chunks.size(),threads,[&](size_t i,int worker) {
        lines(event_files.chunks[i],[&](Text line) {
            std::array<Text,7> f;
            if(fields(line,f) && !f[0].empty())
                raw[worker][bucket(f[0])].push_back({f[0],f[3],line});
        });
    });
    timer.lap("parse and partition raw events");
    std::vector<Parts<Event>> events(threads);
    parallel(buckets,threads,[&](size_t b,int worker) {
        std::unordered_map<Text,const Raw*> first;
        size_t n=0;
        for(auto& src:raw) n+=src[b].size();
        first.reserve(n);
        for(auto& src:raw) for(const auto& row:src[b]) {
            auto [it,added]=first.emplace(row.id,&row);
            if(!added && row.ingest<it->second->ingest) it->second=&row;
        }
        for(auto [key,row]:first) keep_event(*row,dims,win,events[worker]);
        first.clear();
        for(auto& src:raw) std::vector<Raw>().swap(src[b]);
    });
    timer.lap("dedup and validate events");
    std::vector<Parts<Click>> clicks(threads);
    parallel(click_files.chunks.size(),threads,[&](size_t i,int worker) {
        lines(click_files.chunks[i],[&](Text line) {
            std::array<Text,4> f;
            if(!fields(line,f) || f[0].empty() || f[1].empty()) return;
            I64 ts=timestamp(f[3]);
            auto it=dims.campaign_ids.find(f[2]);
            if(ts<win.start-week_ms || ts>=win.end || it==dims.campaign_ids.end()) return;
            Text who=dims.people(f[1]);
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
        std::sort(p.events.begin(),p.events.end(),[](const Event& a,const Event& b) {
            return a.person!=b.person ? a.person<b.person : a.ts<b.ts;
        });
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
            while(ci<c.size() && c[ci].person<e[i].person) ++ci;
            size_t begin=ci;
            while(ci<c.size() && c[ci].person==e[i].person) ++ci;
            const Click* first=begin==ci ? nullptr : c.data()+begin;
            const Click* last=begin==ci ? nullptr : c.data()+ci;
            process_person(e.data()+i,e.data()+j,first,last,dims,campaign_parts[b],user_parts[b]);
        }
        std::vector<Event>().swap(e);
        std::vector<Click>().swap(c);
    });
    timer.lap("sessions, attribution and assists");
    Campaigns all;
    for(auto& part:campaign_parts) for(auto& [key,t]:part) all[key].merge(t);
    parallel(buckets,threads,[&](size_t b,int) {
        if(b==0 || !user_parts[b].empty()) write_users(out/"user_daily"/(std::to_string(b)+".parquet"),user_parts[b]);
    });
    write_campaigns(out/"campaign_daily"/"part.parquet",all,dims.campaigns);
    timer.lap("Parquet and campaign merge");
}
}
int main(int argc,char** argv) {
    try {
        run(argc>1?argv[1]:env("DATA_DIR","/data"),argc>2?argv[2]:env("OUTPUT_DIR","/output"),
            std::clamp(std::stoi(env("NCPU","8")),1,64));
    } catch(const std::exception& e) { std::cerr<<e.what()<<"\n"; return 1; }
    return 0;
}
