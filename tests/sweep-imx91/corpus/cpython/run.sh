#!/bin/sh
# In-guest CPython functional tests. We run the cross-built interpreter against
# INLINE assert-based oracles rather than `python -m test`: the stdlib regrtest
# harness needs the cross-generated _sysconfigdata module (a CPython cross-build
# detail not produced by a plain `make python` — see README/build.sh), but the
# interpreter itself runs the full pure-Python stdlib + our builtin C modules
# fine. Each snippet exercises a stdlib subsystem with a known result -> one
# SOAK row (cpython-<name>), a defined-oracle test in the sweep's own style.
export PYTHONHOME="$PWD/py"; export PYTHONDONTWRITEBYTECODE=1
PY="$PWD/py/bin/python3"

# interpreter sanity
if "$PY" -c 'import sys; assert sys.version_info[:3]==(3,10,14)' 2>e; then
  echo "SOAK:PASS:cpython-interp:$("$PY" -c 'import sys;print(sys.version.split()[0])')"
else
  echo "SOAK:FAIL:cpython-interp:$(tr '\n' ' ' <e|cut -c1-90)"; exit 0
fi

# each case: name | one-liner that asserts a known result and prints OK
t() {  # t <name> <python-code>
  if [ "$("$PY" -c "$2" 2>/dev/null)" = OK ]; then echo "SOAK:PASS:cpython-$1:ok"
  else echo "SOAK:FAIL:cpython-$1:[$("$PY" -c "$2" 2>&1 | tr '\n' ' ' | cut -c1-70)]"; fi
}

t json       'import json;d={"b":[1,2,3],"a":1};s=json.dumps(d,sort_keys=True);assert s=="{\"a\": 1, \"b\": [1, 2, 3]}";assert json.loads(s)["b"][2]==3;print("OK")'
t math       'import math;assert math.factorial(10)==3628800 and round(math.sqrt(2),6)==1.414214 and math.gcd(462,1071)==21;print("OK")'
t cmath      'import cmath;assert abs(cmath.sqrt(-1)-1j)<1e-9;print("OK")'
t struct     'import struct;b=struct.pack(">Ihb",70000,-5,3);assert struct.unpack(">Ihb",b)==(70000,-5,3) and len(b)==7;print("OK")'
t array      'import array;a=array.array("i",range(100));assert sum(a)==4950 and a[50]==50;print("OK")'
t datetime   'import datetime as D;d=D.date(2026,6,27)-D.date(2026,1,1);assert d.days==177;assert D.datetime(2020,2,29).year==2020;print("OK")'
t heapq      'import heapq;h=list(range(20,0,-1));heapq.heapify(h);assert [heapq.heappop(h) for _ in range(3)]==[1,2,3];print("OK")'
t bisect     'import bisect;a=[1,3,5,7,9];assert bisect.bisect(a,5)==3 and bisect.bisect_left(a,5)==2;print("OK")'
t random     'import random;random.seed(1234);x=[random.randint(0,999) for _ in range(3)];random.seed(1234);assert x==[random.randint(0,999) for _ in range(3)];print("OK")'
t statistics 'import statistics as s;d=[2,4,4,4,5,5,7,9];assert s.mean(d)==5 and s.median(d)==4.5 and round(s.pstdev(d),3)==2.0;print("OK")'
t hashlib    'import hashlib;assert hashlib.sha256(b"abc").hexdigest()=="ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad";assert hashlib.md5(b"abc").hexdigest()=="900150983cd24fb0d6963f7d28e17f72";print("OK")'
t pickle     'import pickle;o={"x":[1,2,(3,4)],"y":frozenset({5,6})};assert pickle.loads(pickle.dumps(o,2))==o;print("OK")'
t re         'import re;m=re.match(r"(\d+)-(\w+)","42-foo");assert m.groups()==("42","foo");assert re.sub(r"\d","#","a1b2")=="a#b#";print("OK")'
t base64     'import base64;assert base64.b64encode(b"the quick brown fox")==b"dGhlIHF1aWNrIGJyb3duIGZveA==";assert base64.b64decode(b"YWJj")==b"abc";print("OK")'
t binascii   'import binascii;assert binascii.hexlify(b"AB")==b"4142" and binascii.crc32(b"hello world")==222957957;print("OK")'
t csv        'import csv,io;w=io.StringIO();csv.writer(w).writerow([1,"a,b",3]);assert w.getvalue()=="1,\"a,b\",3\r\n";print("OK")'
t collections 'import collections as c;assert c.Counter("mississippi").most_common(1)==[("i",4)];assert list(c.OrderedDict.fromkeys("abca"))==["a","b","c"];print("OK")'
t itertools  'import itertools as I;assert list(I.islice(I.count(10,2),3))==[10,12,14];assert len(list(I.permutations(range(4),2)))==12;print("OK")'
t functools  'import functools as F;assert F.reduce(lambda a,b:a*b,range(1,6))==120;print("OK")'
t asyncio    'import asyncio;
async def f():
    await asyncio.sleep(0);return sum(await asyncio.gather(*[asyncio.sleep(0,i) for i in range(5)]))
assert asyncio.run(f())==10;print("OK")'
t unicodedata 'import unicodedata as U;assert U.name("A")=="LATIN CAPITAL LETTER A" and U.category("9")=="Nd";print("OK")'
t codecs     'import codecs;assert "héllo".encode("utf-8").decode("utf-8")=="héllo";assert b"\xe4\xb8\xad".decode("utf-8")=="中";print("OK")'
t decimal    'import decimal;d=decimal.Decimal("0.1")*3;assert str(d)=="0.3";print("OK")'
t fractions  'from fractions import Fraction as F;assert F(1,3)+F(1,6)==F(1,2);print("OK")'
t textwrap   'import textwrap;assert textwrap.wrap("a b c d e f",width=5)==["a b c","d e f"];print("OK")'
