#!/bin/sh
set -eu
program=${1:-./cue-clock}
sheet=${2:-./rehearsal.txt}
operator_sheet=${3:-./operator-rehearsal.txt}
printf '%s\n' 'CUE CLOCK | ordinary rehearsal'
"$program" "$sheet"
printf '%s\n' 'CUE CLOCK | cue 2 delayed by 950 ms'
"$program" "$sheet" --delay-cue 2 950
printf '%s\n' 'CUE CLOCK | operator rehearsal'
"$program" "$operator_sheet" --delay-cue 2 950 --operator
printf '%s\n' 'CUE CLOCK | subtitle export'
"$program" "$sheet" --vtt
printf '%s\n' 'CUE CLOCK | rehearsal complete'
