#!/bin/sh

timeout=${LOCAL_START_TIMEOUT:-600}
elapsed=0
pid_file=/tmp/playbacq-backend.pid
failure_file=/tmp/playbacq-local-start-failed

while [ "$elapsed" -lt "$timeout" ]; do
    if [ -f "$failure_file" ]; then
        cat "$failure_file" >&2
        exit 1
    fi

    if [ -f "$pid_file" ]; then
        backend_pid=$(cat "$pid_file")
        if kill -0 "$backend_pid" 2>/dev/null \
            && bash -c '</dev/tcp/127.0.0.1/8080' 2>/dev/null; then
            echo "Local backend is ready on http://localhost:8080."
            exit 0
        fi
    fi

    sleep 1
    elapsed=$((elapsed + 1))
done

echo "Timed out waiting for the local backend after ${timeout} seconds." >&2
exit 1
