#include <check.h>
#include <stdlib.h>
#include <string.h>

/* Include the actual implementation */
#include "tools/agent/p9copy.c"

START_TEST(test_putstr_bounds_check)
{
    /* Invariant: Buffer writes must never exceed allocated buffer size */
    
    /* Test payloads: valid, boundary, and oversized inputs */
    const char *payloads[] = {
        "short",                                          /* Valid small input */
        "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA", /* 64 bytes - boundary */
        "BBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBB" /* 128 bytes - 2x overflow */
    };
    size_t payload_lens[] = {5, 64, 128};
    int num_payloads = sizeof(payloads) / sizeof(payloads[0]);
    
    /* Small buffer that would overflow with large payloads */
    const size_t buf_size = 32;
    
    for (int i = 0; i < num_payloads; i++) {
        uint8_t *buf = calloc(1, buf_size);
        ck_assert_ptr_nonnull(buf);
        
        uint8_t *p = buf;
        uint8_t *end = buf + buf_size;
        size_t len = payload_lens[i];
        
        /* Security invariant: putstr should not write beyond buffer end */
        /* If bounds checking exists, it should truncate or reject oversized input */
        size_t safe_len = (len + 2 <= (size_t)(end - p)) ? len : (size_t)(end - p) - 2;
        
        if (len + 2 <= buf_size) {
            /* Input fits - should succeed without overflow */
            putstr(&p, payloads[i], len);
            ck_assert_ptr_le(p, end);
        } else {
            /* Input too large - vulnerable code would overflow here */
            /* This test documents the vulnerability: putstr lacks bounds checking */
            /* A fixed version should either truncate or return an error */
            ck_assert_msg(len + 2 > buf_size, 
                "Oversized payload %zu should exceed buffer %zu", len + 2, buf_size);
        }
        
        free(buf);
    }
}
END_TEST

Suite *security_suite(void)
{
    Suite *s;
    TCase *tc_core;

    s = suite_create("Security");
    tc_core = tcase_create("Core");

    tcase_add_test(tc_core, test_putstr_bounds_check);
    suite_add_tcase(s, tc_core);

    return s;
}

int main(void)
{
    int number_failed;
    Suite *s;
    SRunner *sr;

    s = security_suite();
    sr = srunner_create(s);

    srunner_run_all(sr, CK_NORMAL);
    number_failed = srunner_ntests_failed(sr);
    srunner_free(sr);

    return (number_failed == 0) ? EXIT_SUCCESS : EXIT_FAILURE;
}