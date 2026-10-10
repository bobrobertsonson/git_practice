#!/bin/sh
# Prints 5 lines, 50 ms apart (ToolRunner streaming test).
for i in 1 2 3 4 5; do
  echo "line $i"
  sleep 0.05
done
