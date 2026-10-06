## HOW TO ADD A NEW LIE

When you discover a place where the engine lies to itself, add it to §OPEN LIES with the five fields: **Symptom**, **Why it is a lie** (which law), **Repayment plan** (with LoC estimate), **Cost**, **Priority**.

Declare it the same day you see it — hidden = cancer.

When you close it: set status CLOSED with a date, move the entry to §CLOSED LIES exactly once (table row + optional archive narrative), and re-run `lie\_budget\_gate`.

---

## §OPEN LIES

| # | Symptom | Why it is a lie | Repayment plan | Cost | Priority |
|---|---------|----------------|----------------|------|----------|
| 1 | `Exception.cpp`: exception handlers registered but never invoked | Handler stored in `g_exceptionHandlers[]`, but `sceKernelRaiseException()` only logs, never calls it | Implement handler invocation: cast stored address to function pointer and call it | ~20 LoC | Medium |
| 2 | `Coredump.cpp`: coredump handlers registered but never invoked | `sceCoredumpRegisterCoredumpHandler` stores handler/context, never called on host crashes | Forward host exceptions to guest coredump handler | ~100 LoC | Low |

---

## §CLOSED LIES

*None yet*

---

**END OF LIE BUDGET (v2)**