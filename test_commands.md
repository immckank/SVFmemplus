# Test Commands for libtiff-jpeg.bc

## Non-interleaving
```json
{"command":"ping","arg":""}
{"command":"find-all-free-caller","arg":""}
{"command":"exit","arg":""}
```

## Interleaving (Stage 0 -> Stage 1 -> Stage 2)
```json
{"command":"ping","arg":""}
{"command":"add-custom-api","arg":"kind=alloc,name=my_alloc"}
{"command":"list-model-spec","arg":""}
{"command":"continue","arg":""}
{"command":"find-all-free-caller","arg":""}
{"command":"continue","arg":""}
{"command":"list-model-spec","arg":""}
{"command":"exit","arg":""}
```
