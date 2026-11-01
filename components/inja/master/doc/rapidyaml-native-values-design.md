# Design: Native Value Nodes for RapidYaml in inja

**Status:** Proposal  
**Date:** 2026-02-26  
**Scope:** RapidYaml-backed runtime values for template evaluation

## 1. Summary

This document proposes extending the RapidYaml integration with **native value nodes** so values are represented by concrete C++ types instead of scalar strings whenever needed:

- `int64_t`
- `uint64_t`
- `double` (float)
- `std::string`
- `bool`

The key property is **lazy conversion** from a basic `ryml::ConstNodeRef`/`ryml::NodeRef` to a native value view. Conversion is performed only when required (for example during type queries, arithmetic, comparisons, truthiness checks, or function dispatch), not eagerly at parse time.

This gives a practical advantage over `nlohmann::json` in the rendering path:
- The renderer keeps RapidYaml’s lightweight node navigation by default.
- Native typing is paid for only at query points.
- Template modifications can operate on typed values without manually parsing strings.

## 2. Goals and Non-Goals

### Goals

- Preserve existing RapidYaml tree ownership and navigation.
- Add a native typed value layer for evaluation and mutation.
- Keep conversions lazy and local to the operation requiring type information.
- Support assignment and mutation with typed values in template statements (`set`, function return values, loop metadata).
- Keep behavior deterministic via documented coercion rules.

### Non-Goals

- Replacing RapidYaml internals or changing `ryml::Tree` memory model.
- Adding new end-user template syntax.
- Full YAML schema/type-system support beyond the 5 target native categories.

## 3. Problem Statement

Current RapidYaml usage treats scalar values mostly as string-like (`c4::csubstr`) content. This causes friction for template engines:

- Numeric operations require repeated string parsing.
- Boolean logic depends on ad-hoc checks.
- Mutation APIs often round-trip through strings.
- Type checks are scattered and expensive when repeated.

A native value abstraction should solve this while keeping `NodeRef` as the primary graph handle.

## 4. Proposed Architecture

## 4.1 Core Concepts

- **Basic node ref**: existing `ryml::ConstNodeRef` / `ryml::NodeRef`.
- **Native value view**: typed interpretation of a scalar node or runtime temporary.
- **Runtime value store**: a tree-owned area for computed temporaries and assigned values.
- **Lazy promotion**: conversion from basic ref to native value only when an operation requires it.

## 4.2 Value Model

Introduce an internal value representation:

```cpp
enum class NativeKind {
  Invalid,
  Int64,
  UInt64,
  Float64,
  String,
  Bool
};

struct NativeValue {
  NativeKind kind {NativeKind::Invalid};
  int64_t i64 {};
  uint64_t u64 {};
  double f64 {};
  bool b {};
  std::string s;
};
```

Notes:
- This structure is an **evaluation/runtime abstraction**. It does not replace RapidYaml node storage.
- Invalid represents “no value / unresolved”.

## 4.3 Native Node Reference

Add an internal wrapper (`NativeNodeRef`) that keeps the original `NodeRef` plus optional typed cache:

```cpp
struct NativeNodeRef {
  ryml::ConstNodeRef node;
  mutable bool has_cached_value {false};
  mutable NativeValue cached;
};
```

For mutable operations, use a mutable variant carrying `ryml::NodeRef`.

Responsibilities:
- Expose quick node metadata (`is_map`, `is_seq`, `valid`).
- Provide `kind()` / `as_*()` APIs that trigger lazy promotion.
- Write back typed values to runtime nodes when mutation occurs.

## 4.4 Lazy Promotion Rules

Promotion is triggered only when needed by:

- `is_number`, `is_string`, `is_bool`, `type` queries
- arithmetic operators (`+`, `-`, `*`, `/`, `%`)
- numeric/string comparisons
- truthiness checks (`if`, `and`, `or`, `not`)
- built-in functions requiring specific types

No promotion is needed for:
- pure structural traversal (`a.b.c`, iteration)
- direct include/path lookup
- rendering where scalar can be printed directly

Promotion algorithm for scalar text:

1. Try `bool` (`true`, `false`, optionally `1`, `0` only in truthiness mode).
2. Try signed integer (`int64_t`) if full parse succeeds and range fits.
3. Try unsigned integer (`uint64_t`) if full parse succeeds and range fits.
4. Try floating (`double`) if full parse succeeds.
5. Fallback to string.

Design rule: parse must consume the entire scalar; partial parses are rejected.

## 4.5 Runtime Value Storage

Computed/assigned values must live in a known mutable subtree of `additional_data`:

- Create `_native_tmp` sequence node once per render.
- Every computed temporary appends one child storing scalar/map/seq result.
- `set` statements write into mutable nodes under this runtime tree (or target tree if explicitly allowed).

This guarantees value lifetime while template evaluation stacks keep references.

## 5. API Sketch (Internal)

## 5.1 Conversion and Query Helpers

```cpp
NativeKind query_kind(ryml::ConstNodeRef n);
NativeValue to_native_value(ryml::ConstNodeRef n);
bool try_to_int64(ryml::ConstNodeRef n, int64_t& out);
bool try_to_uint64(ryml::ConstNodeRef n, uint64_t& out);
bool try_to_double(ryml::ConstNodeRef n, double& out);
bool try_to_bool(ryml::ConstNodeRef n, bool& out);
```

## 5.2 Mutation Helpers

```cpp
ryml::NodeRef append_native_tmp(ryml::NodeRef tmp_root, const NativeValue& v);
void write_native_value(ryml::NodeRef node, const NativeValue& v);
```

`write_native_value` serializes the typed value into the target node while preserving category intent (scalar/map/seq as applicable).

## 5.3 Renderer Stack Integration

Replace plain node stacks with native-capable wrappers where type operations are common:

- expression stack: `std::vector<NativeNodeRef>`
- function args: `std::vector<NativeNodeRef>` (or const variant)

This allows typed operations without losing direct node access.

## 6. Coercion Semantics

To keep behavior deterministic:

- **Arithmetic:**
  - if both sides are integers (`int64`/`uint64`), compute in the widest safe path.
  - if either side is float, promote both to `double`.
  - string arithmetic is invalid except `+` for concatenation when one operand is string.

- **Comparison:**
  - if both numeric-parsable, compare numerically.
  - else if both bool, compare bool.
  - else compare as strings.

- **Truthiness:**
  - `bool` => itself
  - numeric => zero is false
  - string => empty is false
  - seq/map => `num_children() > 0`
  - invalid/missing => false

- **Rendering:**
  - preserve scalar textual output conventions currently used by renderer.

## 7. Why This Is Better Than nlohmann JSON (for this path)

For this specific renderer architecture, RapidYaml + lazy native promotion provides:

- Faster baseline traversal (no eager JSON value materialization for every node).
- Type conversion costs only when queried.
- Better control over conversion policy (explicit coercion table).
- Stable references to YAML tree nodes with runtime temporary ownership handled explicitly.

In short: `NodeRef` remains cheap and structural; native conversion is opt-in and demand-driven.

## 8. Compatibility and Migration

- Existing templates should continue to work with same syntax.
- Edge-case behavior may differ where previous implementation relied on string-only comparisons.
- Document coercion behavior changes in release notes.
- Keep a feature toggle during migration (e.g. `INJA_RYML_NATIVE_VALUES`) to compare old/new runtime behavior if needed.

## 9. Testing Strategy

Add focused tests for:

1. **Type detection**
   - scalar literals for bool/int/uint/float/string
   - overflow and invalid numeric strings

2. **Operations**
   - arithmetic mixed-type cases
   - comparison precedence rules
   - truthiness matrix

3. **Mutation**
   - `set` writes typed values
   - function return values as typed temps

4. **Regression parity**
   - current renderer tests that must remain unchanged
   - explicit snapshots for ambiguous string/number comparisons

5. **Performance sanity**
   - ensure no eager full-tree conversion appears in hot paths

## 10. Implementation Plan

1. Introduce `NativeKind`, `NativeValue`, and parse helpers in utility layer.
2. Add `NativeNodeRef` wrappers and conversion cache.
3. Integrate wrappers in renderer expression/function stacks.
4. Route all typed writes through runtime temp/mutation helpers.
5. Apply coercion rules to operators, conditions, and built-ins.
6. Add tests and migration toggle.
7. Benchmark and remove toggle once parity is confirmed.

## 11. Risks and Mitigations

- **Risk:** subtle behavior shifts in comparisons.  
  **Mitigation:** lock coercion table + targeted regression tests.

- **Risk:** lifetime bugs from temporary node references.  
  **Mitigation:** strict ownership under `_native_tmp` subtree.

- **Risk:** parse overhead under heavy expression usage.  
  **Mitigation:** cache promoted value per wrapper and avoid duplicate parsing within one evaluation step.

## 12. Answered Questions

- `"1"`/`"0"` should NOT be accepted as bool outside truthiness contexts.
- Mixed unsigned-vs-signed comparisons should clamp on overflow boundaries.
- Native strings should preserve YAML quoting style.

---

This design keeps RapidYaml as the source of truth while enabling native typed operations exactly when template evaluation needs them.