# Sourced by the test scripts. run_jobs reads one shell command per line and runs them across
# $JOBS processes. Output comes out in the order given, and it fails if any of them did.
JOBS="${JOBS:-$(nproc)}"

# Long jobs should not get in the way of anything else on the machine.
renice -n 19 -p $$ >/dev/null 2>&1 || true

run_jobs()
{
    jobdir="$(mktemp -d "$1/jobs.XXXXXX")"
    count=0
    while IFS= read -r command; do
        count=$((count + 1))
        printf '%s\n' "$command" >"$jobdir/$count.sh"
    done

    seq 1 "$count" | xargs -P "$JOBS" -I{} sh -c \
        'sh "$0/{}.sh" >"$0/{}.log" 2>&1 || echo {} >>"$0/failed"' "$jobdir"

    number=1
    while [ "$number" -le "$count" ]; do
        cat "$jobdir/$number.log"
        number=$((number + 1))
    done

    if [ -f "$jobdir/failed" ]; then
        echo "failed: $(sort -n "$jobdir/failed" | while read -r job; do cat "$jobdir/$job.sh"; done)" >&2
        return 1
    fi
    rm -rf "$jobdir"
}
