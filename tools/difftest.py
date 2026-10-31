#!/usr/bin/env python3
"""Seeded SQL subset differential test against Python's sqlite3."""

import argparse
import json
import math
import os
import random
import socket
import sqlite3
import subprocess
import tempfile
import time
import urllib.error
import urllib.request


def quote_text(value):
    return "'" + value.replace("'", "''") + "'"


def random_value(rng, kind):
    if rng.randrange(8) == 0:
        return "NULL", None
    if kind == "BIGINT":
        value = rng.randint(-10**9, 10**9)
        return str(value), value
    if kind == "DOUBLE":
        value = rng.randint(-100000, 100000) / 100.0
        return f"{value:.2f}", value
    if kind == "BOOLEAN":
        value = bool(rng.getrandbits(1))
        return ("TRUE" if value else "FALSE"), int(value)
    if kind.startswith("VARCHAR"):
        value = rng.choice(("amber", "blue", "green", "violet", "red"))
        return quote_text(value), value
    value = rng.randint(-100000, 100000)
    return str(value), value


def decode_cell(cell):
    if cell == "NULL":
        return None
    if cell == "TRUE":
        return 1
    if cell == "FALSE":
        return 0
    if len(cell) >= 2 and cell[0] == "'" and cell[-1] == "'":
        return cell[1:-1].replace("''", "'")
    try:
        return float(cell) if any(char in cell for char in ".eE") else int(cell)
    except ValueError:
        return cell


def same_cell(left, right):
    if isinstance(left, (int, float)) and isinstance(right, (int, float)):
        return math.isclose(float(left), float(right), rel_tol=1e-12, abs_tol=1e-12)
    return left == right


def rows_match(expected, actual, ordered):
    if len(expected) != len(actual):
        return False
    if ordered:
        return all(
            len(left) == len(right)
            and all(same_cell(a, b) for a, b in zip(left, right))
            for left, right in zip(expected, actual)
        )
    remaining = list(actual)
    for row in expected:
        match = next(
            (
                index
                for index, candidate in enumerate(remaining)
                if len(row) == len(candidate)
                and all(same_cell(a, b) for a, b in zip(row, candidate))
            ),
            None,
        )
        if match is None:
            return False
        remaining.pop(match)
    return not remaining


class Engine:
    def __init__(self, binary, db_path, port):
        env = os.environ.copy()
        env.update(
            {
                "DB_PATH": db_path,
                "PORT": str(port),
                "DURABILITY": "off",
                "ALLOW_UNSAFE": "1",
            }
        )
        self.process = subprocess.Popen(
            [binary], env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL
        )
        self.url = f"http://127.0.0.1:{port}/api/query"
        for _ in range(200):
            if self.process.poll() is not None:
                raise RuntimeError("database server exited during startup")
            try:
                with urllib.request.urlopen(
                    f"http://127.0.0.1:{port}/health", timeout=0.2
                ) as response:
                    response.read()
                return
            except (OSError, urllib.error.URLError):
                time.sleep(0.05)
        self.close()
        raise RuntimeError("database server did not become ready")

    def execute(self, query):
        request = urllib.request.Request(
            self.url,
            data=json.dumps({"query": query}).encode("utf-8"),
            headers={"Content-Type": "application/json"},
            method="POST",
        )
        try:
            with urllib.request.urlopen(request, timeout=30) as response:
                payload = json.loads(response.read())
        except urllib.error.HTTPError as error:
            payload = json.loads(error.read())
            raise RuntimeError(f"{query}: {payload}") from error
        if payload.get("status") != "success":
            raise RuntimeError(f"{query}: {payload}")
        return payload["result"]

    def close(self):
        if self.process.poll() is None:
            self.process.terminate()
            try:
                self.process.wait(timeout=10)
            except subprocess.TimeoutExpired:
                self.process.kill()
                self.process.wait()


def make_select(rng, ids):
    columns = ("id", "n", "x", "flag", "label")
    choice = rng.randrange(8)
    if choice == 0:
        return "SELECT COUNT(*), MIN(n), MAX(n), SUM(n), AVG(n) FROM rows_table", False
    if choice == 1:
        return (
            "SELECT flag, COUNT(*), SUM(n) FROM rows_table GROUP BY flag",
            False,
        )

    if choice == 2 and ids:
        low = rng.choice(ids)
        high = low + rng.randint(0, 15)
        predicate = f"id BETWEEN {low} AND {high}"
        projections = "id, label, flag"
        return f"SELECT {projections} FROM rows_table WHERE {predicate}", False
    predicates = (
        f"n >= {rng.randint(-100000, 100000)} AND flag IS NOT NULL",
        f"label LIKE '{rng.choice(('a%', 'b%', 'g%', 'r%'))}'",
        f"NOT (n BETWEEN {rng.randint(-100000, 0)} AND {rng.randint(1, 100000)})",
        f"(flag = TRUE OR label IS NULL) AND id >= {rng.randint(0, 100)}",
        f"n IN ({rng.randint(-100000, 100000)}, {rng.randint(-100000, 100000)})",
        f"x IS NULL OR x < {rng.randint(-1000, 1000)}.{rng.randint(0, 99):02d}",
    )
    predicate = rng.choice(predicates)
    return (
        f"SELECT {', '.join(rng.sample(columns, rng.randint(1, 3)))} "
        f"FROM rows_table WHERE {predicate} ORDER BY id LIMIT 20 OFFSET {rng.randint(0, 5)}",
        True,
    )


def run_differential(binary, statements, seed):
    rng = random.Random(seed)
    n_type = rng.choice(("INT", "BIGINT"))
    varchar_type = f"VARCHAR({rng.choice((12, 24, 40))})"
    with tempfile.TemporaryDirectory(prefix="custom-db-diff-") as directory:
        db_path = os.path.join(directory, "engine.db")
        with socket.socket() as sock:
            sock.bind(("127.0.0.1", 0))
            port = sock.getsockname()[1]
        engine = Engine(binary, db_path, port)
        sqlite = sqlite3.connect(":memory:")
        try:
            schema = (
                "CREATE TABLE rows_table (id INT PRIMARY KEY, n "
                + n_type
                + ", x DOUBLE, flag BOOLEAN, label "
                + varchar_type
                + ")"
            )
            engine.execute(schema)
            sqlite.execute(schema)
            next_id = 1
            known_ids = []
            for index in range(statements):
                operation = rng.randrange(100)
                if operation < 35:
                    values = [str(next_id)]
                    for kind in (n_type, "DOUBLE", "BOOLEAN", varchar_type):
                        literal, _ = random_value(rng, kind)
                        values.append(literal)
                    query = "INSERT INTO rows_table VALUES (" + ", ".join(values) + ")"
                    engine.execute(query)
                    sqlite.execute(query)
                    known_ids.append(next_id)
                    next_id += 1
                elif operation < 52:
                    key = rng.randint(1, max(next_id + 10, 10))
                    if rng.randrange(2):
                        literal = str(rng.randint(-10**9, 10**9))
                        query = f"UPDATE rows_table SET n = {literal} WHERE id = {key}"
                    else:
                        literal = "TRUE" if rng.getrandbits(1) else "FALSE"
                        query = f"UPDATE rows_table SET flag = {literal} WHERE id = {key}"
                    engine.execute(query)
                    sqlite.execute(query)
                elif operation < 65:
                    key = rng.randint(1, max(next_id + 10, 10))
                    query = f"DELETE FROM rows_table WHERE id = {key}"
                    engine.execute(query)
                    sqlite.execute(query)
                    if key in known_ids:
                        known_ids.remove(key)
                else:
                    query, ordered = make_select(rng, known_ids)
                    result = engine.execute(query)
                    sqlite_rows = sqlite.execute(query).fetchall()
                    lines = result.splitlines()
                    actual = [
                        tuple(decode_cell(cell) for cell in line.split("\t"))
                        for line in lines[1:]
                    ]
                    expected = [tuple(row) for row in sqlite_rows]
                    if not rows_match(expected, actual, ordered):
                        raise AssertionError(
                            f"seed={seed} statement={index + 1}\n{query}\n"
                            f"SQLite={expected[:25]}\nEngine={actual[:25]}"
                        )
                sqlite.commit()
        finally:
            engine.close()
            sqlite.close()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", required=True, help="path to relational_db")
    parser.add_argument("--statements", type=int, default=10000)
    parser.add_argument("--seed", type=int, default=20250623)
    args = parser.parse_args()
    if args.statements < 1:
        parser.error("--statements must be positive")
    print(f"SQLite differential seed={args.seed} statements={args.statements}", flush=True)
    run_differential(args.binary, args.statements, args.seed)
    print(f"SQLite differential passed: {args.statements} statements", flush=True)


if __name__ == "__main__":
    main()
