#!/usr/bin/env python3
"""Build a codeXek SQLite CTU index (schema v2) from an externalDefMap line.

Usage: mkctu.py [--blob-source] <map.txt> <ast-file> <out.db>

The text map line is "<len>:<usr> <source>"; the database is what the codeXek
product builds for real projects. Used by the dependency-capture protocol
lit tests, which cannot assume a sqlite3 binary in the test environment.
--blob-source stores the source column as a BLOB — an invalid row that the
engine must report as a query error, never as a plain miss (CR02).
"""
import sqlite3
import sys

blob_source = "--blob-source" in sys.argv
args = [a for a in sys.argv[1:] if a != "--blob-source"]
map_path, ast_path, db_path = args
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
    if blob_source:
        source = sqlite3.Binary(source.encode())
    db.execute(
        "INSERT OR IGNORE INTO definitions VALUES('lit',?,?,?)",
        (usr, ast_path, source),
    )
db.commit()
