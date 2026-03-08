# SVFmemplus TODO

## Baseline

- Current branch: `codex/focus-fl-ln-cl`
- Git sync blocker: container `git fetch/pull` failed with `git@github.com: Permission denied (publickey)`, so this plan is based on local `main` commit `7645a462aa4ac2ba0d04b4f74971f83dc7c1f163`.

## Chosen strategy

- Choose strategy 2 from `docs/source_location_command_caller_list.md`.
- Make `fl` / `ln` / `cl` the primary command protocol for location-bearing operations.
- Keep `location` only as a compatibility and presentation field during migration.
- Prefer structured location fields end-to-end because they already match the native data extracted by SVF.

## Migration principles

- Introduce a single authoritative way to read structured location arguments from command JSON.
- Keep compatibility with old `location` string commands during the transition where low cost.
- Do not let new code paths depend on parsing `file:line[:column]` if structured fields are already present.
- Standardize response payloads so structured fields are always available where a location exists.

## Code tasks

1. Introduce a shared structured-location argument reader in graph-reader.
   - Primary file: `svf-llvm/tools/GraphReader/GraphReaderUtil.cpp`
   - Add helper(s) to read command inputs from:
     - `fl`, `ln`, optional `cl`
     - legacy `location` only as fallback during migration
   - The helper should return a structured location object or tuple usable by all commands.

2. Replace direct `cmd.getString("location")` usage in location-based commands.
   - Audit and migrate handlers in:
     - `svf-llvm/tools/GraphReader/GraphReader.cpp`
     - `svf-llvm/tools/GraphReader/PathQuery.cpp`
     - `svf-llvm/tools/GraphReader/FunctionQuery.cpp`
     - any utility path that still takes only raw location strings
   - For commands with paired locations, define consistent structured names such as:
     - `start_fl`, `start_ln`, `start_cl`
     - `target_fl`, `target_ln`, `target_cl`
   - Keep temporary support for old string fields only if needed for incremental rollout.

3. Centralize matching logic on structured location data.
   - Update `parseLocationKey` and related helpers so they are no longer the primary protocol path.
   - `findAllICFGNodesByLocation` and similar utilities should either:
     - accept structured inputs directly, or
     - have structured wrappers that avoid string reparsing
   - Continue matching by filename containment + exact line unless tests justify broader logic.

4. Standardize response payloads.
   - Whenever a response contains source position, expose structured fields consistently:
     - `filename` or `fl`
     - `line` or `ln`
     - `column` or `cl` when available
   - If `location` is still emitted, it should be derived from structured fields and treated as compatibility output only.
   - Avoid differing formatter lambdas across:
     - `GraphReader.cpp`
     - `PathQuery.cpp`
     - `GraphReaderUtil.cpp`

5. Decide and document naming.
   - Pick one response-field convention and use it consistently:
     - either `filename/line/column`
     - or `fl/ln/cl`
   - If both must coexist for compatibility, define which is canonical and which is transitional.
   - Record the rule in code comments and any nearby developer docs.

6. Preserve incremental rollout safety.
   - Do not rename public commands.
   - Do not require NeverFree to migrate every single caller in one commit if compatibility fallback is cheap.
   - Prefer compatibility shims at command entry points over scattered parsing.

## Suggested execution order

1. Build shared structured-location input helper plus tests.
2. Migrate central utility lookup functions to structured inputs.
3. Migrate high-traffic command handlers in `GraphReader.cpp`.
4. Migrate `PathQuery.cpp` start/target location handling.
5. Standardize response payload formatting.
6. Remove duplicated string-only parsing once NeverFree-side callers are updated.

## High-priority commands

- `find-function-body-by-location`
- `list-svfg-nodes-by-location`
- `find-base-lvar-def`
- `analysis-lvar`
- `check-return-pointer`
- `path-cond-func`
- `find-lvalue-key_svfgnode`
- `find-actual_arg-key_svfgnode`
- `find-gep-cl`
- `find-store-cl`
- `get-constrain-inside`
- `find-lvalue-path-inside`
- `find-lvalue-detail-path-inside`
- `find-call-arg-value-path-inside`
- `list-callsite-actual-arg-nodes`
- `find-callsite-return-node`
- `show-code-line`

## Files to inspect first

- `svf-llvm/tools/GraphReader/GraphReaderUtil.cpp`
- `svf-llvm/tools/GraphReader/GraphReaderUtil.h`
- `svf-llvm/tools/GraphReader/GraphReader.cpp`
- `svf-llvm/tools/GraphReader/PathQuery.cpp`
- `svf-llvm/tools/GraphReader/FunctionQuery.cpp`

## Test requirements

1. Add structured-input parser tests.
   - Cases:
     - explicit `fl` / `ln`
     - explicit `fl` / `ln` / `cl`
     - fallback legacy `location = ../../.../region-inl.h:55:44`
     - malformed numeric inputs

2. Add command-level regression tests for migrated handlers.
   - Verify handlers accept structured fields without relying on `location`.
   - Verify compatibility fallback still accepts legacy `location` where intended.
   - Include at least:
     - one path-query-related command
     - one SVFG-node-related command
     - one function-body lookup command

3. Add response-format tests.
   - When source location exists, confirm structured fields are present consistently.
   - If `location` is still returned, confirm it is derived from the structured fields and remains stable.

4. Add utility-level tests for node lookup.
   - Verify line-based matching still works with structured input.
   - Verify optional column does not break lookup behavior when only line matching is required.

5. Add one integration note for manual validation against NeverFree.
   - After both repos are updated, run the libtiff NeverFree analysis flow.
   - Expected result: no backend failure from location-shape mismatch, and structured location fields should survive across IPC calls.

## Acceptance criteria

- Graph-reader accepts `fl` / `ln` / optional `cl` as the primary way to specify source positions.
- Legacy string `location` is only a compatibility path where retained.
- Response payloads expose structured position fields consistently.
- Location-based commands no longer depend on brittle `file:line[:column]` parsing in the common path.
- NeverFree can migrate callers incrementally without breaking existing command names.

## Current implementation status

### Done in this round

- Added a shared structured-location reader in `svf-llvm/tools/GraphReader/GraphReader.cpp`.
  - Canonical request shape is now:
    - `fl`: filename/path
    - `ln`: line number
    - `cl`: optional column number
  - Legacy `location` string is still accepted as fallback for migrated commands.
- Removed direct `cmd.getString("location")` usage from `GraphReader.cpp`, `GraphReaderUtil.cpp`, and `FunctionQuery.cpp`.
- Standardized multiple response payloads to expose `fl` / `ln` / `cl` and compatibility aliases `filename` / `line` / `column`.
- Kept internal low-cost compatibility where existing utility functions still consume `filename:line` strings.
- Verified the current changes with container build:
  - `docker exec -i nf-container /bin/bash -lc 'cd /src/SVFmemplus && source build.sh'`

### Not done yet

- `svf-llvm/tools/GraphReader/PathQuery.cpp` still contains a larger set of `location`-shaped outputs and some internal dependencies on the string form.
- Paired-location commands such as path start/target flows have not yet been fully migrated to explicit `start_fl/start_ln/start_cl` and `target_fl/target_ln/target_cl`.
- No dedicated regression tests have been added yet for the new structured request/response shape.

## Interfaces changed in this round

### Request format rule

- For the migrated commands below, the preferred request form is:

```json
{
  "command": "...",
  "fl": "path/to/file.c",
  "ln": 123,
  "cl": 45
}
```

- `cl` is optional unless the command semantics need a column-equivalent discriminator.
- Legacy compatibility form is still accepted:

```json
{
  "command": "...",
  "location": "path/to/file.c:123"
}
```

- Or with column-bearing legacy callers:

```json
{
  "command": "...",
  "location": "path/to/file.c:123",
  "cl": 45
}
```

### Commands now accepting structured location input

- `find-function-body-by-location`
  - Preferred:
```json
{
  "command": "find-function-body-by-location",
  "fl": "foo.c",
  "ln": 120
}
```
- `find-actual_arg-key_svfgnode`
  - Preferred:
```json
{
  "command": "find-actual_arg-key_svfgnode",
  "fl": "foo.c",
  "ln": 120,
  "callee_function_name": "bar",
  "arg_index": "0"
}
```
- `find-call-arg-value-path-inside`
  - Preferred:
```json
{
  "command": "find-call-arg-value-path-inside",
  "fl": "foo.c",
  "ln": 120,
  "callee_function_name": "bar",
  "arg_index": "0"
}
```
- `analysis-lvar`
  - Preferred:
```json
{
  "command": "analysis-lvar",
  "fl": "foo.c",
  "ln": 120,
  "eq_position": "45"
}
```
- `find-base-lvar-def`
  - Preferred:
```json
{
  "command": "find-base-lvar-def",
  "fl": "foo.c",
  "ln": 120,
  "eq_position": "45"
}
```
- `find-lvalue-path-inside`
  - Preferred:
```json
{
  "command": "find-lvalue-path-inside",
  "fl": "foo.c",
  "ln": 120,
  "eq_position": "45"
}
```
- `find-lvalue-detail-path-inside`
  - Preferred:
```json
{
  "command": "find-lvalue-detail-path-inside",
  "fl": "foo.c",
  "ln": 120,
  "eq_position": "45"
}
```
- `find-lvalue-detail-path-inside-store`
  - Preferred:
```json
{
  "command": "find-lvalue-detail-path-inside-store",
  "fl": "foo.c",
  "ln": 120,
  "eq_position": "45"
}
```
- `find-lvalue-key_svfgnode`
  - Preferred:
```json
{
  "command": "find-lvalue-key_svfgnode",
  "fl": "foo.c",
  "ln": 120,
  "eq_position": "45",
  "offsets": ["0", "4"]
}
```
- `check-return-pointer`
  - Preferred:
```json
{
  "command": "check-return-pointer",
  "fl": "foo.c",
  "ln": 120
}
```
- `find-store-cl`
  - Preferred:
```json
{
  "command": "find-store-cl",
  "fl": "foo.c",
  "ln": 120
}
```
- `find-gep-cl`
  - Preferred:
```json
{
  "command": "find-gep-cl",
  "fl": "foo.c",
  "ln": 120
}
```
- `get-constrain-inside`
  - Preferred:
```json
{
  "command": "get-constrain-inside",
  "fl": "foo.c",
  "ln": 120
}
```
- `show-code-line`
  - Preferred:
```json
{
  "command": "show-code-line",
  "fl": "foo.c",
  "ln": 120
}
```
- `list-callsite-actual-arg-nodes`
  - Preferred:
```json
{
  "command": "list-callsite-actual-arg-nodes",
  "fl": "foo.c",
  "ln": 120,
  "callee_function_name": "bar"
}
```
- `find-callsite-return-node`
  - Preferred:
```json
{
  "command": "find-callsite-return-node",
  "fl": "foo.c",
  "ln": 120,
  "callee_function_name": "bar"
}
```
- `list-svfg-nodes-by-location`
  - Preferred:
```json
{
  "command": "list-svfg-nodes-by-location",
  "fl": "foo.c",
  "ln": 120,
  "cl": 45
}
```
  - Compatibility note:
    - legacy callers may still send `column` instead of `cl`

## Response shape changes in this round

- The following outputs now prioritize structured location fields:
  - `get-svfg-node-info`
  - `list-svfg-nodes-by-location`
  - `find-store-cl`
  - `find-gep-cl`
  - `analysis-lvar`
  - `check-return-pointer`
  - `find-all-function-call-sites`
  - `tracePAGStore`-derived outputs in graph-reader utilities
- Preferred response fields are:

```json
{
  "fl": "foo.c",
  "ln": 120,
  "cl": 45,
  "filename": "foo.c",
  "line": 120,
  "column": 45
}
```

- In this round, `fl` / `ln` / `cl` are treated as canonical.
- `filename` / `line` / `column` are currently still emitted as compatibility aliases in several responses.
- `location` should now be considered transitional only; in the touched files it has been removed or deprioritized from the common path.


## NeverFree interface requirements

### Status from frontend review

- `path-cond-func` is currently a dead-code candidate on the NeverFree side.
  - Only helper definitions exist in `analysis_operators.py`.
  - No active NeverFree analysis flow currently calls it.
  - Backend migration for this command should therefore be treated as planned interface work, not as an urgent production blocker.

- The active NeverFree request path has been cleaned so that migrated IPC calls use structured fields as the canonical request format.
- Remaining frontend cleanup is limited to local helper logic and old compatibility wrappers, not to the live core IPC payload shape.

### Canonical request contract expected by NeverFree

- Single-location commands should accept these canonical request fields:
  - `fl`
  - `ln`
  - optional `cl`

- Paired-location commands should accept these canonical request fields:
  - `start_fl`
  - `start_ln`
  - optional `start_cl`
  - `target_fl`
  - `target_ln`
  - optional `target_cl`

- NeverFree should not need to send these legacy request fields in the steady state:
  - `location`
  - `source_location`
  - `start_location`
  - `target_location`

### Canonical response contract expected by NeverFree

- Whenever a response contains a source position, backend should provide structured fields directly.
- Canonical response fields expected by NeverFree:
  - `fl`
  - `ln`
  - optional `cl`

- Compatibility aliases may still be emitted during transition:
  - `filename`
  - `line`
  - `column`

- Legacy string response fields should be transitional only:
  - `location`
  - `start_location`
  - `target_location`

- If `location`-style compatibility fields are still emitted, they must be derived from the canonical structured fields rather than being independently assembled by ad hoc code paths.

### Backend work requested next

1. Finish `PathQuery.cpp` response migration.
   - Replace `location`, `start_location`, and `target_location` as primary fields with structured location objects/fields.
   - Stop reading branch locations through `getString("location")` in the common path.

2. Finish paired-location command handling.
   - Add explicit graph-reader command entry support for:
     - `start_fl/start_ln/start_cl`
     - `target_fl/target_ln/target_cl`
   - Do not require NeverFree to send `start_location` or `target_location`.

3. Demote legacy `location` fallback to compatibility-only handling.
   - Keep fallback only where cheap and clearly marked.
   - Avoid using raw `location` strings as the common internal representation for newly migrated command paths.

4. Standardize one canonical helper path for emitting response locations.
   - `GraphReader.cpp`, `GraphReaderUtil.cpp`, and `PathQuery.cpp` should not each invent different `location`/`fl`/`filename` formatting logic.

### Commands whose backend contract should be considered migrated only after response cleanup

- `path-cond-func`
- `find-lvalue-path-inside`
- `find-lvalue-detail-path-inside`
- `find-call-arg-value-path-inside`
- `get-constrain-inside`
- any command whose response still uses `location`-shaped branch/event/start/target fields as the primary representation
