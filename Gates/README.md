# Gates

The typed gate table: every `MC2_*` environment variable the engine reads is a row in `gate_table.cpp`, and code reads it by registered name.

- Read: `gate::get<"MC2_X">()` (`gate.h`); a name that is not in the table does not compile.
- Regenerate the derived files and check the table: `py -3 tools/gate_gen.py` / `py -3 tools/gate_gen.py --check`.

## Setting a gate from a test driver or tool

Do not set a gate with a string literal (`_putenv("MC2_X=1")`, `setenv("MC2_X", ...)`): no registry check sees the name, so a rename or a retired row leaves the driver silently setting nothing.
Use `gate::ScopedOverride` (`gate_cold.h`):

```cpp
{
    gate::ScopedOverride<"MC2_SSAO"> ssao("1");   // name checked against the table at compile time
    spawnChild();                                  // the child inherits MC2_SSAO=1
}                                                  // previous value (or "unset") restored, gate cache cleared
```

- The change is made in the process environment, so child processes started inside the scope inherit it.
- The constructor and the destructor both drop the gate's cached value, so `gate::get<>()` in the same process sees the new text at once.
- `nullptr` or `""` clears the variable for the scope (the Windows CRT cannot hold an empty value).
- Scopes nest and unwind in reverse order. Single-threaded phases only, the same contract as `gate::invalidate()`.
- `ScopedEnv(name, value)` is the untyped base; use it only where the name is a runtime string.
