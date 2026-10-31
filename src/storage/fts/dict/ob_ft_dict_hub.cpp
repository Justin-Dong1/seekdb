/*
 * Copyright (c) 2025 OceanBase.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#define USING_LOG_PREFIX STORAGE_FTS

#include "storage/fts/dict/ob_ft_dict_hub.h"

#include "lib/atomic/ob_atomic.h"
#include "lib/ob_errno.h"
#include "lib/oblog/ob_log_module.h"
#include "lib/utility/ob_macro_utils.h"
#include "storage/fts/dict/ob_ft_cache_container.h"
#include "storage/fts/dict/ob_ft_dict_def.h"
#include "storage/fts/dict/ob_ft_range_dict.h"
namespace oceanbase
{
namespace storage
{
int ObFTDictHub::init()
{
  static constexpr int K_MAX_DICT_BUCKET = 128; // for now, only built-in dicts.
  int ret = OB_SUCCESS;
  if (OB_FAIL(dict_map_.create(K_MAX_DICT_BUCKET, "dict_map"))) {
  } else if (OB_FAIL(rw_dict_lock_.init(K_MAX_DICT_BUCKET))) {
  } else {
    is_inited_ = true;
  }
  return ret;
};

int ObFTDictHub::destroy()
{
  int ret = OB_SUCCESS;
  if (OB_NOT_NULL(shared_set_)) {
    if (OB_NOT_NULL(shared_set_->main_dict_)) {
      shared_set_->main_dict_->~ObIFTDict();
      shared_set_->main_dict_ = nullptr;
    }
    if (OB_NOT_NULL(shared_set_->quan_dict_)) {
      shared_set_->quan_dict_->~ObIFTDict();
      shared_set_->quan_dict_ = nullptr;
    }
    if (OB_NOT_NULL(shared_set_->stop_dict_)) {
      shared_set_->stop_dict_->~ObIFTDict();
      shared_set_->stop_dict_ = nullptr;
    }
    shared_set_->main_container_.reset();
    shared_set_->quan_container_.reset();
    shared_set_->stop_container_.reset();
    shared_set_ = nullptr;
  }
  shared_alloc_.reset();
  is_inited_ = false;
  return ret;
}
int ObFTDictHub::build_cache(const ObFTDictDesc &desc, ObFTCacheRangeContainer &container)
{
  int ret = OB_SUCCESS;
  ObFTDictInfoKey key(static_cast<uint64_t>(desc.type_));
  ObFTDictInfo info;
  container.reset();

  if (!is_inited_) {
    ret = OB_NOT_INIT;
  } else {
    ObBucketHashWLockGuard guard(rw_dict_lock_, key.hash());

    // try if valid with no recursive lock
    if (OB_FAIL(get_dict_info(key, info))) {
      if (OB_HASH_NOT_EXIST == ret) {
        // dict not exist, make new one, by caller
        ret = OB_ENTRY_NOT_EXIST;
      } else {
      }
    } else if (OB_FAIL(ObFTRangeDict::try_load_cache(desc, info.range_count_, container))) {
      if (OB_ENTRY_NOT_EXIST == ret) {
      } else {
      }
    }

    if (OB_FAIL(ret)) {
      if (OB_ENTRY_NOT_EXIST == ret) {
        if (OB_FAIL(ObFTRangeDict::build_cache_from_ik_dict(desc, container))) {
        } else if (FALSE_IT(info.range_count_ = container.get_handles().size())) {
        } else if (OB_FAIL(put_dict_info(key, info))) {
        } else {
          ATOMIC_INC(&dict_version_);
        }
      }
    }
  }
  return ret;
}

int ObFTDictHub::load_cache(const ObFTDictDesc &desc, ObFTCacheRangeContainer &container)
{
  int ret = OB_SUCCESS;
  ObFTDictInfo info;
  container.reset();
  ObFTDictInfoKey key(static_cast<uint64_t>(desc.type_));
  if (!is_inited_) {
    ret = OB_NOT_INIT;
  } else {
    {
      ObBucketHashRLockGuard guard(rw_dict_lock_, key.hash());
      if (OB_FAIL(get_dict_info(key, info))) {
        if (OB_HASH_NOT_EXIST == ret) {
          // dict not exist, make new one, by caller
          ret = OB_ENTRY_NOT_EXIST;
        } else {
        }
      }
    }
    if (OB_FAIL(ret)) {
    } else if (OB_FAIL(ObFTRangeDict::try_load_cache(desc, info.range_count_, container))) {
      if (OB_ENTRY_NOT_EXIST == ret) {
        // dict not exist, make new one, by caller
      } else {
      }
    }
  }

  return ret;
}


int ObFTDictHub::get_dict_info(const ObFTDictInfoKey &key, ObFTDictInfo &info)
{
  int ret = OB_SUCCESS;

  if (OB_FAIL(dict_map_.get_refactored(key, info))) {
    if (OB_HASH_NOT_EXIST != ret) {
    }
  }

  return ret;
}

int ObFTDictHub::put_dict_info(const ObFTDictInfoKey &key, const ObFTDictInfo &info)
{
  int ret = OB_SUCCESS;
  const int cover_exist_flag = 1;
  if (OB_FAIL(dict_map_.set_refactored(key, info, cover_exist_flag))) {
  }

  return ret;
}

int ObFTDictHub::build_shared_dict(const ObFTDictDesc &desc,
                                   ObFTCacheRangeContainer &container,
                                   ObIFTDict *&dict)
{
  int ret = OB_SUCCESS;
  dict = nullptr;
  if (OB_FAIL(load_cache(desc, container))) {
    if (OB_ENTRY_NOT_EXIST == ret) {
      if (OB_FAIL(build_cache(desc, container))) {
        STORAGE_FTS_LOG(WARN, "Failed to build dict cache", K(ret), K(desc.type_));
      }
    } else {
      STORAGE_FTS_LOG(WARN, "Failed to load dict cache", K(ret), K(desc.type_));
    }
  }
  if (OB_SUCC(ret)) {
    if (OB_ISNULL(dict = OB_NEWx(ObFTRangeDict,
                                 &shared_alloc_,
                                 shared_alloc_,
                                 &container,
                                 desc))) {
      ret = OB_ALLOCATE_MEMORY_FAILED;
    } else if (OB_FAIL(dict->init())) {
      OB_DELETEx(ObIFTDict, &shared_alloc_, dict);
      dict = nullptr;
    }
  }
  return ret;
}

int ObFTDictHub::get_shared_dicts(ObIFTDict *&main_dict,
                                  ObIFTDict *&quan_dict,
                                  ObIFTDict *&stop_dict)
{
  int ret = OB_SUCCESS;
  main_dict = nullptr;
  quan_dict = nullptr;
  stop_dict = nullptr;
  if (OB_UNLIKELY(!is_inited_)) {
    ret = OB_NOT_INIT;
  } else {
    lib::ObMutexGuard guard(shared_lock_);
    const uint64_t version = ATOMIC_LOAD(&dict_version_);
    if (OB_ISNULL(shared_set_) || shared_set_->version_ != version) {
      ObFTSharedDictSet *set = nullptr;
      if (OB_ISNULL(set = OB_NEWx(ObFTSharedDictSet, &shared_alloc_, shared_alloc_))) {
        ret = OB_ALLOCATE_MEMORY_FAILED;
      } else {
        ObFTDictDesc main_dict_desc("main_dict",
                                    ObFTDictType::DICT_IK_MAIN,
                                    ObCharsetType::CHARSET_UTF8MB4,
                                    ObCollationType::CS_TYPE_UTF8MB4_BIN);
        ObFTDictDesc quan_dict_desc("quan_dict",
                                    ObFTDictType::DICT_IK_QUAN,
                                    ObCharsetType::CHARSET_UTF8MB4,
                                    ObCollationType::CS_TYPE_UTF8MB4_BIN);
        ObFTDictDesc stop_dict_desc("stopword",
                                    ObFTDictType::DICT_IK_STOP,
                                    ObCharsetType::CHARSET_UTF8MB4,
                                    ObCollationType::CS_TYPE_UTF8MB4_BIN);
        if (OB_FAIL(build_shared_dict(main_dict_desc,
                                      set->main_container_,
                                      set->main_dict_))) {
        } else if (OB_FAIL(build_shared_dict(quan_dict_desc,
                                             set->quan_container_,
                                             set->quan_dict_))) {
        } else if (OB_FAIL(build_shared_dict(stop_dict_desc,
                                             set->stop_container_,
                                             set->stop_dict_))) {
        } else {
          // A previous set is intentionally kept alive: readers may still use it.
          set->version_ = ATOMIC_LOAD(&dict_version_);
          shared_set_ = set;
        }
      }
    }
    if (OB_SUCC(ret)) {
      main_dict = shared_set_->main_dict_;
      quan_dict = shared_set_->quan_dict_;
      stop_dict = shared_set_->stop_dict_;
    }
  }
  return ret;
}
} //  namespace storage
} //  namespace oceanbase
