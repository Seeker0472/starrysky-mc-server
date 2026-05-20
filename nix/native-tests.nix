{
  stdenv,
  python3,
  src,
}:

stdenv.mkDerivation {
  pname = "mc-uart-native-tests";
  version = "0.1.0";

  inherit src;

  dontConfigure = true;

  nativeBuildInputs = [
    python3
  ];

  buildPhase = ''
    runHook preBuild
    python3 scripts/generate_compressed_chunks.py --out-dir core/generated --check
    mkdir -p build/native
    cc -std=c11 -Wall -Wextra -Werror -O2 \
      -Icore/include \
      -Ifirmware \
      native/test_main.c \
      native/tests/test_ringbuf.c \
      native/tests/test_varint.c \
      native/tests/test_packet.c \
      native/tests/test_link_codec.c \
      native/tests/test_link_session.c \
      native/tests/test_server_status.c \
      native/tests/test_server_login.c \
      native/tests/test_world.c \
      native/tests/test_integration.c \
      native/tests/test_firmware_config.c \
      native/tests/test_log_capture.c \
      native/tests/test_mc_log_info.c \
      native/tests/test_mc_log_off.c \
      native/tests/test_mc_log_debug.c \
      native/tests/test_mc_log_trace.c \
      native/tests/test_server_trace.c \
      native/tests/test_platform_uart0.c \
      native/tests/test_platform_psram.c \
      core/src/mc_ringbuf.c \
      core/src/mc_varint.c \
      core/src/mc_packet.c \
      core/src/mc_link.c \
      firmware/mc_link_session.c \
      core/src/mc_world.c \
      core/src/mc_world_compressed.c \
      core/src/mc_server.c \
      firmware/mc_log.c \
      firmware/platform_uart0.c \
      firmware/platform_psram.c \
      -o build/native/mc_uart_tests
    ./build/native/mc_uart_tests

    cat > build/native/protocol_compression_test_main.c <<'EOF'
    #include <stdio.h>

    #define main mc_uart_tests_all_main
    #include "../../native/test_main.c"
    #undef main

    int test_packet(void);
    int test_server_login(void);

    int main(void)
    {
      if (test_packet() != 0) {
        fprintf(stderr, "FAIL packet_protocol_compression\n");
        return 1;
      }
      printf("PASS packet_protocol_compression\n");

      if (test_server_login() != 0) {
        fprintf(stderr, "FAIL server_login_protocol_compression\n");
        return 1;
      }
      printf("PASS server_login_protocol_compression\n");
      return 0;
    }
EOF

    cc -std=c11 -Wall -Wextra -Werror -O2 \
      -DMC_PROTOCOL_COMPRESSION_ENABLE=1 \
      -Icore/include -Ifirmware \
      build/native/protocol_compression_test_main.c \
      native/tests/test_ringbuf.c native/tests/test_varint.c native/tests/test_packet.c \
      native/tests/test_link_codec.c native/tests/test_link_session.c native/tests/test_server_status.c \
      native/tests/test_server_login.c native/tests/test_world.c native/tests/test_integration.c \
      native/tests/test_firmware_config.c native/tests/test_log_capture.c native/tests/test_mc_log_info.c \
      native/tests/test_mc_log_off.c native/tests/test_mc_log_debug.c native/tests/test_mc_log_trace.c \
      native/tests/test_server_trace.c native/tests/test_platform_uart0.c native/tests/test_platform_psram.c \
      core/src/mc_ringbuf.c core/src/mc_varint.c core/src/mc_packet.c core/src/mc_link.c \
      firmware/mc_link_session.c core/src/mc_world.c core/src/mc_world_compressed.c \
      core/src/mc_server.c firmware/mc_log.c firmware/platform_uart0.c firmware/platform_psram.c \
      -o build/native/mc_uart_tests_protocol_compression
    ./build/native/mc_uart_tests_protocol_compression

    cat > build/native/compression_test_main.c <<'EOF'
    #include <stdio.h>
    #include <string.h>

    #include "mc_config.h"
    #include "mc_packet.h"
    #include "mc_ringbuf.h"
    #include "mc_world.h"
    #include "mc_world_compressed.h"
    #include "mc_world_compressed_assets.h"

    #define main mc_uart_tests_all_main
    #include "../../native/test_main.c"
    #undef main

    #define test_world mc_uart_compressed_map_legacy_test_world
    #include "../../native/tests/test_world.c"
    #undef test_world

    int test_packet(void);
    int test_server_login(void);

    static int test_world_compressed_map(void)
    {
      uint8_t arena[4096];
      uint8_t tx_storage[4096];
      uint8_t frame[4096];
      uint8_t body[32];
      mc_ringbuf_t tx;
      mc_packet_t packet;
      const mc_world_compressed_chunk_t *chunk;
      const uint8_t *compressed = 0;
      size_t compressed_len = 0;
      size_t body_len = 1u;
      size_t frame_len;
      int32_t data_len = 0;

      ASSERT_TRUE(mc_world_compressed_asset_count == mc_world_spawn_chunk_count());
      ASSERT_TRUE(mc_world_compressed_total_bytes > 0u);
      ASSERT_TRUE(mc_world_compressed_total_bytes < sizeof(arena));
      ASSERT_TRUE(mc_world_compressed_init(arena, sizeof(arena)));
      ASSERT_TRUE(mc_world_compressed_ready());
      ASSERT_EQ(mc_world_compressed_chunk_count(), mc_world_compressed_asset_count);

      chunk = mc_world_compressed_chunk(0u);
      ASSERT_TRUE(chunk != 0);
      ASSERT_TRUE(chunk->compressed >= arena);
      ASSERT_TRUE(chunk->compressed + chunk->compressed_len <= arena + sizeof(arena));

      ASSERT_TRUE(!mc_world_build_chunk_body(0, 0, body, sizeof(body), &body_len));
      ASSERT_EQ(body_len, 0u);

      mc_ringbuf_init(&tx, tx_storage, sizeof(tx_storage));
      ASSERT_TRUE(!mc_world_queue_spawn_chunk(&tx, 0u));
      ASSERT_TRUE(!mc_world_queue_spawn_chunks(&tx));
      ASSERT_EQ(mc_ringbuf_len(&tx), 0u);

      ASSERT_TRUE(mc_world_queue_compressed_spawn_chunk(&tx, 0u));
      frame_len = mc_ringbuf_read(&tx, frame, sizeof(frame));
      ASSERT_TRUE(frame_len > 0u);
      ASSERT_TRUE(mc_packet_try_read(frame, frame_len, &packet));
      ASSERT_TRUE(mc_packet_get_compressed_body(packet.body,
                                               packet.body_len,
                                               &compressed,
                                               &compressed_len,
                                               &data_len));
      ASSERT_EQ(data_len, (int32_t)chunk->raw_body_len);
      ASSERT_TRUE(data_len > (int32_t)MC_CHUNK_SECTION_BYTES);
      ASSERT_EQ(compressed_len, chunk->compressed_len);
      ASSERT_TRUE(memcmp(compressed, chunk->compressed, compressed_len) == 0);

      return 0;
    }

    int main(void)
    {
      if (test_packet() != 0) {
        fprintf(stderr, "FAIL packet_compression\n");
        return 1;
      }
      printf("PASS packet_compression\n");

      if (test_compressed_world_init_and_queue() != 0) {
        fprintf(stderr, "FAIL world_compressed_runtime\n");
        return 1;
      }
      printf("PASS world_compressed_runtime\n");

      if (test_world_compressed_map() != 0) {
        fprintf(stderr, "FAIL world_compressed_map\n");
        return 1;
      }
      printf("PASS world_compressed_map\n");

      if (test_server_login() != 0) {
        fprintf(stderr, "FAIL server_login_compression\n");
        return 1;
      }
      printf("PASS server_login_compression\n");
      return 0;
    }
EOF

    # The compression binary compiles the native test source set with protocol
    # compression enabled, but runs only compression-safe runtime tests. Other
    # runtime tests still assert default-build raw framing or config values.
    cc -std=c11 -Wall -Wextra -Werror -O2 \
      -DMC_PROTOCOL_COMPRESSION_ENABLE=1 \
      -DMC_USE_PSRAM_COMPRESSED_MAP=1 \
      -Icore/include -Icore/generated -Ifirmware \
      build/native/compression_test_main.c \
      native/tests/test_ringbuf.c native/tests/test_varint.c native/tests/test_packet.c \
      native/tests/test_link_codec.c native/tests/test_link_session.c native/tests/test_server_status.c \
      native/tests/test_server_login.c native/tests/test_world.c native/tests/test_integration.c \
      native/tests/test_firmware_config.c native/tests/test_log_capture.c native/tests/test_mc_log_info.c \
      native/tests/test_mc_log_off.c native/tests/test_mc_log_debug.c native/tests/test_mc_log_trace.c \
      native/tests/test_server_trace.c native/tests/test_platform_uart0.c native/tests/test_platform_psram.c \
      core/src/mc_ringbuf.c core/src/mc_varint.c core/src/mc_packet.c core/src/mc_link.c \
      firmware/mc_link_session.c core/src/mc_world.c core/src/mc_world_compressed.c \
      core/generated/mc_world_compressed_assets.c core/src/mc_server.c firmware/mc_log.c \
      firmware/platform_uart0.c firmware/platform_psram.c \
      -o build/native/mc_uart_tests_compression
    ./build/native/mc_uart_tests_compression

    cc -std=c11 -Wall -Wextra -Werror -O2 \
      -Icore/include \
      -Ifirmware \
      -DMC_PSRAM_FLOW_TEST_BYTES=8388612u \
      -DMC_TEST_PSRAM_OVERSIZE_FLOW_BYTES=1 \
      native/tests/test_platform_psram.c \
      firmware/platform_psram.c \
      -o build/native/psram_oversize_flow_test
    ./build/native/psram_oversize_flow_test

    cc -std=c11 -Wall -Wextra -Werror -Icore/include -Ifirmware \
      -DMC_BRIDGE_UART_ID=MC_UART_ID_1 \
      -DMC_LOG_UART_ID=MC_UART_ID_0 \
      -DMC_EXPECT_BRIDGE_UART_ID=MC_UART_ID_1 \
      -DMC_EXPECT_LOG_UART_ID=MC_UART_ID_0 \
      -c native/tests/test_firmware_config_compile.c \
      -o build/native/firmware_config_reversed.o

    if cc -std=c11 -Wall -Wextra -Werror -Icore/include -Ifirmware \
      -DMC_BRIDGE_UART_ID=MC_UART_ID_0 \
      -DMC_LOG_UART_ID=MC_UART_ID_0 \
      -c native/tests/test_firmware_config_compile.c \
      -o build/native/firmware_config_conflict.o; then
      echo "conflicting UART role mapping unexpectedly compiled" >&2
      exit 1
    fi

    if cc -std=c11 -Wall -Wextra -Werror -Icore/include -Ifirmware \
      -DMC_BRIDGE_UART_ID=2 \
      -DMC_LOG_UART_ID=MC_UART_ID_1 \
      -c native/tests/test_firmware_config_compile.c \
      -o build/native/firmware_config_invalid.o; then
      echo "invalid UART role mapping unexpectedly compiled" >&2
      exit 1
    fi

    if cc -std=c11 -Wall -Wextra -Werror -Icore/include -Ifirmware \
      -DMC_UART0_WRITE_BURST_BYTES=0 \
      -c native/tests/test_firmware_config_compile.c \
      -o build/native/firmware_config_zero_burst.o; then
      echo "zero UART0 write burst unexpectedly compiled" >&2
      exit 1
    fi

    if cc -std=c11 -Wall -Wextra -Werror -Icore/include -Ifirmware \
      -DMC_PSRAM_ENABLE=2 \
      -c native/tests/test_firmware_config_compile.c \
      -o build/native/firmware_config_invalid_psram_enable.o; then
      echo "invalid PSRAM enable unexpectedly compiled" >&2
      exit 1
    fi

    if cc -std=c11 -Wall -Wextra -Werror -Icore/include -Ifirmware \
      -DMC_PSRAM_FLOW_TEST=2 \
      -c native/tests/test_firmware_config_compile.c \
      -o build/native/firmware_config_invalid_psram_flow_test.o; then
      echo "invalid PSRAM flow test unexpectedly compiled" >&2
      exit 1
    fi

    if cc -std=c11 -Wall -Wextra -Werror -Icore/include -Ifirmware \
      -DMC_PSRAM_FLOW_TEST_BYTES=0 \
      -c native/tests/test_firmware_config_compile.c \
      -o build/native/firmware_config_zero_psram_flow_bytes.o; then
      echo "zero PSRAM flow test bytes unexpectedly compiled" >&2
      exit 1
    fi

    if cc -std=c11 -Wall -Wextra -Werror -Icore/include -Ifirmware \
      -DMC_PSRAM_FLOW_TEST_BYTES=3u \
      -c native/tests/test_firmware_config_compile.c \
      -o build/native/firmware_config_unaligned_psram_flow_bytes.o; then
      echo "unaligned PSRAM flow test bytes unexpectedly compiled" >&2
      exit 1
    fi

    cc -std=c11 -Wall -Wextra -Werror -Icore/include -Ifirmware \
      -DMC_M2C_AGGRESSIVE_TX=1 \
      -DMC_EXPECT_M2C_AGGRESSIVE_TX=1 \
      -DMC_EXPECT_UART0_WRITE_BURST_BYTES=128u \
      -DMC_EXPECT_LINK_TX_MAX_BYTES_PER_LOOP=MC_TICK_BUDGET_TX_BYTES \
      -DMC_EXPECT_LINK_TX_PENDING_BYTES=MC_TICK_BUDGET_TX_BYTES \
      -DMC_EXPECT_UART0_TX_PACE_LOOPS=384u \
      -c native/tests/test_firmware_config_compile.c \
      -o build/native/firmware_config_m2c_aggressive.o

    cc -std=c11 -Wall -Wextra -Werror -Icore/include -Ifirmware \
      -DMC_PROTOCOL_COMPRESSION_ENABLE=1 \
      -DMC_USE_PSRAM_COMPRESSED_MAP=1 \
      -DMC_COMPRESSION_THRESHOLD=8192u \
      -DMC_EXPECT_PROTOCOL_COMPRESSION_ENABLE=1 \
      -DMC_EXPECT_USE_PSRAM_COMPRESSED_MAP=1 \
      -DMC_EXPECT_COMPRESSION_THRESHOLD=8192u \
      -c native/tests/test_firmware_config_compile.c \
      -o build/native/firmware_config_compressed_map.o

    if cc -std=c11 -Wall -Wextra -Werror -Icore/include -Ifirmware \
      -DMC_PROTOCOL_COMPRESSION_ENABLE=0 \
      -DMC_USE_PSRAM_COMPRESSED_MAP=1 \
      -c native/tests/test_firmware_config_compile.c \
      -o build/native/firmware_config_compressed_map_without_protocol.o; then
      echo "PSRAM compressed map without protocol compression unexpectedly compiled" >&2
      exit 1
    fi

    cc -std=c11 -Wall -Wextra -Werror -Icore/include -Ifirmware \
      -DMC_M2C_AGGRESSIVE_TX=0 \
      -DCONFIG_CPU_FREQ_MHZ=96u \
      -DMC_EXPECT_UART0_TX_PACE_LOOPS=1041u \
      -c native/tests/test_firmware_config_compile.c \
      -o build/native/firmware_config_board_cpu_freq.o

    cc -std=c11 -Wall -Wextra -Werror -Icore/include -Ifirmware \
      -DMC_LOG_LEVEL=MC_LOG_TRACE \
      -c native/tests/test_firmware_config_compile.c \
      -o build/native/firmware_config_log_trace.o
    runHook postBuild
  '';

  installPhase = ''
    runHook preInstall
    mkdir -p "$out/bin"
    install -Dm755 build/native/mc_uart_tests "$out/bin/mc_uart_tests"
    runHook postInstall
  '';
}
