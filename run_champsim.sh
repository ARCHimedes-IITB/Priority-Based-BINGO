#!/bin/bash

if [ "$#" -lt 5 ]; then
    echo "Illegal number of parameters"
    echo "Usage: ./run_champsim.sh [BINARY] [N_WARM] [N_SIM] [BANDWIDTH] [TRACE] [OPTION]"
    exit 1
fi

# I have my traces here, outside the CS305-Project directory
TRACE_DIR=../dpc3_traces
BINARY=${1}
N_WARM=${2}
N_SIM=${3}
BANDWIDTH=${4}
TRACE=${5}
OPTION=${6}

# Sanity check
if [ -z $TRACE_DIR ] || [ ! -d "$TRACE_DIR" ]; then
    echo "[ERROR] Cannot find a trace directory: $TRACE_DIR"
    exit 1
fi

# Default Bandwidth is 3200 MT/s
if [ -z $BANDWIDTH ]; then
    BANDWIDTH=32
fi

if [ ! -f "bin/$BINARY" ]; then
    echo "[ERROR] Cannot find a ChampSim binary: bin/$BINARY"
    exit 1
fi

re='^[0-9]+$'
if ! [[ $N_WARM =~ $re ]] || [ -z $N_WARM ]; then
    echo "[ERROR]: Number of warmup instructions is NOT a number" >&2
    exit 1
fi

re='^[0-9]+$'
if ! [[ $N_SIM =~ $re ]] || [ -z $N_SIM ]; then
    echo "[ERROR]: Number of simulation instructions is NOT a number" >&2
    exit 1
fi

if [ ! -f "$TRACE_DIR/$TRACE" ]; then
    echo "[ERROR] Cannot find a trace file: $TRACE_DIR/$TRACE"
    exit 1
fi

filename=$(basename ${TRACE}-${BINARY}${OPTION}.txt)
dirname results/${N_SIM}M_${BANDWIDTH}B/${filename} | xargs mkdir -p
# echo "(./bin/${BINARY} -warmup_instructions ${N_WARM}000000 -simulation_instructions ${N_SIM}000000 -low_bandwidth ${BANDWIDTH}00 ${OPTION} -traces ${TRACE_DIR}/${TRACE}) &>results/${N_SIM}M_${BANDWIDTH}B/${filename}"
(./bin/${BINARY} -warmup_instructions ${N_WARM}000000 -simulation_instructions ${N_SIM}000000 \
    -low_bandwidth ${BANDWIDTH}00 ${OPTION} -traces ${TRACE_DIR}/${TRACE}) \
    &>results/${N_SIM}M_${BANDWIDTH}B/${filename}
