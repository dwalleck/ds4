/* Model-independent checks for the Windows POSIX regex/fnmatch facades.
 * Link the ten TRE C sources listed in third_party/tre/NOTICE and
 * compat/win32/fnmatch.c. No model, shell, or filesystem fixture is needed. */
#include <fnmatch.h>
#include <regex.h>
#include <locale.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(expression) do { \
    if (!(expression)) { \
        fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #expression); \
        abort(); \
    } \
} while (0)

static void expect_regex(const char *pattern, const char *text,
                         int flags, int expected) {
    regex_t regex;
    int rc = regcomp(&regex, pattern, REG_EXTENDED | REG_NOSUB | flags);
    if (rc != 0) {
        char message[256];
        regerror(rc, &regex, message, sizeof(message));
        fprintf(stderr, "regcomp(%s): %s\n", pattern, message);
        abort();
    }
    CHECK(regexec(&regex, text, 0, NULL, 0) == expected);
    regfree(&regex);
}

static void test_search_expressions(void) {
    /* Search is unanchored, but anchors, alternatives, and interval bounds
     * are genuine POSIX ERE operators rather than wildcard approximations. */
    expect_regex("(error|warning):[[:space:]]+[[:digit:]]{2,4}",
                 "prefix warning: 123 suffix", 0, 0);
    expect_regex("^(error|warning):[[:space:]]+[[:digit:]]{2,4}$",
                 "prefix warning: 123 suffix", 0, REG_NOMATCH);
    expect_regex("^(error|warning):[[:space:]]+[[:digit:]]{2,4}$",
                 "error: 12", 0, 0);
    expect_regex("^[[:alpha:]_][[:alnum:]_]*$", "ds4_worker_9", 0, 0);
    expect_regex("^[[:alpha:]_][[:alnum:]_]*$", "9_worker", 0, REG_NOMATCH);
    expect_regex("^colou?r(s|ed)?$", "colored", 0, 0);
    expect_regex("^colou?r(s|ed)?$", "colourss", 0, REG_NOMATCH);
    expect_regex("^a{2,3}b+$", "aaabbb", 0, 0);
    expect_regex("^a{2,3}b+$", "aaaab", 0, REG_NOMATCH);
    expect_regex("^file\\.c$", "file.c", 0, 0);
    expect_regex("^file\\.c$", "filexc", 0, REG_NOMATCH);
    expect_regex("^[^[:digit:]]+$", "alpha_-", 0, 0);
    expect_regex("^[^[:digit:]]+$", "alpha4", 0, REG_NOMATCH);
    expect_regex("^[]-]+$", "]--]", 0, 0);
    expect_regex("^(foo|bar)$", "FOO", 0, REG_NOMATCH);
    expect_regex("^(foo|bar)$", "FOO", REG_ICASE, 0);
    expect_regex("^[a-z]+$", "MiXeD", REG_ICASE, 0);
    expect_regex("^[[:lower:]]+$", "MiXeD", REG_ICASE, 0);
    expect_regex("^$", "", 0, 0);
    expect_regex("a*", "", 0, 0);
    expect_regex("a.b", "a\nb", 0, 0);
    expect_regex("a.b", "a\nb", REG_NEWLINE, REG_NOMATCH);
    expect_regex("^target$", "before\ntarget\nafter", REG_NEWLINE, 0);
}

static void test_offsets_and_execution_flags(void) {
    regex_t regex;
    regmatch_t matches[3];
    CHECK(regcomp(&regex, "(a|aa)(b?)", REG_EXTENDED) == 0);
    CHECK(regexec(&regex, "zaab!", 3, matches, 0) == 0);
    CHECK(matches[0].rm_so == 1 && matches[0].rm_eo == 4);
    CHECK(matches[1].rm_so == 1 && matches[1].rm_eo == 3);
    CHECK(matches[2].rm_so == 3 && matches[2].rm_eo == 4);
    regfree(&regex);

    /* POSIX chooses the longest alternative, not the first alternative. */
    CHECK(regcomp(&regex, "a|aa", REG_EXTENDED) == 0);
    CHECK(regexec(&regex, "aa", 1, matches, 0) == 0);
    CHECK(matches[0].rm_so == 0 && matches[0].rm_eo == 2);
    regfree(&regex);

    CHECK(regcomp(&regex, "^word$", REG_EXTENDED) == 0);
    CHECK(regexec(&regex, "word", 0, NULL, 0) == 0);
    CHECK(regexec(&regex, "word", 0, NULL, REG_NOTBOL) == REG_NOMATCH);
    CHECK(regexec(&regex, "word", 0, NULL, REG_NOTEOL) == REG_NOMATCH);
    regfree(&regex);

}

static void test_invalid_expressions(void) {
    static const char *const invalid[] = {
        "[abc", "(abc", "abc\\", "[[:notaclass:]]", "a{3,2}", "[z-a]"
    };
    for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++) {
        regex_t regex = {0};
        int rc = regcomp(&regex, invalid[i], REG_EXTENDED | REG_NOSUB);
        CHECK(rc != 0);
        char message[256];
        size_t needed = regerror(rc, &regex, message, sizeof(message));
        CHECK(needed > 1 && needed == strlen(message) + 1);
        CHECK(regerror(rc, &regex, NULL, 0) == needed);
        char tiny[2] = {'x', 'x'};
        CHECK(regerror(rc, &regex, tiny, sizeof(tiny)) == needed);
        CHECK(tiny[1] == '\0');
        char one = 'x';
        CHECK(regerror(rc, &regex, &one, 1) == needed);
        CHECK(one == '\0');
        /* POSIX does not require regfree after a failed regcomp. */
    }
    /* Exercise successful compile/execute/free after repeated parser failures. */
    for (int i = 0; i < 128; i++)
        expect_regex("^(file|dir)[[:digit:]]+$", "file42", 0, 0);
}

static void test_wildcard_boundaries(void) {
    static const struct {
        const char *pattern;
        const char *text;
        int expected;
    } cases[] = {
        {"", "", 0},
        {"", "x", FNM_NOMATCH},
        {"*", "", 0},
        {"?", "", FNM_NOMATCH},
        {"?", "x", 0},
        {"?", "xy", FNM_NOMATCH},
        {"*.c", "ds4_agent.c", 0},
        {"*.c", "src/ds4_agent.c", 0},
        {"*.c", "src\\ds4_agent.c", 0},
        {"*.c", "ds4_agent.cpp", FNM_NOMATCH},
        {"*.C", "ds4_agent.c", FNM_NOMATCH},
        {"*", ".git", 0},
        {"*", "a/b", 0},
        {"a?b", "a/b", 0},
        {"src/*.c", "src/deep/file.c", 0},
        {"src/*.c", "other/file.c", FNM_NOMATCH},
        {"a*b*c", "abc", 0},
        {"a*b*c", "a123b456c", 0},
        {"a*b*c", "a123c456b", FNM_NOMATCH},
        {"*ab*ab", "abab", 0},
        {"*ab*ab", "ab", FNM_NOMATCH},
        {"**a***b**", "a___b", 0},
        {"*?x", "x", FNM_NOMATCH},
        {"*?x", "ax", 0},
        {"file[0-9].c", "file7.c", 0},
        {"file[0-9].c", "filea.c", FNM_NOMATCH},
        {"[!a-c]", "d", 0},
        {"[!a-c]", "b", FNM_NOMATCH},
        {"[[:digit:]][[:alpha:]]", "4Z", 0},
        {"[[:digit:]][[:alpha:]]", "Z4", FNM_NOMATCH},
        {"[]]", "]", 0},
        {"[-a]", "-", 0},
        {"[a-]", "-", 0},
        {"[a-cx]", "-", FNM_NOMATCH},
        {"[a-cx]", "b", 0},
        {"[a-cx]", "x", 0},
        {"[", "[", 0},
        {"[abc", "[abc", 0},
        {"literal\\*", "literal*", 0},
        {"literal\\?", "literal?", 0},
        {"literal\\[", "literal[", 0},
        {"dir\\*.c", "dir*.c", 0},
        {"dir\\*.c", "dir/file.c", FNM_NOMATCH},
        {"*suffix", "suffixextra", FNM_NOMATCH},
        {"*suffix", "prefixsuffix", 0}
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        int rc = fnmatch(cases[i].pattern, cases[i].text, 0);
        if (rc != cases[i].expected) {
            fprintf(stderr, "fnmatch(%s, %s, 0): %d, expected %d\n",
                    cases[i].pattern, cases[i].text, rc, cases[i].expected);
            abort();
        }
    }
    CHECK(fnmatch("*.c", "dir/file.c", FNM_PATHNAME) == FNM_NOMATCH);
    CHECK(fnmatch("*", ".git", FNM_PERIOD) == FNM_NOMATCH);
    CHECK(fnmatch("*.C", "file.c", FNM_CASEFOLD) == 0);
    CHECK(fnmatch("a\\*", "a\\file", FNM_NOESCAPE) == 0);
}

static void test_multibyte_locale(void) {
    /* Windows UCRT's explicit UTF-8 locale exercises the non-single-byte path
     * independently of the machine's ANSI code page. Use BMP characters,
     * representable by Windows wchar_t. */
    CHECK(setlocale(LC_ALL, ".UTF8") != NULL);
    const char *e_acute = "\xc3\xa9";
    const char *a_umlaut = "\xc3\xa4";
    expect_regex("^.$", e_acute, 0, 0);
    expect_regex("^\xc3\x84$", a_umlaut, REG_ICASE, 0);
    CHECK(fnmatch("?", e_acute, 0) == 0);
    CHECK(fnmatch("??", e_acute, 0) == FNM_NOMATCH);
    CHECK(fnmatch("*?z", "prefix\xc3\xa9z", 0) == 0);
    CHECK(fnmatch("*\xc3\xa9", "prefix\xc3\xa9", 0) == 0);
    CHECK(fnmatch("*\xc3\xa9", "prefix\xc3\xa9x", 0) == FNM_NOMATCH);
    CHECK(fnmatch("[\xc3\xa0-\xc3\xbf]", e_acute, 0) == 0);
    CHECK(fnmatch("\xc3\x84", a_umlaut, FNM_CASEFOLD) == 0);
    CHECK(setlocale(LC_ALL, "C") != NULL);
}

int main(void) {
    CHECK(setlocale(LC_ALL, "C") != NULL);
    test_search_expressions();
    test_offsets_and_execution_flags();
    test_invalid_expressions();
    test_wildcard_boundaries();
    test_multibyte_locale();
    puts("Windows pattern compatibility tests passed");
    return 0;
}
