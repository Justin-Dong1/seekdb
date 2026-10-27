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

#define USING_LOG_PREFIX SQL_RESV
#include "sql/resolver/ddl/ob_explain_resolver.h"
#include "sql/resolver/ob_resolver.h"
#include "sql/parser/ob_parser.h"
#include "sql/engine/basic/ob_file_scan_utils.h"
#include "lib/utility/utility.h"

using namespace oceanbase;
using namespace sql;
using namespace common;
namespace
{
// Schema inference and path/feature checks have already run through the ordinary
// child resolver. Render its metadata as a native constant SELECT, without
// executing the file query or treating DESCRIBE as EXPLAIN.
int resolve_file_description(ObResolverParams &params,
                             const ObSelectStmt &query, ObStmt *&description)
{
  int ret = OB_SUCCESS;
  const TableItem *table = query.get_table_item(0);
  ObSqlString sql;
  if (OB_ISNULL(table) || OB_ISNULL(table->file_table_def_)
      || OB_ISNULL(params.allocator_) || OB_ISNULL(params.session_info_)) {
    ret = OB_ERR_UNEXPECTED;
  } else if (OB_FAIL(sql.append(
        "select column_name,column_type,`null`,`key`,`default`,extra from ("))) {
  } else {
    const ObFileTableDef &file = *table->file_table_def_;
    for (int64_t i = 0; OB_SUCC(ret) && i < query.get_select_item_size(); ++i) {
      const SelectItem &item = query.get_select_item(i);
      if (OB_ISNULL(item.expr_) || !item.expr_->is_column_ref_expr()) {
        ret = OB_NOT_SUPPORTED;
        LOG_USER_ERROR(OB_NOT_SUPPORTED, "DESCRIBE file expressions other than columns");
      } else {
        const ObColumnRefRawExpr *column = static_cast<const ObColumnRefRawExpr *>(item.expr_);
        const int64_t index = static_cast<int64_t>(column->get_column_id() - OB_APP_MIN_COLUMN_ID);
        if (column->get_table_id() != table->table_id_
            || index < 0 || index >= file.columns_.count()) {
          ret = OB_ERR_UNEXPECTED;
        } else {
          const ObFileTableColumnDef &metadata = file.columns_.at(index);
          if (i > 0 && OB_FAIL(sql.append(" union all "))) {
          } else if (OB_FAIL(sql.append_fmt("select %lld as ordinal,convert(", static_cast<long long>(i)))) {
          } else if (OB_FAIL(sql_append_hex_escape_str(item.alias_name_, sql))) {
          } else if (OB_FAIL(sql.append_fmt(
                " using utf8mb4) as column_name,'%s' as column_type,'%s' as `null`,"
                "NULL as `key`,NULL as `default`,NULL as extra",
                ObFileScanUtils::column_type_name(metadata.type_),
                metadata.nullable_ ? "YES" : "NO"))) {
          }
        }
      }
    }
    if (OB_SUCC(ret) && OB_FAIL(sql.append(") file_description order by ordinal"))) {
    }
  }
  if (OB_SUCC(ret)) {
    ParseResult parsed;
    ObParser parser(*params.allocator_, params.session_info_->get_sql_mode(),
                    params.session_info_->get_charsets4parser());
    if (OB_FAIL(parser.parse(sql.string(), parsed))) {
    } else if (OB_ISNULL(parsed.result_tree_)
               || 1 != parsed.result_tree_->num_child_
               || OB_ISNULL(parsed.result_tree_->children_)
               || OB_ISNULL(parsed.result_tree_->children_[0])) {
      ret = OB_ERR_UNEXPECTED;
    } else {
      ObResolver resolver(params);
      ret = resolver.resolve(ObResolver::IS_NOT_PREPARED_STMT,
                             *parsed.result_tree_->children_[0], description);
    }
  }
  return ret;
}
} // namespace

const static int64_t EXPLAIN_FORMAT = 0;
const static int64_t EXPLAIN_DISPLAY_OPTION = 1;
const static int64_t EXPLAIN_INTO_TABLE = 2;
const static int64_t EXPLAIN_STMT_ID = 3;
const static int64_t EXPLAIN_CHILD_STMT = 4;
int ObExplainResolver::resolve(const ParseNode &parse_tree)
{
  int ret = OB_SUCCESS;
  ObExplainStmt *explain_stmt = NULL;
  if (!(T_EXPLAIN == parse_tree.type_  && 5 == parse_tree.num_child_)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("invalid EXPLAIN syntax",
             "type", parse_tree.type_,
             "# of child", parse_tree.num_child_);
  } else if (NULL == (explain_stmt = create_stmt<ObExplainStmt>())) {
    ret = OB_SQL_RESOLVER_NO_MEMORY;
    LOG_WARN("failed to create explain stmt");
  } else {
    ParseNode *child_node = parse_tree.children_[EXPLAIN_CHILD_STMT];
    ObStmt *child_stmt = NULL;
    ObExplainDisplayOpt opt;
    opt.with_tree_line_ = true;
    if (OB_ISNULL(child_node)) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("empty parse tree for stmt");
    } else if (!(T_SELECT == child_node->type_
                 || T_DELETE == child_node->type_
                 || T_INSERT == child_node->type_
                 || T_UPDATE == child_node->type_
                 || T_REPLACE == child_node->type_)) {
      ret = OB_NOT_SUPPORTED;
      LOG_WARN("invalid stmt type for EXPLAIN", "type", child_node->type_);
      LOG_USER_ERROR(OB_NOT_SUPPORTED, "stmt type for EXPLAIN");
    } else {
      ObResolver resolver(params_);
      if (OB_FAIL(resolver.resolve(ObResolver::IS_NOT_PREPARED_STMT,
                                   *child_node,
                                   child_stmt))) {
      } else if (OB_ISNULL(child_stmt)) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("failed to generate stmt for child stmt");
      } else if (parse_tree.str_len_ > 0 && child_stmt->is_select_stmt()
                 && 1 == static_cast<ObSelectStmt *>(child_stmt)->get_table_size()
                 && OB_NOT_NULL(static_cast<ObSelectStmt *>(child_stmt)->get_table_item(0))
                 && static_cast<ObSelectStmt *>(child_stmt)->get_table_item(0)->is_file_table()) {
        return resolve_file_description(params_, *static_cast<ObSelectStmt *>(child_stmt), stmt_);
      } else {
        explain_stmt->set_explain_query_stmt(static_cast<ObDMLStmt *>(child_stmt));
        ParseNode *format = parse_tree.children_[EXPLAIN_FORMAT];
        if (NULL != format) {
          switch (format->type_) {
          case T_OUTLINE:
          {
            explain_stmt->set_explain_format(EXPLAIN_OUTLINE);
            opt.with_tree_line_ = true;
          } break;
          case T_EXTENDED:
          {
            explain_stmt->set_explain_format(EXPLAIN_EXTENDED);
            opt.with_tree_line_ = true;
          } break;
          case T_PARTITIONS:
          {
            explain_stmt->set_explain_format(EXPLAIN_PARTITIONS);
            opt.with_tree_line_ = true;
          } break;
          case T_TRADITIONAL:
          {
            explain_stmt->set_explain_format(EXPLAIN_TRADITIONAL);
            opt.with_tree_line_ = true;
          } break;
          case T_FORMAT_JSON:
          {
            explain_stmt->set_explain_format(EXPLAIN_FORMAT_JSON);
            opt.with_tree_line_ = false;
          } break;
          case T_BASIC: {
            explain_stmt->set_explain_format(EXPLAIN_BASIC);
            opt.with_tree_line_ = true;
            break;
          }
          case T_EXTENDED_NOADDR: {
            explain_stmt->set_explain_format(EXPLAIN_EXTENDED_NOADDR);
            opt.with_tree_line_ = true;
            break;
          }
          case T_PLANREGRESS: {
            explain_stmt->set_explain_format(EXPLAIN_PLANREGRESS);
            opt.with_tree_line_ = true;
            break;
          }
          default:
            ret = OB_ERROR;
            LOG_WARN("unknown explain format", "format", format->type_);
            break;
          }
        }
      }
    }
    ParseNode *opt_node = parse_tree.children_[EXPLAIN_DISPLAY_OPTION];
    if (OB_SUCC(ret) && NULL != opt_node) {
      switch (opt_node->type_) {
      case T_PRETTY_COLOR:
        opt.with_color_ = true;
        // pass
      case T_PRETTY:
        opt.with_tree_line_ = true;
        break;
      default:
        ret = OB_ERR_UNEXPECTED;
      }
    }
    if (OB_SUCC(ret)) {
      explain_stmt->set_display_opt(opt);
    }
    ParseNode *into_table_node = parse_tree.children_[EXPLAIN_INTO_TABLE];
    if (OB_SUCC(ret) && NULL != into_table_node) {
      ObString into_table(into_table_node->str_len_, into_table_node->str_value_);
      explain_stmt->set_into_table(into_table);
    }
    ParseNode *statement_id_node = parse_tree.children_[EXPLAIN_STMT_ID];
    if (OB_SUCC(ret) && NULL != statement_id_node) {
      ObString statement_id(statement_id_node->str_len_, statement_id_node->str_value_);
      explain_stmt->set_statement_id(statement_id);
    }
  }

  return ret;
}
