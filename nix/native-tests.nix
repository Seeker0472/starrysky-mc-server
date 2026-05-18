{
  stdenv,
  src,
}:

stdenv.mkDerivation {
  pname = "mc-uart-native-tests";
  version = "0.1.0";

  inherit src;

  dontConfigure = true;

  buildPhase = ''
    runHook preBuild
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
      core/src/mc_ringbuf.c \
      core/src/mc_varint.c \
      core/src/mc_packet.c \
      core/src/mc_link.c \
      firmware/mc_link_session.c \
      core/src/mc_world.c \
      core/src/mc_server.c \
      firmware/mc_log.c \
      firmware/platform_uart0.c \
      -o build/native/mc_uart_tests
    ./build/native/mc_uart_tests

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
