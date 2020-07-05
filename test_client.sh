#!/bin/bash

if [ $# -ne 2 ]; then
    echo "Usage $0 <file_output> <N=numero_client>"
    exit -1
fi

OUT=$1
N=$2

# "$!" is the PID of the last program your shell ran in the background
# "$$" means current PID.
echo ""	> $OUT
for((i=0;i<N;i++)); do
	./client "ciao $i" "Hello" \
	"tesssTTTTssss" \
	"RANCOREEEEEEEEEEeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeee"	\
	2>&1	>>$OUT &
	PID[$i]=$!
done

for((i=0;i<N;i++)); do
	wait ${PID[$i]}
done
