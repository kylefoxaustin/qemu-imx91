#!/bin/sh
# In-guest Lua self-test.
if ./lua selftest.lua 2>err.out | grep -q "LUA SELFTEST OK"; then
  echo "SOAK:PASS:lua-selftest:core asserts ok"
else
  echo "SOAK:FAIL:lua-selftest:$(tail -1 err.out 2>/dev/null)"
fi
