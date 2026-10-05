#!/usr/bin/env python3
"""Build a codeXek SQLite CTU index (schema v2) from an externalDefMap line.

Usage: mkctu.py <map.txt> <ast-file> <out.db>

The text map line is "<len>:<usr> <source>"; the database is what the codeXek
product builds for real projects. Used by the dependency-capture protocol
lit test, which cannot assume a sqlite3 binary in the test environment.
"""
import sqlite3
import sys

map_path, ast_path, db_path = sys.argv[1], sys.argv[2], sys.argv[3]
db = sqlite3.connect(db_path)
db.executescript(
    "CREATE TABLE metadata(version INTEGER NOT NULL, namespace TEXT NOT NULL);"
    "CREATE TABLE definitions(namespace TEXT NOT NULL, usr TEXT NOT NULL,"
    " ast TEXT NOT NULL, source TEXT NOT NULL,"
    " PRIMARY KEY(namespace,usr,ast)) WITHOUT ROWID;"
)
db.execute("INSERT INTO metadata VALUES(2,'lit')")
for line in open(map_path):
    line = line.strip()
    if not line:
        continue
    colon = line.index(":")
    n = int(line[:colon])
    usr = line[colon + 1:colon + 1 + n]
    assert line[colon + 1 + n] == " ", line
    source = line[colon + 2 + n:]
    db.execute(
        "INSERT OR IGNORE INTO definitions VALUES('lit',?,?,?)",
        (usr, ast_path, source),
    )
db.commit()
