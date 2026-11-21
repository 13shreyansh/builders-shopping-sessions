#pragma once
#include "input.hpp"
#include <arrow/api.h>
#include <arrow/io/file.h>
#include <parquet/arrow/writer.h>
#include <parquet/column_writer.h>
#include <parquet/file_writer.h>

inline void check(arrow::Status s) {
    if (!s.ok()) throw std::runtime_error(s.ToString());
}
template<class T> T take(arrow::Result<T> r) {
    check(r.status());
    return std::move(r).ValueOrDie();
}

struct UserRow { Person user; int day; int sessions; int events; I64 revenue; Text top; bool has_top; };
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
    auto sink=take(::arrow::io::FileOutputStream::Open(path.string()));
    check(parquet::arrow::WriteTable(*table,arrow::default_memory_pool(),sink,65536));
    check(sink->Close());
}

inline std::shared_ptr<parquet::schema::GroupNode> user_schema() {
    using namespace parquet;
    using schema::PrimitiveNode;
    static const auto schema=std::static_pointer_cast<schema::GroupNode>(schema::GroupNode::Make(
        "schema",Repetition::REQUIRED,{
            PrimitiveNode::Make("user_id",Repetition::REQUIRED,Type::BYTE_ARRAY,ConvertedType::UTF8),
            PrimitiveNode::Make("day",Repetition::REQUIRED,Type::INT32,ConvertedType::DATE),
            PrimitiveNode::Make("sessions",Repetition::REQUIRED,Type::INT32),
            PrimitiveNode::Make("events",Repetition::REQUIRED,Type::INT32),
            PrimitiveNode::Make("revenue_cents",Repetition::REQUIRED,Type::INT64),
            PrimitiveNode::Make("top_category",Repetition::OPTIONAL,Type::BYTE_ARRAY,ConvertedType::UTF8)
        }));
    return schema;
}
inline void write_users(const fs::path& path,const std::vector<UserRow>& rows,const People& people) {
    using namespace parquet;
    auto sink=take(::arrow::io::FileOutputStream::Open(path.string()));
    static const auto properties=WriterProperties::Builder().disable_statistics()->disable_dictionary()->build();
    auto writer=ParquetFileWriter::Open(sink,user_schema(),properties);
    for(size_t base=0;base<rows.size();base+=65536) {
        size_t n=std::min<size_t>(65536,rows.size()-base);
        auto group=writer->AppendRowGroup();
        std::vector<ByteArray> text(n);
        std::vector<std::array<char,9>> names(n);
        for(size_t i=0;i<n;++i) text[i]=people.name(rows[base+i].user,names[i]);
        static_cast<ByteArrayWriter*>(group->NextColumn())->WriteBatch(n,nullptr,nullptr,text.data());
        std::vector<int32_t> ints(n);
        auto write_int=[&](int UserRow::* field) {
            for(size_t i=0;i<n;++i) ints[i]=rows[base+i].*field;
            static_cast<Int32Writer*>(group->NextColumn())->WriteBatch(n,nullptr,nullptr,ints.data());
        };
        write_int(&UserRow::day);
        write_int(&UserRow::sessions);
        write_int(&UserRow::events);
        std::vector<int64_t> revenue(n);
        for(size_t i=0;i<n;++i) revenue[i]=rows[base+i].revenue;
        static_cast<Int64Writer*>(group->NextColumn())->WriteBatch(n,nullptr,nullptr,revenue.data());
        std::vector<int16_t> present(n);
        size_t values=0;
        for(size_t i=0;i<n;++i) {
            const auto& row=rows[base+i];
            present[i]=row.has_top;
            if(row.has_top) text[values++]=row.top;
        }
        static_cast<ByteArrayWriter*>(group->NextColumn())->WriteBatch(n,present.data(),nullptr,text.data());
        group->Close();
    }
    writer->Close();
    check(sink->Close());
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
