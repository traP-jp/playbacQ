#!/bin/sh

build_dir=${LOCAL_BUILD_DIR:-/app/build}
build_jobs=${LOCAL_BUILD_JOBS:-6}
backend_pid=""
pid_file=/tmp/playbacq-backend.pid
failure_file=/tmp/playbacq-local-start-failed

keep_container_running() {
    echo "The development container will remain running for investigation and debugging."
    exec sleep infinity
}

record_failure() {
    echo "$1" | tee "$failure_file" >&2
}

stop_backend() {
    if [ -n "$backend_pid" ] && kill -0 "$backend_pid" 2>/dev/null; then
        kill -TERM "$backend_pid"
        wait "$backend_pid"
    fi
    rm -f "$pid_file"
    exit 0
}

trap stop_backend INT TERM
rm -f "$pid_file" "$failure_file"

if ! cmake -S /app -B "$build_dir" -DBUILD_LOCAL_DEV=ON -DUSE_LOCAL_WORKER=ON; then
    record_failure "Local configure failed."
    keep_container_running
fi

if ! cmake --build "$build_dir" --parallel "$build_jobs"; then
    record_failure "Local build failed."
    keep_container_running
fi

"$build_dir/playbacq" &
backend_pid=$!
echo "$backend_pid" > "$pid_file"
echo "Local backend started with PID $backend_pid."

wait "$backend_pid"
backend_status=$?
rm -f "$pid_file"
record_failure "Local backend exited with status $backend_status."
keep_container_running
