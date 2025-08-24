#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include "listpack.h"
#include "test_common.h"

void _serverAssert(const char *filename, int linenum, const char *condition) {
    printf("Assertion failed at %s:%d: %s\n", filename, linenum, condition);
    abort();
}

void test_listpack_new_and_free() {
    printf("Testing listpack new and free...\n");

    size_t capacity = 10;
    unsigned char * lp;
    lp = lpNew(capacity);
    assert_true(lp != NULL, "Listpack should be created successfully");
    lpFree(lp);
    printf("PASSED\n");
}

void test_listpack_insert() {
    printf("Testing listpack insert...\n");

    unsigned char *lp;
    lp = lpNew(0);

    // Insert string
    lp = lpAppend(lp, (unsigned char*)"hello", 5);
    assert_true(lpLength(lp) == 1, "Listpack should have 1 element");

    // Insert integer
    lp = lpAppendInteger(lp, 12345);
    assert_true(lpLength(lp) == 2, "Listpack should have 2 elements");

    // Check first element
    unsigned char *p = lpFirst(lp);
    unsigned int slen;
    long long lval;
    lpGetValue(p, &slen, &lval);
    assert_true(slen == 5, "First element should be a string of length 5");

    // Check second element
    p = lpNext(lp, p);
    lpGetValue(p, &slen, &lval);
    assert_true(slen == 5, "Second element should be an integer");
    assert_true(lval == 12345, "Second element should have value 12345");

    lpFree(lp);
    printf("PASSED\n");
}

void test_listpack_length_and_bytes() {
    printf("Testing listpack length and bytes...\n");

    unsigned char *lp = lpNew(0);

    // Empty listpack
    assert_true(lpLength(lp) == 0, "Empty listpack should have length 0");
    assert_true(lpBytes(lp) > 0, "Empty listpack should have some bytes for header");

    // Add elements
    lp = lpAppend(lp, (unsigned char*)"a", 1);
    assert_true(lpLength(lp) == 1, "Listpack should have 1 element");

    lp = lpAppend(lp, (unsigned char*)"bb", 2);
    assert_true(lpLength(lp) == 2, "Listpack should have 2 elements");

    lp = lpAppendInteger(lp, 42);
    assert_true(lpLength(lp) == 3, "Listpack should have 3 elements");

    lpFree(lp);
    printf("PASSED\n");
}

void test_listpack_seek_and_iterate() {
    printf("Testing listpack seek and iteration...\n");

    unsigned char *lp = lpNew(0);

    // Add several elements
    lp = lpAppend(lp, (unsigned char*)"first", 5);
    lp = lpAppendInteger(lp, 100);
    lp = lpAppend(lp, (unsigned char*)"third", 5);
    lp = lpAppendInteger(lp, 200);

    // Test lpFirst and lpLast
    unsigned char *first = lpFirst(lp);
    unsigned char *last = lpLast(lp);
    assert_true(first != NULL, "First element should exist");
    assert_true(last != NULL, "Last element should exist");

    // Test lpSeek
    unsigned char *second = lpSeek(lp, 1);
    assert_true(second != NULL, "Second element should exist");

    unsigned char *out_of_range = lpSeek(lp, 10);
    assert_true(out_of_range == NULL, "Out of range seek should return NULL");

    // Test forward iteration
    unsigned char *p = first;
    int count = 0;
    while (p != NULL) {
        count++;
        p = lpNext(lp, p);
    }
    assert_true(count == 4, "Should iterate through 4 elements");

    // Test backward iteration
    p = last;
    count = 0;
    while (p != NULL) {
        count++;
        p = lpPrev(lp, p);
    }
    assert_true(count == 4, "Should iterate backwards through 4 elements");

    lpFree(lp);
    printf("PASSED\n");
}

int main(int argc, char *argv[]) {
    printf("Starting listpack tests...\n\n");

    test_listpack_new_and_free();
    test_listpack_insert();
    test_listpack_length_and_bytes();
    test_listpack_seek_and_iterate();

    printf("\nAll listpack tests passed!\n");
    return 0;
}
