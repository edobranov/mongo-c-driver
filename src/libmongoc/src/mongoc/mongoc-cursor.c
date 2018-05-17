/*
 * Copyright 2013 MongoDB, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */


#include "mongoc-cursor.h"
#include "mongoc-cursor-private.h"
#include "mongoc-client-private.h"
#include "mongoc-counters-private.h"
#include "mongoc-error.h"
#include "mongoc-log.h"
#include "mongoc-trace-private.h"
#include "mongoc-read-concern-private.h"
#include "mongoc-util-private.h"
#include "mongoc-write-concern-private.h"
#include "mongoc-read-prefs-private.h"


#undef MONGOC_LOG_DOMAIN
#define MONGOC_LOG_DOMAIN "cursor"


#define CURSOR_FAILED(cursor_) ((cursor_)->error.domain != 0)

static bool
_translate_query_opt (const char *query_field,
                      const char **cmd_field,
                      int *len);


bool
_mongoc_cursor_set_opt_int64 (mongoc_cursor_t *cursor,
                              const char *option,
                              int64_t value)
{
   bson_iter_t iter;

   if (bson_iter_init_find (&iter, &cursor->opts, option)) {
      if (!BSON_ITER_HOLDS_INT64 (&iter)) {
         return false;
      }

      bson_iter_overwrite_int64 (&iter, value);
      return true;
   }

   return BSON_APPEND_INT64 (&cursor->opts, option, value);
}


static int64_t
_mongoc_cursor_get_opt_int64 (const mongoc_cursor_t *cursor,
                              const char *option,
                              int64_t default_value)
{
   bson_iter_t iter;

   if (bson_iter_init_find (&iter, &cursor->opts, option)) {
      return bson_iter_as_int64 (&iter);
   }

   return default_value;
}


static bool
_mongoc_cursor_set_opt_bool (mongoc_cursor_t *cursor,
                             const char *option,
                             bool value)
{
   bson_iter_t iter;

   if (bson_iter_init_find (&iter, &cursor->opts, option)) {
      if (!BSON_ITER_HOLDS_BOOL (&iter)) {
         return false;
      }

      bson_iter_overwrite_bool (&iter, value);
      return true;
   }

   return BSON_APPEND_BOOL (&cursor->opts, option, value);
}


bool
_mongoc_cursor_get_opt_bool (const mongoc_cursor_t *cursor, const char *option)
{
   bson_iter_t iter;

   if (bson_iter_init_find (&iter, &cursor->opts, option)) {
      return bson_iter_as_bool (&iter);
   }

   return false;
}


int32_t
_mongoc_n_return (mongoc_cursor_t *cursor)
{
   int64_t limit;
   int64_t batch_size;
   int64_t n_return;

   /* calculate numberToReturn according to:
    * https://github.com/mongodb/specifications/blob/master/source/crud/crud.rst#combining-limit-and-batch-size-for-the-wire-protocol
    */
   limit = mongoc_cursor_get_limit (cursor);
   batch_size = mongoc_cursor_get_batch_size (cursor);

   if (limit < 0) {
      n_return = limit;
   } else if (limit == 0) {
      n_return = batch_size;
   } else if (batch_size == 0) {
      n_return = limit;
   } else if (limit < batch_size) {
      n_return = limit;
   } else {
      n_return = batch_size;
   }

   /* if a specified limit exists, account for documents already returned. */
   if (limit > 0 && cursor->count) {
      int64_t remaining = limit - cursor->count;
      BSON_ASSERT (remaining > 0);
      n_return = BSON_MIN (n_return, remaining);
   }

   /* check boundary conditions */
   if (n_return < INT32_MIN) {
      return INT32_MIN;
   } else if (n_return > INT32_MAX) {
      return INT32_MAX;
   } else {
      return (int32_t) n_return;
   }
}


void
_mongoc_set_cursor_ns (mongoc_cursor_t *cursor, const char *ns, uint32_t nslen)
{
   const char *dot;

   bson_strncpy (cursor->ns, ns, sizeof cursor->ns);
   cursor->nslen = BSON_MIN (nslen, sizeof cursor->ns);
   dot = strstr (cursor->ns, ".");

   if (dot) {
      cursor->dblen = (uint32_t) (dot - cursor->ns);
   } else {
      /* a database name with no collection name */
      cursor->dblen = cursor->nslen;
   }
}


/* return first key beginning with $, or NULL. precondition: bson is valid. */
static const char *
_first_dollar_field (const bson_t *bson)
{
   bson_iter_t iter;
   const char *key;

   BSON_ASSERT (bson_iter_init (&iter, bson));
   while (bson_iter_next (&iter)) {
      key = bson_iter_key (&iter);

      if (key[0] == '$') {
         return key;
      }
   }

   return NULL;
}


/* if src is non-NULL, it is validated and copied to dst. returns false and
 * sets the cursor error if validation fails. */
bool
_mongoc_cursor_check_and_copy_to (mongoc_cursor_t *cursor,
                                  const char *err_prefix,
                                  const bson_t *src,
                                  bson_t *dst)
{
   bson_error_t validate_err;
   bson_init (dst);
   if (src) {
      if (!bson_validate_with_error (
             src, BSON_VALIDATE_EMPTY_KEYS, &validate_err)) {
         bson_set_error (&cursor->error,
                         MONGOC_ERROR_CURSOR,
                         MONGOC_ERROR_CURSOR_INVALID_CURSOR,
                         "Invalid %s: %s",
                         err_prefix,
                         validate_err.message);
         return false;
      }

      bson_destroy (dst);
      bson_copy_to (src, dst);
   }
   return true;
}


mongoc_cursor_t *
_mongoc_cursor_new_with_opts (mongoc_client_t *client,
                              const char *db_and_collection,
                              const bson_t *opts,
                              const mongoc_read_prefs_t *read_prefs,
                              const mongoc_read_concern_t *read_concern)
{
   mongoc_cursor_t *cursor;
   mongoc_topology_description_type_t td_type;
   uint32_t server_id;
   bson_error_t validate_err;
   const char *dollar_field;
   bson_iter_t iter;

   ENTRY;

   BSON_ASSERT (client);

   cursor = (mongoc_cursor_t *) bson_malloc0 (sizeof *cursor);
   cursor->client = client;
   cursor->state = UNPRIMED;

   bson_init (&cursor->opts);
   bson_init (&cursor->error_doc);

   if (opts) {
      if (!bson_validate_with_error (
             opts, BSON_VALIDATE_EMPTY_KEYS, &validate_err)) {
         bson_set_error (&cursor->error,
                         MONGOC_ERROR_CURSOR,
                         MONGOC_ERROR_CURSOR_INVALID_CURSOR,
                         "Invalid opts: %s",
                         validate_err.message);
         GOTO (finish);
      }

      dollar_field = _first_dollar_field (opts);
      if (dollar_field) {
         bson_set_error (&cursor->error,
                         MONGOC_ERROR_CURSOR,
                         MONGOC_ERROR_CURSOR_INVALID_CURSOR,
                         "Cannot use $-modifiers in opts: \"%s\"",
                         dollar_field);
         GOTO (finish);
      }

      if (bson_iter_init_find (&iter, opts, "sessionId")) {
         if (!_mongoc_client_session_from_iter (
                client, &iter, &cursor->client_session, &cursor->error)) {
            GOTO (finish);
         }

         cursor->explicit_session = true;
      }

      /* true if there's a valid serverId or no serverId, false on err */
      if (!_mongoc_get_server_id_from_opts (opts,
                                            MONGOC_ERROR_CURSOR,
                                            MONGOC_ERROR_CURSOR_INVALID_CURSOR,
                                            &server_id,
                                            &cursor->error)) {
         GOTO (finish);
      }

      if (server_id) {
         (void) mongoc_cursor_set_hint (cursor, server_id);
      }

      bson_copy_to_excluding_noinit (
         opts, &cursor->opts, "serverId", "sessionId", NULL);
   }

   cursor->read_prefs = read_prefs
                           ? mongoc_read_prefs_copy (read_prefs)
                           : mongoc_read_prefs_new (MONGOC_READ_PRIMARY);

   cursor->read_concern = read_concern ? mongoc_read_concern_copy (read_concern)
                                       : mongoc_read_concern_new ();

   if (db_and_collection) {
      _mongoc_set_cursor_ns (
         cursor, db_and_collection, (uint32_t) strlen (db_and_collection));
   }

   if (_mongoc_cursor_get_opt_bool (cursor, MONGOC_CURSOR_EXHAUST)) {
      if (_mongoc_cursor_get_opt_int64 (cursor, MONGOC_CURSOR_LIMIT, 0)) {
         bson_set_error (&cursor->error,
                         MONGOC_ERROR_CURSOR,
                         MONGOC_ERROR_CURSOR_INVALID_CURSOR,
                         "Cannot specify both 'exhaust' and 'limit'.");
         GOTO (finish);
      }

      td_type = _mongoc_topology_get_type (client->topology);

      if (td_type == MONGOC_TOPOLOGY_SHARDED) {
         bson_set_error (&cursor->error,
                         MONGOC_ERROR_CURSOR,
                         MONGOC_ERROR_CURSOR_INVALID_CURSOR,
                         "Cannot use exhaust cursor with sharded cluster.");
         GOTO (finish);
      }
   }

   (void) _mongoc_read_prefs_validate (read_prefs, &cursor->error);

finish:
   mongoc_counter_cursors_active_inc ();

   RETURN (cursor);
}


static bool
_translate_query_opt (const char *query_field, const char **cmd_field, int *len)
{
   if (query_field[0] != '$') {
      *cmd_field = query_field;
      *len = -1;
      return true;
   }

   /* strip the leading '$' */
   query_field++;

   if (!strcmp (MONGOC_CURSOR_ORDERBY, query_field)) {
      *cmd_field = MONGOC_CURSOR_SORT;
      *len = MONGOC_CURSOR_SORT_LEN;
   } else if (!strcmp (MONGOC_CURSOR_SHOW_DISK_LOC,
                       query_field)) { /* <= MongoDb 3.0 */
      *cmd_field = MONGOC_CURSOR_SHOW_RECORD_ID;
      *len = MONGOC_CURSOR_SHOW_RECORD_ID_LEN;
   } else if (!strcmp (MONGOC_CURSOR_HINT, query_field)) {
      *cmd_field = MONGOC_CURSOR_HINT;
      *len = MONGOC_CURSOR_HINT_LEN;
   } else if (!strcmp (MONGOC_CURSOR_COMMENT, query_field)) {
      *cmd_field = MONGOC_CURSOR_COMMENT;
      *len = MONGOC_CURSOR_COMMENT_LEN;
   } else if (!strcmp (MONGOC_CURSOR_MAX_SCAN, query_field)) {
      *cmd_field = MONGOC_CURSOR_MAX_SCAN;
      *len = MONGOC_CURSOR_MAX_SCAN_LEN;
   } else if (!strcmp (MONGOC_CURSOR_MAX_TIME_MS, query_field)) {
      *cmd_field = MONGOC_CURSOR_MAX_TIME_MS;
      *len = MONGOC_CURSOR_MAX_TIME_MS_LEN;
   } else if (!strcmp (MONGOC_CURSOR_MAX, query_field)) {
      *cmd_field = MONGOC_CURSOR_MAX;
      *len = MONGOC_CURSOR_MAX_LEN;
   } else if (!strcmp (MONGOC_CURSOR_MIN, query_field)) {
      *cmd_field = MONGOC_CURSOR_MIN;
      *len = MONGOC_CURSOR_MIN_LEN;
   } else if (!strcmp (MONGOC_CURSOR_RETURN_KEY, query_field)) {
      *cmd_field = MONGOC_CURSOR_RETURN_KEY;
      *len = MONGOC_CURSOR_RETURN_KEY_LEN;
   } else if (!strcmp (MONGOC_CURSOR_SNAPSHOT, query_field)) {
      *cmd_field = MONGOC_CURSOR_SNAPSHOT;
      *len = MONGOC_CURSOR_SNAPSHOT_LEN;
   } else {
      /* not a special command field, must be a query operator like $or */
      return false;
   }

   return true;
}


/* set up a new opt bson from older ways of specifying options.
 * slave_ok may be NULL.
 * error may be NULL.
 */
void
_mongoc_cursor_flags_to_opts (mongoc_query_flags_t qflags,
                              bson_t *opts, /* IN/OUT */
                              bool *slave_ok /* OUT */)
{
   ENTRY;
   BSON_ASSERT (opts);

   if (slave_ok) {
      *slave_ok = !!(qflags & MONGOC_QUERY_SLAVE_OK);
   }

   if (qflags & MONGOC_QUERY_TAILABLE_CURSOR) {
      bson_append_bool (
         opts, MONGOC_CURSOR_TAILABLE, MONGOC_CURSOR_TAILABLE_LEN, true);
   }

   if (qflags & MONGOC_QUERY_OPLOG_REPLAY) {
      bson_append_bool (opts,
                        MONGOC_CURSOR_OPLOG_REPLAY,
                        MONGOC_CURSOR_OPLOG_REPLAY_LEN,
                        true);
   }

   if (qflags & MONGOC_QUERY_NO_CURSOR_TIMEOUT) {
      bson_append_bool (opts,
                        MONGOC_CURSOR_NO_CURSOR_TIMEOUT,
                        MONGOC_CURSOR_NO_CURSOR_TIMEOUT_LEN,
                        true);
   }

   if (qflags & MONGOC_QUERY_AWAIT_DATA) {
      bson_append_bool (
         opts, MONGOC_CURSOR_AWAIT_DATA, MONGOC_CURSOR_AWAIT_DATA_LEN, true);
   }

   if (qflags & MONGOC_QUERY_EXHAUST) {
      bson_append_bool (
         opts, MONGOC_CURSOR_EXHAUST, MONGOC_CURSOR_EXHAUST_LEN, true);
   }

   if (qflags & MONGOC_QUERY_PARTIAL) {
      bson_append_bool (opts,
                        MONGOC_CURSOR_ALLOW_PARTIAL_RESULTS,
                        MONGOC_CURSOR_ALLOW_PARTIAL_RESULTS_LEN,
                        true);
   }
}

/* Checks if the passed query was wrapped in a $query, and if so, parses the
 * query modifiers:
 * https://docs.mongodb.com/manual/reference/operator/query-modifier/
 * and translates them to find command options:
 * https://docs.mongodb.com/manual/reference/command/find/
 * opts must be initialized, and may already have options set.
 * unwrapped must be uninitialized, and will be initialized at return.
 * Returns true if query was unwrapped. */
bool
_mongoc_cursor_translate_dollar_query_opts (const bson_t *query,
                                            bson_t *opts,
                                            bson_t *unwrapped,
                                            bson_error_t *error)
{
   bool has_filter = false;
   const char *key;
   bson_iter_t iter;
   const char *opt_key;
   int len;
   uint32_t data_len;
   const uint8_t *data;
   bson_error_t error_local = {0};

   ENTRY;
   BSON_ASSERT (query);
   BSON_ASSERT (opts);
   /* If the query is explicitly specified wrapped in $query, unwrap it and
    * translate the options to new options. */
   if (bson_has_field (query, "$query")) {
      /* like "{$query: {a: 1}, $orderby: {b: 1}, $otherModifier: true}" */
      if (!bson_iter_init (&iter, query)) {
         bson_set_error (&error_local,
                         MONGOC_ERROR_BSON,
                         MONGOC_ERROR_BSON_INVALID,
                         "Invalid BSON in query document");
         GOTO (done);
      }
      while (bson_iter_next (&iter)) {
         key = bson_iter_key (&iter);
         if (key[0] != '$') {
            bson_set_error (&error_local,
                            MONGOC_ERROR_CURSOR,
                            MONGOC_ERROR_CURSOR_INVALID_CURSOR,
                            "Cannot mix $query with non-dollar field '%s'",
                            key);
            GOTO (done);
         }
         if (!strcmp (key, "$query")) {
            /* set "filter" to the incoming document's "$query" */
            bson_iter_document (&iter, &data_len, &data);
            if (!bson_init_static (unwrapped, data, (size_t) data_len)) {
               bson_set_error (&error_local,
                               MONGOC_ERROR_BSON,
                               MONGOC_ERROR_BSON_INVALID,
                               "Invalid BSON in $query subdocument");
               GOTO (done);
            }
            has_filter = true;
         } else if (_translate_query_opt (key, &opt_key, &len)) {
            /* "$orderby" becomes "sort", etc., "$unknown" -> "unknown" */
            if (!bson_append_iter (opts, opt_key, len, &iter)) {
               bson_set_error (&error_local,
                               MONGOC_ERROR_BSON,
                               MONGOC_ERROR_BSON_INVALID,
                               "Error adding \"%s\" to query",
                               opt_key);
            }
         } else {
            /* strip leading "$" */
            if (!bson_append_iter (opts, key + 1, -1, &iter)) {
               bson_set_error (&error_local,
                               MONGOC_ERROR_BSON,
                               MONGOC_ERROR_BSON_INVALID,
                               "Error adding \"%s\" to query",
                               key);
            }
         }
      }
   }
done:
   if (error) {
      memcpy (error, &error_local, sizeof (bson_error_t));
   }
   if (!has_filter) {
      bson_init (unwrapped);
   }
   RETURN (has_filter);
}


void
mongoc_cursor_destroy (mongoc_cursor_t *cursor)
{
   char db[MONGOC_NAMESPACE_MAX];
   ENTRY;

   BSON_ASSERT (cursor);

   if (cursor->impl.destroy) {
      cursor->impl.destroy (&cursor->impl);
   }

   if (cursor->in_exhaust) {
      cursor->client->in_exhaust = false;
      if (cursor->state != DONE) {
         /* The only way to stop an exhaust cursor is to kill the connection */
         mongoc_cluster_disconnect_node (
            &cursor->client->cluster, cursor->server_id, false, NULL);
      }
   } else if (cursor->cursor_id) {
      bson_strncpy (db, cursor->ns, cursor->dblen + 1);

      _mongoc_client_kill_cursor (cursor->client,
                                  cursor->server_id,
                                  cursor->cursor_id,
                                  cursor->operation_id,
                                  db,
                                  cursor->ns + cursor->dblen + 1,
                                  cursor->client_session);
   }

   if (cursor->client_session && !cursor->explicit_session) {
      mongoc_client_session_destroy (cursor->client_session);
   }

   mongoc_read_prefs_destroy (cursor->read_prefs);
   mongoc_read_concern_destroy (cursor->read_concern);
   mongoc_write_concern_destroy (cursor->write_concern);

   bson_destroy (&cursor->opts);
   bson_destroy (&cursor->error_doc);
   bson_free (cursor);

   mongoc_counter_cursors_active_dec ();
   mongoc_counter_cursors_disposed_inc ();

   EXIT;
}


mongoc_server_stream_t *
_mongoc_cursor_fetch_stream (mongoc_cursor_t *cursor)
{
   mongoc_server_stream_t *server_stream;

   ENTRY;

   if (cursor->server_id) {
      server_stream =
         mongoc_cluster_stream_for_server (&cursor->client->cluster,
                                           cursor->server_id,
                                           true /* reconnect_ok */,
                                           &cursor->error);
   } else {
      server_stream = mongoc_cluster_stream_for_reads (
         &cursor->client->cluster, cursor->read_prefs, &cursor->error);

      if (server_stream) {
         cursor->server_id = server_stream->sd->id;
      }
   }

   RETURN (server_stream);
}


bool
_mongoc_cursor_monitor_command (mongoc_cursor_t *cursor,
                                mongoc_server_stream_t *server_stream,
                                const bson_t *cmd,
                                const char *cmd_name)
{
   mongoc_client_t *client;
   mongoc_apm_command_started_t event;
   char db[MONGOC_NAMESPACE_MAX];

   ENTRY;

   client = cursor->client;
   if (!client->apm_callbacks.started) {
      /* successful */
      RETURN (true);
   }

   bson_strncpy (db, cursor->ns, cursor->dblen + 1);

   mongoc_apm_command_started_init (&event,
                                    cmd,
                                    db,
                                    cmd_name,
                                    client->cluster.request_id,
                                    cursor->operation_id,
                                    &server_stream->sd->host,
                                    server_stream->sd->id,
                                    client->apm_context);

   client->apm_callbacks.started (&event);
   mongoc_apm_command_started_cleanup (&event);

   RETURN (true);
}


/* append array of docs from current cursor batch */
static void
_mongoc_cursor_append_docs_array (mongoc_cursor_t *cursor,
                                  bson_t *docs,
                                  mongoc_cursor_response_legacy_t *response)
{
   bool eof = false;
   char str[16];
   const char *key;
   uint32_t i = 0;
   size_t keylen;
   const bson_t *doc;

   while ((doc = bson_reader_read (response->reader, &eof))) {
      keylen = bson_uint32_to_string (i, &key, str, sizeof str);
      bson_append_document (docs, key, (int) keylen, doc);
   }

   bson_reader_reset (response->reader);
}


void
_mongoc_cursor_monitor_succeeded (mongoc_cursor_t *cursor,
                                  mongoc_cursor_response_legacy_t *response,
                                  int64_t duration,
                                  bool first_batch,
                                  mongoc_server_stream_t *stream,
                                  const char *cmd_name)
{
   bson_t docs_array;
   mongoc_apm_command_succeeded_t event;
   mongoc_client_t *client;
   bson_t reply;
   bson_t reply_cursor;

   ENTRY;

   client = cursor->client;

   if (!client->apm_callbacks.succeeded) {
      EXIT;
   }

   /* we sent OP_QUERY/OP_GETMORE, fake a reply to find/getMore command:
    * {ok: 1, cursor: {id: 17, ns: "...", first/nextBatch: [ ... docs ... ]}}
    */
   bson_init (&docs_array);
   _mongoc_cursor_append_docs_array (cursor, &docs_array, response);

   bson_init (&reply);
   bson_append_int32 (&reply, "ok", 2, 1);
   bson_append_document_begin (&reply, "cursor", 6, &reply_cursor);
   bson_append_int64 (&reply_cursor, "id", 2, mongoc_cursor_get_id (cursor));
   bson_append_utf8 (&reply_cursor, "ns", 2, cursor->ns, cursor->nslen);
   bson_append_array (&reply_cursor,
                      first_batch ? "firstBatch" : "nextBatch",
                      first_batch ? 10 : 9,
                      &docs_array);
   bson_append_document_end (&reply, &reply_cursor);
   bson_destroy (&docs_array);

   mongoc_apm_command_succeeded_init (&event,
                                      duration,
                                      &reply,
                                      cmd_name,
                                      client->cluster.request_id,
                                      cursor->operation_id,
                                      &stream->sd->host,
                                      stream->sd->id,
                                      client->apm_context);

   client->apm_callbacks.succeeded (&event);

   mongoc_apm_command_succeeded_cleanup (&event);
   bson_destroy (&reply);

   EXIT;
}


void
_mongoc_cursor_monitor_failed (mongoc_cursor_t *cursor,
                               int64_t duration,
                               mongoc_server_stream_t *stream,
                               const char *cmd_name)
{
   mongoc_apm_command_failed_t event;
   mongoc_client_t *client;
   bson_t reply;

   ENTRY;

   client = cursor->client;

   if (!client->apm_callbacks.failed) {
      EXIT;
   }

   /* we sent OP_QUERY/OP_GETMORE, fake a reply to find/getMore command:
    * {ok: 0}
    */
   bson_init (&reply);
   bson_append_int32 (&reply, "ok", 2, 0);

   mongoc_apm_command_failed_init (&event,
                                   duration,
                                   cmd_name,
                                   &cursor->error,
                                   &reply,
                                   client->cluster.request_id,
                                   cursor->operation_id,
                                   &stream->sd->host,
                                   stream->sd->id,
                                   client->apm_context);

   client->apm_callbacks.failed (&event);

   mongoc_apm_command_failed_cleanup (&event);
   bson_destroy (&reply);

   EXIT;
}


#define ADD_FLAG(_flags, _value)                                   \
   do {                                                            \
      if (!BSON_ITER_HOLDS_BOOL (&iter)) {                         \
         bson_set_error (&cursor->error,                           \
                         MONGOC_ERROR_COMMAND,                     \
                         MONGOC_ERROR_COMMAND_INVALID_ARG,         \
                         "invalid option %s, should be type bool", \
                         key);                                     \
         return false;                                             \
      }                                                            \
      if (bson_iter_as_bool (&iter)) {                             \
         *_flags |= _value;                                        \
      }                                                            \
   } while (false);

bool
_mongoc_cursor_opts_to_flags (mongoc_cursor_t *cursor,
                              mongoc_server_stream_t *stream,
                              mongoc_query_flags_t *flags /* OUT */)
{
   bson_iter_t iter;
   const char *key;

   *flags = MONGOC_QUERY_NONE;

   if (!bson_iter_init (&iter, &cursor->opts)) {
      bson_set_error (&cursor->error,
                      MONGOC_ERROR_BSON,
                      MONGOC_ERROR_BSON_INVALID,
                      "Invalid 'opts' parameter.");
      return false;
   }

   while (bson_iter_next (&iter)) {
      key = bson_iter_key (&iter);

      if (!strcmp (key, MONGOC_CURSOR_ALLOW_PARTIAL_RESULTS)) {
         ADD_FLAG (flags, MONGOC_QUERY_PARTIAL);
      } else if (!strcmp (key, MONGOC_CURSOR_AWAIT_DATA)) {
         ADD_FLAG (flags, MONGOC_QUERY_AWAIT_DATA);
      } else if (!strcmp (key, MONGOC_CURSOR_EXHAUST)) {
         ADD_FLAG (flags, MONGOC_QUERY_EXHAUST);
      } else if (!strcmp (key, MONGOC_CURSOR_NO_CURSOR_TIMEOUT)) {
         ADD_FLAG (flags, MONGOC_QUERY_NO_CURSOR_TIMEOUT);
      } else if (!strcmp (key, MONGOC_CURSOR_OPLOG_REPLAY)) {
         ADD_FLAG (flags, MONGOC_QUERY_OPLOG_REPLAY);
      } else if (!strcmp (key, MONGOC_CURSOR_TAILABLE)) {
         ADD_FLAG (flags, MONGOC_QUERY_TAILABLE_CURSOR);
      }
   }

   if (cursor->slave_ok) {
      *flags |= MONGOC_QUERY_SLAVE_OK;
   } else if (cursor->server_id &&
              (stream->topology_type == MONGOC_TOPOLOGY_RS_WITH_PRIMARY ||
               stream->topology_type == MONGOC_TOPOLOGY_RS_NO_PRIMARY) &&
              stream->sd->type != MONGOC_SERVER_RS_PRIMARY) {
      *flags |= MONGOC_QUERY_SLAVE_OK;
   }

   return true;
}

bool
_mongoc_cursor_run_command (mongoc_cursor_t *cursor,
                            const bson_t *command,
                            const bson_t *opts,
                            bson_t *reply)
{
   mongoc_cluster_t *cluster;
   mongoc_server_stream_t *server_stream;
   bson_iter_t iter;
   mongoc_cmd_parts_t parts;
   const char *cmd_name;
   bool is_primary;
   mongoc_read_prefs_t *prefs = NULL;
   char db[MONGOC_NAMESPACE_MAX];
   mongoc_session_opt_t *session_opts;
   bool ret = false;

   ENTRY;

   cluster = &cursor->client->cluster;
   mongoc_cmd_parts_init (
      &parts, cursor->client, db, MONGOC_QUERY_NONE, command);
   parts.is_read_command = true;
   parts.read_prefs = cursor->read_prefs;
   parts.assembled.operation_id = cursor->operation_id;
   server_stream = _mongoc_cursor_fetch_stream (cursor);

   if (!server_stream) {
      _mongoc_bson_init_if_set (reply);
      GOTO (done);
   }

   if (opts) {
      if (!bson_iter_init (&iter, opts)) {
         _mongoc_bson_init_if_set (reply);
         bson_set_error (&cursor->error,
                         MONGOC_ERROR_BSON,
                         MONGOC_ERROR_BSON_INVALID,
                         "Invalid BSON in opts document");
         GOTO (done);
      }
      if (!mongoc_cmd_parts_append_opts (&parts,
                                         &iter,
                                         server_stream->sd->max_wire_version,
                                         &cursor->error)) {
         _mongoc_bson_init_if_set (reply);
         GOTO (done);
      }
   }

   if (parts.assembled.session) {
      /* initial query/aggregate/etc, and opts contains "sessionId" */
      BSON_ASSERT (!cursor->client_session);
      BSON_ASSERT (!cursor->explicit_session);
      cursor->client_session = parts.assembled.session;
      cursor->explicit_session = true;
   } else if (cursor->client_session) {
      /* a getMore with implicit or explicit session already acquired */
      mongoc_cmd_parts_set_session (&parts, cursor->client_session);
   } else {
      /* try to create an implicit session. not causally consistent. we keep
       * the session but leave cursor->explicit_session as 0, so we use the
       * same lsid for getMores but destroy the session when the cursor dies.
       */
      session_opts = mongoc_session_opts_new ();
      mongoc_session_opts_set_causal_consistency (session_opts, false);
      /* returns NULL if sessions aren't supported. ignore errors. */
      cursor->client_session =
         mongoc_client_start_session (cursor->client, session_opts, NULL);
      mongoc_cmd_parts_set_session (&parts, cursor->client_session);
      mongoc_session_opts_destroy (session_opts);
   }

   if (!mongoc_cmd_parts_set_read_concern (&parts,
                                           cursor->read_concern,
                                           server_stream->sd->max_wire_version,
                                           &cursor->error)) {
      _mongoc_bson_init_if_set (reply);
      GOTO (done);
   }

   bson_strncpy (db, cursor->ns, cursor->dblen + 1);
   parts.assembled.db_name = db;

   if (!_mongoc_cursor_opts_to_flags (
          cursor, server_stream, &parts.user_query_flags)) {
      _mongoc_bson_init_if_set (reply);
      GOTO (done);
   }

   /* we might use mongoc_cursor_set_hint to target a secondary but have no
    * read preference, so the secondary rejects the read. same if we have a
    * direct connection to a secondary (topology type "single"). with
    * OP_QUERY we handle this by setting slaveOk. here we use $readPreference.
    */
   cmd_name = _mongoc_get_command_name (command);
   is_primary =
      !cursor->read_prefs || cursor->read_prefs->mode == MONGOC_READ_PRIMARY;

   if (strcmp (cmd_name, "getMore") != 0 &&
       server_stream->sd->max_wire_version >= WIRE_VERSION_OP_MSG &&
       is_primary && parts.user_query_flags & MONGOC_QUERY_SLAVE_OK) {
      parts.read_prefs = prefs =
         mongoc_read_prefs_new (MONGOC_READ_PRIMARY_PREFERRED);
   } else {
      parts.read_prefs = cursor->read_prefs;
   }

   if (cursor->write_concern &&
       !mongoc_write_concern_is_default (cursor->write_concern) &&
       server_stream->sd->max_wire_version >= WIRE_VERSION_CMD_WRITE_CONCERN) {
      parts.assembled.is_acknowledged =
         mongoc_write_concern_is_acknowledged (cursor->write_concern);
      mongoc_write_concern_append (cursor->write_concern, &parts.extra);
   }

   if (!mongoc_cmd_parts_assemble (&parts, server_stream, &cursor->error)) {
      _mongoc_bson_init_if_set (reply);
      GOTO (done);
   }

   ret = mongoc_cluster_run_command_monitored (
      cluster, &parts.assembled, reply, &cursor->error);

   if (cursor->error.domain) {
      bson_destroy (&cursor->error_doc);
      bson_copy_to (reply, &cursor->error_doc);
   }

   /* Read and Write Concern Spec: "Drivers SHOULD parse server replies for a
    * "writeConcernError" field and report the error only in command-specific
    * helper methods that take a separate write concern parameter or an options
    * parameter that may contain a write concern option.
    *
    * Only command helpers with names like "_with_write_concern" can create
    * cursors with a non-NULL write_concern field.
    */
   if (ret && cursor->write_concern) {
      ret = !_mongoc_parse_wc_err (reply, &cursor->error);
   }

done:
   mongoc_server_stream_cleanup (server_stream);
   mongoc_cmd_parts_cleanup (&parts);
   mongoc_read_prefs_destroy (prefs);

   return ret;
}


void
_mongoc_cursor_collection (const mongoc_cursor_t *cursor,
                           const char **collection,
                           int *collection_len)
{
   /* ns is like "db.collection". Collection name is located past the ".". */
   *collection = cursor->ns + (cursor->dblen + 1);
   /* Collection name's length is ns length, minus length of db name and ".". */
   *collection_len = cursor->nslen - cursor->dblen - 1;

   BSON_ASSERT (*collection_len > 0);
}


void
_mongoc_cursor_prepare_find_command (mongoc_cursor_t *cursor,
                                     const bson_t *filter,
                                     bson_t *command)
{
   const char *collection;
   int collection_len;

   _mongoc_cursor_collection (cursor, &collection, &collection_len);
   bson_append_utf8 (command,
                     MONGOC_CURSOR_FIND,
                     MONGOC_CURSOR_FIND_LEN,
                     collection,
                     collection_len);
   bson_append_document (
      command, MONGOC_CURSOR_FILTER, MONGOC_CURSOR_FILTER_LEN, filter);
}


bool
mongoc_cursor_error (mongoc_cursor_t *cursor, bson_error_t *error)
{
   ENTRY;

   RETURN (mongoc_cursor_error_document (cursor, error, NULL));
}


bool
mongoc_cursor_error_document (mongoc_cursor_t *cursor,
                              bson_error_t *error,
                              const bson_t **doc)
{
   ENTRY;

   BSON_ASSERT (cursor);

   if (BSON_UNLIKELY (CURSOR_FAILED (cursor))) {
      bson_set_error (error,
                      cursor->error.domain,
                      cursor->error.code,
                      "%s",
                      cursor->error.message);

      if (doc) {
         *doc = &cursor->error_doc;
      }

      RETURN (true);
   }

   if (doc) {
      *doc = NULL;
   }

   RETURN (false);
}


static mongoc_cursor_state_t
_call_transition (mongoc_cursor_t *cursor)
{
   mongoc_cursor_state_t state = cursor->state;
   _mongoc_cursor_impl_transition_t fn = NULL;
   switch (state) {
   case UNPRIMED:
      fn = cursor->impl.prime;
      break;
   case IN_BATCH:
      fn = cursor->impl.pop_from_batch;
      break;
   case END_OF_BATCH:
      fn = cursor->impl.get_next_batch;
      break;
   case DONE:
   default:
      fn = NULL;
      break;
   }
   if (!fn) {
      return DONE;
   }
   state = fn (cursor);
   if (cursor->error.domain) {
      state = DONE;
   }
   return state;
}


bool
mongoc_cursor_next (mongoc_cursor_t *cursor, const bson_t **bson)
{
   bool ret = false;
   bool attempted_refresh = false;

   ENTRY;

   BSON_ASSERT (cursor);
   BSON_ASSERT (bson);

   TRACE ("cursor_id(%" PRId64 ")", cursor->cursor_id);

   if (bson) {
      *bson = NULL;
   }

   if (CURSOR_FAILED (cursor)) {
      RETURN (false);
   }

   if (cursor->state == DONE) {
      bson_set_error (&cursor->error,
                      MONGOC_ERROR_CURSOR,
                      MONGOC_ERROR_CURSOR_INVALID_CURSOR,
                      "Cannot advance a completed or failed cursor.");
      RETURN (false);
   }

   /*
    * We cannot proceed if another cursor is receiving results in exhaust mode.
    */
   if (cursor->client->in_exhaust && !cursor->in_exhaust) {
      bson_set_error (&cursor->error,
                      MONGOC_ERROR_CLIENT,
                      MONGOC_ERROR_CLIENT_IN_EXHAUST,
                      "Another cursor derived from this client is in exhaust.");
      RETURN (false);
   }

   cursor->current = NULL;

   /* if an error was set on this cursor before calling next, transition to DONE
    * immediately. */
   if (cursor->error.domain) {
      cursor->state = DONE;
      GOTO (done);
   }

   while (cursor->state != DONE) {
      /* even when there is no data to return, some cursors remain open and
       * continue sending empty batches (e.g. a tailable or change stream
       * cursor). in that case, do not attempt to get another batch. */
      if (cursor->state == END_OF_BATCH) {
         if (attempted_refresh) {
            RETURN (false);
         }
         attempted_refresh = true;
      }

      cursor->state = _call_transition (cursor);

      /* check if we received a document. */
      if (cursor->current) {
         *bson = cursor->current;
         ret = true;
         GOTO (done);
      }

      if (cursor->state == DONE) {
         GOTO (done);
      }
   }

done:
   cursor->count++;
   RETURN (ret);
}


bool
mongoc_cursor_more (mongoc_cursor_t *cursor)
{
   ENTRY;

   BSON_ASSERT (cursor);

   if (CURSOR_FAILED (cursor)) {
      RETURN (false);
   }

   RETURN (cursor->state != DONE);
}


void
mongoc_cursor_get_host (mongoc_cursor_t *cursor, mongoc_host_list_t *host)
{
   mongoc_server_description_t *description;

   BSON_ASSERT (cursor);
   BSON_ASSERT (host);

   memset (host, 0, sizeof *host);

   if (!cursor->server_id) {
      MONGOC_WARNING ("%s(): Must send query before fetching peer.", BSON_FUNC);
      return;
   }

   description = mongoc_topology_server_by_id (
      cursor->client->topology, cursor->server_id, &cursor->error);
   if (!description) {
      return;
   }

   *host = description->host;

   mongoc_server_description_destroy (description);

   EXIT;
}


mongoc_cursor_t *
mongoc_cursor_clone (const mongoc_cursor_t *cursor)
{
   mongoc_cursor_t *_clone;

   BSON_ASSERT (cursor);

   _clone = (mongoc_cursor_t *) bson_malloc0 (sizeof *_clone);

   _clone->client = cursor->client;
   _clone->nslen = cursor->nslen;
   _clone->dblen = cursor->dblen;
   _clone->explicit_session = cursor->explicit_session;

   if (cursor->read_prefs) {
      _clone->read_prefs = mongoc_read_prefs_copy (cursor->read_prefs);
   }

   if (cursor->read_concern) {
      _clone->read_concern = mongoc_read_concern_copy (cursor->read_concern);
   }

   if (cursor->write_concern) {
      _clone->write_concern = mongoc_write_concern_copy (cursor->write_concern);
   }

   if (cursor->explicit_session) {
      _clone->client_session = cursor->client_session;
   }

   bson_copy_to (&cursor->opts, &_clone->opts);
   bson_init (&_clone->error_doc);

   bson_strncpy (_clone->ns, cursor->ns, sizeof _clone->ns);

   /* copy the context functions by default. */
   memcpy (&_clone->impl, &cursor->impl, sizeof (cursor->impl));
   if (cursor->impl.clone) {
      cursor->impl.clone (&_clone->impl, &cursor->impl);
   }

   mongoc_counter_cursors_active_inc ();

   RETURN (_clone);
}


/*
 *--------------------------------------------------------------------------
 *
 * mongoc_cursor_is_alive --
 *
 *       Deprecated for mongoc_cursor_more.
 *
 *--------------------------------------------------------------------------
 */

bool
mongoc_cursor_is_alive (const mongoc_cursor_t *cursor) /* IN */
{
   return mongoc_cursor_more ((mongoc_cursor_t *) cursor);
}


const bson_t *
mongoc_cursor_current (const mongoc_cursor_t *cursor) /* IN */
{
   BSON_ASSERT (cursor);

   return cursor->current;
}


void
mongoc_cursor_set_batch_size (mongoc_cursor_t *cursor, uint32_t batch_size)
{
   BSON_ASSERT (cursor);

   _mongoc_cursor_set_opt_int64 (
      cursor, MONGOC_CURSOR_BATCH_SIZE, (int64_t) batch_size);
}


uint32_t
mongoc_cursor_get_batch_size (const mongoc_cursor_t *cursor)
{
   BSON_ASSERT (cursor);

   return (uint32_t) _mongoc_cursor_get_opt_int64 (
      cursor, MONGOC_CURSOR_BATCH_SIZE, 0);
}


bool
mongoc_cursor_set_limit (mongoc_cursor_t *cursor, int64_t limit)
{
   BSON_ASSERT (cursor);

   if (cursor->state == UNPRIMED) {
      if (limit < 0) {
         return _mongoc_cursor_set_opt_int64 (
                   cursor, MONGOC_CURSOR_LIMIT, -limit) &&
                _mongoc_cursor_set_opt_bool (
                   cursor, MONGOC_CURSOR_SINGLE_BATCH, true);
      } else {
         return _mongoc_cursor_set_opt_int64 (
            cursor, MONGOC_CURSOR_LIMIT, limit);
      }
   } else {
      return false;
   }
}


int64_t
mongoc_cursor_get_limit (const mongoc_cursor_t *cursor)
{
   int64_t limit;
   bool single_batch;

   BSON_ASSERT (cursor);

   limit = _mongoc_cursor_get_opt_int64 (cursor, MONGOC_CURSOR_LIMIT, 0);
   single_batch =
      _mongoc_cursor_get_opt_bool (cursor, MONGOC_CURSOR_SINGLE_BATCH);

   if (limit > 0 && single_batch) {
      limit = -limit;
   }

   return limit;
}


bool
mongoc_cursor_set_hint (mongoc_cursor_t *cursor, uint32_t server_id)
{
   BSON_ASSERT (cursor);

   if (cursor->server_id) {
      MONGOC_ERROR ("mongoc_cursor_set_hint: server_id already set");
      return false;
   }

   if (!server_id) {
      MONGOC_ERROR ("mongoc_cursor_set_hint: cannot set server_id to 0");
      return false;
   }

   cursor->server_id = server_id;

   return true;
}


uint32_t
mongoc_cursor_get_hint (const mongoc_cursor_t *cursor)
{
   BSON_ASSERT (cursor);

   return cursor->server_id;
}


int64_t
mongoc_cursor_get_id (const mongoc_cursor_t *cursor)
{
   BSON_ASSERT (cursor);

   return cursor->cursor_id;
}


void
mongoc_cursor_set_max_await_time_ms (mongoc_cursor_t *cursor,
                                     uint32_t max_await_time_ms)
{
   BSON_ASSERT (cursor);

   if (cursor->state == UNPRIMED) {
      _mongoc_cursor_set_opt_int64 (
         cursor, MONGOC_CURSOR_MAX_AWAIT_TIME_MS, (int64_t) max_await_time_ms);
   }
}


uint32_t
mongoc_cursor_get_max_await_time_ms (const mongoc_cursor_t *cursor)
{
   bson_iter_t iter;

   BSON_ASSERT (cursor);

   if (bson_iter_init_find (
          &iter, &cursor->opts, MONGOC_CURSOR_MAX_AWAIT_TIME_MS)) {
      return (uint32_t) bson_iter_as_int64 (&iter);
   }

   return 0;
}


/*
 *--------------------------------------------------------------------------
 *
 * mongoc_cursor_new_from_command_reply --
 *
 *       Low-level function to initialize a mongoc_cursor_t from the
 *       reply to a command like "aggregate", "find", or "listCollections".
 *
 *       Useful in drivers that wrap the C driver; in applications, use
 *       high-level functions like mongoc_collection_aggregate instead.
 *
 * Returns:
 *       A cursor.
 *
 * Side effects:
 *       On failure, the cursor's error is set: retrieve it with
 *       mongoc_cursor_error. On success or failure, "reply" is
 *       destroyed.
 *
 *--------------------------------------------------------------------------
 */

mongoc_cursor_t *
mongoc_cursor_new_from_command_reply (mongoc_client_t *client,
                                      bson_t *reply,
                                      uint32_t server_id)
{
   mongoc_cursor_t *cursor;
   bson_t cmd = BSON_INITIALIZER;
   bson_t opts = BSON_INITIALIZER;

   BSON_ASSERT (client);
   BSON_ASSERT (reply);
   /* options are passed through by adding them to reply. */
   bson_copy_to_excluding_noinit (reply,
                                  &opts,
                                  "cursor",
                                  "ok",
                                  "operationTime",
                                  "$clusterTime",
                                  "$gleStats",
                                  NULL);

   cursor =
      _mongoc_cursor_cmd_new_from_reply (client, &cmd, &opts, reply, server_id);
   bson_destroy (&cmd);
   bson_destroy (&opts);

   return cursor;
}


bool
_mongoc_cursor_start_reading_response (mongoc_cursor_t *cursor,
                                       mongoc_cursor_response_t *response)
{
   bson_iter_t iter;
   bson_iter_t child;
   const char *ns;
   uint32_t nslen;
   bool in_batch = false;

   if (bson_iter_init_find (&iter, &response->reply, "cursor") &&
       BSON_ITER_HOLDS_DOCUMENT (&iter) && bson_iter_recurse (&iter, &child)) {
      while (bson_iter_next (&child)) {
         if (BSON_ITER_IS_KEY (&child, "id")) {
            cursor->cursor_id = bson_iter_as_int64 (&child);
         } else if (BSON_ITER_IS_KEY (&child, "ns")) {
            ns = bson_iter_utf8 (&child, &nslen);
            _mongoc_set_cursor_ns (cursor, ns, nslen);
         } else if (BSON_ITER_IS_KEY (&child, "firstBatch") ||
                    BSON_ITER_IS_KEY (&child, "nextBatch")) {
            if (BSON_ITER_HOLDS_ARRAY (&child) &&
                bson_iter_recurse (&child, &response->batch_iter)) {
               in_batch = true;
            }
         }
      }
   }

   /* Driver Sessions Spec: "When an implicit session is associated with a
    * cursor for use with getMore operations, the session MUST be returned to
    * the pool immediately following a getMore operation that indicates that the
    * cursor has been exhausted." */
   if (cursor->cursor_id == 0 && cursor->client_session &&
       !cursor->explicit_session) {
      mongoc_client_session_destroy (cursor->client_session);
      cursor->client_session = NULL;
   }

   return in_batch;
}


void
_mongoc_cursor_response_read (mongoc_cursor_t *cursor,
                              mongoc_cursor_response_t *response,
                              const bson_t **bson)
{
   const uint8_t *data = NULL;
   uint32_t data_len = 0;

   ENTRY;

   if (bson_iter_next (&response->batch_iter) &&
       BSON_ITER_HOLDS_DOCUMENT (&response->batch_iter)) {
      bson_iter_document (&response->batch_iter, &data_len, &data);

      /* bson_iter_next guarantees valid BSON, so this must succeed */
      BSON_ASSERT (bson_init_static (&response->current_doc, data, data_len));
      *bson = &response->current_doc;
   }
}

/* sets cursor error if could not get the next batch. */
void
_mongoc_cursor_response_refresh (mongoc_cursor_t *cursor,
                                 const bson_t *command,
                                 const bson_t *opts,
                                 mongoc_cursor_response_t *response)
{
   ENTRY;

   bson_destroy (&response->reply);

   /* server replies to find / aggregate with {cursor: {id: N, firstBatch: []}},
    * to getMore command with {cursor: {id: N, nextBatch: []}}. */
   if (_mongoc_cursor_run_command (cursor, command, opts, &response->reply) &&
       _mongoc_cursor_start_reading_response (cursor, response)) {
      return;
   }
   if (!cursor->error.domain) {
      bson_set_error (&cursor->error,
                      MONGOC_ERROR_PROTOCOL,
                      MONGOC_ERROR_PROTOCOL_INVALID_REPLY,
                      "Invalid reply to %s command.",
                      _mongoc_get_command_name (command));
   }
}


void
_mongoc_cursor_prepare_getmore_command (mongoc_cursor_t *cursor,
                                        bson_t *command)
{
   const char *collection;
   int collection_len;
   int64_t batch_size;
   bool await_data;
   int32_t max_await_time_ms;

   ENTRY;

   _mongoc_cursor_collection (cursor, &collection, &collection_len);

   bson_init (command);
   bson_append_int64 (command, "getMore", 7, mongoc_cursor_get_id (cursor));
   bson_append_utf8 (command, "collection", 10, collection, collection_len);

   batch_size = mongoc_cursor_get_batch_size (cursor);

   /* See find, getMore, and killCursors Spec for batchSize rules */
   if (batch_size) {
      bson_append_int64 (command,
                         MONGOC_CURSOR_BATCH_SIZE,
                         MONGOC_CURSOR_BATCH_SIZE_LEN,
                         abs (_mongoc_n_return (cursor)));
   }

   /* Find, getMore And killCursors Commands Spec: "In the case of a tailable
      cursor with awaitData == true the driver MUST provide a Cursor level
      option named maxAwaitTimeMS (See CRUD specification for details). The
      maxTimeMS option on the getMore command MUST be set to the value of the
      option maxAwaitTimeMS. If no maxAwaitTimeMS is specified, the driver
      SHOULD not set maxTimeMS on the getMore command."
    */
   await_data = _mongoc_cursor_get_opt_bool (cursor, MONGOC_CURSOR_TAILABLE) &&
                _mongoc_cursor_get_opt_bool (cursor, MONGOC_CURSOR_AWAIT_DATA);


   if (await_data) {
      max_await_time_ms =
         (int32_t) mongoc_cursor_get_max_await_time_ms (cursor);
      if (max_await_time_ms) {
         bson_append_int32 (command,
                            MONGOC_CURSOR_MAX_TIME_MS,
                            MONGOC_CURSOR_MAX_TIME_MS_LEN,
                            max_await_time_ms);
      }
   }
}

/* sets the cursor to be empty so it returns NULL on the first call to
 * cursor_next but does not return an error. */
void
_mongoc_cursor_set_empty (mongoc_cursor_t *cursor)
{
   memset (&cursor->error, 0, sizeof (bson_error_t));
   bson_reinit (&cursor->error_doc);
   cursor->state = IN_BATCH;
}

void
_mongoc_cursor_prime (mongoc_cursor_t *cursor)
{
   cursor->state = cursor->impl.prime (cursor);
}