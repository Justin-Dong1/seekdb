#pragma once

#include "common/mysqlclient/ob_mysql_proxy.h"
#include "common/mysqlclient/ob_mysql_transaction.h"
#include "common/mysqlclient/ob_mysql_result.h"
#include "common/json_type/ob_json_tree.h"
#include "share/ob_server_struct.h"

namespace oceanbase { namespace omt {
namespace {
using namespace common;

int ai_hex(ObSqlString &sql, const ObString &value)
{
  int ret = sql.append("X'");
  for (int64_t i = 0; OB_SUCC(ret) && i < value.length(); ++i) {
    ret = sql.append_fmt("%02x", static_cast<unsigned char>(value.ptr()[i]));
  }
  if (OB_SUCC(ret)) { ret = sql.append("'"); }
  return ret;
}

int ai_identifier(ObSqlString &sql, const ObString &name)
{
  int ret = sql.append("`");
  for (int64_t i = 0; OB_SUCC(ret) && i < name.length(); ++i) {
    if (name.ptr()[i] == '`') { OZ(sql.append("`")); }
    OZ(sql.append(name.ptr() + i, 1));
  }
  OZ(sql.append("`"));
  return ret;
}

struct AiPendingResult {
  ObString task, request, predicate, snapshot, spec, column, database, table, result;
  int64_t table_id = 0, column_id = 0, retry = 0, max_retries = 0;
};

int ai_copy_field(sqlclient::ObMySQLResult &result, const char *name,
                  ObIAllocator &allocator, ObString &destination)
{
  int ret = OB_SUCCESS;
  ObString source;
  OZ(result.get_varchar(name, source));
  OZ(ob_write_string(allocator, source, destination));
  return ret;
}

int process_ai_column_group()
{
  int ret = OB_SUCCESS;
  if (GCTX.sql_proxy_ == nullptr) { return OB_NOT_INIT; }
  ObArenaAllocator allocator("AiColumnWorker");
  ObSEArray<AiPendingResult, 4> jobs;
  ObString request;
  {
    alignas(16) ObMySQLProxy::MySQLResult result;
    OZ(GCTX.sql_proxy_->read(result, "SELECT request_id FROM oceanbase.__all_ai_column_task "
        "WHERE eligible_ts<=now(6) ORDER BY eligible_ts LIMIT 1"));
    if (OB_SUCC(ret)) {
      OZ(result.get_result()->next());
      OZ(ai_copy_field(*result.get_result(), "request_id", allocator, request));
    }
  }
  ObSqlString select;
  OZ(select.append("SELECT q.task_id,q.request_id,q.table_id,q.column_id,q.retry_count,q.max_retries,"
      "q.rowkey_predicate,q.request_payload,q.result_spec,a.column_name,d.database_name,t.table_name "
      "FROM oceanbase.__all_ai_column_task q "
      "JOIN oceanbase.__all_ai_column a ON a.table_id=q.table_id AND a.column_id=q.column_id "
      "JOIN oceanbase.__all_table t ON t.table_id=q.table_id "
      "JOIN oceanbase.__all_database d ON d.database_id=t.database_id WHERE q.request_id="));
  OZ(ai_hex(select, request));
  {
    alignas(16) ObMySQLProxy::MySQLResult result;
    OZ(GCTX.sql_proxy_->read(result, select.ptr()));
    while (OB_SUCC(ret)) {
      if (OB_FAIL(result.get_result()->next())) { break; }
      auto &row = *result.get_result();
      AiPendingResult job;
      OZ(ai_copy_field(row, "task_id", allocator, job.task));
      OZ(ai_copy_field(row, "request_id", allocator, job.request));
      OZ(ai_copy_field(row, "rowkey_predicate", allocator, job.predicate));
      OZ(ai_copy_field(row, "request_payload", allocator, job.snapshot));
      OZ(ai_copy_field(row, "result_spec", allocator, job.spec));
      OZ(ai_copy_field(row, "column_name", allocator, job.column));
      OZ(ai_copy_field(row, "database_name", allocator, job.database));
      OZ(ai_copy_field(row, "table_name", allocator, job.table));
      OZ(row.get_int("table_id", job.table_id));
      OZ(row.get_int("column_id", job.column_id));
      OZ(row.get_int("retry_count", job.retry));
      OZ(row.get_int("max_retries", job.max_retries));
      OZ(jobs.push_back(job));
    }
    if (ret == OB_ITER_END) { ret = OB_SUCCESS; }
  }
  if (OB_FAIL(ret) || jobs.empty()) { return ret; }
  int inference_error = OB_SUCCESS;
  for (int64_t i = 0; OB_SUCC(ret) && i < jobs.count(); ++i) {
    auto &job = jobs.at(i);
    ObIJsonBase *spec = nullptr, *expression = nullptr, *prompt = nullptr, *function = nullptr;
    ObSqlString query;
    OZ(ObJsonBaseFactory::get_json_base(&allocator, job.spec,
        ObJsonInType::JSON_TREE, ObJsonInType::JSON_TREE, spec));
    OZ(spec->get_object_value("expression", expression));
    OZ(spec->get_object_value("system_prompt", prompt));
    OZ(spec->get_object_value("function", function));
    ObSqlString call;
    ObString expression_text(expression->get_data_length(), expression->get_data());
    // Locate top-level argument separators without treating commas in quoted
    // literals or nested functions as separators.
    int depth = 0, commas = 0; char quote = 0;
    for (int64_t p = 0; p < expression_text.length(); ++p) {
      const char c = expression_text.ptr()[p];
      if (quote != 0) {
        if (c == '\\') { ++p; }
        else if (c == quote) {
          if (p + 1 < expression_text.length() && expression_text.ptr()[p+1] == quote) { ++p; }
          else { quote = 0; }
        }
      } else if (c == '\'' || c == '"' || c == '`') { quote = c; }
      else if (c == '(') { ++depth; }
      else if (c == ')') { --depth; }
      else if (c == ',' && depth == 1) { ++commas; }
    }
    if (ObString(function->get_data_length(), function->get_data()).case_compare("ai_complete") == 0
        && prompt->get_data_length() > 0 && commas == 1) {
      int64_t end = expression_text.length();
      while (end > 0 && isspace(expression_text.ptr()[end-1])) { --end; }
      OZ(call.append(expression_text.ptr(), end-1));
      OZ(call.append(", JSON_OBJECT('system_prompt',CONVERT("));
      OZ(ai_hex(call, ObString(prompt->get_data_length(), prompt->get_data())));
      OZ(call.append(" USING utf8mb4)))"));
    } else { OZ(call.append(expression_text)); }
    OZ(query.append_fmt("SELECT CAST((%s) AS CHAR) AS ai_result FROM (%.*s) AS ai_input",
        call.ptr(), job.snapshot.length(), job.snapshot.ptr()));
    alignas(16) ObMySQLProxy::MySQLResult result;
    OZ(GCTX.sql_proxy_->read(result, query.ptr()));
    if (OB_SUCC(ret)) {
      OZ(result.get_result()->next());
      OZ(ai_copy_field(*result.get_result(), "ai_result", allocator, job.result));
    }
  }
  inference_error = ret;
  ret = OB_SUCCESS;

  // Inference holds no row locks. Recheck and lock the exact task versions before
  // publishing, so a concurrent UPDATE/DELETE cannot receive an obsolete result.
  ObMySQLTransaction transaction;
  OZ(transaction.start(GCTX.sql_proxy_));
  bool current = true;
  for (int64_t i = 0; OB_SUCC(ret) && current && i < jobs.count(); ++i) {
    ObSqlString lock;
    OZ(lock.append("SELECT task_id FROM oceanbase.__all_ai_column_task WHERE task_id="));
    OZ(ai_hex(lock, jobs.at(i).task));
    OZ(lock.append(" FOR UPDATE"));
    alignas(16) ObMySQLProxy::MySQLResult result;
    OZ(transaction.read(result, lock.ptr()));
    if (OB_SUCC(ret)) {
      ret = result.get_result()->next();
      if (ret == OB_ITER_END) { current = false; ret = OB_SUCCESS; }
    }
  }
  bool retry = false;
  for (int64_t i = 0; i < jobs.count(); ++i) {
    retry |= jobs.at(i).retry < jobs.at(i).max_retries;
  }
  ObSqlString write;
  int64_t affected = 0;
  if (OB_SUCC(ret) && current && inference_error == OB_SUCCESS) {
    OZ(write.append("UPDATE "));
    OZ(ai_identifier(write, jobs.at(0).database));
    OZ(write.append("."));
    OZ(ai_identifier(write, jobs.at(0).table));
    OZ(write.append(" SET "));
    for (int64_t i = 0; OB_SUCC(ret) && i < jobs.count(); ++i) {
      if (i > 0) { OZ(write.append(",")); }
      OZ(ai_identifier(write, jobs.at(i).column));
      OZ(write.append("=CONVERT("));
      OZ(ai_hex(write, jobs.at(i).result));
      OZ(write.append(" USING utf8mb4)"));
      OZ(write.append(","));
      ObSqlString state_name;
      OZ(state_name.append_fmt("__ai_%lu_state", jobs.at(i).column_id));
      OZ(ai_identifier(write, state_name.string()));
      OZ(write.append("=1"));
    }
    OZ(write.append_fmt(" WHERE %.*s", jobs.at(0).predicate.length(), jobs.at(0).predicate.ptr()));
    OZ(transaction.write(write.ptr(), affected));
  }
  if (OB_SUCC(ret) && current && inference_error != OB_SUCCESS && !retry) {
    // Terminal failure is persisted on the row before task cleanup, so strict
    // reads can distinguish FAILED from a legitimate nullable result.
    OZ(write.append("UPDATE "));
    OZ(ai_identifier(write, jobs.at(0).database));
    OZ(write.append("."));
    OZ(ai_identifier(write, jobs.at(0).table));
    OZ(write.append(" SET "));
    for (int64_t i = 0; OB_SUCC(ret) && i < jobs.count(); ++i) {
      if (i > 0) { OZ(write.append(",")); }
      ObSqlString state_name;
      OZ(state_name.append_fmt("__ai_%lu_state", jobs.at(i).column_id));
      OZ(ai_identifier(write, state_name.string()));
      OZ(write.append("=2"));
    }
    OZ(write.append_fmt(" WHERE %.*s", jobs.at(0).predicate.length(), jobs.at(0).predicate.ptr()));
    OZ(transaction.write(write.ptr(), affected));
  }
  if (OB_SUCC(ret) && current) {
    write.reset();
    if (inference_error != OB_SUCCESS && retry) {
      OZ(write.append_fmt("UPDATE oceanbase.__all_ai_column_task SET state=3,retry_count=retry_count+1,"
          "last_error_code=%d,eligible_ts=DATE_ADD(now(6),INTERVAL 1 SECOND) WHERE request_id=", inference_error));
    } else {
      OZ(write.append("DELETE FROM oceanbase.__all_ai_column_task WHERE request_id="));
    }
    OZ(ai_hex(write, request));
    OZ(transaction.write(write.ptr(), affected));
  }
  if (transaction.is_started()) {
    const int end_ret = transaction.end(OB_SUCC(ret) && current);
    if (OB_SUCC(ret)) { ret = end_ret; }
  }
  return ret;
}
} // namespace
} } // namespace oceanbase::omt
