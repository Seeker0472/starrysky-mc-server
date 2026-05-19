#include "mc_firmware_config.h"

#ifndef MC_EXPECT_BRIDGE_UART_ID
#define MC_EXPECT_BRIDGE_UART_ID MC_UART_ID_0
#endif

#ifndef MC_EXPECT_LOG_UART_ID
#define MC_EXPECT_LOG_UART_ID MC_UART_ID_1
#endif

#ifndef MC_EXPECT_M2C_AGGRESSIVE_TX
#define MC_EXPECT_M2C_AGGRESSIVE_TX MC_M2C_AGGRESSIVE_TX
#endif

#ifndef MC_EXPECT_UART0_BAUD
#define MC_EXPECT_UART0_BAUD MC_UART0_BAUD
#endif

#ifndef MC_EXPECT_UART1_BAUD
#define MC_EXPECT_UART1_BAUD MC_UART1_BAUD
#endif

#ifndef MC_EXPECT_UART0_WRITE_BURST_BYTES
#define MC_EXPECT_UART0_WRITE_BURST_BYTES MC_UART0_WRITE_BURST_BYTES
#endif

#ifndef MC_EXPECT_LINK_TX_MAX_BYTES_PER_LOOP
#define MC_EXPECT_LINK_TX_MAX_BYTES_PER_LOOP MC_LINK_TX_MAX_BYTES_PER_LOOP
#endif

#ifndef MC_EXPECT_LINK_TX_PENDING_BYTES
#define MC_EXPECT_LINK_TX_PENDING_BYTES MC_LINK_TX_PENDING_BYTES
#endif

#ifndef MC_EXPECT_UART0_TX_PACE_LOOPS_ZERO
#define MC_EXPECT_UART0_TX_PACE_LOOPS_ZERO 0
#endif

#ifndef MC_EXPECT_UART0_TX_PACE_LOOPS
#define MC_EXPECT_UART0_TX_PACE_LOOPS MC_UART0_TX_PACE_LOOPS
#endif

#if MC_BRIDGE_UART_ID != MC_EXPECT_BRIDGE_UART_ID
#error "unexpected bridge uart mapping"
#endif

#if MC_LOG_UART_ID != MC_EXPECT_LOG_UART_ID
#error "unexpected log uart mapping"
#endif

#if MC_M2C_AGGRESSIVE_TX != MC_EXPECT_M2C_AGGRESSIVE_TX
#error "unexpected M2C aggressive TX setting"
#endif

#if MC_UART0_BAUD != MC_EXPECT_UART0_BAUD
#error "unexpected UART0 baud"
#endif

#if MC_UART1_BAUD != MC_EXPECT_UART1_BAUD
#error "unexpected UART1 baud"
#endif

#if MC_UART0_WRITE_BURST_BYTES != MC_EXPECT_UART0_WRITE_BURST_BYTES
#error "unexpected UART0 write burst"
#endif

#if MC_LINK_TX_MAX_BYTES_PER_LOOP != MC_EXPECT_LINK_TX_MAX_BYTES_PER_LOOP
#error "unexpected link TX loop budget"
#endif

#if MC_LINK_TX_PENDING_BYTES != MC_EXPECT_LINK_TX_PENDING_BYTES
#error "unexpected link TX pending buffer size"
#endif

#if MC_EXPECT_UART0_TX_PACE_LOOPS_ZERO && MC_UART0_TX_PACE_LOOPS != 0
#error "expected zero UART0 TX pace loops"
#endif

#if MC_UART0_TX_PACE_LOOPS != MC_EXPECT_UART0_TX_PACE_LOOPS
#error "unexpected UART0 TX pace loops"
#endif

int firmware_config_compile_probe = 0;
