#pragma once
#include "input.hpp"
#include <arrow/api.h>
#include <arrow/io/file.h>
#include <parquet/arrow/writer.h>

inline void check(arrow::Status s) {
    if (!s.ok()) throw std::runtime_error(s.ToString());
}
template<class T> T take(arrow::Result<T> r) {
    check(r.status());
    return std::move(r).ValueOrDie();
}

struct UserRow { Text user; int day; int sessions; int events; I64 revenue; Text top; bool has_top; };
struct Totals {
    I64 sessions=0, purchases=0, revenue=0, assists=0;
    void merge(const Totals& o) {
        sessions+=o.sessions; purchases+=o.purchases; revenue+=o.revenue; assists+=o.assists;
    }
};
using CampaignDay = std::pair<int,int>;
struct KeyHash {
    size_t operator()(CampaignDay k) const {
        return std::hash<uint64_t>{}((uint64_t(uint32_t(k.first))<<32)|uint32_t(k.second));
    }
};
using Campaigns = std::unordered_map<CampaignDay,Totals,KeyHash>;
struct Campaign { Text id,channel; };

inline void parquet_file(const fs::path& path, const std::vector<std::string>& names,
                         std::initializer_list<arrow::ArrayBuilder*> builders) {
    std::vector<std::shared_ptr<arrow::Array>> arrays;
    std::vector<std::shared_ptr<arrow::Field>> fields;
    for(auto* builder:builders) {
        arrays.push_back(take(builder->Finish()));
        fields.push_back(arrow::field(names[fields.size()],arrays.back()->type()));
    }
    auto table=arrow::Table::Make(arrow::schema(fields),arrays);
    auto sink=take(arrow::io::FileOutputStream::Open(path.string()));
    check(parquet::arrow::WriteTable(*table,arrow::default_memory_pool(),sink,65536));
    check(sink->Close());
}

inline void write_users(const fs::path& path,const std::vector<UserRow>& rows) {
    arrow::StringBuilder user,top;
    arrow::Date32Builder day;
    arrow::Int32Builder sessions,events;
    arrow::Int64Builder revenue;
    for (const auto& r:rows) {
        check(user.Append(r.user)); check(day.Append(r.day)); check(sessions.Append(r.sessions));
        check(events.Append(r.events)); check(revenue.Append(r.revenue));
        if(r.has_top) check(top.Append(r.top)); else check(top.AppendNull());
    }
    parquet_file(path,{"user_id","day","sessions","events","revenue_cents","top_category"},
                  {&user,&day,&sessions,&events,&revenue,&top});
}
inline void write_campaigns(const fs::path& path,const Campaigns& counts,const std::vector<Campaign>& dims) {
    arrow::StringBuilder id,channel;
    arrow::Date32Builder day;
    arrow::Int64Builder sessions,purchases,revenue,assists;
    for (auto& [key,r]:counts) {
        auto& dim=dims[key.first];
        check(id.Append(dim.id)); check(channel.Append(dim.channel)); check(day.Append(key.second));
        check(sessions.Append(r.sessions)); check(purchases.Append(r.purchases)); check(revenue.Append(r.revenue)); check(assists.Append(r.assists));
    }
    parquet_file(path,{"campaign_id","channel","day","sessions","purchasing_sessions","revenue_cents","assisted_sessions"},
                  {&id,&channel,&day,&sessions,&purchases,&revenue,&assists});
}
