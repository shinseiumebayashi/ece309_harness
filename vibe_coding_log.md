# Vibe Coding Log — ECE 309 Project 1

**Author:** shinseiumebayashi
**Repository:** ece309_harness
**Environment:** macOS (arm64), Apple clang 21.0.0, zsh

---

## 1. Architectural Rules (Specification Driven Development)

Before prompting any AI assistant, I fixed the following rules. The AI is not
permitted to deviate from them; any deviation is treated as a defect and fed
back for correction.

### R1. Hard constraints
- Standard C only. Permitted headers: `<stdio.h>`, `<stdlib.h>`, `<string.h>`.
- No external libraries, no POSIX-specific extensions beyond what these provide.
- Must compile cleanly with: `gcc -Wall -Wextra harness.c -o harness`
- Single source file, heavily commented.

### R2. State machine
```
START
  └─> initialise empty history
      └─> LOOP
            ├─ read one line from stdin (fgets)
            ├─ if EOF (Ctrl+D)      -> goto SHUTDOWN
            ├─ if line == "exit"    -> goto SHUTDOWN
            ├─ if line starts "calc:" -> TOOL path
            └─ else                 -> MODEL path
                  └─ append (user, reply) to history, back to LOOP
SHUTDOWN
  └─> free every allocated message -> return 0
```

### R3. Memory model
- History holds at most **5 turns**. One turn = one user message + one reply.
- Storage is a **ring buffer**; when a 6th turn arrives, the oldest turn is
  freed and overwritten.
- Every message is heap-allocated with `malloc` and released with `free`.
  Rationale: a fixed-size array would make the leak check meaningless. Using
  the heap creates a real opportunity to leak, so verifying "no leaks" actually
  proves something.
- Max message length: 256 characters including the terminator.

### R4. Tool execution (v1 — prefix dispatch)
- A line beginning with `calc:` is routed to the calculator tool, never to the
  model.
- Supported grammar: `calc: <number> <op> <number>` where op is `+ - * /`.
- Output format: `[tool] result = <value>`

### R5. Mock model behaviour
| Input contains | Reply |
|---|---|
| `hello` or `hi` | fixed greeting |
| `history` | prints the retained turns, oldest first |
| anything else | `[model] I received: <input>` |

The `history` command exists so that context management can be verified
deterministically from a test script.

### R6. Robustness — the program must never crash
- Input longer than 255 chars: truncate, warn, continue.
- Division by zero: print an error, continue the loop.
- Unparseable `calc:` expression: print an error, continue the loop.
- `fgets` returning NULL: treat as `exit` and shut down cleanly.

---

## 2. Prompts

### Prompt 1 — initial generation

> I need a single-file C program. I am a beginner, so keep it as simple as
> possible and comment it line by line. Use only `<stdio.h>`, `<stdlib.h>` and
> `<string.h>` — no external libraries. It must compile with no warnings under
> `gcc -Wall -Wextra`.
>
> The program is a minimal "agent harness": a loop that reads user input, sends
> it either to a mock model function or to a tool function, prints the result,
> and keeps a short conversation history.
>
> Specification:
>
> 1. **Main loop.** Read one line at a time from stdin using `fgets` into a
>    buffer of 256 bytes. Strip the trailing newline.
> 2. **Exit.** If the line is exactly `exit`, or if `fgets` returns NULL,
>    break out of the loop and shut down cleanly.
> 3. **Tool dispatch.** If the line begins with `calc:`, pass the rest to a
>    function `char *run_tool(const char *expr)`. It parses
>    `<number> <op> <number>` where op is one of `+ - * /`, and prints
>    `[tool] result = <value>`. On division by zero or a parse failure, print
>    an error message and continue the loop — do not exit, do not crash.
> 4. **Mock model.** Otherwise call `char *mock_model(const char *input)`:
>    - if the input contains `hello` or `hi`, return a fixed greeting
>    - if the input is `history`, print the stored history, oldest first
>    - otherwise return `[model] I received: <input>`
> 5. **History.** Store at most 5 turns (a turn = user message + reply) in a
>    ring buffer. Each message is `malloc`'d. When a 6th turn arrives, `free`
>    the oldest before overwriting it. Free everything before returning from
>    `main`.
> 6. **Robustness.** Input longer than 255 characters is truncated with a
>    warning. The program must never crash on any input.
>
> Please write the whole thing as one `harness.c`.

**Result:** *(paste the AI's response summary and any problems you found here)*

---

### Prompt 2 — compilation errors

> *(paste the exact compiler output here, then:)*
> Here is the output of `gcc -Wall -Wextra harness.c -o harness`. Fix the code
> and explain what was wrong.

**Result:** *(record what changed)*

---

### Prompt 3 — test script

> I have a compiled C program named `harness` in the current directory. Write a
> Bash script `test.sh` for macOS that tests it without any human interaction.
>
> Requirements:
> - Pipe a fixed sequence of lines into the program and compare the output
>   against expected values. Cover: a greeting, a `calc:` expression, a division
>   by zero, more than 5 turns followed by `history` (to prove the oldest turn
>   was dropped), and finally `exit`.
> - Print PASS or FAIL per case and exit non-zero if any case fails.
> - **Do not use valgrind.** This is Apple Silicon macOS and valgrind is not
>   available. For the memory check, recompile with
>   `gcc -fsanitize=address -g harness.c -o harness_debug`, run the same input
>   through `harness_debug`, and report any AddressSanitizer leak output.

**Result:** *(record what changed)*

---

## 3. Iterations and Defects Found

| # | Symptom | Cause | Fix |
|---|---|---|---|
| 1 | `this is a test` returns the greeting | R5 was written as a substring test, so `strstr` matches the `hi` inside `this`. The AI implemented the spec correctly — the spec itself was underspecified. | Require a word boundary, or match `hi` only as a whole line. |
| 2 | `hello calc: 3 + 4` goes to the model, not the tool | R4 dispatches only on a line *prefix*, so `calc:` mid-line is invisible to the harness. | Not a bug in v1. It is the structural limit of user-driven dispatch, and the motivation for the v2 model-driven design in section 4. |

---

## 4. Planned Extension — model-driven tool calls (v2)

The `calc:` prefix in R4 is a shortcut: the *user* decides that a tool runs.
In a real agent the *model* decides. Version 2 changes the contract so that
`mock_model` may return a sentinel string such as:

```
TOOL_CALL: multiply(12, 5)
```

The harness detects the sentinel, executes the tool, and feeds the result back
into the model for a final natural-language reply:

```
> what is 12 * 5
[model] TOOL_CALL: multiply(12, 5)
[tool]  result = 60
[model] The answer is 60.
```

This matches how a real harness mediates between a model and the operating
system, which is the architectural point of the assignment. The prefix version
is built first so that the loop, the history and the memory discipline are
known-good before the extra parsing stage is added.

**Prompt for v2:** *(add once v1 passes its tests)*

---

## 5. Verification Record

```
$ gcc -Wall -Wextra harness.c -o harness
(paste output)

$ bash test.sh
(paste output)

$ gcc -fsanitize=address -g harness.c -o harness_debug
$ ./harness_debug < test_input.txt
(paste output — expect no leak report)
```
