#!/bin/sh
# Prints its pid, then becomes `sleep 30` (exec: killing the pid ends the process and closes the pipe).
echo $$
exec sleep 30
