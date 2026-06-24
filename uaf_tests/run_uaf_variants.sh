#!/bin/bash
set -e
CLANG=/src/SVFmemplus/llvm-16.0.0.obj/bin/clang
SABER=/src/SVFmemplus/Release-build/bin/saber
DIR=/src/SVFmemplus/uaf_tests
OUT=/tmp/uaf_variant_results.txt
: > "$OUT"

for c in "$DIR"/*.c; do
    name=$(basename "$c" .c)
    bc="$DIR/${name}.bc"
    $CLANG -emit-llvm -c -g -O0 "$c" -o "$bc"
    echo "===== $name =====" | tee -a "$OUT"
    if $SABER -uaf "$bc" 2>&1 | tee /tmp/saber_${name}.txt | grep -q "Use After Free"; then
        echo "RESULT: HIT" | tee -a "$OUT"
    else
        echo "RESULT: MISS" | tee -a "$OUT"
        grep -E '\[UAF\]|UafSources|UafReported|NoFree|NoUse' /tmp/saber_${name}.txt | tee -a "$OUT" || true
    fi
    echo | tee -a "$OUT"
done

echo "Summary written to $OUT"
