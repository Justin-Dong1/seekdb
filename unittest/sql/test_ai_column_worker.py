"""End-to-end checks for asynchronous AI-column execution and strict reads."""
import argparse
import time

import pymysql


def wait_until(predicate, message, timeout=15):
    deadline = time.time() + timeout
    while time.time() < deadline:
        if predicate():
            return
        time.sleep(0.05)
    raise AssertionError(message)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", required=True, type=int)
    args = parser.parse_args()
    connection = pymysql.connect(host="127.0.0.1", port=args.port, user="root",
                                 charset="utf8mb4", autocommit=True,
                                 read_timeout=30)
    with connection, connection.cursor() as cursor:
        cursor.execute("DROP DATABASE IF EXISTS ai_column_worker_test")
        cursor.execute("CREATE DATABASE ai_column_worker_test")
        for statement in (
            "CALL DBMS_AI_SERVICE.DROP_AI_MODEL_ENDPOINT('worker_ok_ep')",
            "CALL DBMS_AI_SERVICE.DROP_AI_MODEL_ENDPOINT('worker_fail_ep')",
            "CALL DBMS_AI_SERVICE.DROP_AI_MODEL('worker_ok')",
            "CALL DBMS_AI_SERVICE.DROP_AI_MODEL('worker_fail')",
        ):
            try:
                cursor.execute(statement)
            except pymysql.MySQLError:
                pass
        cursor.execute("""CALL DBMS_AI_SERVICE.CREATE_AI_MODEL('worker_ok',
            '{"type":"completion","model_name":"mock-complete"}')""")
        cursor.execute("""CALL DBMS_AI_SERVICE.CREATE_AI_MODEL('worker_fail',
            '{"type":"completion","model_name":"mock-fail"}')""")
        cursor.execute("""CALL DBMS_AI_SERVICE.CREATE_AI_MODEL_ENDPOINT('worker_ok_ep',
            '{"ai_model_name":"worker_ok","url":"http://127.0.0.1:19122/v1/chat/completions","access_key":"test-key","provider":"openai","request_model_name":"mock-complete"}')""")
        cursor.execute("""CALL DBMS_AI_SERVICE.CREATE_AI_MODEL_ENDPOINT('worker_fail_ep',
            '{"ai_model_name":"worker_fail","url":"http://127.0.0.1:19122/v1/always-fail/completions","access_key":"test-key","provider":"openai","request_model_name":"mock-fail"}')""")
        cursor.execute("""CREATE TABLE ai_column_worker_test.docs(
            id BIGINT PRIMARY KEY, content VARCHAR(200),
            result VARCHAR(400) AI COLUMN(AI_COMPLETE('worker_ok', content))
              WITH (sync_mode='async', max_retries=2))""")

        cursor.execute("INSERT INTO ai_column_worker_test.docs(id, content) VALUES(1, 'hello')")
        cursor.execute("SET SESSION ai_column_strict_read=ON")
        cursor.execute("SELECT id FROM ai_column_worker_test.docs")
        assert cursor.fetchall() == ((1,),), "strict mode must allow non-AI columns"
        try:
            cursor.execute("SELECT result FROM ai_column_worker_test.docs")
        except pymysql.MySQLError as error:
            assert error.args[0] == 11120, error
        else:
            raise AssertionError("strict mode accepted a pending AI result")
        cursor.execute("SET SESSION ai_column_strict_read=OFF")

        def result_equals(row_id, expected):
            cursor.execute("SELECT result FROM ai_column_worker_test.docs WHERE id=%s", (row_id,))
            row = cursor.fetchone()
            return row is not None and row[0] == expected

        wait_until(lambda: result_equals(1, "mock:hello"), "worker did not write result")
        cursor.execute("SET SESSION ai_column_strict_read=ON")
        cursor.execute("SELECT result FROM ai_column_worker_test.docs")
        assert cursor.fetchone() == ("mock:hello",)
        cursor.execute("SET SESSION ai_column_strict_read=OFF")

        cursor.execute("""CREATE TABLE ai_column_worker_test.typed_docs(
            id BIGINT PRIMARY KEY, content VARCHAR(200),
            tags JSON AI COLUMN(AI_COMPLETE('worker_ok', CONCAT('Classify: ', content)))
              WITH (sync_mode='async', max_retries=2),
            score BIGINT AI COLUMN(AI_COMPLETE('worker_ok', CONCAT('Score: ', content)))
              WITH (sync_mode='async', max_retries=2))""")
        cursor.execute("INSERT INTO ai_column_worker_test.typed_docs(id, content) VALUES(10, 'typed')")

        def typed_ready():
            cursor.execute("""SELECT JSON_UNQUOTE(JSON_EXTRACT(tags, '$.category')), score
                FROM ai_column_worker_test.typed_docs WHERE id=10""")
            return cursor.fetchone() == ("mock", 7)

        wait_until(typed_ready, "grouped typed results were not written")
        cursor.execute("""SELECT COUNT(*) FROM oceanbase.__all_ai_column_task q
            JOIN oceanbase.__all_table t ON t.table_id=q.table_id
            JOIN oceanbase.__all_database d ON d.database_id=t.database_id
            WHERE d.database_name='ai_column_worker_test' AND t.table_name='typed_docs'""")
        assert cursor.fetchone()[0] == 0, "grouped result write left task rows"

        cursor.execute("INSERT INTO ai_column_worker_test.docs(id, content) VALUES(2, 'retry-once')")
        wait_until(lambda: result_equals(2, "mock:retry-once"),
                   "retry did not eventually write the result")
        cursor.execute("""SELECT COUNT(*) FROM oceanbase.__all_ai_column_task q
            JOIN oceanbase.__all_table t ON t.table_id=q.table_id
            JOIN oceanbase.__all_database d ON d.database_id=t.database_id
            WHERE d.database_name='ai_column_worker_test'""")
        assert cursor.fetchone()[0] == 0, "successful retry left task rows"

        cursor.execute("""CREATE TABLE ai_column_worker_test.fail_docs(
            id BIGINT PRIMARY KEY, content VARCHAR(200),
            result VARCHAR(400) AI COLUMN(AI_COMPLETE('worker_fail', content))
              WITH (sync_mode='async', max_retries=0))""")
        cursor.execute("INSERT INTO ai_column_worker_test.fail_docs(id, content) VALUES(3, 'terminal')")

        def failed_task_cleaned():
            cursor.execute("""SELECT COUNT(*) FROM oceanbase.__all_ai_column_task q
                JOIN oceanbase.__all_table t ON t.table_id=q.table_id
                JOIN oceanbase.__all_database d ON d.database_id=t.database_id
                WHERE d.database_name='ai_column_worker_test' AND t.table_name='fail_docs'""")
            return cursor.fetchone()[0] == 0

        wait_until(failed_task_cleaned, "terminal failure did not clean task rows")
        cursor.execute("SET SESSION ai_column_strict_read=ON")
        try:
            cursor.execute("SELECT result FROM ai_column_worker_test.fail_docs")
        except pymysql.MySQLError as error:
            assert error.args[0] == 11120, error
        else:
            raise AssertionError("strict mode accepted a terminally failed AI result")
        cursor.execute("SET SESSION ai_column_strict_read=OFF")
        print("AI_COLUMN_WORKER_STRICT_TEST_PASS")


if __name__ == "__main__":
    main()
