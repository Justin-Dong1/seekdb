/* Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0. */
#include "src/oblib/lib/vector/hipvs_hnsw_import.h"
#include <functional>
#include <iostream>
#include <random>
#include <sstream>
#ifdef AMD_TEST_VSAG
#include <vsag/vsag.h>
#include "src/oblib/lib/vector/hipvs_hnsw_snapshot.h"
#endif

using oceanbase::common::obvsag::hipvs::HnswImport;
namespace {
void check(bool condition, const char *message) {
  if (!condition) { throw std::runtime_error(message); }
}
template <typename T> void append(std::vector<char> &out, T value) {
  const char *ptr = reinterpret_cast<const char *>(&value);
  out.insert(out.end(), ptr, ptr + sizeof(value));
}
template <typename T> void patch(std::vector<char> &out, size_t offset, T value) {
  std::memcpy(out.data() + offset, &value, sizeof(value));
}
struct Fixture {
  static constexpr uint64_t N=32, D=8, M=16, DATA=132, LABEL=164, STRIDE=172;
  std::vector<float> vectors;
  std::vector<int64_t> ids;
  std::vector<char> bytes;
  Fixture() {
    std::mt19937 random(2026);
    for (uint64_t i=0; i<N*D; ++i) { vectors.push_back(float(random()%100000)/100000.0f); }
    for (uint64_t i=0; i<N; ++i) { ids.push_back((INT64_C(1)<<40) + 1001 + i*7); }
    for (uint64_t value : {UINT64_C(0),N,N,STRIDE,LABEL,DATA}) { append(bytes,value); }
    append(bytes,int32_t(0)); append(bytes,uint32_t(0));
    append(bytes,M); append(bytes,2*M); append(bytes,M);
    append(bytes,1.0/std::log(double(M))); append(bytes,uint64_t(200));
    for (uint64_t i=0; i<N; ++i) {
      append(bytes,uint32_t(N-1));
      for (uint32_t j=0; j<N; ++j) { if (j!=i) { append(bytes,j); } }
      append(bytes,uint32_t(0)); // one unused slot in the 2*M adjacency array
      const uint64_t row=(i*7)%N; // internal graph order differs from source row order
      for (uint64_t d=0; d<D; ++d) { append(bytes,vectors[row*D+d]); }
      append(bytes,row);
    }
    for (uint64_t i=0; i<N; ++i) { append(bytes,uint32_t(0)); }
  }
};
int rejected=0;
void reject(const std::function<void(Fixture&)> &mutate) {
  Fixture f; mutate(f); const auto original=f.bytes;
  bool failed=false;
  try { HnswImport::relabel(f.bytes,f.vectors.data(),f.ids.data(),f.N,f.D,f.M); }
  catch (const std::invalid_argument&) { failed=true; }
  check(failed,"corrupt fixture was accepted");
  check(f.bytes==original,"failed import modified the file");
  ++rejected;
}
#ifdef AMD_TEST_VSAG
std::vector<char> graph_fixture(const float *vectors, uint64_t rows, uint64_t dim, uint32_t m, uint32_t ef) {
  const uint64_t offset=4+8*m, label=offset+4*dim, stride=label+8;
  std::vector<char> out;
  for(uint64_t value:{UINT64_C(0),rows,rows,stride,label,offset}) { append(out,value); }
  append(out,int32_t(0));append(out,uint32_t(0));
  append(out,uint64_t(m));append(out,uint64_t(2*m));append(out,uint64_t(m));
  append(out,1.0/std::log(double(m)));append(out,uint64_t(ef));
  for(uint64_t i=0;i<rows;++i) {
    std::vector<std::pair<double,uint32_t>> edges;
    for(uint32_t j=0;j<rows;++j) {
      if(i==j) { continue; }
      double d=0;
      for(uint64_t k=0;k<dim;++k) { const double v=double(vectors[i*dim+k])-vectors[j*dim+k];d+=v*v; }
      edges.emplace_back(d,j);
    }
    std::sort(edges.begin(),edges.end());
    const uint32_t count=static_cast<uint32_t>(std::min<uint64_t>(2*m,rows-1));
    append(out,count);
    for(uint32_t j=0;j<2*m;++j) { append(out,j<count?edges[j].second:uint32_t(0)); }
    for(uint64_t j=0;j<dim;++j) { append(out,vectors[i*dim+j]); }
    append(out,i);
  }
  for(uint64_t i=0;i<rows;++i) { append(out,uint32_t(0)); }
  return out;
}
void snapshot_contract() {
  using oceanbase::common::obvsag::hipvs::HnswSnapshot;
  const std::string params=R"({"dtype":"float32","metric_type":"l2","dim":8,"hnsw":{"max_degree":16,"ef_construction":200,"use_conjugate_graph":false}})";
  std::mt19937 rng(1314);
  std::vector<float> vectors(512*8);
  std::vector<int64_t> ids(512);
  for(auto &value:vectors) { value=float(rng()%100000)/100000.0f; }
  for(size_t i=0;i<ids.size();++i) { ids[i]=(INT64_C(1)<<44)+i*17; }
  int calls=0;
  auto builder=[&](const float *v,uint64_t n,uint64_t d,uint32_t m,uint32_t ef) {
    ++calls; return graph_fixture(v,n,d,m,ef);
  };
  HnswSnapshot snapshot(8,16,200,params,nullptr,builder);
  for(size_t i=0;i<512;i+=128) { snapshot.append(vectors.data()+i*8,ids.data()+i,128); }
  check(calls==0&&snapshot.size()==512,"snapshot constructed before finalization");
  check(snapshot.bounds().first==ids.front()&&snapshot.bounds().second==ids.back(),"pending VID bounds mismatch");
  auto native=snapshot.materialize();
  check(calls==1&&native->GetNumElements()==512,"bulk backend not called exactly once");
  check(snapshot.materialize()==native&&calls==1,"repeated materialization rebuilt the graph");
  auto q=vsag::Dataset::Make()->Dim(8)->NumElements(1)->Float32Vectors(vectors.data()+8*31)->Owner(false);
  auto found=native->KnnSearch(q,10,R"({"hnsw":{"ef_search":512}})");
  check(bool(found)&&found.value()->GetIds()[0]==ids[31],"bulk snapshot native query failed");
  bool rejected_append=false;
  try { snapshot.append(vectors.data(),ids.data(),1); } catch(const std::invalid_argument&) { rejected_append=true; }
  check(rejected_append,"finalized snapshot accepted more buffered input");
  int attempts=0;
  HnswSnapshot retry(8,16,200,params,nullptr,[&](const float *v,uint64_t n,uint64_t d,uint32_t m,uint32_t ef) {
    if(++attempts==1) { throw std::runtime_error("injected build failure"); }
    return graph_fixture(v,n,d,m,ef);
  });
  retry.append(vectors.data(),ids.data(),512);
  bool failed=false;
  try { retry.materialize(); } catch(const std::runtime_error&) { failed=true; }
  check(failed&&retry.size()==512,"failed backend changed pending data or silently fell back");
  check(retry.materialize()->GetNumElements()==512&&attempts==2,"failed snapshot could not retry");
  HnswSnapshot small(8,16,200,params,nullptr,[](const float *,uint64_t,uint64_t,uint32_t,uint32_t)->std::vector<char> {
    throw std::runtime_error("small snapshot unexpectedly called GPU backend");
  });
  small.append(vectors.data(),ids.data(),32);
  check(small.materialize()->GetNumElements()==32,"explicit small-snapshot CPU fallback failed");
  int invalid_calls=0;
  HnswSnapshot invalid(8,16,200,params,nullptr,[&](const float *,uint64_t,uint64_t,uint32_t,uint32_t) {
    ++invalid_calls;return std::vector<char>{};
  });
  ids[1]=ids[0];invalid.append(vectors.data(),ids.data(),512);
  bool duplicate=false;
  try { invalid.materialize(); } catch(const std::invalid_argument&) { duplicate=true; }
  check(duplicate&&invalid_calls==0,"duplicate VIDs reached the GPU builder");
  std::cout<<"Snapshot module: batching, one build, failure/retry, fallback, native query and input validation PASS (CPU fixtures)\n";
}
void native_roundtrip(Fixture &f) {
  const std::string params=R"({"dtype":"float32","metric_type":"l2","dim":8,"hnsw":{"max_degree":16,"ef_construction":200,"use_conjugate_graph":false}})";
  auto created=vsag::Factory::CreateIndex("hnsw",params);
  check(bool(created),"native VSAG factory failed");
  std::istringstream stream(std::string(f.bytes.begin(),f.bytes.end()),std::ios::binary);
  auto loaded=created.value()->Deserialize(stream);
  if (!loaded) { throw std::runtime_error(loaded.error().message); }
  check(created.value()->GetNumElements()==f.N,"native row count mismatch");
  auto q=vsag::Dataset::Make()->Dim(f.D)->NumElements(1)
      ->Float32Vectors(f.vectors.data()+5*f.D)->Owner(false);
  auto found=created.value()->KnnSearch(q,10,R"({"hnsw":{"ef_search":100}})");
  check(bool(found)&&found.value()->GetDim()==10,"native search failed");
  std::vector<std::pair<double,int64_t>> expected;
  for (uint64_t i=0;i<f.N;++i) {
    double distance=0;
    for (uint64_t d=0;d<f.D;++d) {
      const double delta=double(f.vectors[i*f.D+d])-f.vectors[5*f.D+d];
      distance+=delta*delta;
    }
    expected.emplace_back(distance,f.ids[i]);
  }
  std::sort(expected.begin(),expected.end());
  for (int i=0;i<10;++i) { check(found.value()->GetIds()[i]==expected[i].second,"native top-k mismatch"); }
  std::stringstream persisted(std::ios::in|std::ios::out|std::ios::binary);
  check(bool(created.value()->Serialize(persisted)),"native serialization failed");
  auto again=vsag::Factory::CreateIndex("hnsw",params);
  check(bool(again)&&bool(again.value()->Deserialize(persisted)),"native reload failed");
  auto after=again.value()->KnnSearch(q,10,R"({"hnsw":{"ef_search":100}})");
  check(bool(after)&&after.value()->GetIds()[0]==f.ids[5],"native reload ID mismatch");
  std::vector<float> extra(f.vectors.begin()+5*f.D,f.vectors.begin()+6*f.D);
  for(float &value:extra) { value+=0.02f; }
  int64_t new_id=(INT64_C(1)<<50)+17;
  auto incremental=vsag::Dataset::Make()->Dim(f.D)->NumElements(1)
      ->Float32Vectors(extra.data())->Ids(&new_id)->Owner(false);
  check(bool(again.value()->Add(incremental)),"native incremental add failed");
  auto extra_q=vsag::Dataset::Make()->Dim(f.D)->NumElements(1)->Float32Vectors(extra.data())->Owner(false);
  auto added=again.value()->KnnSearch(extra_q,1,R"({"hnsw":{"ef_search":100}})");
  check(bool(added)&&added.value()->GetIds()[0]==new_id,"added VID was not queryable");
  check(bool(again.value()->Remove(new_id)),"native delete failed");
  auto removed=again.value()->KnnSearch(extra_q,1,R"({"hnsw":{"ef_search":100}})");
  check(bool(removed)&&removed.value()->GetIds()[0]!=new_id,"deleted VID still returned");
  std::cout<<"Native VSAG: exact top-k, 64-bit/permuted IDs, serialization/reload, add/delete PASS\n";
}
#endif
}
int main() {
  try {
    Fixture f;
    auto stats=HnswImport::relabel(f.bytes,f.vectors.data(),f.ids.data(),f.N,f.D,f.M);
    check(stats.rows==32&&stats.dimension==8,"valid import stats mismatch");
    for(uint64_t i=0;i<f.N;++i) {
      int64_t label; std::memcpy(&label,f.bytes.data()+96+i*f.STRIDE+f.LABEL,8);
      check(label==f.ids[(i*7)%f.N],"permuted business VID mapping failed");
    }
    reject([](Fixture &x){x.bytes.resize(80);});
    reject([](Fixture &x){x.bytes.resize(100);});
    reject([](Fixture &x){x.bytes.pop_back();});
    reject([](Fixture &x){x.bytes.push_back(0);});
    reject([](Fixture &x){patch(x.bytes,16,uint64_t(31));});
    reject([](Fixture &x){patch(x.bytes,8,uint64_t(1));});
    reject([](Fixture &x){patch(x.bytes,56,uint64_t(32));});
    reject([](Fixture &x){patch(x.bytes,40,uint64_t(128));});
    reject([](Fixture &x){patch(x.bytes,48,int32_t(1));});
    reject([](Fixture &x){patch(x.bytes,52,uint32_t(32));});
    reject([](Fixture &x){patch(x.bytes,96+x.LABEL,uint64_t(100));});
    reject([](Fixture &x){patch(x.bytes,96+x.STRIDE+x.LABEL,uint64_t(0));});
    reject([](Fixture &x){patch(x.bytes,96+x.DATA,float(9.0));});
    reject([](Fixture &x){patch(x.bytes,96,uint32_t(65536));});
    reject([](Fixture &x){patch(x.bytes,100,uint32_t(32));});
    reject([](Fixture &x){patch(x.bytes,100,uint32_t(0));});
    reject([](Fixture &x){patch(x.bytes,104,uint32_t(1));});
    reject([](Fixture &x){x.ids[1]=x.ids[0];});
    reject([](Fixture &x){x.vectors[0]=std::numeric_limits<float>::quiet_NaN();patch(x.bytes,96+x.DATA,x.vectors[0]);});
    std::cout<<"Wire validation: valid/permuted import and "<<rejected<<" corrupt/invalid cases PASS\n";
#ifdef AMD_TEST_VSAG
    native_roundtrip(f);
    snapshot_contract();
#endif
  } catch(const std::exception &error) {
    std::cerr<<"FAIL: "<<error.what()<<'\n'; return 1;
  }
  return 0;
}
