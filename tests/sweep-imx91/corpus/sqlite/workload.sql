-- Deterministic SQLite workload: schema, bulk insert via recursive CTE,
-- aggregates, joins, window functions, json1, and an on-disk roundtrip.
.mode list
.headers off
PRAGMA foreign_keys=ON;

CREATE TABLE nums(n INTEGER PRIMARY KEY);
WITH RECURSIVE c(n) AS (SELECT 1 UNION ALL SELECT n+1 FROM c WHERE n<10000)
INSERT INTO nums SELECT n FROM c;
SELECT 'count', COUNT(*) FROM nums;
SELECT 'sum', SUM(n) FROM nums;
SELECT 'avg', AVG(n) FROM nums;
SELECT 'primes_lt_50', COUNT(*) FROM nums WHERE n<50 AND n>1 AND NOT EXISTS
  (SELECT 1 FROM nums d WHERE d.n>1 AND d.n<nums.n AND nums.n%d.n=0);

CREATE TABLE items(id INTEGER PRIMARY KEY, cat TEXT, qty INTEGER);
INSERT INTO items(cat,qty) VALUES
  ('a',5),('b',3),('a',7),('c',2),('b',8),('a',1),('c',9),('b',4);
SELECT 'bycat', cat, SUM(qty) FROM items GROUP BY cat ORDER BY cat;
SELECT 'ranked', cat, qty, RANK() OVER (PARTITION BY cat ORDER BY qty DESC)
  FROM items ORDER BY cat, qty DESC;

SELECT 'json', json_extract('{"a":{"b":[10,20,30]}}','$.a.b[1]');
SELECT 'jsonarr', json_array_length('[1,2,3,4,5]');

-- on-disk: write a table to a file db, read it back
ATTACH DATABASE 'wk.db' AS disk;
CREATE TABLE disk.t(k TEXT, v INTEGER);
INSERT INTO disk.t VALUES ('x',111),('y',222),('z',333);
SELECT 'disksum', SUM(v) FROM disk.t;
DETACH DATABASE disk;
