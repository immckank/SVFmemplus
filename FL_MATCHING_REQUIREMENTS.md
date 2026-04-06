# FL Matching Requirements for GraphReader Backend

## 1. Current `fl` Matching Problem

The current backend behavior is too strict and too dependent on the exact raw `fl` string shape captured during graph construction.

A concrete example:

- Backend data may contain:
  `prebuilts/clang/ohos/linux-x86_64/llvm/bin/../include/libcxx-ohos/include/c++/v1/__memory/allocate_at_least.h`
- Frontend may send:
  `prebuilts/clang/ohos/linux-x86_64/llvm/include/libcxx-ohos/include/c++/v1/__memory/allocate_at_least.h`

These two paths are lexically equivalent after normalizing `bin/../include` into `include`, but the backend may still fail to match them and report:

- `No ICFG nodes found at location: ...`

This is not just a one-off request failure. It creates systemic friction:

- The frontend must guess the exact raw `fl` shape used during backend modeling.
- Agent workflows must preserve a brittle, backend-specific path spelling.
- Equivalent source locations may behave inconsistently across requests.
- The user-facing API becomes harder to use correctly than necessary.

In essence, the current problem is:

- The backend treats `fl` as an exact or near-exact string token.
- But `fl` coming from analysis artifacts, frontend normalization, user input, and agent reasoning is not guaranteed to preserve the exact raw string shape used during graph construction.


## 2. Backend Should Own This Responsibility

The backend should be responsible for accepting equivalent `fl` representations and resolving them to the actual modeled source locations.

The reason is simple:

- Only the backend knows the set of source locations that actually exist in the modeled ICFG/SVFG/PAG.
- Only the backend can reliably map a user-facing query to the internal graph data.
- The frontend and agent should not be required to predict the exact raw path string embedded in debug info.

The backend does **not** need to know in advance what the “correct prefix” is.
Instead, it should treat the raw `fl` strings already present in the graph as ground truth, and build a robust matching layer on top of them.

### Required design principle

The backend should distinguish between:

- `raw_fl`: the original file path string extracted from modeled graph/debug information
- `normalized_fl`: a lexically normalized comparison key derived from `raw_fl`

The backend should preserve `raw_fl` for output and traceability, but perform lookup using `normalized_fl`.

### Modification strategy

#### 2.1 Introduce a lexical path normalization helper

Add a helper in the backend that normalizes a path string without requiring filesystem access.

Expected normalization behavior:

- unify path separators (`\` to `/`)
- collapse repeated separators
- remove `.` path segments
- fold `..` segments lexically where possible
- preserve relative/absolute nature as-is
- do **not** require `realpath`, `canonical`, or disk existence checks

This is a string-level normalization step, not a filesystem resolution step.

#### 2.2 Match on normalized keys, not raw substrings

Current substring-based matching is too weak and unstable for equivalent-but-differently-spelled paths.

The backend should replace it with a layered matching strategy:

1. exact raw match
2. exact normalized match
3. suffix match on normalized path components
4. basename-only fallback only when explicitly needed

This should be component-aware rather than plain substring matching.

For example:

- `a/b/bin/../include/x.h`
- `a/b/include/x.h`

should normalize to the same key and therefore match the same modeled file.

#### 2.3 Build an index from modeled graph locations

When graph data is available, the backend should gather all source locations from modeled nodes and build lookup structures such as:

- `normalized_fl -> [raw_fl_1, raw_fl_2, ...]`
- optionally `basename -> [raw_fl_1, raw_fl_2, ...]`

This allows the backend to:

- resolve user/frontend input into one or more modeled raw paths
- avoid rescanning all nodes for every request
- provide good diagnostics when multiple raw paths collapse to the same normalized form

#### 2.4 Resolve query `fl` into modeled `raw_fl`

When the backend receives a request with `fl`/`ln`/`cl`, it should:

1. normalize the query `fl`
2. look up candidate raw modeled paths by normalized key
3. filter by `ln`
4. optionally filter by `cl`
5. if unique, use that modeled raw path internally
6. if multiple candidates remain, return an ambiguity error with candidates
7. if none remain, return a helpful diagnostic with near matches

This is the key responsibility shift:

- the client provides an equivalent file path
- the backend resolves it to the actual modeled path representation

#### 2.5 Keep raw modeled `fl` in outputs

Even after matching via normalized keys, backend responses should continue to expose the actual modeled `raw_fl` in returned node/location objects.

This gives three benefits:

- preserves traceability to the graph/debug info
- lets the frontend cache and reuse backend-preferred path strings
- makes debugging easier when users inspect returned locations

#### 2.6 Improve error reporting

Current failure messages are too opaque for path-shape mismatches.

When no node is found, the backend should try to return:

- queried `fl`
- normalized queried `fl`
- whether any modeled file matched by normalized path
- same-basename candidates
- same-line candidates if practical

Example desired diagnostic shape:

- requested `fl`
- normalized `fl`
- candidate modeled files:
  - `prebuilts/.../llvm/bin/../include/.../allocate_at_least.h`

This will greatly reduce debugging cost.

#### 2.7 Optionally add a dedicated resolving API

To make the system more explicit, the backend may expose a helper command such as:

- `resolve-source-file`

Input:

- `fl`

Output:

- normalized query key
- resolved modeled `raw_fl`
- all candidate modeled files if ambiguous

This is not strictly required if the matching is fixed centrally, but it would help frontend and agent workflows.


## 3. Why This Must Not Be Pushed to the Frontend

The frontend may normalize paths for display and request cleanliness, but it should not bear the correctness burden.

Reasons:

- The frontend does not know which exact `fl` strings exist in modeled graph data.
- The frontend cannot reliably infer hidden prefixes or debug-info-specific path spellings.
- Different projects, toolchains, and build systems may emit different raw path forms.
- Agent-generated requests are inherently less stable than deterministic backend graph data.

If correctness depends on the frontend guessing the exact path spelling, the API remains brittle even after UI improvements.

Therefore:

- frontend normalization is optional convenience
- backend path equivalence resolution is required correctness logic


## 4. Possible `fl` Formats From Frontend and Agent Workflows

The backend should assume requests may contain several valid but differently shaped `fl` formats.

### 4.1 Raw modeled path copied from backend output

Example:

- `prebuilts/clang/ohos/linux-x86_64/llvm/bin/../include/libcxx-ohos/include/c++/v1/__memory/allocate_at_least.h`

This is the most backend-friendly form and should continue to work.

### 4.2 Lexically normalized path

Example:

- `prebuilts/clang/ohos/linux-x86_64/llvm/include/libcxx-ohos/include/c++/v1/__memory/allocate_at_least.h`

This is a very natural frontend form and must be accepted.

### 4.3 Different but equivalent relative path spellings

Examples:

- `./prebuilts/clang/ohos/linux-x86_64/llvm/include/...`
- `prebuilts/clang/ohos/linux-x86_64/llvm/./include/...`
- `prebuilts/clang/ohos/linux-x86_64/llvm/bin/../include/...`

These should all resolve to the same modeled file when lexically equivalent.

### 4.4 Partially shortened project-relative paths

Examples:

- `llvm/include/libcxx-ohos/include/c++/v1/__memory/allocate_at_least.h`
- `include/libcxx-ohos/include/c++/v1/__memory/allocate_at_least.h`

These may appear in frontend shortcuts, copied logs, or agent-generated requests.
The backend should attempt normalized suffix/component matching, while still detecting ambiguity safely.

### 4.5 Basename-only paths

Example:

- `allocate_at_least.h`

This may appear in user prompts or low-context agent requests.
The backend may support this only as a low-priority fallback, and ambiguity must be reported explicitly.

### 4.6 Absolute host/container paths

Examples:

- `/src/NeverFree/prebuilts/clang/ohos/linux-x86_64/llvm/include/...`
- `/Users/xunyc/Documents/MemoryLeak/NeverFree/prebuilts/clang/ohos/linux-x86_64/llvm/include/...`

These may appear if an agent or tool copies a path from the local filesystem.
If the modeled path is project-relative, the backend should still attempt to strip known prefixes and compare normalized suffixes.

### 4.7 Paths copied from diagnostics or stack-like strings

Examples:

- `prebuilts/.../allocate_at_least.h:54`
- `prebuilts/.../allocate_at_least.h:54:9`

The backend should continue to parse and separate `fl` / `ln` / `cl` consistently.


## 5. Functional Requirements

The backend should satisfy the following:

1. Accept equivalent `fl` spellings that differ only by lexical normalization.
2. Resolve query `fl` against actual modeled graph file paths.
3. Avoid depending on exact raw debug-info path spelling from clients.
4. Preserve modeled raw `fl` in outputs.
5. Report ambiguity explicitly instead of silently choosing an unsafe match.
6. Return useful diagnostics when no match is found.
7. Apply this logic centrally so all location-based commands benefit consistently.


## 6. Non-Functional Requirements

The change should also aim for:

- consistency across all location-based backend commands
- no filesystem dependency for normalization
- low per-request overhead via indexed lookup
- backward compatibility with existing raw `fl` requests
- debuggability for frontend and agent consumers


## 7. Expected Outcome

After this change:

- frontend requests using normalized `fl` should work
- agent-generated requests should no longer need exact raw path memorization
- backend APIs should become stable across equivalent path spellings
- debugging path mismatches should become much easier

In short, the backend should treat `fl` as a user/query-facing locator that must be resolved against modeled graph reality, rather than as a brittle exact token that the caller must guess perfectly.
