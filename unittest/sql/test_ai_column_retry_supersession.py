"""Regression: RETRY must retire the selected row and column task versions.
Run against a disposable kernel: python test_ai_column_retry_schema.py --port PORT.
"""

import argparse, json, threading, time, uuid
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import pymysql


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", type=int, required=True)
    args = parser.parse_args()
    release = threading.Event()
    hold = threading.Event()
    counts = {}
    lock = threading.Lock()

    class Model(BaseHTTPRequestHandler):
        def log_message(self, *args):
            pass

        def do_POST(self):
            body = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
            text = next(m["content"] for m in body["messages"] if m["role"] == "user")
            with lock:
                counts[text] = counts.get(text, 0) + 1
                attempt = counts[text]
            if hold.is_set() and text.startswith("one"):
                release.wait(5)
            value = {
                "choices": [
                    {
                        "message": {
                            "role": "assistant",
                            "content": "mock:" + text + ":" + str(attempt),
                        }
                    }
                ]
            }
            raw = json.dumps(value).encode()
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(raw)))
            self.end_headers()
            try:
                self.wfile.write(raw)
            except (BrokenPipeError, ConnectionResetError):
                pass

    model = ThreadingHTTPServer(("127.0.0.1", 0), Model)
    threading.Thread(target=model.serve_forever, daemon=True).start()
    suffix = uuid.uuid4().hex[:8]
    database = "retry_guard_" + suffix
    name = "retry_model_" + suffix
    connection = pymysql.connect(
        host="127.0.0.1",
        port=args.port,
        user="root",
        charset="utf8mb4",
        autocommit=True,
        read_timeout=10,
    )
    try:
        with connection.cursor() as c:
            c.execute("CREATE DATABASE " + database)
            c.execute("USE " + database)
            c.execute(
                "CALL DBMS_AI_SERVICE.CREATE_AI_MODEL(%s,%s)",
                (name, json.dumps({"type": "completion", "model_name": "mock"})),
            )
            endpoint = {
                "ai_model_name": name,
                "url": "http://127.0.0.1:"
                + str(model.server_address[1])
                + "/v1/chat/completions",
                "access_key": "mock-only",
                "provider": "openai",
                "request_model_name": "mock",
            }
            c.execute(
                "CALL DBMS_AI_SERVICE.CREATE_AI_MODEL_ENDPOINT(%s,%s)",
                (name + "_ep", json.dumps(endpoint)),
            )
            c.execute(
                "CREATE TABLE docs(id BIGINT PRIMARY KEY, content VARCHAR(40), result VARCHAR(100) AI COLUMN(AI_COMPLETE('"
                + name
                + "',content)))"
            )
            c.execute("INSERT INTO docs(id,content) VALUES(1,'one'),(2,'two')")

            def results():
                c.execute("SELECT result FROM docs ORDER BY id")
                return c.fetchall()

            def wait_for(expected):
                deadline = time.monotonic() + 10
                while time.monotonic() < deadline:
                    if results() == expected:
                        return
                    time.sleep(0.03)
                raise AssertionError(
                    "Timed out waiting for results: " + repr(results())
                )

            wait_for((("mock:one:1",), ("mock:two:1",)))
            hold.set()
            # The compilation schema guard is released before DDL execution.
            # This returned 4014 before the executor acquired its own guard.
            c.execute("ALTER TABLE docs RETRY AI COLUMN result WHERE id=1")
            deadline = time.monotonic() + 5
            while counts.get("one", 0) < 2 and time.monotonic() < deadline:
                time.sleep(0.01)
            assert counts.get("one") == 2, "First retry request did not start"
            c.execute("ALTER TABLE docs RETRY AI COLUMN result WHERE id=1")
            c.execute("SELECT count(*) FROM oceanbase.__all_ai_column_task")
            assert c.fetchone()[0] == 1, "RETRY retained a superseded task version"
            assert results() == ((None,), ("mock:two:1",)), (
                "RETRY must invalidate only the selected row"
            )
            c.execute("SET SESSION ai_column_strict_read=ON")
            c.execute("SELECT id FROM docs ORDER BY id")
            assert c.fetchall() == ((1,), (2,))
            try:
                c.execute("SELECT result FROM docs WHERE id=2")
            except pymysql.MySQLError as error:
                assert error.args[0] == 11120, error
            else:
                raise AssertionError(
                    "Strict read must check the entire referenced AI column"
                )
            c.execute("SET SESSION ai_column_strict_read=OFF")
            release.set()
            wait_for((("mock:one:3",), ("mock:two:1",)))
            c.execute("ALTER TABLE docs RETRY AI COLUMN result WHERE id=999")
            assert results() == (("mock:one:3",), ("mock:two:1",)), (
                "Zero-match retry must preserve ready results"
            )
            assert counts == {"one": 3, "two": 1}, "RETRY invoked an unselected row"
            # DML snapshots and RETRY snapshots serialize numeric keys differently.
            # Retire by row identity, without comparing their SQL predicate text.
            hold.set()
            release.clear()
            c.execute("UPDATE docs SET content='one-again' WHERE id=1")
            deadline = time.monotonic() + 5
            while counts.get("one-again", 0) < 1 and time.monotonic() < deadline:
                time.sleep(0.01)
            assert counts.get("one-again") == 1, "DML request did not start"
            c.execute("ALTER TABLE docs RETRY AI COLUMN result WHERE id=1")
            c.execute("SELECT count(*) FROM oceanbase.__all_ai_column_task")
            assert c.fetchone()[0] == 1, "RETRY retained a superseded DML task"
            release.set()
            wait_for((("mock:one-again:2",), ("mock:two:1",)))
            print(
                "PASS: retry supersession, in-flight DML supersession, strict read, zero-match retry"
            )
    finally:
        release.set()
        with connection.cursor() as c:
            for sql, parameters in [
                ("DROP DATABASE IF EXISTS " + database, None),
                ("CALL DBMS_AI_SERVICE.DROP_AI_MODEL_ENDPOINT(%s)", (name + "_ep",)),
                ("CALL DBMS_AI_SERVICE.DROP_AI_MODEL(%s)", (name,)),
            ]:
                try:
                    c.execute(sql, parameters)
                except pymysql.MySQLError:
                    pass
        connection.close()
        model.shutdown()
        model.server_close()


if __name__ == "__main__":
    main()
