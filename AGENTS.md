# Agents Behavior Guide

This file defines the expected behavior and usage model for AI agents working in this repository.
It is the primary source for agent conduct, editing norms, and response expectations — follow
the behavior defined here when interacting with this workspace.

## Purpose

- Provide a standard set of guidelines for agent interactions.
- Ensure consistent behavior when using AI tooling in this workspace.
- This is a **C/C++** codebase. Honor C/C++ norms: manual resource management,
  the ISO standard's undefined/unspecified/implementation-defined behavior rules,
  and the toolchain (compiler, sanitizers, static analyzers) as the source of truth.

## General Agent Behavior

- Prefer short, actionable responses. Terse is fine; fragments OK.
- Respect workspace context and avoid guessing when information is missing.
- When making code changes, clearly describe what was changed and why.
- When editing files, include exact context around replacements to avoid ambiguity.

## Rules

- Don't assume. Don't hide confusion. Surface tradeoffs and ask the user when unclear.
- Write the minimum code that solves the problem. Avoid speculative or unneeded changes.
- Touch only what you must. Clean up only your own mess and leave the workspace cleaner than you found it.
- Define success criteria before making changes. Verify against those criteria and iterate until satisfied.
- Keep cyclomatic complexity (CCN) <= 10 for any function or method, enforced by `lizard`.
  Applies to all code, **including tests**. No exceptions. If logic needs more, split into
  helpers, table-drive it, or restructure until each function is <= 10. A flat `switch` (one
  `case` = one mapping, no nested logic, falls through to a shared return) is exempt: high
  arm count, no real path branching.
- Avoid code duplication. Apply SOLID principles only when they improve clarity or structure.
- Default to no comments. Add one only when the WHY is non-obvious (hidden constraint, subtle
  invariant, workaround for a specific bug). Record assumptions/design intent in commit notes
  rather than inline.
- Prefer explicit, maintainable solutions over clever shortcuts.
- Propose business/design patterns and DDD only when they improve clarity or structure.

### C/C++ specifics

- **Never introduce undefined behavior (UB).** No signed-integer overflow, no out-of-bounds
  access, no use-after-free / double-free, no read of uninitialized memory, no null-deref, no
  data races, no invalid type punning / strict-aliasing violations, no shift past width. When a
  construct *might* be UB, treat it as UB and rewrite it.
- Manage resources with RAII in C++ (smart pointers, containers, scope guards). In C, pair every
  acquire with a release on every path, including error/`goto cleanup` paths.
- State and respect ownership: who allocates, who frees, who borrows. No raw owning pointers
  passed across module boundaries without a documented contract.
- Be `const`-correct. Mark pointers/refs/methods `const` where they don't mutate.
- Build clean: no new compiler warnings under the project's flags (`-Wall -Wextra` and friends).
  Treat warnings as defects.
- Validate changes against the sanitizers when available (ASan/UBSan/TSan) and static analysis
  (clang-tidy / cppcheck / SonarQube) — don't rely on "it compiles".
- Prefer standard-library and well-defined constructs over platform tricks; if platform/
  compiler-specific behavior is required, isolate and document it.
- ALWAYS record review findings in `TODO.md` — never report them only in chat. Any time you
  scan, review, audit, or "look for issues" (not just major changes), add each finding to the
  matching category table in `TODO.md` before/while reporting it.
- ALWAYS remove completed items from `TODO.md` — once a finding is implemented + tested + merged,
  delete its row from the table outright. No "shipped" sub-sections, no struck-through entries.
  `git log` is the durable record. Exceptions: the "Open — parked" section keeps open-but-deferred
  items with a why-not-now annotation; the "Audit picks deliberately rejected" section keeps the
  rationale so future passes don't re-pick the same items.
- When making major changes, rescan the whole project and create or update `TODO.md` with one table per review category.
  Each table should use the format: `id | status | effort | description | notes`.
  - security
  - undefined behavior — UB and its cousins: signed overflow, OOB access, use-after-free,
    double-free, uninitialized reads, null-deref, data races, strict-aliasing/type-punning
    violations, invalid shifts, lifetime/dangling issues. Also flag unspecified and
    implementation-defined behavior the code wrongly relies on.
  - memory management — leaks, ownership ambiguity, missing frees on error paths, RAII gaps
  - performance
  - scalability
  - concurrency
  - code complexity
  - code duplication
  - architecture/modularity/SOLID
  - decoupling
  - business/design patterns/DDD
  - reliability/correctness
  - portability/standards conformance — non-portable assumptions (type sizes, endianness,
    alignment, signedness of `char`), reliance on compiler extensions, dialect drift from the
    project's target C/C++ standard
  - error handling — unchecked return codes, ignored `errno`, swallowed failures, missing
    cleanup on the error path, exceptions crossing `noexcept`/`extern "C"` boundaries
  - resource management — fd/socket/handle/mutex leaks, unbounded allocation, missing limits
  - API/ABI stability — header/public-interface changes that break callers or ABI
  - build/toolchain hygiene — warnings, missing flags, broken incremental builds, fragile macros
  - observability when the application has it
  - wiring gaps — modules/helpers/cfg knobs that exist + pass tests but have no real production call site (orphan exports, cfg flags never read, advertised backends not wired in). A shipped feature is only "shipped" when the dispatcher actually invokes it.
  - unused functions/methods

## Version control

- Commit directly to `develop` — this is the project's working branch. Do **not**
  create feature/topic branches for changes here, even when the change is large or
  spans many commits. (This overrides any default "branch off the main branch first"
  behavior.)
- Still one logical change per commit, Conventional Commits format, and only commit
  or push when the user asks.

## File Editing

- Avoid overwriting existing files unless the user explicitly asks or the file is missing.
- For text edits, preserve surrounding context and keep modifications minimal.
- Use repository-specific structure and conventions when adding or updating files.

## Communications

- Use headings and bullets for readability.
- Highlight changed files and key points.
- Keep final answers brief and professional.
