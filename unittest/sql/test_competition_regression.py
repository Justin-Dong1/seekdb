#!/usr/bin/env python3
"""Run the linear FileSQL/AI mysqltests against a disposable local seekdb.

Requires PyMySQL. This adapter uses the checked-in expected results unchanged;
it does not replace the competition's mysqltest runner. Only fixture transport
and the model endpoint host/port are adapted for a local Windows instance.
"""
import argparse
import base64
import datetime
import difflib
import json
from pathlib import Path
import tempfile
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

import pymysql


class ModelMock(BaseHTTPRequestHandler):
    def log_message(self, *args):
        pass

    def do_POST(self):
        body = json.loads(self.rfile.read(int(self.headers['Content-Length'])))
        if self.path.endswith('/chat/completions'):
            response = {'choices': [{'message': {'content': 'mock completion'}}]}
        elif self.path.endswith('/api/generate'):
            response = {'response': 'mock completion', 'done': True}
        elif self.path.endswith('/embeddings') or self.path.endswith('/api/embed'):
            inputs = body.get('input', '')
            inputs = inputs if isinstance(inputs, list) else [inputs]
            vectors = [[float(i + 1)] * body.get('dimensions', 768)
                       for i in range(len(inputs))]
            if self.path.endswith('/api/embed'):
                response = {'embeddings': vectors}
            else:
                response = {'data': [{'index': i, 'embedding': vector}
                                     for i, vector in enumerate(vectors)]}
        elif self.path.endswith('/rerank'):
            response = {'results': [{'index': i, 'relevance_score': 1.0 / (i + 1)}
                                    for i in range(len(body['documents']))]}
        else:
            self.send_error(404)
            return
        payload = json.dumps(response).encode()
        self.send_response(200)
        self.send_header('Content-Type', 'application/json')
        self.send_header('Content-Length', str(len(payload)))
        self.end_headers()
        self.wfile.write(payload)


def format_value(value):
    if value is None:
        return 'NULL'
    if isinstance(value, bytes):
        return value.decode('utf-8')
    if isinstance(value, datetime.datetime):
        return value.isoformat(sep=' ')
    return str(value)


def run_suite(connection, root, suite, case, fixtures, mock_port):
    source = root / 'tools/deploy/mysql_test/test_suite' / suite
    expected = (source / 'r/mysql' / (case + '.result')).read_text(encoding='utf-8')
    output = []
    pending = []
    query_log = result_log = True
    errors = {0}
    statements = 0
    with connection.cursor() as cursor:
        for raw in (source / 't' / (case + '.test')).read_text(encoding='utf-8').splitlines():
            line = raw.strip()
            if not line or line.startswith('#'):
                continue
            if line.startswith('--'):
                if line.startswith('--echo '):
                    output.append(line[7:])
                elif line == '--disable_query_log':
                    query_log = False
                elif line == '--enable_query_log':
                    query_log = True
                elif line == '--disable_result_log':
                    result_log = False
                elif line == '--enable_result_log':
                    result_log = True
                elif line.startswith('--error '):
                    errors = {int(number) for number in line[8:].split(',')}
                elif line in ('--disable_warnings', '--enable_warnings'):
                    pass
                elif line.startswith('--source mysql_test/test_suite/file_sql/include/'):
                    # Fixtures are copied locally below instead of over ssh/scp.
                    pass
                else:
                    raise ValueError('Unsupported directive: ' + line)
                continue
            if line == 'connection default;' or line.startswith('let $FILE_SQL_DATA_DIR ='):
                continue
            if line.startswith('eval '):
                line = line[5:]
            pending.append(line)
            if not line.endswith(';'):
                continue
            original = '\n'.join(pending)
            query = original.replace('$FILE_SQL_DATA_DIR', fixtures.as_posix())
            query = query.replace('http://0.0.0.0:8000', f'http://127.0.0.1:{mock_port}')
            if query_log:
                output.extend(pending)
            statements += 1
            try:
                cursor.execute(query)
                if 0 not in errors:
                    raise AssertionError(f'Expected error {errors}, succeeded: {query}')
                if cursor.description and result_log:
                    output.append('\t'.join(item[0] for item in cursor.description))
                    output.extend('\t'.join(format_value(value) for value in row)
                                  for row in cursor.fetchall())
            except pymysql.MySQLError as error:
                if error.args[0] not in errors:
                    raise AssertionError(f'Statement {statements}: {query}') from error
            pending = []
            errors = {0}
    if pending:
        raise AssertionError('Unterminated SQL')
    actual = '\n'.join(output) + '\n'
    if actual != expected:
        print(''.join(difflib.unified_diff(expected.splitlines(True), actual.splitlines(True),
                                         fromfile='expected', tofile='actual')))
        raise AssertionError(f'{suite}.{case}: output mismatch')
    print(f'PASS {suite}.{case}: {statements} SQL statements; exact result match')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--port', type=int, required=True)
    parser.add_argument('--suite', choices=['file_sql', 'ai_function', 'both'], default='both')
    parser.add_argument('--mock-port', type=int, default=19130)
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[2]
    mock = ThreadingHTTPServer(('127.0.0.1', args.mock_port), ModelMock)
    threading.Thread(target=mock.serve_forever, daemon=True).start()
    try:
        # Keep fixtures on the server's drive: Windows '/' is drive-relative.
        with tempfile.TemporaryDirectory(prefix='seekdb-regression-', dir=root / 'build_release') as directory:
            fixtures = Path(directory)
            data = root / 'tools/deploy/mysql_test/test_suite/file_sql/data'
            for name in ['basic.csv', 'basic.jsonl', 'invalid.txt']:
                # Git's Windows checkout converts the text fixtures to CRLF.
                (fixtures / name).write_bytes((data / name).read_bytes().replace(b'\r\n', b'\n'))
            (fixtures / 'simple.parquet').write_bytes(base64.b64decode(
                (data / 'simple.parquet.b64').read_bytes()))
            converters = pymysql.converters.conversions.copy()
            for field_type in (pymysql.constants.FIELD_TYPE.FLOAT, pymysql.constants.FIELD_TYPE.DOUBLE):
                converters[field_type] = str  # Preserve the server's wire representation.
            with pymysql.connect(host='127.0.0.1', port=args.port, user='root', conv=converters,
                                 charset='utf8mb4', autocommit=True, read_timeout=30) as conn:
                with conn.cursor() as cur:
                    cur.execute('CREATE DATABASE IF NOT EXISTS competition_local_regression')
                    cur.execute('USE competition_local_regression')
                if args.suite in ('file_sql', 'both'):
                    run_suite(conn, root, 'file_sql', 'file_sql_basic', fixtures, args.mock_port)
                if args.suite in ('ai_function', 'both'):
                    run_suite(conn, root, 'ai_function', 'ai_function_enhancement', fixtures, args.mock_port)
    finally:
        mock.shutdown()
        mock.server_close()


if __name__ == '__main__':
    main()
