# Full-text token processing optimization

Base revision: `76f871eb5a84b664d487b03b3412ce50194135b5` (`2026-competition`).

## Bottleneck and changes

`ObAddWord::groupby_word` previously fetched a count and then overwrote the
hash-map entry. Inspection of `ObHashTable::set_refactored` shows that this
overwrite allocates a replacement node and frees the previous node. Every
repeated token therefore performed two hashes, two bucket acquisitions, and
node replacement. The implementation now uses the existing `set_or_update`
API, updating the count under the bucket lock and allocating only for new keys.
It retains the last spelling of collation-equivalent keys and the original
initial-count semantics, including token frequencies greater than one.

`ObStopWordChecker::check_stopword` constructed an arena allocator for every
token, including UTF-8 inputs for which charset conversion merely aliases the
input. Valid UTF-8 collations now directly probe the existing stopword set with
its original UTF8MB4_GENERAL_CI metadata. Other inputs still take the original
conversion path. Case/accent-insensitive matching and invalid-collation errors
are retained. No shared cache or new parser state is introduced.

## Correctness

`unittest/storage/fts/test_fts_token_hotpath.cpp` covers all built-in stopwords,
the three supported UTF-8 collations, binary conversion, uppercase and accented
tokens, invalid/unsupported collations, uninitialized state, repeated words,
frequency accumulation, last spelling, map reuse, and non-grouping duplicates.
The same three regression groups passed against both baseline and changed
production FTS translation units.

## Local component measurement

Windows x64, Clang 18, optimized `/O2` build. A standalone test executable links
the actual FTS translation unit and existing seekdb dependencies. The local
dependency build needed a test-only scalar implementation of the newer MSVC
`__std_find_first_of_trivial_pos_1` runtime entry point referenced by prebuilt
gRPC. This compatibility file is outside the product repository and is linked
identically in both executables; it does not implement any FTS behavior.

Each sample executes 1,000,000 stopword checks and 1,000,000 word additions over
eight synthetic tokens. Five alternating baseline/optimized runs after initial
warmup gave the following times, in milliseconds:

| Run | Baseline stopword | Optimized stopword | Baseline grouping | Optimized grouping |
| --- | ---: | ---: | ---: | ---: |
| 1 | 71.211 | 60.099 | 200.791 | 90.460 |
| 2 | 70.722 | 71.112 | 205.418 | 92.733 |
| 3 | 69.297 | 60.418 | 200.438 | 90.475 |
| 4 | 69.401 | 59.647 | 201.641 | 88.168 |
| 5 | 69.286 | 59.998 | 198.787 | 89.317 |
| Median | 69.401 | 60.099 | 200.791 | 90.460 |

Median time reductions are approximately 13.4% and 54.9%, respectively. Every
run verifies stopword outputs and all accumulated token counts. The standalone
entry point and measurement are enabled only by `FTS_HOTPATH_STANDALONE`; the
default source exposes Google Test cases for the storage test suite.

These are component measurements, not end-to-end database results or an
official score. The full fixed 20,000-document Linux benchmark has not been run
locally against this build. Official index-build, TOKENIZE and MATCH performance
and integration correctness still require the competition evaluator. Benchmark
inputs, SQL, scoring scripts, and baseline files are unchanged.

## TOKENIZE execution-context reuse

The generated full-text columns and the SQL `TOKENIZE` function previously
rebuilt the same parser configuration, repeated the IK dictionary readiness
check, and allocated a new token hash table for every evaluated row. Constant
parser arguments are now copied once into the expression execution context.
The initialized parser helper and a correctly sized hash table are reused while
row-local token memory still comes from the current evaluation allocator.
Dynamic parser arguments retain the original path.

The official 20,000-row benchmark was run before and after this change on the
same Windows x64 host, optimized build, and fresh seekdb instances. Query hit
counts were unchanged (8,001 Chinese, 11,000 English, 7,332 mixed, and 20 with
LIMIT). Relevant timings were:

| Metric | Before | After | Change |
| --- | ---: | ---: | ---: |
| IK title + content build | 6.319 s | 6.127 s | -3.0% |
| IK content build | 4.957 s | 4.842 s | -2.3% |
| BENG English build | 2.423 s | 2.448 s | +1.0% |
| IK TOKENIZE | 0.2954 ms | 0.2868 ms | -2.9% |
| BENG TOKENIZE | 0.1966 ms | 0.1971 ms | +0.3% |

The first prototype used a fixed 997-bucket table and regressed short
`TOKENIZE` inputs. The final implementation preserves the original
text-length-based bucket sizing while reusing the allocated table. Full builds,
the benchmark's SQL correctness checks, and the existing token hot-path tests
passed with the final implementation.

## IK character pipeline (2026-10-30)

The character classifier now decodes each input once rather than repeating the
same Unicode decode in up to six category predicates. Category precedence and
UTF-16-only surrogate classification are preserved. The IK main loop passes its
already-read character and category to processors rather than making every
processor fetch both again. UTF-8 ASCII input uses a one-byte path; non-ASCII
and other encodings retain the existing validation path.

The standalone component executable passes the original three regression groups
and classification checks for all 1,112,064 valid Unicode scalar values, plus
cursor checks for all 128 ASCII bytes. This is not an end-to-end speed result.
No database server was started for this revision. MATCH result equivalence and
index-build/query timings still require the official evaluator. The previous
platform score was 2.632, with build_total_sec=14.605 and tokenize_ik_avg_ms=0.0754.
Benchmark SQL, datasets, expected results and scoring rules remain unchanged.
