"""Verify AI-column task insertion against a local, disposable seekdb server."""
import argparse
import pymysql


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--port', required=True, type=int)
    args = parser.parse_args()
    options = dict(host='127.0.0.1', port=args.port, user='root',
                   charset='utf8mb4', autocommit=True, read_timeout=30)
    with pymysql.connect(**options) as writer, pymysql.connect(**options) as reader:
        with writer.cursor() as cur, reader.cursor() as other:
            cur.execute('CREATE DATABASE IF NOT EXISTS ai_column_transaction_test')
            cur.execute('USE ai_column_transaction_test')
            cur.execute('DROP TABLE IF EXISTS docs')
            cur.execute("""CREATE TABLE docs (
                id BIGINT PRIMARY KEY, content VARCHAR(200), title INT DEFAULT 0,
                tags JSON AI COLUMN(AI_COMPLETE('transaction_mock', content)),
                summary VARCHAR(200) AI COLUMN(AI_COMPLETE('transaction_mock', content)))""")
            cur.execute("""SELECT t.table_id FROM oceanbase.__all_table t
                JOIN oceanbase.__all_database d ON d.database_id=t.database_id
                WHERE d.database_name='ai_column_transaction_test' AND t.table_name='docs'""")
            table_id = cur.fetchone()[0]
            task_count = 'SELECT COUNT(*) FROM oceanbase.__all_ai_column_task WHERE table_id=%s'

            def count(cursor):
                cursor.execute(task_count, (table_id,))
                return cursor.fetchone()[0]

            writer.begin()
            cur.execute('INSERT INTO docs(id,content) VALUES(%s,%s)', (1, "quote' and `identifier`"))
            assert count(cur) == 2, 'one task per AI column in writer transaction'
            assert count(other) == 0, 'uncommitted tasks must be invisible to worker'
            cur.execute("""SELECT COUNT(DISTINCT request_id), MIN(eligible_ts <= NOW(6))
                FROM oceanbase.__all_ai_column_task WHERE table_id=%s""", (table_id,))
            assert cur.fetchone() == (1, 1), 'same-row tasks share a request and are immediately eligible'
            cur.execute('SELECT tags IS NULL AND summary IS NULL FROM docs WHERE id=1')
            assert cur.fetchone()[0] == 1
            cur.execute('SELECT request_payload FROM oceanbase.__all_ai_column_task WHERE table_id=%s LIMIT 1',
                        (table_id,))
            snapshot_sql = cur.fetchone()[0]
            cur.execute(snapshot_sql)
            assert cur.fetchone() == (1, "quote' and `identifier`", 0), 'snapshot preserves SQL quoting'
            writer.rollback()
            assert count(cur) == 0 and count(other) == 0, 'rollback must remove tasks'
            cur.execute('SELECT COUNT(*) FROM docs')
            assert cur.fetchone()[0] == 0, 'rollback must remove source row'

            writer.begin()
            cur.execute("INSERT INTO docs(id,content) VALUES(2,'second'),(3,'third')")
            assert count(cur) == 4
            writer.commit()
            assert count(other) == 4, 'committed batch tasks visible'
            cur.execute("""SELECT COUNT(DISTINCT request_id) FROM oceanbase.__all_ai_column_task
                WHERE table_id=%s""", (table_id,))
            assert cur.fetchone()[0] == 2, 'different rows need different request ids'
            try:
                cur.execute("INSERT INTO docs(id,content) VALUES(4,'rolled back'),(2,'duplicate')")
            except pymysql.IntegrityError as error:
                assert error.args[0] == 1062
            else:
                raise AssertionError('expected duplicate key')
            assert count(other) == 4, 'failed statement must leave no orphan tasks'
            cur.execute('SELECT COUNT(*) FROM docs WHERE id=4')
            assert cur.fetchone()[0] == 0
            cur.execute("UPDATE docs SET tags=JSON_OBJECT('ready',true), summary='ready' WHERE id=2")
            cur.execute('UPDATE docs SET title=1 WHERE id=2')
            cur.execute("SELECT tags IS NOT NULL AND summary='ready' FROM docs WHERE id=2")
            assert cur.fetchone()[0] == 1, 'unrelated update preserves AI results'
            writer.begin()
            cur.execute("UPDATE docs SET content='changed' WHERE id=2")
            cur.execute('SELECT tags IS NULL AND summary IS NULL FROM docs WHERE id=2')
            assert cur.fetchone()[0] == 1, 'dependency update invalidates both results'
            assert count(cur) == 4, 'update replaces old tasks instead of duplicating them'
            writer.rollback()
            cur.execute("SELECT content='second' AND summary='ready' FROM docs WHERE id=2")
            assert cur.fetchone()[0] == 1, 'update rollback restores results and source'
            cur.execute('UPDATE docs SET id=20 WHERE id=2')
            cur.execute('SELECT tags IS NULL AND summary IS NULL FROM docs WHERE id=20')
            assert cur.fetchone()[0] == 1, 'primary key update invalidates AI results'
            cur.execute("SELECT COUNT(*) FROM oceanbase.__all_ai_column_task WHERE table_id=%s AND rowkey_predicate='`id` = 20'",
                        (table_id,))
            assert cur.fetchone()[0] == 2, 'tasks use new primary key'
            writer.begin()
            cur.execute('DELETE FROM docs WHERE id=20')
            assert count(cur) == 2, 'delete removes pending tasks in its own transaction'
            assert count(other) == 4, 'uncommitted delete must not remove visible tasks'
            writer.rollback()
            assert count(cur) == 4, 'delete rollback restores pending tasks'
            cur.execute('DELETE FROM docs WHERE id=20')
            assert count(other) == 2, 'committed delete removes only its row tasks'
            cur.execute('DROP TABLE docs')
            assert count(other) == 0
            cur.execute('DROP TABLE IF EXISTS separate_dependencies')
            cur.execute("""CREATE TABLE separate_dependencies(
                id BIGINT PRIMARY KEY, a VARCHAR(40), b VARCHAR(40),
                x VARCHAR(40) AI COLUMN(AI_COMPLETE('transaction_mock', a)),
                y VARCHAR(40) AI COLUMN(AI_COMPLETE('transaction_mock', b)))""")
            cur.execute("INSERT INTO separate_dependencies(id,a,b) VALUES(1,'a','b')")
            cur.execute("UPDATE separate_dependencies SET x='ready-a',y='ready-b' WHERE id=1")
            cur.execute("UPDATE separate_dependencies SET a='new-a' WHERE id=1")
            cur.execute("SELECT x IS NULL, y='ready-b' FROM separate_dependencies WHERE id=1")
            assert cur.fetchone() == (1, 1), 'only dependent AI columns are invalidated'
            cur.execute('DROP TABLE separate_dependencies')
    print('PASS AI column transactions: visibility, rollback, batching, duplicate rollback, selective update, primary key update, delete, cleanup')


if __name__ == '__main__':
    main()
