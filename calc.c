// Name Surname: Gulnara Akbarli
// Student ID: 251ADB250
// https://github.com/Gulka2008/calc-lab-home
// Compile with: gcc -O2 -Wall -Wextra -std=c17 -o calc calc.c -lm
// Resources used: Ortus, Claude AI promt "i created calc.c, and opened files, now tell me what i need to write codes" with the lab handout and calc-tests.zip attached. https://claude.ai/share/36f80145-223d-4afd-ab3e-9a72067e842a
// Claude wrote this function. I get it as: it reads the file one character at a time, skips spaces and tabs, and remembers where each token starts so errors can show the position.
#define _POSIX_C_SOURCE 200809L
 
#include <dirent.h>
#include <errno.h>
#include <math.h>
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
 
// My details. They go into the output file names: <input>_<name>_<lastname>_<id>.txt

#define STUDENT_NAME "Gulnara"
#define STUDENT_LASTNAME "Akbarli"
#define STUDENT_ID "251ADB250"
 
// so I allow 2000 and report an error after that instead of crashing.
#define MAX_DEPTH 2000
 
// Small helper: is this character a digit 0-9?
#define IS_DIGIT(c) ((c) >= '0' && (c) <= '9')
 
static void *xmalloc(size_t n) {
    void *p = malloc(n ? n : 1);
    if (!p) {
        fprintf(stderr, "calc: out of memory\n");
        exit(1);
    }
    return p;
}

// Same idea for realloc (make a buffer bigger).
static void *xrealloc(void *old, size_t n) {
    void *p = realloc(old, n ? n : 1);
    if (!p) {
        fprintf(stderr, "calc: out of memory\n");
        exit(1);
    }
    return p;
}
 
typedef struct {
    int is_float;
    long long i;
    double d;
} Value;
 
static Value make_int(long long i) {
    Value v = {0, i, 0.0};
    return v;
}
 
static Value make_float(double d) {
    Value v = {1, 0, d};
    return v;
}
 
static double as_double(Value v) { return v.is_float ? v.d : (double)v.i; }
 
// The kinds of tokens. A token is one piece of the line: a number, an operator, a bracket.
// T_EOL means "end of the line" (the \n, or the end of the file). I treat it like a normal token,
// so "missing operand" and "missing )" errors point at the \n without any special code.
// T_BAD is any character the calculator does not know (&, #, a letter, a lonely dot ...).
typedef enum {
    T_INT, T_FLOAT, T_PLUS, T_MINUS, T_STAR, T_SLASH, T_POW, T_LPAREN, T_RPAREN,
    T_EOL,
    T_BAD
} TokKind;
 
typedef struct {
    TokKind kind;
    size_t start;
    long long ival;
    double fval;
} Token;
 
// One operand of a ** chain, used by parse_power(). I also keep where the ** in front of it and the operand itself started, because errors are reported at those places.
typedef struct {
    Value val;
    size_t oppos;
    size_t start;
} PowOperand;
 
typedef struct {
    const char *buf;
    size_t len;
    size_t pos;
    Token cur;
    int depth;
    jmp_buf fail;
    size_t err_pos;
    PowOperand *stack;
    size_t nstack, cap_stack;
    char *out;
    size_t out_len, out_cap;
} Parser;
 
// We read from left to right and stop immediately, so the first error is always the one reported.
static _Noreturn void fail(Parser *p, size_t off) {
    p->err_pos = off + 1;
    longjmp(p->fail, 1);
}
 
static void next_token(Parser *p) {
    const char *b = p->buf;
    size_t n = p->len, i = p->pos;
    Token *t = &p->cur;
 
    while (i < n && (b[i] == ' ' || b[i] == '\t')) i++;
    t->start = i;
 
    if (i >= n || b[i] == '\n') {
        t->kind = T_EOL;
        p->pos = i;
        return;
    }
 
    char c = b[i];
    if (IS_DIGIT(c)) {
        size_t s = i;
        unsigned long long v = 0;
        while (i < n && IS_DIGIT(b[i])) {
            v = v * 10 + (unsigned long long)(b[i] - '0');
            i++;
        }
        if (i + 1 < n && b[i] == '.' && IS_DIGIT(b[i + 1])) {
            i++;
            while (i < n && IS_DIGIT(b[i])) i++;
            size_t tl = i - s;
            char small[64];
            char *tmp = tl < sizeof small ? small : xmalloc(tl + 1);
            memcpy(tmp, b + s, tl);
            tmp[tl] = '\0';
            double d = strtod(tmp, NULL);
            if (tmp != small) free(tmp);
            if (!isfinite(d)) {
                t->kind = T_BAD;
            } else {
                t->kind = T_FLOAT;
                t->fval = d;
            }
        } else {
            t->kind = T_INT;
            t->ival = (long long)v;
        }
        p->pos = i;
        return;
    }
 
    p->pos = i + 1;
    switch (c) {
    case '+': t->kind = T_PLUS; break;
    case '-': t->kind = T_MINUS; break;
    case '/': t->kind = T_SLASH; break;
    case '(': t->kind = T_LPAREN; break;
    case ')': t->kind = T_RPAREN; break;
    // "**" is ONE token. "* *" with a space are two separate stars, so for "2 * * 3" the
    // second star is reported as "two operators in a row".
    case '*':
        if (i + 1 < n && b[i + 1] == '*') {
            t->kind = T_POW;
            p->pos = i + 2;
        } else {
            t->kind = T_STAR;
        }
        break;
    default: t->kind = T_BAD; break;
    }
}
 
static void check_finite(Parser *p, double r, size_t oppos) {
    if (!isfinite(r)) fail(p, oppos);
}
 
static Value apply_add_sub_mul(Parser *p, TokKind op, Value a, Value b, size_t oppos) {
    if (!a.is_float && !b.is_float) {
        unsigned long long x = (unsigned long long)a.i, y = (unsigned long long)b.i, r;
        r = op == T_PLUS ? x + y : op == T_MINUS ? x - y : x * y;
        return make_int((long long)r);
    }
    double x = as_double(a), y = as_double(b), r;
    r = op == T_PLUS ? x + y : op == T_MINUS ? x - y : x * y;
    check_finite(p, r, oppos);
    return make_float(r);
}
 
// For two integers, C's / cuts toward zero, which is what the lab asks for (-7 / 2 = -3).
static Value apply_div(Parser *p, Value a, Value b, size_t oppos, size_t rstart) {
    if (!a.is_float && !b.is_float) {
        if (b.i == 0) fail(p, rstart);
        // Dividing the smallest long long by -1 would crash the CPU, so -1 is handled separately.
        if (b.i == -1) return make_int((long long)(0ULL - (unsigned long long)a.i));
        return make_int(a.i / b.i);
    }
    double y = as_double(b);
    if (y == 0.0) fail(p, rstart);
    double r = as_double(a) / y;
    check_finite(p, r, oppos);
    return make_float(r);
}
 

static Value apply_pow(Parser *p, Value a, Value b, size_t oppos, size_t rstart) {
    if (!a.is_float && !b.is_float) {
        if (b.i < 0) fail(p, rstart);
        unsigned long long result = 1, base = (unsigned long long)a.i, e = (unsigned long long)b.i;
        while (e) {
            if (e & 1) result *= base;
            base *= base;
            e >>= 1;
        }
        return make_int((long long)result);
    }
    double r = pow(as_double(a), as_double(b));
    check_finite(p, r, oppos);
    return make_float(r);
}
 
static Value parse_expr(Parser *p);
 
// The smallest piece: a number, or an expression inside brackets.
// There is no case for '-', which is why unary minus ("-5") is an error.
static Value parse_primary(Parser *p) {
    Token t = p->cur;
    if (t.kind == T_INT) {
        next_token(p);
        return make_int(t.ival);
    }
    if (t.kind == T_FLOAT) {
        next_token(p);
        return make_float(t.fval);
    }
    if (t.kind == T_LPAREN) {
        if (++p->depth > MAX_DEPTH) fail(p, t.start);
        next_token(p);
        Value v = parse_expr(p);
        if (p->cur.kind != T_RPAREN) fail(p, p->cur.start);
        next_token(p);
        p->depth--;
        return v;
    }
    fail(p, t.start);
}
 
// Combining happens only after the whole chain was read, so a syntax error later in the chain
// is found before an arithmetic error earlier in it, same as with the recursive version.
static Value parse_power(Parser *p) {
    size_t base = p->nstack;
 
    for (int first = 1;; first = 0) {
        size_t oppos = 0, start;
        if (!first) {
            oppos = p->cur.start;
            next_token(p);
        }
        start = p->cur.start;
        Value v = parse_primary(p);
        if (p->nstack == p->cap_stack) {
            p->cap_stack = p->cap_stack ? p->cap_stack * 2 : 64;
            p->stack = xrealloc(p->stack, p->cap_stack * sizeof *p->stack);
        }
        PowOperand op = {v, oppos, start};
        p->stack[p->nstack++] = op;
        if (p->cur.kind != T_POW) break;
    }
 
    while (p->nstack > base + 1) {
        PowOperand r = p->stack[--p->nstack];
        PowOperand *l = &p->stack[p->nstack - 1];
        l->val = apply_pow(p, l->val, r.val, r.oppos, r.start);
    }
    p->nstack = base;
    return p->stack[base].val;
}
 
static Value parse_term(Parser *p) {
    Value v = parse_power(p);
    while (p->cur.kind == T_STAR || p->cur.kind == T_SLASH) {
        TokKind op = p->cur.kind;
        size_t oppos = p->cur.start;
        next_token(p);
        size_t rstart = p->cur.start;
        Value r = parse_power(p);
        v = op == T_STAR ? apply_add_sub_mul(p, op, v, r, oppos) : apply_div(p, v, r, oppos, rstart);
    }
    return v;
}
 
static Value parse_expr(Parser *p) {
    Value v = parse_term(p);
    while (p->cur.kind == T_PLUS || p->cur.kind == T_MINUS) {
        TokKind op = p->cur.kind;
        size_t oppos = p->cur.start;
        next_token(p);
        Value r = parse_term(p);
        v = apply_add_sub_mul(p, op, v, r, oppos);
    }
    return v;
}
 
static void out_append(Parser *p, const char *s, size_t n) {
    if (p->out_len + n > p->out_cap) {
        size_t cap = p->out_cap ? p->out_cap : 4096;
        while (cap < p->out_len + n) cap *= 2;
        p->out = xrealloc(p->out, cap);
        p->out_cap = cap;
    }
    memcpy(p->out + p->out_len, s, n);
    p->out_len += n;
}
 
static void eval_lines(Parser *p) {
    const char *b = p->buf;
    size_t n = p->len, i = 0;
 
    while (i < n) {
        size_t j = i;
        while (j < n && (b[j] == ' ' || b[j] == '\t')) j++;
        if (j >= n) break;
        if (b[j] == '\n') {
            i = j + 1;
            continue;
        }
        if (b[j] == '#') {
            const char *nl = memchr(b + j, '\n', n - j);
            i = nl ? (size_t)(nl - b) + 1 : n;
            continue;
        }
 
        p->pos = i;
        p->nstack = 0;
        p->depth = 0;
        next_token(p);
        Value v = parse_expr(p);
        if (p->cur.kind != T_EOL) fail(p, p->cur.start);
 
        char tmp[64];
        int len = v.is_float ? snprintf(tmp, sizeof tmp, "%.15g\n", v.d)
                             : snprintf(tmp, sizeof tmp, "%lld\n", v.i);
        out_append(p, tmp, (size_t)len);
 
        i = p->cur.start + 1;
    }
}
 
static size_t run_file(Parser *p) {
    if (setjmp(p->fail)) return p->err_pos;
    eval_lines(p);
    return 0;
}
 
static char *read_file(const char *path, size_t *out_len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
 
    size_t cap = 65536, len = 0;
    struct stat st;
    if (stat(path, &st) == 0 && S_ISREG(st.st_mode)) cap = (size_t)st.st_size + 1;
    char *buf = xmalloc(cap);
 
    for (;;) {
        if (len == cap) {
            cap *= 2;
            buf = xrealloc(buf, cap);
        }
        size_t got = fread(buf + len, 1, cap - len, f);
        len += got;
        if (got == 0) break;
    }
    int bad = ferror(f);
    int saved = errno;
    fclose(f);
    if (bad) {
        free(buf);
        errno = saved ? saved : EIO;
        return NULL;
    }
    *out_len = len;
    return buf;
}
 
static char *make_base(const char *path, int strip_txt) {
    size_t n = strlen(path);
    while (n > 1 && path[n - 1] == '/') n--;
    size_t s = n;
    while (s > 0 && path[s - 1] != '/') s--;
    size_t m = n - s;
    if (strip_txt && m >= 4 && memcmp(path + s + m - 4, ".txt", 4) == 0) m -= 4;
    char *r = xmalloc(m + 1);
    memcpy(r, path + s, m);
    r[m] = '\0';
    return r;
}
 
static char *join_path(const char *dir, const char *file) {
    size_t n = strlen(dir) + strlen(file) + 2;
    char *r = xmalloc(n);
    snprintf(r, n, "%s/%s", dir, file);
    return r;
}
 
static int ensure_dir(const char *path) {
    struct stat st;
    if (stat(path, &st) == 0) {
        if (S_ISDIR(st.st_mode)) return 0;
        fprintf(stderr, "calc: '%s' exists and is not a folder\n", path);
        return -1;
    }
    if (mkdir(path, 0777) != 0 && errno != EEXIST) {
        fprintf(stderr, "calc: cannot create folder '%s': %s\n", path, strerror(errno));
        return -1;
    }
    return 0;
}
 
// Does everything for one input file: read, evaluate, write <outdir>/<base>_<name>_<lastname>_<id>.txt.
// I read the input BEFORE creating the output folder, so a missing input file doesn't leave an
static int process_file(const char *input, const char *outdir, int outdir_ready) {
    size_t len = 0;
    char *buf = read_file(input, &len);
    if (!buf) {
        fprintf(stderr, "calc: cannot read '%s': %s\n", input, strerror(errno));
        return 1;
    }
 
    Parser p;
    memset(&p, 0, sizeof p);
    p.buf = buf;
    p.len = len;
    size_t err = run_file(&p);
 
    int rc = 0;
    if (!outdir_ready && ensure_dir(outdir) != 0) rc = 1;
 
    if (rc == 0) {
        char *base = make_base(input, 1);
        size_t fn = strlen(base) + 64 + strlen(STUDENT_NAME STUDENT_LASTNAME STUDENT_ID);
        char *fname = xmalloc(fn);
        snprintf(fname, fn, "%s_%s_%s_%s.txt", base, STUDENT_NAME, STUDENT_LASTNAME, STUDENT_ID);
        char *path = join_path(outdir, fname);
 
        FILE *f = fopen(path, "wb");
        if (!f) {
            fprintf(stderr, "calc: cannot write '%s': %s\n", path, strerror(errno));
            rc = 1;
        } else {
            int ok;
            // An "ERROR:<pos>" result is a normal result, not a program failure, so the exit code stays 0.
            if (err) {
                ok = fprintf(f, "ERROR:%zu\n", err) > 0;
            } else {
                ok = fwrite(p.out, 1, p.out_len, f) == p.out_len;
            }
            if (fclose(f) != 0) ok = 0;
            if (!ok) {
                fprintf(stderr, "calc: error writing '%s'\n", path);
                rc = 1;
            }
        }
        free(path);
        free(fname);
        free(base);
    }
 
    free(p.out);
    free(p.stack);
    free(buf);
    return rc;
}
 
static int cmp_names(const void *a, const void *b) {
    return strcmp(*(char *const *)a, *(char *const *)b);
}
 
// -d mode: take every *.txt file directly inside the folder. Other files and sub-folders are
// ignored. The names are sorted so the order is always the same.
static int process_dir(const char *dir, const char *outdir) {
    DIR *d = opendir(dir);
    if (!d) {
        fprintf(stderr, "calc: cannot open folder '%s': %s\n", dir, strerror(errno));
        return 1;
    }
    char **names = NULL;
    size_t count = 0, cap = 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        size_t l = strlen(e->d_name);
        if (l <= 4 || strcmp(e->d_name + l - 4, ".txt") != 0) continue;
        char *path = join_path(dir, e->d_name);
        struct stat st;
        int regular = stat(path, &st) == 0 && S_ISREG(st.st_mode);
        free(path);
        // A folder can also be called something.txt; stat tells us if it is a real file.
        if (!regular) continue;
        if (count == cap) {
            cap = cap ? cap * 2 : 16;
            names = xrealloc(names, cap * sizeof *names);
        }
        names[count] = xmalloc(l + 1);
        memcpy(names[count], e->d_name, l + 1);
        count++;
    }
    closedir(d);
    if (count > 1) qsort(names, count, sizeof *names, cmp_names);
 
    int rc = 0;
    if (ensure_dir(outdir) != 0) {
        rc = 1;
    } else {
        for (size_t k = 0; k < count; k++) {
            char *path = join_path(dir, names[k]);
            if (process_file(path, outdir, 1) != 0) rc = 1;
            free(path);
        }
    }
    for (size_t k = 0; k < count; k++) free(names[k]);
    free(names);
    return rc;
}
 
static void usage(void) {
    fprintf(stderr,
            "usage: calc [-o OUTDIR | --output-dir OUTDIR] input.txt\n"
            "       calc [-d DIR | --dir DIR] [-o OUTDIR | --output-dir OUTDIR]\n");
}
 
// Returns NULL (with a message) if the value is missing.
static const char *option_value(int argc, char **argv, int *i, const char *longname) {
    const char *a = argv[*i];
    size_t ln = strlen(longname);
    if (strncmp(a, longname, ln) == 0 && a[ln] == '=') return a + ln + 1;
    if (*i + 1 >= argc) {
        fprintf(stderr, "calc: option '%s' needs a value\n", a);
        usage();
        return NULL;
    }
    return argv[++*i];
}
 
// Exit codes: 0 = all fine (even if a file has an ERROR result), 1 = a file or folder problem,
// 2 = wrong command line.
int main(int argc, char **argv) {
    const char *dir = NULL, *outdir = NULL, *input = NULL;
 
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (strcmp(a, "-d") == 0 || strncmp(a, "--dir", 5) == 0) {
            if (strcmp(a, "-d") != 0 && strcmp(a, "--dir") != 0 && strncmp(a, "--dir=", 6) != 0) goto unknown;
            if (!(dir = option_value(argc, argv, &i, "--dir"))) return 2;
        } else if (strcmp(a, "-o") == 0 || strncmp(a, "--output-dir", 12) == 0) {
            if (strcmp(a, "-o") != 0 && strcmp(a, "--output-dir") != 0 && strncmp(a, "--output-dir=", 13) != 0) goto unknown;
            if (!(outdir = option_value(argc, argv, &i, "--output-dir"))) return 2;
        } else if (a[0] == '-') {
        unknown:
            fprintf(stderr, "calc: unknown option '%s'\n", a);
            usage();
            return 2;
        } else if (input) {
            fprintf(stderr, "calc: more than one input file given\n");
            usage();
            return 2;
        } else {
            input = a;
        }
    }
 
    if (!dir && !input) {
        usage();
        return 2;
    }
    if (dir && input) {
        fprintf(stderr, "calc: give either an input file or -d DIR, not both\n");
        usage();
        return 2;
    }
 
    // Default output folder when there is no -o: <base>_<name>_<id> in the current folder, where base is the file name without .txt, or (with -d) the name of the folder.
    char *base = make_base(dir ? dir : input, dir == NULL);
    char *default_out = NULL;
    if (!outdir) {
        size_t n = strlen(base) + strlen(STUDENT_NAME) + strlen(STUDENT_ID) + 3;
        default_out = xmalloc(n);
        snprintf(default_out, n, "%s_%s_%s", base, STUDENT_NAME, STUDENT_ID);
        outdir = default_out;
    }
 
    int rc = dir ? process_dir(dir, outdir) : process_file(input, outdir, 0);
 
    free(default_out);
    free(base);
    return rc ? 1 : 0;
}
 