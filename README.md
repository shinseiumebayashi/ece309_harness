# ECE 309 Project 1 — An LLM Mini-Harness in C

A minimal agent harness written in C. The harness is the layer that sits
between a language model and the machine it runs on: it reads user input,
passes it to the model, notices when the model asks for a tool, runs that tool,
hands the result back, and keeps a bounded conversation history.

The model here is a mock — a plain C function that maps an input to a canned
reply. No network, no API key. The point of the project is the harness, not the
model.

The code was produced by *vibe coding*: the specification was written first by
hand, and an AI assistant generated the C from it. The specification, the
prompts, and the four defects found while verifying the output are recorded in
[`vibe_coding_log.md`](vibe_coding_log.md).

---

## Build

```
gcc harness.c -o harness
```

It also compiles clean with warnings turned all the way up, which is what the
specification actually required:

```
gcc -Wall -Wextra harness.c -o harness
```

Standard C only — `<stdio.h>`, `<stdlib.h>`, `<string.h>`, `<ctype.h>`. No
external libraries.

## Run

```
./harness
```

| Input | What happens |
|---|---|
| `what is 12 * 5` | The model asks for a tool; the harness runs it and the model answers |
| `what is 100 divided by 4` | Same, with word operators (`plus`, `minus`, `times`, `over`, `divided by`) |
| `calc: 3 + 4` | Explicit tool call, bypassing the model |
| `hello` / `hi` | Mock model returns a greeting |
| `history` | Prints the retained turns, oldest first |
| `exit` | Frees everything and shuts down |
| anything else | Mock model echoes it back |

Ctrl+D (end of input) is treated exactly like `exit`.

```
ECE 309 mini-harness (v2). Type 'exit' to quit.
> what is 12 * 5
TOOL_CALL: multiply(12, 5)
[tool]  result = 60
[model] The answer is 60.
> what is 10 / 0
TOOL_CALL: divide(10, 0)
[tool] error: division by zero
[harness] the tool call could not be completed.
> exit
[harness] goodbye.
```

---

## Design

### The tool-call round trip

This is the part worth explaining, because it is what the word *harness* means.

Version 1 of this project let the **user** decide when a tool ran, by typing a
`calc:` prefix. That works, but it is not what a harness does — it makes the
user the router.

In version 2 the **model** decides. `mock_model()` can decline to answer and
return a sentinel line instead:

```
TOOL_CALL: multiply(12, 5)
```

The harness recognises the sentinel, parses it, executes `multiply(12, 5)`, and
calls the model a second time with the result so it can phrase a final answer.
Three stages, three actors:

| Stage | Who acts | What happens |
|---|---|---|
| 1 | model | Recognises it cannot do arithmetic reliably; emits a tool request |
| 2 | **harness** | Parses the request, executes it, captures the output |
| 3 | model | Receives the output, phrases the answer |

The model never touches the machine. It only ever emits text. Everything that
actually *happens* — parsing, dispatch, execution, memory — happens in the
harness. That separation is the architectural point of the assignment, and it
is why version 2 exists.

`calc:` still works as an explicit escape hatch, which is what let the version 1
test suite carry over unchanged.

### Core loop

`fgets` reads one line into a 256-byte buffer. `exit` or EOF breaks the loop. A
`calc:` prefix goes straight to the tool; everything else goes to the model, and
the model's reply is checked for the sentinel. The final reply — not the
sentinel, not the raw tool output — is what enters the history.

### Context management

History is a **ring buffer of 5 turns**, where one turn is a user message paired
with its reply. Every message is heap-allocated with `malloc`. When a sixth turn
arrives the oldest is `free`d before its slot is reused, so memory stays bounded
no matter how long the session runs. Everything is released before `main`
returns.

The heap was chosen deliberately over fixed char arrays: a fixed array cannot
leak, which would make the leak check meaningless. Using the heap creates a real
opportunity to leak, so verifying that none occur actually proves something.

The `history` command exists so this state is observable from a test script
without a human watching.

The v2 round trip allocates three strings per tool-using turn (sentinel, tool
output, final reply) and keeps only the last. The other two are freed as soon as
they have been consumed.

### Robustness

Division by zero, a malformed tool call, an unknown tool name, an unparseable
`calc:` expression, and input longer than the buffer are all reported and
survived. **No input causes the program to exit or crash** — that was a hard
requirement, and test cases 3, 4 and 9 check it.

---

## Tests

```
bash test.sh
```

Ten cases, no human interaction:

| # | Checks |
|---|---|
| 1 | Greeting path reaches the mock model |
| 2 | Explicit `calc:` arithmetic returns the right answer |
| 3 | Division by zero is reported and survived (exit status 0) |
| 4 | A parse error is reported and survived (exit status 0) |
| 5 | After 8 turns the history holds the last 5 and dropped the first 3 |
| 6 | EOF without `exit` still shuts down cleanly |
| 7 | The full model → harness → tool → model round trip |
| 8 | `hi` no longer matches inside `this` |
| 9 | A refused tool call does not break the round trip |
| 10 | No memory leaks |

Test 7 requires all three stages to appear — the sentinel, the tool result, and
the final phrasing. Any one missing fails the case.

Tests 1–6 were written against version 1 and passed **unmodified** against
version 2, after a rewrite that changed the dispatch model, the greeting logic
and the memory lifetime of every reply. That is what the test suite was for.

### A note on the memory check

The first version of the test script used AddressSanitizer's LeakSanitizer. It
reported no leaks — but so did a version of the harness with a `free()`
deliberately deleted. **LeakSanitizer is not implemented on macOS**, so
`ASAN_OPTIONS=detect_leaks=1` was silently ignored and the check was inspecting
nothing at all.

`test.sh` now branches on `uname`: `leaks(1)` on macOS, AddressSanitizer on
Linux. Both branches were verified by re-injecting the same missing `free()` and
confirming that the case fails and names `dup_string` as the allocation site.

The lesson generalises, and it is the main thing this project taught:
**a test that passes proves nothing until you have watched it fail.**

`leaks(1)` prints some noise on recent macOS about `MallocStackLogging` and the
process not being debuggable. That is a sandboxing message, not a defect —
detection works regardless.

---

## Known limitations

**Arithmetic detection is shallow.** `detect_arithmetic()` scans for a number,
an operator and another number. It handles `what is 12 * 5` and
`100 divided by 4`, and correctly ignores `I have 2 cats and 3 dogs`, but it is
pattern matching, not understanding. A real model's decision to call a tool is
learned; this one is hard-coded. That is deliberate — the interesting part of
this project is what the harness does *with* the decision, not how the decision
is reached.

**One tool call per turn.** The harness runs the tool and gives the model one
follow-up turn. A real agent loops until the model stops asking, with a cap.
Adding that loop would mostly be a matter of wrapping the sentinel check in a
bounded `while`.

**Only binary arithmetic.** Two operands, one operator. No nesting, no
precedence, no `sqrt`.

**The mock model has no memory of the conversation.** The history is stored and
printable, but it is not fed back into `mock_model()`. A real harness re-sends
the retained turns with every request — that is what the context window *is*.
Wiring it in would be the natural next step.

---

## Files

| File | Purpose |
|---|---|
| `harness.c` | The harness — single file, heavily commented |
| `test.sh` | Automated tests, including the memory check |
| `vibe_coding_log.md` | Specification, prompts, and the defects found |
| `README.md` | This file |

## Environment

Developed on macOS (Apple Silicon) with Apple clang 21.0.0, and cross-checked on
Linux with GCC 13.3.0.
