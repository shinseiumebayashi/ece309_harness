/* ============================================================
 * harness.c  --  ECE 309 Project 1: a minimal LLM agent harness
 *
 * Build:  gcc -Wall -Wextra harness.c -o harness
 * Run:    ./harness
 *
 * The harness sits between the user and a (mock) model.  Every line
 * the user types is either sent to the mock model or intercepted and
 * handed to a tool.  A short conversation history is kept on the heap.
 * ============================================================ */

#include <stdio.h>   /* printf, fgets, fputs, sscanf */
#include <stdlib.h>  /* malloc, free */
#include <string.h>  /* strlen, strcmp, strncmp, strstr, strcpy */

/* ---------- tunable limits ---------- */
#define MAX_LINE 256  /* buffer size for one input line, incl. '\0' */
#define MAX_TURNS 5   /* how many turns the history remembers        */

/* ============================================================
 * Conversation history
 *
 * One "turn" is a pair: what the user said, and what came back.
 * The history is a ring buffer: once it holds MAX_TURNS turns, the
 * next turn overwrites the oldest one (after freeing it).
 *
 * The buffer is file-scope (a global) so that mock_model() can read
 * it without changing the function signature required by the spec.
 * ============================================================ */
struct Turn {
    char *user;  /* heap copy of the user's line  */
    char *reply; /* heap copy of the reply        */
};

static struct Turn history[MAX_TURNS]; /* zero-initialised: all NULL */
static int history_count = 0;          /* how many slots are in use  */
static int history_next = 0;           /* slot the next turn goes in */

/* ------------------------------------------------------------
 * dup_string: make a heap copy of a string.
 * (strdup is not in the C standard before C23, so we write our own
 *  to stay strictly standard.)
 * Returns NULL if the allocation fails.
 * ------------------------------------------------------------ */
static char *dup_string(const char *src)
{
    size_t n = strlen(src) + 1;   /* +1 for the '\0' terminator */
    char *copy = malloc(n);       /* ask the heap for n bytes   */
    if (copy == NULL) {           /* malloc can fail            */
        return NULL;
    }
    memcpy(copy, src, n);         /* copy the bytes, terminator included */
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
     * This is what keeps the memory usage bounded at MAX_TURNS. */
    free(history[history_next].user);
    free(history[history_next].reply);

    history[history_next].user = user;
    history[history_next].reply = reply;

    /* Advance the write position, wrapping around at the end. */
    history_next = (history_next + 1) % MAX_TURNS;

    /* Grow the count until the buffer is full, then stop. */
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
        /* When the buffer is full, the oldest entry sits at
         * history_next; when it is not yet full, it sits at 0.
         * The modulo arithmetic below handles both cases. */
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
 * run_tool: evaluate "<number> <op> <number>".
 * Returns a heap string that the caller owns, or NULL on failure
 * (a parse error or a division by zero).  Never crashes.
 * ------------------------------------------------------------ */
char *run_tool(const char *expr)
{
    double a = 0.0, b = 0.0, result = 0.0;
    char op = '?';
    char extra = '\0';
    char out[MAX_LINE];
    int matched;

    /* Read: optional spaces, a number, an operator, a number.
     * The trailing " %c" is a trap: if it matches, there was junk
     * after the expression, so we reject the whole thing. */
    matched = sscanf(expr, " %lf %c %lf %c", &a, &op, &b, &extra);

    if (matched != 3) {
        printf("[tool] error: could not parse '%s'"
               " (expected: <number> <op> <number>)\n", expr);
        return NULL;
    }

    switch (op) {
        case '+': result = a + b; break;
        case '-': result = a - b; break;
        case '*': result = a * b; break;
        case '/':
            if (b == 0.0) {
                printf("[tool] error: division by zero\n");
                return NULL;
            }
            result = a / b;
            break;
        default:
            printf("[tool] error: unknown operator '%c'"
                   " (use + - * /)\n", op);
            return NULL;
    }

    /* %g prints 7 rather than 7.000000, which reads better. */
    snprintf(out, sizeof(out), "[tool] result = %g", result);
    printf("%s\n", out);
    return dup_string(out);
}

/* ============================================================
 * The mock model
 * ============================================================ */

/* ------------------------------------------------------------
 * contains_word: case-sensitive substring test, kept as a named
 * helper so the intent is obvious at the call site.
 * ------------------------------------------------------------ */
static int contains_word(const char *haystack, const char *needle)
{
    return strstr(haystack, needle) != NULL;
}

/* ------------------------------------------------------------
 * mock_model: stands in for a real LLM.  No network, no API key --
 * it just maps an input to a canned reply.
 * Returns a heap string that the caller owns, or NULL on allocation
 * failure.
 * ------------------------------------------------------------ */
char *mock_model(const char *input)
{
    char out[MAX_LINE + 32];   /* room for the input plus a prefix */

    /* "history" is handled here so that the conversation state is
     * observable from a test script without human interaction. */
    if (strcmp(input, "history") == 0) {
        print_history();
        return dup_string("[model] (printed the history above)");
    }

    if (contains_word(input, "hello") || contains_word(input, "hi")) {
        return dup_string("[model] Hello! I am a mock model. "
                          "Try 'calc: 3 + 4', 'history', or 'exit'.");
    }

    snprintf(out, sizeof(out), "[model] I received: %s", input);
    return dup_string(out);
}

/* ============================================================
 * main -- the core loop
 * ============================================================ */
int main(void)
{
    char line[MAX_LINE];

    printf("ECE 309 mini-harness. Type 'exit' to quit.\n");

    for (;;) {
        char *newline;
        char *reply;
        char *user_copy;

        printf("> ");
        fflush(stdout);   /* make sure the prompt appears before we block */

        /* ---- read one line ---- */
        if (fgets(line, sizeof(line), stdin) == NULL) {
            /* NULL means end of input (Ctrl+D, or a pipe running dry).
             * Treat it exactly like "exit" so the shutdown path is
             * the same one the tests exercise. */
            printf("\n[harness] end of input, shutting down.\n");
            break;
        }

        /* ---- strip the newline, or handle an over-long line ---- */
        newline = strchr(line, '\n');
        if (newline != NULL) {
            *newline = '\0';          /* normal case: cut the '\n' off */
        } else {
            /* No newline in the buffer means the user typed more than
             * we can hold.  Keep what we have and throw away the rest
             * so the leftovers are not read as a second command. */
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

        /* ---- dispatch: tool or model? ---- */
        if (strncmp(line, "calc:", 5) == 0) {
            /* Everything after "calc:" is the expression. */
            reply = run_tool(line + 5);
            if (reply == NULL) {
                /* The tool already printed why it failed.  We do not
                 * record failed turns, and we certainly do not exit. */
                continue;
            }
        } else {
            reply = mock_model(line);
            if (reply == NULL) {
                printf("[harness] error: out of memory.\n");
                continue;
            }
            printf("%s\n", reply);
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