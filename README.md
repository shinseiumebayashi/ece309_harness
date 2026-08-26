# ECE 309 Project 1 — An LLM Mini-Harness in C

A minimal agent harness written in C. The harness is the layer that sits
between a language model and the machine it runs on: it reads user input,
decides whether that input goes to the model or to a tool, runs the tool,
prints the result, and keeps a bounded conversation history.

The model here is a mock — a plain C function that maps an input to a canned
reply. No network, no API key. The point of the project is the harness, not
the model.

This code was produced by *vibe coding*: the specification was written first
by hand, and an AI assistant generated the C from it. The full specification,
the prompts, and the defects found while verifying the output are recorded in
[`vibe_coding_log.md`](vibe_coding_log.md).

---

## Build

```
gcc harness.c -o harness
```

It also compiles clean with warnings turned all the way up, which is the
condition the specification actually required:

```
gcc -Wall -Wextra harness.c -o harness
```

Standard C only — `<stdio.h>`, `<stdlib.h>`, `<string.h>`. No external
libraries.

## Run

```
./harness
```

| Input | What happens |
|---|---|
| `calc: 3 + 4` | Routed to the calculator tool → `[tool] result = 7` |
| `hello` / `hi` | Mock model returns a greeting |
| `history` | Prints the retained turns, oldest first |
| `exit` | Frees everything and shuts down |
| anything else | Mock model echoes it back |

Ctrl+D (end of input) is treated exactly like `exit`.

Example session:

```
ECE 309 mini-harness. Type 'exit' to quit.
> hello
[model] Hello! I am a mock model. Try 'calc: 3 + 4', 'history', or 'exit'.
> calc: 3 + 4
[tool] result = 7
> calc: 10 / 0
[tool] error: division by zero
> exit
[harness] goodbye.
```

---

## Design

### Core loop

`fgets` reads one line into a 256-byte buffer. A line beginning with `calc:`
goes to `run_tool()`; everything else goes to `mock_model()`. The reply is
printed and the turn is appended to the history. `exit` or EOF breaks the loop.

### Context management

History is a **ring buffer of 5 turns**, where one turn is a user message
paired with its reply. Every message is heap-allocated with `malloc`. When a
sixth turn arrives, the oldest is `free`d before its slot is reused, so memory
usage stays bounded no matter how long the session runs. Everything is
released before `main` returns.

The heap was chosen deliberately over a fixed array of char buffers: a fixed
array cannot leak, which would make the leak check in the test suite
meaningless. Using the heap creates a real opportunity to leak, so verifying
that none occur actually proves something.

The `history` command exists so this state is observable from a test script
without a human watching.

### Tool execution

`run_tool()` parses `<number> <op> <number>` where the operator is `+`, `-`,
`*` or `/`. Division by zero and unparseable input print an error and return
to the loop. **No input causes the program to exit or crash** — that was a
hard requirement in the specification, and it is what test cases 3 and 4
check.

---

## Tests

```
bash test.sh
```

Seven cases run with no human interaction:

| # | Checks |
|---|---|
| 1 | Greeting path reaches the mock model |
| 2 | Tool arithmetic returns the right answer |
| 3 | Division by zero is reported and survived (exit status 0) |
| 4 | A parse error is reported and survived (exit status 0) |
| 5 | After 8 turns the history holds the last 5 and dropped the first 3 |
| 6 | EOF without `exit` still shuts down cleanly |
| 7 | No memory leaks |

### A note on the memory check

The first version of the test script used AddressSanitizer's LeakSanitizer.
It reported no leaks — but so did a version of the harness with a `free()`
deliberately deleted. **LeakSanitizer is not implemented on macOS**, so
`ASAN_OPTIONS=detect_leaks=1` was silently ignored and the check was
inspecting nothing at all.

`test.sh` now branches on `uname`: `leaks(1)` on macOS, AddressSanitizer on
Linux. Both were verified by re-injecting the same missing `free()` and
confirming that test 7 fails and names `dup_string` as the allocation site.

The lesson generalises: a test that passes proves nothing until you have
watched it fail.

`leaks(1)` prints some noise on recent macOS about `MallocStackLogging` and
the process not being debuggable. This is a sandboxing message, not a defect —
detection works regardless.

---

## Known limitations

These are real and were found during verification. They are documented rather
than hidden.

**`hi` matches inside other words.** The specification said the model replies
to input *containing* `hello` or `hi`, so `strstr` was the correct
implementation of what was asked for — and `this is a test` returns the
greeting. The defect is in the specification, not the generated code. A word
boundary check would fix it.

**Tool dispatch only looks at the start of the line.** `hello calc: 3 + 4`
goes to the model, because `calc:` is only recognised as a prefix. This is
not a bug so much as the structural limit of letting the *user* decide when a
tool runs.

That second limitation is the interesting one. In a real agent the *model*
decides that a tool is needed, and the harness mediates. A version 2 of this
project would have `mock_model()` return a sentinel:

```
> what is 12 * 5
[model] TOOL_CALL: multiply(12, 5)
[tool]  result = 60
[model] The answer is 60.
```

The harness detects the sentinel, runs the tool, and feeds the result back
into the model for a final reply. That round trip is what a harness is
actually for. The prefix version was built first so that the loop, the
history, and the memory discipline were known-good before the extra parsing
stage was added.

---

## Files

| File | Purpose |
|---|---|
| `harness.c` | The harness — single file, heavily commented |
| `test.sh` | Automated tests, including the memory check |
| `vibe_coding_log.md` | Specification, prompts, and the defects found |
| `README.md` | This file |

## Environment

Developed on macOS (Apple Silicon) with Apple clang 21.0.0, and verified on
Linux with GCC 13.3.0.
