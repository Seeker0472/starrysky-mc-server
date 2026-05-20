#include <stdio.h>

int test_ringbuf(void);
int test_varint(void);
int test_packet(void);
int test_link_codec(void);
int test_link_session(void);
int test_server_status(void);
int test_server_login(void);
int test_world(void);
int test_integration(void);
int test_firmware_config(void);
int test_mc_log_info(void);
int test_mc_log_off(void);
int test_mc_log_debug(void);
int test_mc_log_trace(void);
int test_server_trace(void);
int test_firmware_main(void);
int test_platform_uart0(void);
int test_platform_psram(void);

static int run_test(const char *name, int (*fn)(void))
{
    int rc = fn();
    if (rc != 0) {
        fprintf(stderr, "FAIL %s\n", name);
        return 1;
    }
    printf("PASS %s\n", name);
    return 0;
}

int main(void)
{
    int failed = 0;
    failed |= run_test("ringbuf", test_ringbuf);
    failed |= run_test("varint", test_varint);
    failed |= run_test("packet", test_packet);
    failed |= run_test("link_codec", test_link_codec);
    failed |= run_test("link_session", test_link_session);
    failed |= run_test("server_status", test_server_status);
    failed |= run_test("server_login", test_server_login);
    failed |= run_test("world", test_world);
    failed |= run_test("integration", test_integration);
    failed |= run_test("firmware_config", test_firmware_config);
    failed |= run_test("mc_log_info", test_mc_log_info);
    failed |= run_test("mc_log_off", test_mc_log_off);
    failed |= run_test("mc_log_debug", test_mc_log_debug);
    failed |= run_test("mc_log_trace", test_mc_log_trace);
    failed |= run_test("server_trace", test_server_trace);
    failed |= run_test("firmware_main", test_firmware_main);
    failed |= run_test("platform_uart0", test_platform_uart0);
    failed |= run_test("platform_psram", test_platform_psram);
    return failed ? 1 : 0;
}
