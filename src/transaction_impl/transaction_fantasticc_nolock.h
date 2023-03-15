#pragma once

#include "splinterdb/data.h"
#include "platform.h"
#include "data_internal.h"
#include "splinterdb/transaction.h"
#include "util.h"
#include "experimental_mode.h"
#include "splinterdb_internal.h"
#include "iceberg_table.h"
#include "poison.h"

typedef struct transactional_splinterdb_config {
   splinterdb_config           kvsb_cfg;
   transaction_isolation_level isol_level;
   uint64                      tscache_log_slots;
} transactional_splinterdb_config;

typedef struct transactional_splinterdb {
   splinterdb                      *kvsb;
   transactional_splinterdb_config *tcfg;
   iceberg_table                   *tscache;
} transactional_splinterdb;

typedef struct {
   txn_timestamp lock_bit : 1;
   txn_timestamp delta : 15;
   txn_timestamp wts : 48;
} timestamp_set __attribute__((aligned(sizeof(txn_timestamp))));

static inline bool
timestamp_set_is_equal(const timestamp_set *s1, const timestamp_set *s2)
{
   return memcmp((const void *)s1, (const void *)s2, sizeof(timestamp_set))
          == 0;
}

static inline txn_timestamp
timestamp_set_get_rts(timestamp_set *ts)
{
   return ts->wts + ts->delta;
}

static inline bool
timestamp_set_compare_and_swap(timestamp_set *ts,
                               timestamp_set *v1,
                               timestamp_set *v2)
{
   uint64 v1_p = *(uint64 *)v1;
   uint64 v2_p = *(uint64 *)v2;
   return __sync_bool_compare_and_swap((uint64 *)ts, v1_p, v2_p);
}

typedef struct rw_entry {
   slice          key;
   message        msg; // value + op
   txn_timestamp  wts;
   txn_timestamp  rts;
   timestamp_set *tuple_ts;
   char           is_read;
   char           need_to_keep_key;
   char           need_to_decrease_refcount;
} rw_entry;


/*
 * This function has the following effects:
 * A. If entry key is not in the cache, it inserts the key in the cache with
 * refcount=1 and value=0. B. If the key is already in the cache, it just
 * increases the refcount. C. returns the pointer to the value.
 */
static inline bool
rw_entry_iceberg_insert(transactional_splinterdb *txn_kvsb, rw_entry *entry)
{
   // Make sure increasing the refcount only once
   if (entry->tuple_ts) {
      return FALSE;
   }

   KeyType key_ht = (KeyType)slice_data(entry->key);
   // ValueType value_ht = {0};
#if EXPERIMENTAL_MODE_KEEP_ALL_KEYS == 1
   // bool is_new_item = iceberg_insert_without_increasing_refcount(
   //    txn_kvsb->tscache, key_ht, value_ht, platform_get_tid());

   timestamp_set ts = {0};
   entry->tuple_ts  = &ts;
   bool is_new_item = iceberg_insert_and_get_without_increasing_refcount(
      txn_kvsb->tscache,
      key_ht,
      (ValueType **)&entry->tuple_ts,
      platform_get_tid());
   platform_assert(entry->tuple_ts != &ts);
#else
   // increase refcount for key
   timestamp_set ts = {0};
   entry->tuple_ts  = &ts;
   bool is_new_item = iceberg_insert_and_get(txn_kvsb->tscache,
                                             key_ht,
                                             (ValueType **)&entry->tuple_ts,
                                             platform_get_tid());
#endif

   // get the pointer of the value from the iceberg
   // platform_assert(iceberg_get_value(txn_kvsb->tscache,
   //                                   key_ht,
   //                                   (ValueType **)&entry->tuple_ts,
   //                                   platform_get_tid()));

   platform_assert(((uint64)entry->tuple_ts) % sizeof(uint64) == 0);

   entry->need_to_keep_key = entry->need_to_keep_key || is_new_item;
   return is_new_item;
}

static inline void
rw_entry_iceberg_remove(transactional_splinterdb *txn_kvsb, rw_entry *entry)
{
   if (!entry->tuple_ts) {
      return;
   }

   entry->tuple_ts = NULL;

#if !EXPERIMENTAL_MODE_KEEP_ALL_KEYS
   KeyType   key_ht   = (KeyType)slice_data(entry->key);
   ValueType value_ht = {0};
   if (iceberg_get_and_remove(
          txn_kvsb->tscache, &key_ht, &value_ht, platform_get_tid()))
   {
      if (slice_data(entry->key) != key_ht) {
         platform_free_from_heap(0, key_ht);
      } else {
         entry->need_to_keep_key = 0;
      }
   }
#endif
}

static rw_entry *
rw_entry_create()
{
   rw_entry *new_entry;
   new_entry = TYPED_ZALLOC(0, new_entry);
   platform_assert(new_entry != NULL);
   new_entry->tuple_ts = NULL;
   return new_entry;
}

static inline void
rw_entry_deinit(rw_entry *entry)
{
   bool can_key_free = !slice_is_null(entry->key) && !entry->need_to_keep_key;
   if (can_key_free) {
      platform_free_from_heap(0, (void *)slice_data(entry->key));
   }

   if (!message_is_null(entry->msg)) {
      platform_free_from_heap(0, (void *)message_data(entry->msg));
   }
}

static inline void
rw_entry_set_key(rw_entry *e, slice key, const data_config *cfg)
{
   char *key_buf;
   key_buf = TYPED_ARRAY_ZALLOC(0, key_buf, KEY_SIZE);
   memcpy(key_buf, slice_data(key), slice_length(key));
   e->key = slice_create(KEY_SIZE, key_buf);
}

/*
 * The msg is the msg from app.
 * In EXPERIMENTAL_MODE_TICTOC_DISK, this function adds timestamps at the begin
 * of the msg
 */
static inline void
rw_entry_set_msg(rw_entry *e, message msg)
{
   char *msg_buf;
   msg_buf = TYPED_ARRAY_ZALLOC(0, msg_buf, message_length(msg));
   memcpy(msg_buf, message_data(msg), message_length(msg));
   e->msg = message_create(message_class(msg),
                           slice_create(message_length(msg), msg_buf));
}

static inline bool
rw_entry_is_read(const rw_entry *entry)
{
   return entry->is_read;
}

static inline bool
rw_entry_is_write(const rw_entry *entry)
{
   return !message_is_null(entry->msg);
}

/*
 * Will Set timestamps in entry later
 */
static inline rw_entry *
rw_entry_get(transactional_splinterdb *txn_kvsb,
             transaction              *txn,
             slice                     user_key,
             const data_config        *cfg,
             const bool                is_read)
{
   bool      need_to_create_new_entry = TRUE;
   rw_entry *entry                    = NULL;
   const key ukey                     = key_create_from_slice(user_key);
   for (int i = 0; i < txn->num_rw_entries; ++i) {
      entry = txn->rw_entries[i];

      if (data_key_compare(cfg, ukey, key_create_from_slice(entry->key)) == 0) {
         need_to_create_new_entry = FALSE;
         break;
      }
   }

   if (need_to_create_new_entry) {
      entry = rw_entry_create();
      rw_entry_set_key(entry, user_key, cfg);
      txn->rw_entries[txn->num_rw_entries++] = entry;
   }

   entry->is_read = entry->is_read || is_read;
   return entry;
}

static int
rw_entry_key_compare(const void *elem1, const void *elem2, void *args)
{
   const data_config *cfg = (const data_config *)args;

   rw_entry *e1 = *((rw_entry **)elem1);
   rw_entry *e2 = *((rw_entry **)elem2);

   key akey = key_create_from_slice(e1->key);
   key bkey = key_create_from_slice(e2->key);

   return data_key_compare(cfg, akey, bkey);
}

static inline bool
rw_entry_try_lock(rw_entry *entry)
{
   timestamp_set v1, v2;
   v1 = v2 = *entry->tuple_ts;
   if (v1.lock_bit) {
      return false;
   }
   v2.lock_bit = 1;
   return timestamp_set_compare_and_swap(entry->tuple_ts, &v1, &v2);
}

static inline void
rw_entry_unlock(rw_entry *entry)
{
   timestamp_set v1, v2;
   do {
      v1 = v2     = *entry->tuple_ts;
      v2.lock_bit = 0;
   } while (!timestamp_set_compare_and_swap(entry->tuple_ts, &v1, &v2));
}


static void
transactional_splinterdb_config_init(
   transactional_splinterdb_config *txn_splinterdb_cfg,
   const splinterdb_config         *kvsb_cfg)
{
   memcpy(&txn_splinterdb_cfg->kvsb_cfg,
          kvsb_cfg,
          sizeof(txn_splinterdb_cfg->kvsb_cfg));

   txn_splinterdb_cfg->tscache_log_slots = 29;

   // TODO things like filename, logfile, or data_cfg would need a
   // deep-copy
   txn_splinterdb_cfg->isol_level = TRANSACTION_ISOLATION_LEVEL_SERIALIZABLE;
}

static int
transactional_splinterdb_create_or_open(const splinterdb_config   *kvsb_cfg,
                                        transactional_splinterdb **txn_kvsb,
                                        bool open_existing)
{
   check_experimental_mode_is_valid();
   print_current_experimental_modes();

   transactional_splinterdb_config *txn_splinterdb_cfg;
   txn_splinterdb_cfg = TYPED_ZALLOC(0, txn_splinterdb_cfg);
   transactional_splinterdb_config_init(txn_splinterdb_cfg, kvsb_cfg);

   transactional_splinterdb *_txn_kvsb;
   _txn_kvsb       = TYPED_ZALLOC(0, _txn_kvsb);
   _txn_kvsb->tcfg = txn_splinterdb_cfg;

   int rc = splinterdb_create_or_open(
      &txn_splinterdb_cfg->kvsb_cfg, &_txn_kvsb->kvsb, open_existing);
   bool fail_to_create_splinterdb = (rc != 0);
   if (fail_to_create_splinterdb) {
      platform_free(0, _txn_kvsb);
      platform_free(0, txn_splinterdb_cfg);
      return rc;
   }

   iceberg_table *tscache;
   tscache = TYPED_ZALLOC(0, tscache);
   platform_assert(iceberg_init(tscache, txn_splinterdb_cfg->tscache_log_slots)
                   == 0);
   _txn_kvsb->tscache = tscache;

   *txn_kvsb = _txn_kvsb;

   return 0;
}

int
transactional_splinterdb_create(const splinterdb_config   *kvsb_cfg,
                                transactional_splinterdb **txn_kvsb)
{
   return transactional_splinterdb_create_or_open(kvsb_cfg, txn_kvsb, FALSE);
}


int
transactional_splinterdb_open(const splinterdb_config   *kvsb_cfg,
                              transactional_splinterdb **txn_kvsb)
{
   return transactional_splinterdb_create_or_open(kvsb_cfg, txn_kvsb, TRUE);
}

void
transactional_splinterdb_close(transactional_splinterdb **txn_kvsb)
{
   transactional_splinterdb *_txn_kvsb = *txn_kvsb;

   iceberg_print_state(_txn_kvsb->tscache);

   splinterdb_close(&_txn_kvsb->kvsb);

   platform_free(0, _txn_kvsb->tscache);
   platform_free(0, _txn_kvsb->tcfg);
   platform_free(0, _txn_kvsb);

   *txn_kvsb = NULL;
}

void
transactional_splinterdb_register_thread(transactional_splinterdb *kvs)
{
   splinterdb_register_thread(kvs->kvsb);
}

void
transactional_splinterdb_deregister_thread(transactional_splinterdb *kvs)
{
   splinterdb_deregister_thread(kvs->kvsb);
}

int
transactional_splinterdb_begin(transactional_splinterdb *txn_kvsb,
                               transaction              *txn)
{
   platform_assert(txn);
   memset(txn, 0, sizeof(*txn));
   return 0;
}

static inline void
transaction_deinit(transactional_splinterdb *txn_kvsb, transaction *txn)
{
   for (int i = 0; i < txn->num_rw_entries; ++i) {
      rw_entry_iceberg_remove(txn_kvsb, txn->rw_entries[i]);
      rw_entry_deinit(txn->rw_entries[i]);
      platform_free(0, txn->rw_entries[i]);
   }
}

int
transactional_splinterdb_commit(transactional_splinterdb *txn_kvsb,
                                transaction              *txn)
{
   txn_timestamp commit_ts = 0;

   int       num_reads                    = 0;
   int       num_writes                   = 0;
   rw_entry *read_set[RW_SET_SIZE_LIMIT]  = {0};
   rw_entry *write_set[RW_SET_SIZE_LIMIT] = {0};

   for (int i = 0; i < txn->num_rw_entries; i++) {
      rw_entry *entry = txn->rw_entries[i];
      if (rw_entry_is_write(entry)) {
         write_set[num_writes++] = entry;
      }

      if (rw_entry_is_read(entry)) {
         read_set[num_reads++] = entry;

         txn_timestamp wts = entry->wts;
#if EXPERIMENTAL_MODE_SILO == 1
         wts += 1;
#endif
         commit_ts = MAX(commit_ts, wts);
      }
   }

   platform_sort_slow(write_set,
                      num_writes,
                      sizeof(rw_entry *),
                      rw_entry_key_compare,
                      (void *)txn_kvsb->tcfg->kvsb_cfg.data_cfg,
                      NULL);

RETRY_LOCK_WRITE_SET:
{
   for (int lock_num = 0; lock_num < num_writes; ++lock_num) {
      rw_entry *w = write_set[lock_num];
      if (!w->tuple_ts) {
         rw_entry_iceberg_insert(txn_kvsb, w);
      }

      if (!rw_entry_try_lock(w)) {
         // This is "no-wait" optimization in the TicToc paper.
         for (int i = 0; i < lock_num; ++i) {
            rw_entry_unlock(write_set[i]);
         }

         // 1us is the value that is mentioned in the paper
         platform_sleep_ns(1000);

         goto RETRY_LOCK_WRITE_SET;
      }
   }
}

   for (uint64 i = 0; i < num_writes; ++i) {
      commit_ts =
         MAX(commit_ts, timestamp_set_get_rts(write_set[i]->tuple_ts) + 1);
   }

   bool is_abort = FALSE;
   for (uint64 i = 0; !is_abort && i < num_reads; ++i) {
      rw_entry *r = read_set[i];
      platform_assert(rw_entry_is_read(r));

      if (r->rts < commit_ts) {
         bool          is_success;
         timestamp_set v1, v2;
         do {
            is_success = TRUE;
            v1 = v2                            = *r->tuple_ts;
            bool          is_wts_different     = r->wts != v1.wts;
            txn_timestamp rts                  = timestamp_set_get_rts(&v1);
            bool          is_locked_by_another = rts <= commit_ts
                                        && r->tuple_ts->lock_bit
                                        && !rw_entry_is_write(r);
            if (is_wts_different || is_locked_by_another) {
               is_abort = TRUE;
               break;
            }
            if (rts <= commit_ts) {
               txn_timestamp delta = commit_ts - v1.wts;
               txn_timestamp shift = delta - (delta & 0x7fff);
               v2.wts += shift;
               v2.delta = delta - shift;
               is_success =
                  timestamp_set_compare_and_swap(r->tuple_ts, &v1, &v2);
            }
         } while (!is_success);
      }
   }

   if (!is_abort) {
      int rc = 0;

      for (uint64 i = 0; i < num_writes; ++i) {
         rw_entry *w = write_set[i];
         platform_assert(rw_entry_is_write(w));

#if EXPERIMENTAL_MODE_BYPASS_SPLINTERDB == 1
         platform_sleep_ns(100);

         if (0) {
#endif
            switch (message_class(w->msg)) {
               case MESSAGE_TYPE_INSERT:
                  rc = splinterdb_insert(
                     txn_kvsb->kvsb, w->key, message_slice(w->msg));
                  break;
               case MESSAGE_TYPE_UPDATE:
                  rc = splinterdb_update(
                     txn_kvsb->kvsb, w->key, message_slice(w->msg));
                  break;
               case MESSAGE_TYPE_DELETE:
                  rc = splinterdb_delete(txn_kvsb->kvsb, w->key);
                  break;
               default:
                  break;
            }

            platform_assert(rc == 0, "Error from SplinterDB: %d\n", rc);
#if EXPERIMENTAL_MODE_BYPASS_SPLINTERDB == 1
         }
#endif
         timestamp_set v1, v2;
         do {
            v1 = v2     = *w->tuple_ts;
            v2.wts      = commit_ts;
            v2.delta    = 0;
            v2.lock_bit = 0;
         } while (!timestamp_set_compare_and_swap(w->tuple_ts, &v1, &v2));
      }
   } else {
      for (uint64 i = 0; i < num_writes; ++i) {
         rw_entry_unlock(write_set[i]);
      }
   }

   transaction_deinit(txn_kvsb, txn);

   return (-1 * is_abort);
}

int
transactional_splinterdb_abort(transactional_splinterdb *txn_kvsb,
                               transaction              *txn)
{
   transaction_deinit(txn_kvsb, txn);

   return 0;
}

static int
local_write(transactional_splinterdb *txn_kvsb,
            transaction              *txn,
            slice                     user_key,
            message                   msg)
{
   const data_config *cfg   = txn_kvsb->tcfg->kvsb_cfg.data_cfg;
   const key          ukey  = key_create_from_slice(user_key);
   rw_entry          *entry = rw_entry_get(txn_kvsb, txn, user_key, cfg, FALSE);
   if (message_class(msg) == MESSAGE_TYPE_UPDATE
       || message_class(msg) == MESSAGE_TYPE_DELETE)
   {
      rw_entry_iceberg_insert(txn_kvsb, entry);
      timestamp_set v = *entry->tuple_ts;
      entry->wts      = v.wts;
      entry->rts      = timestamp_set_get_rts(&v);
   }

   if (message_is_null(entry->msg)) {
      rw_entry_set_msg(entry, msg);
   } else {
      // TODO it needs to be checked later for upsert
      key wkey = key_create_from_slice(entry->key);
      if (data_key_compare(cfg, wkey, ukey) == 0) {
         if (message_is_definitive(msg)) {
            platform_free_from_heap(0, (void *)message_data(entry->msg));
            rw_entry_set_msg(entry, msg);
         } else {
            platform_assert(message_class(entry->msg) != MESSAGE_TYPE_DELETE);

            merge_accumulator new_message;
            merge_accumulator_init_from_message(&new_message, 0, msg);
            data_merge_tuples(cfg, ukey, entry->msg, &new_message);
            platform_free_from_heap(0, (void *)message_data(entry->msg));
            entry->msg = merge_accumulator_to_message(&new_message);
         }
      }
   }
   return 0;
}

int
transactional_splinterdb_insert(transactional_splinterdb *txn_kvsb,
                                transaction              *txn,
                                slice                     user_key,
                                slice                     value)
{
   return local_write(
      txn_kvsb, txn, user_key, message_create(MESSAGE_TYPE_INSERT, value));
}

int
transactional_splinterdb_delete(transactional_splinterdb *txn_kvsb,
                                transaction              *txn,
                                slice                     user_key)
{
   return local_write(txn_kvsb, txn, user_key, DELETE_MESSAGE);
}

int
transactional_splinterdb_update(transactional_splinterdb *txn_kvsb,
                                transaction              *txn,
                                slice                     user_key,
                                slice                     delta)
{
   return local_write(
      txn_kvsb, txn, user_key, message_create(MESSAGE_TYPE_UPDATE, delta));
}

int
transactional_splinterdb_lookup(transactional_splinterdb *txn_kvsb,
                                transaction              *txn,
                                slice                     user_key,
                                splinterdb_lookup_result *result)
{
   const data_config *cfg   = txn_kvsb->tcfg->kvsb_cfg.data_cfg;
   rw_entry          *entry = rw_entry_get(txn_kvsb, txn, user_key, cfg, TRUE);

   int rc = 0;

   rw_entry_iceberg_insert(txn_kvsb, entry);

   timestamp_set v1, v2;
   do {
      v1 = *entry->tuple_ts;

#if EXPERIMENTAL_MODE_BYPASS_SPLINTERDB == 1
      platform_sleep_ns(100);
#else
      if (rw_entry_is_write(entry)) {
         // read my write
         // TODO This works for simple insert/update. However, it doesn't work
         // for upsert.
         // TODO if it succeeded, this read should not be considered for
         // validation. entry->is_read should be false.
         _splinterdb_lookup_result *_result =
            (_splinterdb_lookup_result *)result;
         merge_accumulator_resize(&_result->value, message_length(entry->msg));
         memcpy(merge_accumulator_data(&_result->value),
                message_data(entry->msg),
                message_length(entry->msg));
      } else {
         rc = splinterdb_lookup(txn_kvsb->kvsb, user_key, result);
      }
#endif

      v2 = *entry->tuple_ts;
   } while (memcmp(&v1, &v2, sizeof(v1)) != 0 || v1.lock_bit);

   entry->wts = v1.wts;
   entry->rts = timestamp_set_get_rts(&v1);

   return rc;
}

void
transactional_splinterdb_lookup_result_init(
   transactional_splinterdb *txn_kvsb,   // IN
   splinterdb_lookup_result *result,     // IN/OUT
   uint64                    buffer_len, // IN
   char                     *buffer      // IN
)
{
   return splinterdb_lookup_result_init(
      txn_kvsb->kvsb, result, buffer_len, buffer);
}

void
transactional_splinterdb_set_isolation_level(
   transactional_splinterdb   *txn_kvsb,
   transaction_isolation_level isol_level)
{
   platform_assert(isol_level > TRANSACTION_ISOLATION_LEVEL_INVALID);
   platform_assert(isol_level < TRANSACTION_ISOLATION_LEVEL_MAX_VALID);

   txn_kvsb->tcfg->isol_level = isol_level;
}