
void assert_true(int condition, const char *msg) {
    if (!condition) {
        fprintf(stderr, "Assertion failed: %s\n", msg);
        exit(1);
    }
}

void assert_equal_int(long long expected, long long actual, const char *msg) {
    if (expected != actual) {
        fprintf(stderr, "Assertion failed: %s. Expected: %lld, Actual: %lld\n",
                msg, expected, actual);
        exit(1);
    }
}

void assert_equal_ptr(void *expected, void *actual, const char *msg) {
    if (expected != actual) {
        fprintf(stderr, "Assertion failed: %s. Expected: %p, Actual: %p\n",
                msg, expected, actual);
        exit(1);
    }
}

void assert_str_equal(const char *expected, const char *actual, const char *msg) {
    if (strcmp(expected, actual) != 0) {
        fprintf(stderr, "Assertion failed: %s. Expected: '%s', Actual: '%s'\n",
                msg, expected, actual);
        exit(1);
    }
}
