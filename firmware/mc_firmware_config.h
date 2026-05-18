#ifndef MC_FIRMWARE_CONFIG_H
#define MC_FIRMWARE_CONFIG_H

#define MC_UART_ID_0 0u
#define MC_UART_ID_1 1u

#ifndef MC_UART0_BAUD
#define MC_UART0_BAUD 115200u
#endif

#ifndef MC_UART1_BAUD
#define MC_UART1_BAUD 115200u
#endif

#ifndef MC_BRIDGE_UART_ID
#define MC_BRIDGE_UART_ID MC_UART_ID_0
#endif

#ifndef MC_LOG_UART_ID
#define MC_LOG_UART_ID MC_UART_ID_1
#endif

#define MC_LOG_OFF 0
#define MC_LOG_INFO 1
#define MC_LOG_DEBUG 2
#define MC_LOG_TRACE 3

#ifndef MC_LOG_LEVEL
#define MC_LOG_LEVEL MC_LOG_INFO
#endif

#ifndef MC_UART0_WRITE_BURST_BYTES
#define MC_UART0_WRITE_BURST_BYTES 16u
#endif

#if MC_UART0_BAUD <= 0
#error "MC_UART0_BAUD must be positive"
#endif

#if MC_UART1_BAUD <= 0
#error "MC_UART1_BAUD must be positive"
#endif

#if MC_UART0_WRITE_BURST_BYTES <= 0
#error "MC_UART0_WRITE_BURST_BYTES must be positive"
#endif

#if MC_BRIDGE_UART_ID != MC_UART_ID_0 && MC_BRIDGE_UART_ID != MC_UART_ID_1
#error "MC_BRIDGE_UART_ID must be MC_UART_ID_0 or MC_UART_ID_1"
#endif

#if MC_LOG_UART_ID != MC_UART_ID_0 && MC_LOG_UART_ID != MC_UART_ID_1
#error "MC_LOG_UART_ID must be MC_UART_ID_0 or MC_UART_ID_1"
#endif

#if MC_BRIDGE_UART_ID == MC_LOG_UART_ID
#error "MC_BRIDGE_UART_ID and MC_LOG_UART_ID must be different"
#endif

#if MC_LOG_LEVEL != MC_LOG_OFF && MC_LOG_LEVEL != MC_LOG_INFO && MC_LOG_LEVEL != MC_LOG_DEBUG && MC_LOG_LEVEL != MC_LOG_TRACE
#error "MC_LOG_LEVEL must be MC_LOG_OFF, MC_LOG_INFO, MC_LOG_DEBUG, or MC_LOG_TRACE"
#endif

#define MC_BRIDGE_UART_BAUD \
    ((MC_BRIDGE_UART_ID == MC_UART_ID_0) ? MC_UART0_BAUD : MC_UART1_BAUD)

#define MC_LOG_UART_BAUD \
    ((MC_LOG_UART_ID == MC_UART_ID_0) ? MC_UART0_BAUD : MC_UART1_BAUD)

#endif
