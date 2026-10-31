/* Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0. */
#pragma once
#include "hipvs_hnsw_import.h"
#include <vsag/vsag.h>
#include <cstdio>
#include <functional>
#include <memory>
#include <sstream>
#include <string>

namespace oceanbase { namespace common { namespace obvsag { namespace hipvs {

// Snapshot construction only: callers serialize access while accumulating and
// publishing a snapshot. The bridge builds graph structure; native VSAG owns the
// final index and all subsequent queries, persistence, and incremental updates.
class HnswSnapshot {
public:
  using Build = std::function<std::vector<char>(const float *, uint64_t, uint64_t, uint32_t, uint32_t)>;
  HnswSnapshot(uint32_t dim, uint32_t degree, uint32_t ef,
               std::string parameters, vsag::Allocator *allocator, Build build)
      : dim_(dim), degree_(degree), ef_(ef), parameters_(std::move(parameters)),
        allocator_(allocator), build_(std::move(build))
  {
    if (!dim || degree < 2 || ef < degree || !build_) {
      throw std::invalid_argument("invalid hipVS snapshot configuration");
    }
  }
  uint64_t size() const { return ids_.size(); }
  std::pair<int64_t, int64_t> bounds() const
  {
    if (ids_.empty()) { throw std::invalid_argument("empty hipVS snapshot bounds"); }
    const auto found = std::minmax_element(ids_.begin(), ids_.end());
    return {*found.first, *found.second};
  }
  void append(const float *vectors, const int64_t *ids, uint64_t count)
  {
    if (index_ || !vectors || !ids || count > INT32_MAX || ids_.size() > INT32_MAX - count ||
        count > (SIZE_MAX - vectors_.size()) / dim_) {
      throw std::invalid_argument("invalid/oversized/finalized hipVS snapshot append");
    }
    const size_t needed_vectors = vectors_.size() + static_cast<size_t>(count) * dim_;
    const size_t needed_ids = ids_.size() + static_cast<size_t>(count);
    if (needed_vectors > vectors_.capacity()) {
      const size_t grown = vectors_.capacity() > SIZE_MAX / 2 ? needed_vectors : 2 * vectors_.capacity();
      vectors_.reserve(std::max(needed_vectors, grown));
    }
    if (needed_ids > ids_.capacity()) { ids_.reserve(std::max(needed_ids, 2 * ids_.capacity())); }
    // Both reserves succeeded; inserting trivial scalar types does not allocate.
    vectors_.insert(vectors_.end(), vectors, vectors + static_cast<size_t>(count) * dim_);
    ids_.insert(ids_.end(), ids, ids + count);
  }
  std::shared_ptr<vsag::Index> materialize()
  {
    if (index_) { return index_; }
    if (ids_.empty()) { throw std::invalid_argument("cannot build an empty hipVS snapshot"); }
    std::unordered_set<int64_t> unique;
    unique.reserve(ids_.size());
    for (const int64_t id : ids_) {
      if (!unique.insert(id).second) { throw std::invalid_argument("duplicate snapshot VID"); }
    }
    for (const float value : vectors_) {
      if (!std::isfinite(value)) { throw std::invalid_argument("non-finite snapshot vector"); }
    }
    auto made = vsag::Factory::CreateIndex("hnsw", parameters_, allocator_);
    if (!made) { throw std::runtime_error(made.error().message); }
    const uint64_t rows = ids_.size();
    if (rows < 256 || rows <= 4UL * degree_) {
      std::fprintf(stderr, "SEEKDB_HIPVS phase=cpu_fallback reason=small_snapshot rows=%llu\n",
                   static_cast<unsigned long long>(rows));
      auto base = vsag::Dataset::Make()->Dim(dim_)->NumElements(rows)
          ->Ids(ids_.data())->Float32Vectors(vectors_.data())->Owner(false);
      auto built = made.value()->Build(base);
      if (!built) { throw std::runtime_error(built.error().message); }
      if (!built.value().empty()) { throw std::runtime_error("incomplete CPU fallback snapshot"); }
    } else {
      auto bytes = build_(vectors_.data(), rows, dim_, degree_, ef_);
      auto stats = HnswImport::relabel(bytes, vectors_.data(), ids_.data(), rows, dim_, degree_);
      std::istringstream input(std::string(bytes.begin(), bytes.end()), std::ios::binary);
      auto loaded = made.value()->Deserialize(input);
      if (!loaded) { throw std::runtime_error(loaded.error().message); }
      std::fprintf(stderr, "SEEKDB_HIPVS phase=native_import_complete rows=%llu dim=%llu max_level=%d cpu_fallback=0\n",
          static_cast<unsigned long long>(stats.rows), static_cast<unsigned long long>(stats.dimension), stats.max_level);
      std::fflush(stderr);
    }
    if (made.value()->GetNumElements() != static_cast<int64_t>(rows)) {
      throw std::runtime_error("native snapshot row count mismatch");
    }
    index_ = made.value(); // publication only after the entire import has succeeded
    return index_;
  }

private:
  uint32_t dim_, degree_, ef_;
  std::string parameters_;
  vsag::Allocator *allocator_;
  Build build_;
  std::vector<float> vectors_;
  std::vector<int64_t> ids_;
  std::shared_ptr<vsag::Index> index_;
};

}}}} // namespace oceanbase::common::obvsag::hipvs
