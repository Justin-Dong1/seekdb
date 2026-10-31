/* Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0. */
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <unordered_set>
#include <vector>

namespace oceanbase { namespace common { namespace obvsag { namespace hipvs {

struct HnswImportStats {
  uint64_t rows;
  uint64_t dimension;
  uint64_t max_degree;
  int32_t max_level;
};

// This is the 64-bit little-endian hnswlib wire format emitted by hipVS
// cuvsHnswSerialize after from_cagra(hierarchy=CPU). NONE is not compatible.
// Validate the whole file before changing labels. Graph offsets remain internal
// node offsets; only external labels are translated back to seekdb VIDs.
class HnswImport {
public:
  static HnswImportStats relabel(std::vector<char> &bytes,
                                const float *vectors, const int64_t *ids,
                                uint64_t rows, uint64_t dimension, uint64_t degree)
  {
    const uint16_t endian = 1;
    require(*reinterpret_cast<const uint8_t *>(&endian) == 1,
            "HNSW import requires little-endian host");
    require(sizeof(float) == 4 && std::numeric_limits<float>::is_iec559,
            "HNSW import requires IEEE float32");
    require(vectors && ids && rows > 0 && rows <= UINT32_MAX && dimension > 0,
            "invalid HNSW import input");
    require(degree >= 2 && degree <= 1024, "unsupported HNSW degree");
    checked_mul(rows, dimension);
    require(bytes.size() >= HEADER_SIZE, "truncated HNSW header");
    const uint64_t max_elements = read<uint64_t>(bytes, 8);
    const uint64_t stride = read<uint64_t>(bytes, 24);
    const uint64_t label_offset = read<uint64_t>(bytes, 32);
    const uint64_t data_offset = read<uint64_t>(bytes, 40);
    const int32_t max_level = read<int32_t>(bytes, 48);
    const uint32_t entry = read<uint32_t>(bytes, 52);
    require(read<uint64_t>(bytes, 0) == 0, "unsupported level-zero offset");
    require(read<uint64_t>(bytes, 16) == rows && max_elements >= rows,
            "HNSW row count mismatch");
    require(read<uint64_t>(bytes, 56) == degree &&
            read<uint64_t>(bytes, 64) == 2 * degree &&
            read<uint64_t>(bytes, 72) == degree, "HNSW degree mismatch");
    const double multiplier = read<double>(bytes, 80);
    require(std::isfinite(multiplier) && std::abs(multiplier - 1.0 / std::log(double(degree))) < 1e-12 &&
            read<uint64_t>(bytes, 88) >= degree, "invalid HNSW construction parameters");
    require(max_level >= 0 && max_level <= 64 && entry < rows,
            "invalid HNSW hierarchy header");
    const uint64_t vector_bytes = checked_mul(dimension, sizeof(float));
    require(data_offset == 4 + 2 * degree * 4 &&
            label_offset == checked_add(data_offset, vector_bytes) &&
            stride == checked_add(label_offset, 8), "HNSW vector layout mismatch");
    const uint64_t data_end = checked_add(HEADER_SIZE, checked_mul(rows, stride));
    require(data_end <= bytes.size(), "truncated HNSW vectors");
    std::unordered_set<int64_t> unique_ids;
    unique_ids.reserve(static_cast<size_t>(rows));
    for (uint64_t i = 0; i < rows; ++i) {
      require(unique_ids.insert(ids[i]).second, "duplicate input VID");
    }
    std::vector<uint8_t> seen(static_cast<size_t>(rows), 0);
    std::vector<uint32_t> labels(static_cast<size_t>(rows));
    std::vector<uint32_t> levels(static_cast<size_t>(rows));
    std::vector<uint64_t> upper_offsets(static_cast<size_t>(rows));
    std::vector<uint32_t> scratch;
    scratch.reserve(static_cast<size_t>(2 * degree));
    for (uint64_t i = 0; i < rows; ++i) {
      const uint64_t start = HEADER_SIZE + i * stride;
      const uint64_t label = read<uint64_t>(bytes, start + label_offset);
      require(label < rows && !seen[static_cast<size_t>(label)],
              "HNSW labels must be a permutation of input rows");
      seen[static_cast<size_t>(label)] = 1;
      labels[static_cast<size_t>(i)] = static_cast<uint32_t>(label);
      require(std::memcmp(bytes.data() + start + data_offset,
                          vectors + label * dimension, static_cast<size_t>(vector_bytes)) == 0,
              "HNSW vectors do not match input row labels");
      for (uint64_t d = 0; d < dimension; ++d) {
        require(std::isfinite(vectors[label * dimension + d]), "non-finite vector value");
      }
      validate_edges(bytes, start, 2 * degree, i, rows, scratch);
    }
    uint64_t cursor = data_end;
    int32_t observed_max_level = 0;
    const uint64_t upper_stride = 4 + degree * 4;
    for (uint64_t i = 0; i < rows; ++i) {
      const uint32_t length = read<uint32_t>(bytes, cursor);
      cursor = checked_add(cursor, 4);
      require(length % upper_stride == 0 && length / upper_stride <= 64,
              "invalid HNSW upper-layer length");
      upper_offsets[static_cast<size_t>(i)] = cursor;
      levels[static_cast<size_t>(i)] = static_cast<uint32_t>(length / upper_stride);
      observed_max_level = std::max(observed_max_level,
                                    static_cast<int32_t>(length / upper_stride));
      cursor = checked_add(cursor, length);
      require(cursor <= bytes.size(), "truncated HNSW upper layers");
    }
    require(cursor == bytes.size(), "unexpected HNSW trailing data");
    require(observed_max_level == max_level && levels[entry] == static_cast<uint32_t>(max_level),
            "HNSW entry point does not match hierarchy");
    for (uint64_t i = 0; i < rows; ++i) {
      for (uint32_t level = 1; level <= levels[static_cast<size_t>(i)]; ++level) {
        const uint64_t start = upper_offsets[static_cast<size_t>(i)] + (level - 1) * upper_stride;
        validate_edges(bytes, start, degree, i, rows, scratch);
        for (uint32_t neighbor : scratch) {
          require(levels[neighbor] >= level, "HNSW edge targets an absent upper layer");
        }
      }
    }
    // Only commit the label rewrite once every structural check has passed.
    // Spare serialized capacity is not data and must not amplify host allocation.
    std::memcpy(bytes.data() + 8, &rows, sizeof(rows));
    for (uint64_t i = 0; i < rows; ++i) {
      const int64_t id = ids[labels[static_cast<size_t>(i)]];
      std::memcpy(bytes.data() + HEADER_SIZE + i * stride + label_offset, &id, sizeof(id));
    }
    return {rows, dimension, degree, max_level};
  }

private:
  static constexpr uint64_t HEADER_SIZE = 96;
  static void require(bool condition, const char *message)
  {
    if (!condition) { throw std::invalid_argument(message); }
  }
  static uint64_t checked_mul(uint64_t a, uint64_t b)
  {
    require(b == 0 || a <= UINT64_MAX / b, "HNSW size overflow");
    return a * b;
  }
  static uint64_t checked_add(uint64_t a, uint64_t b)
  {
    require(a <= UINT64_MAX - b, "HNSW size overflow");
    return a + b;
  }
  template <typename T> static T read(const std::vector<char> &bytes, uint64_t offset)
  {
    require(offset <= bytes.size() && sizeof(T) <= bytes.size() - offset,
            "truncated HNSW record");
    T value;
    std::memcpy(&value, bytes.data() + offset, sizeof(value));
    return value;
  }
  static void validate_edges(const std::vector<char> &bytes, uint64_t offset,
                             uint64_t capacity, uint64_t self, uint64_t rows,
                             std::vector<uint32_t> &scratch)
  {
    const uint32_t count = read<uint32_t>(bytes, offset);
    require(count <= capacity, "invalid/deleted HNSW link record");
    scratch.clear();
    for (uint32_t i = 0; i < count; ++i) {
      const uint32_t neighbor = read<uint32_t>(bytes, offset + 4 + i * 4);
      require(neighbor < rows && neighbor != self, "invalid HNSW edge target");
      scratch.push_back(neighbor);
    }
    std::sort(scratch.begin(), scratch.end());
    require(std::adjacent_find(scratch.begin(), scratch.end()) == scratch.end(),
            "duplicate HNSW graph edge");
  }
};

}}}} // namespace oceanbase::common::obvsag::hipvs
