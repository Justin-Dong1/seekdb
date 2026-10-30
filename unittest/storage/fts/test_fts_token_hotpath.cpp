/*
 * Copyright (c) 2025 OceanBase.
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 * http://www.apache.org/licenses/LICENSE-2.0
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#include "storage/fts/ob_fts_stop_word.h"
#include "storage/fts/ik/ob_ik_char_util.h"
#include "storage/fts/ik/ob_ik_processor.h"
#include "storage/fts/ob_fts_parser_property.h"
#ifdef FTS_HOTPATH_STANDALONE
#include <cstdio>
#include <cstdlib>
#include <chrono>
#define ASSERT_EQ(a, b) do { const auto expected = (a); const auto actual = (b); if (expected != actual) { std::fprintf(stderr, "failure at %d: %lld != %lld\n", __LINE__, static_cast<long long>(expected), static_cast<long long>(actual)); std::abort(); } } while (0)
#define TEST(suite, name) void name()
#else
#include <gtest/gtest.h>
#endif

using namespace oceanbase::common;
using namespace oceanbase::storage;

static ObObjMeta token_meta(ObCollationType collation)
{
  ObObjMeta meta;
  meta.set_varchar();
  meta.set_collation_type(collation);
  return meta;
}

TEST(FTSTokenHotpath, stopword_collations_and_errors)
{
  ObStopWordChecker checker;
  bool found = false;
  const ObObjMeta utf8 = token_meta(CS_TYPE_UTF8MB4_GENERAL_CI);
  ASSERT_EQ(OB_NOT_INIT, checker.check_stopword(ObFTWord(3, "the", utf8), found));
  ASSERT_EQ(OB_SUCCESS, checker.init());
  ASSERT_EQ(OB_INVALID_ARGUMENT, checker.check_stopword(ObFTWord(), found));
  const ObCollationType collations[] = {
      CS_TYPE_UTF8MB4_GENERAL_CI, CS_TYPE_UTF8MB4_BIN, CS_TYPE_UTF8MB4_UNICODE_CI, CS_TYPE_BINARY};
  for (const auto collation : collations) {
    const ObObjMeta meta = token_meta(collation);
    for (const auto &stopword : ob_stop_word_list) {
      ASSERT_EQ(OB_SUCCESS, checker.check_stopword(ObFTWord(STRLEN(stopword), stopword, meta), found));
      ASSERT_EQ(true, found);
    }
    ASSERT_EQ(OB_SUCCESS, checker.check_stopword(ObFTWord(3, "THE", meta), found));
    ASSERT_EQ(true, found);
    ASSERT_EQ(OB_SUCCESS, checker.check_stopword(ObFTWord(8, "database", meta), found));
    ASSERT_EQ(false, found);
  }
  // The stopword collation is accent-insensitive even for binary source collation.
  ASSERT_EQ(OB_SUCCESS, checker.check_stopword(
      ObFTWord(2, "\xc3\xa1", token_meta(CS_TYPE_UTF8MB4_BIN)), found));
  ASSERT_EQ(true, found);
  ASSERT_EQ(OB_INVALID_ARGUMENT, checker.check_stopword(
      ObFTWord(1, "\xe1", token_meta(CS_TYPE_LATIN1_SWEDISH_CI)), found));
  ASSERT_EQ(OB_INVALID_ARGUMENT, checker.check_stopword(
      ObFTWord(3, "the", token_meta(CS_TYPE_INVALID)), found));
}

TEST(FTSTokenHotpath, grouping_preserves_frequency_and_last_spelling)
{
  ObArenaAllocator allocator;
  ObFTWordMap words;
  ASSERT_EQ(OB_SUCCESS, words.create(37, "FtsTest", "FtsTest"));
  ObFTParserProperty property;
  ObAddWordFlag flags;
  flags.set_groupby_word();
  ObObjMeta meta = token_meta(CS_TYPE_UTF8MB4_GENERAL_CI);
  ObAddWord add(property, meta, flags, allocator, words);
  ASSERT_EQ(OB_SUCCESS, add.process_word("Alpha", 5, 5, 3));
  ASSERT_EQ(OB_SUCCESS, add.process_word("alpha", 5, 5, 2));
  ASSERT_EQ(OB_SUCCESS, add.process_word("ALPHA", 5, 5, 4));
  int64_t frequency = 0;
  ASSERT_EQ(OB_SUCCESS, words.get_refactored(ObFTWord(5, "alpha", meta), frequency));
  ASSERT_EQ(7, frequency); // Existing semantics: first occurrence is one, subsequent ones add frequency.
  ASSERT_EQ(9, add.get_add_word_count());
  ASSERT_EQ(1, words.size());
  ASSERT_EQ(0, MEMCMP(words.begin()->first.get_word().ptr_, "ALPHA", 5));
  ASSERT_EQ(OB_INVALID_ARGUMENT, add.process_word("alpha", 5, 5, 0));
  ASSERT_EQ(OB_SUCCESS, words.reuse());
  ASSERT_EQ(OB_SUCCESS, add.process_word("alpha", 5, 5, 1));
  ASSERT_EQ(OB_SUCCESS, words.get_refactored(ObFTWord(5, "alpha", meta), frequency));
  ASSERT_EQ(1, frequency);
  words.destroy();
}

TEST(FTSTokenHotpath, non_grouped_duplicates_and_uninitialized_map)
{
  ObArenaAllocator allocator;
  ObFTWordMap words;
  ObFTParserProperty property;
  ObAddWordFlag flags;
  flags.set_groupby_word();
  const ObObjMeta meta = token_meta(CS_TYPE_UTF8MB4_BIN);
  ObAddWord grouped(property, meta, flags, allocator, words);
  ASSERT_EQ(OB_NOT_INIT, grouped.process_word("word", 4, 4, 1));
  ASSERT_EQ(OB_SUCCESS, words.create(37, "FtsTest", "FtsTest"));
  flags.clear();
  ObAddWord ungrouped(property, meta, flags, allocator, words);
  ASSERT_EQ(OB_SUCCESS, ungrouped.process_word("word", 4, 4, 1));
  ASSERT_EQ(OB_HASH_EXIST, ungrouped.process_word("word", 4, 4, 1));
  ASSERT_EQ(OB_SUCCESS, ungrouped.process_word("WORD", 4, 4, 1));
  ASSERT_EQ(2, words.size());
  words.destroy();
}

TEST(FTSTokenHotpath, unicode_classification_and_ascii_cursor)
{
  using Type = ObFTCharUtil::CharType;
  // Exhaust all valid scalar values, including supplementary CJK and emoji.
  for (uint32_t cp = 0; cp <= 0x10ffff; ++cp) {
    if (cp >= 0xd800 && cp <= 0xdfff) { continue; }
    char bytes[4]; uint8_t n = 0;
    if (cp < 0x80) { bytes[n++] = cp; }
    else if (cp < 0x800) { bytes[n++] = 0xc0 | (cp >> 6); bytes[n++] = 0x80 | (cp & 63); }
    else if (cp < 0x10000) { bytes[n++] = 0xe0 | (cp >> 12); bytes[n++] = 0x80 | ((cp >> 6) & 63); bytes[n++] = 0x80 | (cp & 63); }
    else { bytes[n++] = 0xf0 | (cp >> 18); bytes[n++] = 0x80 | ((cp >> 12) & 63); bytes[n++] = 0x80 | ((cp >> 6) & 63); bytes[n++] = 0x80 | (cp & 63); }
    Type reference_type = ObUnicodeBlockUtils::is_alpha(cp) ? Type::ENGLISH_LETTER
        : ObUnicodeBlockUtils::is_arabic(cp) ? Type::ARABIC_LETTER
        : ObUnicodeBlockUtils::is_chinese(cp) ? Type::CHINESE
        : ObUnicodeBlockUtils::is_other_cjk(cp) ? Type::OTHER_CJK : Type::USELESS;
    Type parsed_type;
    ASSERT_EQ(OB_SUCCESS, ObFTCharUtil::classify_first_char(CS_TYPE_UTF8MB4_BIN, bytes, n, parsed_type));
    ASSERT_EQ(reference_type, parsed_type);
    if (cp < 128) {
      ObArenaAllocator allocator;
      TokenizeContext ctx(CS_TYPE_UTF8MB4_BIN, allocator, bytes, n, false);
      ASSERT_EQ(OB_SUCCESS, ctx.init());
      ASSERT_EQ(OB_SUCCESS, ctx.current_char_type(parsed_type));
      ASSERT_EQ(reference_type, parsed_type);
      ASSERT_EQ(OB_ITER_END, ctx.step_next());
    }
  }
}

#ifdef FTS_HOTPATH_STANDALONE
static void benchmark_hotpath()
{
  ObStopWordChecker checker;
  ASSERT_EQ(OB_SUCCESS, checker.init());
  const ObObjMeta meta = token_meta(CS_TYPE_UTF8MB4_GENERAL_CI);
  const char *tokens[] = {"database", "the", "index", "with", "performance", "storage", "query", "and"};
  constexpr int iterations = 1000000;
  int64_t hits = 0;
  const auto start = std::chrono::steady_clock::now();
  for (int i = 0; i < iterations; ++i) {
    const char *token = tokens[i % 8];
    bool found = false;
    ASSERT_EQ(OB_SUCCESS, checker.check_stopword(ObFTWord(STRLEN(token), token, meta), found));
    hits += found;
  }
  const auto stop = std::chrono::steady_clock::now();
  ObArenaAllocator allocator;
  ObFTWordMap words;
  ASSERT_EQ(OB_SUCCESS, words.create(37, "FtsBench", "FtsBench"));
  ObFTParserProperty property;
  ObAddWordFlag flags;
  flags.set_groupby_word();
  ObAddWord add(property, meta, flags, allocator, words);
  const auto group_start = std::chrono::steady_clock::now();
  for (int i = 0; i < iterations; ++i) {
    const char *token = tokens[i % 8];
    ASSERT_EQ(OB_SUCCESS, add.process_word(token, STRLEN(token), STRLEN(token), 1));
  }
  const auto group_end = std::chrono::steady_clock::now();
  ASSERT_EQ(iterations, add.get_add_word_count());
  ASSERT_EQ(8, words.size());
  for (const char *token : tokens) {
    int64_t frequency = 0;
    ASSERT_EQ(OB_SUCCESS, words.get_refactored(ObFTWord(STRLEN(token), token, meta), frequency));
    ASSERT_EQ(iterations / 8, frequency);
  }
  std::printf("stopword_ms=%.3f grouping_ms=%.3f hits=%lld iterations=%d\n",
      std::chrono::duration<double, std::milli>(stop - start).count(),
      std::chrono::duration<double, std::milli>(group_end - group_start).count(),
      static_cast<long long>(hits), iterations);
  words.destroy();
}
int main()
{
  ASSERT_EQ(OB_SUCCESS, ObCharset::init_charset());
  stopword_collations_and_errors();
  grouping_preserves_frequency_and_last_spelling();
  non_grouped_duplicates_and_uninitialized_map();
  std::puts("FTS hotpath: 3 regression groups passed");
  unicode_classification_and_ascii_cursor();
  std::puts("Unicode scalar classification and ASCII cursor passed");
  benchmark_hotpath();
}
#endif
