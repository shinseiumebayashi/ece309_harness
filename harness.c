/* ============================================================
 * harness.c  --  ECE 309 Project 1: a minimal LLM agent harness
 *
 * Version 2: model-driven tool calls.
 *
 * Build:  gcc -Wall -Wextra harness.c -o harness
 * Run:    ./harness
 *
 * In version 1 the *user* decided when a tool ran, by typing a
 * "calc:" prefix. That is not what a real harness does. Here the
 * *model* decides: mock_model() may answer with a sentinel line
 *
 *     TOOL_CALL: multiply(12, 5)
 *
 * which the harness intercepts, executes, and feeds back into the
 * model so it can phrase a final answer. That round trip -- model
 * asks, harness acts, model speaks -- is the whole point of a
 * harness, and it is what this version demonstrates.
 *
 * The "calc:" prefix is kept as an explicit escape hatch so the
 * version 1 test suite still applies unchanged.
 * ============================================================ */

#include <stdio.h>   /* printf, fgets, sscanf, snprintf */
#include <stdlib.h>  /* malloc, free, strtod */
#include <string.h>  /* strlen, strcmp, strncmp, strstr, memcpy */
#include <ctype.h>   /* isalpha, isdigit, isspace */

/* ---------- tunable limits ---------- */
#define MAX_LINE 256   /* buffer size for one input line, incl. '\0' */
#define MAX_TURNS 5    /* how many turns the history remembers       */
#define SENTINEL "TOOL_CALL:"   /* what the model says to ask for a tool */

/* ============================================================
 * Conversation history
 *
 * One "turn" is a pair: what the user said, and what came back.
 * The history is a ring buffer: once it holds MAX_TURNS turns, the
 * next turn overwrites the oldest one (after freeing it).
 * ============================================================ */
struct Turn {
    char *user;  /* heap copy of the user's line */
    char *reply; /* heap copy of the reply       */
};

static struct Turn history[MAX_TURNS]; /* zero-initialised: all NULL */
static int history_count = 0;          /* how many slots are in use  */
static int history_next = 0;           /* slot the next turn goes in */

/* ------------------------------------------------------------
 * dup_string: make a heap copy of a string.
 * (strdup is not in the C standard before C23, so we write our own.)
 * Returns NULL if the allocation fails.
 * ------------------------------------------------------------ */
static char *dup_string(const char *src)
{
    size_t n = strlen(src) + 1;   /* +1 for the '\0' terminator */
    char *copy = malloc(n);
    if (copy == NULL) {
        return NULL;
    }
    memcpy(copy, src, n);
    return copy;
}

/* ------------------------------------------------------------
 * add_turn: store one (user, reply) pair in the ring buffer.
 * Takes ownership of both pointers -- they must be heap strings,
 * and the caller must not free them afterwards.
 * ------------------------------------------------------------ */
static void add_turn(char *user, char *reply)
{
    /* If this slot already holds an old turn, release it first.
     * This is what keeps memory usage bounded at MAX_TURNS. */
    free(history[history_next].user);
    free(history[history_next].reply);

    history[history_next].user = user;
    history[history_next].reply = reply;

    history_next = (history_next + 1) % MAX_TURNS;

    if (history_count < MAX_TURNS) {
        history_count++;
    }
}

/* ------------------------------------------------------------
 * print_history: show the retained turns, oldest first.
 * ------------------------------------------------------------ */
static void print_history(void)
{
    int i;

    if (history_count == 0) {
        printf("[history] (empty)\n");
        return;
    }

    printf("[history] %d turn(s), oldest first:\n", history_count);

    for (i = 0; i < history_count; i++) {
        /* When the buffer is full the oldest entry sits at
         * history_next; when it is not yet full it sits at 0.
         * The modulo arithmetic below covers both cases. */
        int slot = (history_next - history_count + i + MAX_TURNS) % MAX_TURNS;
        printf("  %d. user : %s\n", i + 1, history[slot].user);
        printf("     reply: %s\n", history[slot].reply);
    }
}

/* ------------------------------------------------------------
 * free_history: release everything before the program exits.
 * ------------------------------------------------------------ */
static void free_history(void)
{
    int i;
    for (i = 0; i < MAX_TURNS; i++) {
        free(history[i].user);   /* free(NULL) is legal and does nothing */
        free(history[i].reply);
        history[i].user = NULL;
        history[i].reply = NULL;
    }
    history_count = 0;
    history_next = 0;
}

/* ============================================================
 * The tool
 * ============================================================ */

/* ------------------------------------------------------------
 * apply_op: do the arithmetic.
 * Returns 1 on success and writes the answer through *out;
 * returns 0 on division by zero or an unknown operator.
 * ------------------------------------------------------------ */
static int apply_op(double a, char op, double b, double *out)
{
    switch (op) {
        case '+': *out = a + b; return 1;
        case '-': *out = a - b; return 1;
        case '*': *out = a * b; return 1;
        case '/':
            if (b == 0.0) {
                return 0;       /* caller reports the error */
            }
            *out = a / b;
            return 1;
        default:
            return 0;
    }
}

/* ------------------------------------------------------------
 * run_tool: evaluate "<number> <op> <number>".
 * Prints the result and returns a heap string the caller owns,
 * or NULL on a parse error or a division by zero. Never crashes.
 *
 * This is the version 1 entry point, used by the "calc:" prefix.
 * ------------------------------------------------------------ */
char *run_tool(const char *expr)
{
    double a = 0.0, b = 0.0, result = 0.0;
    char op = '?';
    char extra = '\0';
    char out[MAX_LINE];
    int matched;

    /* Read: optional spaces, a number, an operator, a number.
     * The trailing " %c" is a trap: if it matches there was junk
     * after the expression, so we reject the whole thing. */
    matched = sscanf(expr, " %lf %c %lf %c", &a, &op, &b, &extra);

    if (matched != 3) {
        printf("[tool] error: could not parse '%s'"
               " (expected: <number> <op> <number>)\n", expr);
        return NULL;
    }

    if (!apply_op(a, op, b, &result)) {
        if (op == '/') {
            printf("[tool] error: division by zero\n");
        } else {
            printf("[tool] error: unknown operator '%c' (use + - * /)\n", op);
        }
        return NULL;
    }

    /* %g prints 7 rather than 7.000000, which reads better. */
    snprintf(out, sizeof(out), "[tool] result = %g", result);
    printf("%s\n", out);
    return dup_string(out);
}

/* ------------------------------------------------------------
 * op_from_name: turn a tool name into an operator character.
 * Returns '?' if the name is not one we know.
 * ------------------------------------------------------------ */
static char op_from_name(const char *name)
{
    if (strcmp(name, "add") == 0)      return '+';
    if (strcmp(name, "subtract") == 0) return '-';
    if (strcmp(name, "multiply") == 0) return '*';
    if (strcmp(name, "divide") == 0)   return '/';
    return '?';
}

/* ------------------------------------------------------------
 * run_named_tool: execute a call of the form  name(a, b).
 * This is the version 2 entry point, used when the *model* asks
 * for a tool rather than the user.
 *
 * Prints the result and returns a heap string the caller owns,
 * or NULL if the call could not be honoured.
 * ------------------------------------------------------------ */
static char *run_named_tool(const char *call)
{
    char name[32];
    double a = 0.0, b = 0.0, result = 0.0;
    char op;
    char out[MAX_LINE];

    /* %31[a-z] reads the tool name without risking an overflow. */
    if (sscanf(call, " %31[a-z] ( %lf , %lf )", name, &a, &b) != 3) {
        printf("[tool] error: malformed tool call '%s'\n", call);
        return NULL;
    }

    op = op_from_name(name);
    if (op == '?') {
        printf("[tool] error: no such tool '%s'"
               " (have: add, subtract, multiply, divide)\n", name);
        return NULL;
    }

    if (!apply_op(a, op, b, &result)) {
        printf("[tool] error: division by zero\n");
        return NULL;
    }

    snprintf(out, sizeof(out), "[tool]  result = %g", result);
    printf("%s\n", out);
    return dup_string(out);
}

/* ============================================================
 * The mock model
 * ============================================================ */

/* ------------------------------------------------------------
 * has_word: true if `needle` appears in `haystack` as a whole word.
 *
 * Version 1 used plain strstr here, which meant the greeting fired
 * on "this is a test" -- the "hi" inside "this" matched. Checking
 * that the characters on either side are not letters fixes that.
 * ------------------------------------------------------------ */
static int has_word(const char *haystack, const char *needle)
{
    size_t nlen = strlen(needle);
    const char *p = haystack;

    while ((p = strstr(p, needle)) != NULL) {
        /* Character before the match: start of string, or not a letter. */
        int left_ok = (p == haystack) || !isalpha((unsigned char)p[-1]);
        /* Character after the match: end of string, or not a letter. */
        int right_ok = !isalpha((unsigned char)p[nlen]);

        if (left_ok && right_ok) {
            return 1;
        }
        p += nlen;   /* keep looking further along the string */
    }
    return 0;
}

/* ------------------------------------------------------------
 * word_op: map an English operator word to its symbol.
 * Returns '?' if the word is not an operator we recognise.
 * ------------------------------------------------------------ */
static char word_op(const char *word)
{
    if (strcmp(word, "plus") == 0)     return '+';
    if (strcmp(word, "minus") == 0)    return '-';
    if (strcmp(word, "times") == 0)    return '*';
    if (strcmp(word, "over") == 0)     return '/';
    if (strcmp(word, "divided") == 0)  return '/';   /* "divided by" */
    return '?';
}

/* ------------------------------------------------------------
 * detect_arithmetic: look for a sum inside ordinary prose.
 *
 * Recognises "what is 12 * 5" and "what is 12 times 5" alike.
 * Returns 1 and fills in *a, *op and *b when it finds one.
 *
 * This stands in for the pattern-matching a real model does when it
 * decides a question needs a calculator. It is deliberately shallow:
 * the interesting part of this project is what the harness does with
 * the decision, not how the decision is made.
 * ------------------------------------------------------------ */
static int detect_arithmetic(const char *text, double *a, char *op, double *b)
{
    const char *p = text;

    while (*p != '\0') {
        char *after_first = NULL;
        double first, second;
        char word[16];
        char symbol;

        /* Find somewhere a number could start. */
        if (!isdigit((unsigned char)*p)) {
            p++;
            continue;
        }

        first = strtod(p, &after_first);
        if (after_first == p) {      /* not actually a number */
            p++;
            continue;
        }

        /* Skip the spaces between the number and the operator. */
        while (isspace((unsigned char)*after_first)) {
            after_first++;
        }

        /* Case 1: a symbol operator, e.g. "12 * 5". */
        symbol = *after_first;
        if (symbol == '+' || symbol == '-' || symbol == '*' || symbol == '/') {
            if (sscanf(after_first + 1, " %lf", &second) == 1) {
                *a = first;
                *op = symbol;
                *b = second;
                return 1;
            }
        }

        /* Case 2: a word operator, e.g. "12 times 5". */
        if (sscanf(after_first, " %15[a-z]", word) == 1) {
            char w = word_op(word);
            if (w != '?') {
                const char *rest = after_first + strlen(word);
                /* "divided by 5": step over the "by". */
                if (strcmp(word, "divided") == 0) {
                    while (isspace((unsigned char)*rest)) rest++;
                    if (strncmp(rest, "by", 2) == 0) rest += 2;
                }
                if (sscanf(rest, " %lf", &second) == 1) {
                    *a = first;
                    *op = w;
                    *b = second;
                    return 1;
                }
            }
        }

        p = after_first;   /* no match here; carry on scanning */
    }

    return 0;
}

/* ------------------------------------------------------------
 * name_from_op: the inverse of op_from_name, used when the model
 * writes out the tool call it wants.
 * ------------------------------------------------------------ */
static const char *name_from_op(char op)
{
    switch (op) {
        case '+': return "add";
        case '-': return "subtract";
        case '*': return "multiply";
        case '/': return "divide";
        default:  return "unknown";
    }
}

/* ------------------------------------------------------------
 * mock_model: stands in for a real LLM. No network, no API key --
 * it maps an input to a canned reply.
 *
 * It may also decline to answer and ask for a tool instead, by
 * returning a line that starts with SENTINEL. The harness is what
 * notices that and acts on it; the model never touches the machine
 * itself. That separation is the architectural point.
 *
 * Returns a heap string the caller owns, or NULL on allocation
 * failure.
 * ------------------------------------------------------------ */
char *mock_model(const char *input)
{
    char out[MAX_LINE + 64];
    double a, b;
    char op;

    /* "history" is handled here so the conversation state is
     * observable from a test script without human interaction. */
    if (strcmp(input, "history") == 0) {
        print_history();
        return dup_string("[model] (printed the history above)");
    }

    if (has_word(input, "hello") || has_word(input, "hi")) {
        return dup_string("[model] Hello! I am a mock model. "
                          "Try 'what is 12 * 5', 'history', or 'exit'.");
    }

    /* Arithmetic is something a language model is bad at, so the
     * model asks for the calculator rather than guessing. */
    if (detect_arithmetic(input, &a, &op, &b)) {
        snprintf(out, sizeof(out), "%s %s(%g, %g)",
                 SENTINEL, name_from_op(op), a, b);
        return dup_string(out);
    }

    snprintf(out, sizeof(out), "[model] I received: %s", input);
    return dup_string(out);
}

/* ------------------------------------------------------------
 * mock_model_after_tool: the model's second turn, once the harness
 * has run the tool and can hand back a result.
 *
 * A real harness re-sends the whole conversation plus the tool
 * output. Here we just pass the result, which is enough to show
 * the shape of the exchange.
 * ------------------------------------------------------------ */
static char *mock_model_after_tool(const char *tool_output)
{
    char out[MAX_LINE + 64];
    const char *equals = strchr(tool_output, '=');
    const char *value = (equals != NULL) ? equals + 1 : tool_output;

    while (isspace((unsigned char)*value)) {
        value++;
    }

    snprintf(out, sizeof(out), "[model] The answer is %s.", value);
    return dup_string(out);
}

/* ============================================================
 * main -- the core loop
 * ============================================================ */
int main(void)
{
    char line[MAX_LINE];

    printf("ECE 309 mini-harness (v2). Type 'exit' to quit.\n");

    for (;;) {
        char *newline;
        char *reply;
        char *user_copy;

        printf("> ");
        fflush(stdout);   /* make sure the prompt appears before we block */

        /* ---- read one line ---- */
        if (fgets(line, sizeof(line), stdin) == NULL) {
            /* NULL means end of input (Ctrl+D, or a pipe running dry).
             * Treat it exactly like "exit" so the shutdown path is the
             * same one the tests exercise. */
            printf("\n[harness] end of input, shutting down.\n");
            break;
        }

        /* ---- strip the newline, or handle an over-long line ---- */
        newline = strchr(line, '\n');
        if (newline != NULL) {
            *newline = '\0';          /* normal case: cut the '\n' off */
        } else {
            /* No newline in the buffer means the user typed more than we
             * can hold. Keep what we have and discard the rest so the
             * leftovers are not read as a second command. */
            int c;
            printf("[harness] warning: input too long, truncated to %d characters.\n",
                   MAX_LINE - 1);
            while ((c = getchar()) != '\n' && c != EOF) {
                /* discard */
            }
        }

        /* ---- exit ---- */
        if (strcmp(line, "exit") == 0) {
            printf("[harness] goodbye.\n");
            break;
        }

        /* ---- ignore a blank line ---- */
        if (line[0] == '\0') {
            continue;
        }

        /* ---- explicit tool call (the version 1 escape hatch) ---- */
        if (strncmp(line, "calc:", 5) == 0) {
            reply = run_tool(line + 5);
            if (reply == NULL) {
                /* The tool already printed why it failed. Failed turns
                 * are not recorded, and we certainly do not exit. */
                continue;
            }
        } else {
            /* ---- ask the model ---- */
            reply = mock_model(line);
            if (reply == NULL) {
                printf("[harness] error: out of memory.\n");
                continue;
            }

            printf("%s\n", reply);

            /* ---- did the model ask for a tool? ----
             *
             * This block is the harness doing its actual job. The
             * model produced text; the harness recognises that the
             * text is a request to act, performs the action, and
             * gives the model another turn to speak. The model has
             * no access to the machine -- only the harness does.
             */
            if (strncmp(reply, SENTINEL, strlen(SENTINEL)) == 0) {
                char *tool_output = run_named_tool(reply + strlen(SENTINEL));

                if (tool_output != NULL) {
                    char *final = mock_model_after_tool(tool_output);
                    free(tool_output);      /* the harness owned this */

                    if (final != NULL) {
                        printf("%s\n", final);
                        free(reply);        /* the sentinel is not the answer */
                        reply = final;      /* the final wording is */
                    }
                } else {
                    /* The tool refused. The turn still happened, so we
                     * keep the sentinel in the history as a record of
                     * what the model asked for. */
                    printf("[harness] the tool call could not be completed.\n");
                }
            }
        }

        /* ---- remember this turn ---- */
        user_copy = dup_string(line);
        if (user_copy == NULL) {
            printf("[harness] error: out of memory.\n");
            free(reply);      /* do not leak the reply we just built */
            continue;
        }
        add_turn(user_copy, reply);   /* the history owns them now */
    }

    /* ---- shutdown: give every byte back ---- */
    free_history();
    return 0;
}
