/*
 * port.h - The whole contract between the protocol engine and the hardware.
 *
 * Everything under engine/ is written against this header and nothing else.
 * port_rp2350.c implements it over the RP2350's PL011, PWM slices and DWT
 * cycle counter. A port_r5f.c on the target would implement the same handful
 * of functions over XUartPs, a Triple Timer Counter and the PMU cycle
 * counter, and the engine sources would move across unchanged.
 *
 * That is the claim this project exists to demonstrate, so this file is
 * deliberately small and deliberately boring.
 */
#ifndef R5F_PORT_H
#define R5F_PORT_H

#include <stdbool.h>
#include <stdint.h>

/* How the transmit line gets back to the receive line.
 *
 * INTERNAL uses the PL011's own loopback path (UARTCR.LBE). The byte is
 * still serialised and deserialised at the configured baud rate, so it still
 * occupies the line for one byte time and still raises one receive interrupt.
 * EXTERNAL expects a wire between the TX and RX pins. The two are
 * indistinguishable in every number this project reports; INTERNAL is the
 * default only because a wire can fall out. */
typedef enum {
    LOOPBACK_INTERNAL = 0,
    LOOPBACK_EXTERNAL = 1,
} loopback_mode_t;

/* Brings up the cycle counter, the UART, the probe pins and both periodic
 * ticks. Does not start the ticks; see port_start_ticks(). */
void port_init(loopback_mode_t mode);

/* Starts the 86.8 us byte tick and the 135 us CAN slot tick. Called once the
 * scheduler is running so the first tick does not fire into a half-built
 * system. */
void port_start_ticks(void);

/* Free-running core-clock cycle counter. Wraps every 2^32 cycles, which is
 * 28.6 seconds at 150 MHz - far longer than anything we time with it. */
uint32_t port_cycles(void);

/* True if the cycle counter was actually enabled. Checked at startup and
 * reported in telemetry, because a silently dead counter would turn every
 * timing number in this project into a zero. */
bool port_cycles_ok(void);

/* Pushes one byte into the transmit holding register. Returns false if the
 * transmitter was still busy, which would mean the byte tick has drifted
 * ahead of the line rate. */
bool port_uart_tx(uint8_t b);

/* Bytes the UART receiver itself dropped, read from the PL011 overrun flag.
 * This is a hardware count, independent of anything the firmware believes. */
uint32_t port_uart_overrun(void);

/* Which loopback path is live, for reporting. */
loopback_mode_t port_loopback_mode(void);

/* Core clock in Hz, read back from the hardware rather than assumed. */
uint32_t port_clk_hz(void);

/* The baud rate the divider actually produced, which is not exactly 115200.
 * Reported so the small difference between it and the byte tick is visible
 * rather than hidden. */
uint32_t port_actual_baud(void);

/* Drives the probe pin that is high for the length of a CAN burst, so the
 * 19.845 ms window can be confirmed on a scope rather than only believed. */
void port_probe_burst(bool on);

/* ------------------------------------------------------------------------
 * Baud sweep support. None of this is used by the normal measurement; it
 * exists to find where the per-byte interrupt architecture stops working.
 * ---------------------------------------------------------------------- */

typedef enum {
    RX_MODE_PER_BYTE = 0,   /* FIFO off, one interrupt per byte */
    RX_MODE_FIFO_TH,        /* FIFO on, interrupt at half full */
    RX_MODE_DMA,            /* DMA into the ring, interrupt per block */
} rx_mode_t;

/* Sets the baud rate and returns what the divider actually produced. The
 * PL011 divides by 16 before anything else, so the ceiling is clk_peri/16
 * and a byte can never take fewer than 160 core clocks - which is the reason
 * this sweep needs a synthetic mode as well as a real one. */
uint32_t  port_set_baud(uint32_t want);
void      port_set_rx_mode(rx_mode_t mode);
rx_mode_t port_rx_mode(void);

/* Feeds the transmitter from DMA so that generating the stimulus costs no
 * CPU at all. Without this the transmit pacer would be a second interrupt
 * competing with the one being measured, at exactly the rates where that
 * matters most. */
void port_tx_dma_start(const uint8_t *buf, uint32_t len);
void port_tx_dma_stop(void);

void port_stop_ticks(void);

/* Fires engine_on_synth_tick every `cycles` core clocks, so the receive
 * handler's own workload can be driven past any rate the UART can produce.
 *
 * It stops itself after `budget` interrupts, and that is not a convenience.
 * Past the saturation point the handler re-enters before it has finished
 * returning, no task ever runs again, and the system cannot report the very
 * thing the sweep exists to find. A budget the interrupt counts down itself
 * is the only thing that still works when nothing else does. */
void port_synth_start(uint32_t cycles, uint32_t budget);
bool port_synth_done(void);
void port_synth_stop(void);

/* Callbacks for the modes above. Same rules: no FreeRTOS API. */
void engine_on_rx_block(uint32_t bytes);   /* DMA wrote this many */
void engine_on_synth_tick(void);

/* The DMA receive mode writes straight into the engine's ring, so the port
 * has to know where it is. */
void engine_rx_ring_storage(uint8_t **buf, uint32_t *size_pow2);

/* Short critical section. Used only to take consistent snapshots of counters
 * that an interrupt also writes; never held across anything slow. */
uint32_t port_irq_save(void);
void     port_irq_restore(uint32_t state);

/* Marks a function as belonging on the hot path, so it is placed in fast
 * memory rather than in execute-in-place flash.
 *
 * On RP2350 this puts it in SRAM, using the same .time_critical section the
 * SDK's own __not_in_flash_func macro uses - spelled out here rather than
 * included, so engine code stays free of SDK headers. On the target the
 * equivalent annotation places the function in the R5F's tightly coupled
 * memory, which is where the specification says the bridge will run. On a
 * host build it does nothing.
 *
 * This is not a micro-optimisation. Without it the interrupt occasionally
 * pays a flash cache miss, and the worst case is what this project reports. */
#if defined(PICO_RP2350) || defined(PICO_RP2040)
#  define PORT_HOT(fn) __attribute__((section(".time_critical." #fn))) fn
#else
#  define PORT_HOT(fn) fn
#endif

/* ------------------------------------------------------------------------
 * Callbacks the engine implements and the port calls from interrupt context.
 * None of these may call a FreeRTOS API: they run above the kernel's syscall
 * priority ceiling.
 * ---------------------------------------------------------------------- */

/* One received byte, together with the raw PL011 data register value so the
 * engine can see the hardware's own framing / overrun flags for that byte. */
void engine_on_rx_byte(uint8_t b, uint32_t dr_flags);

/* The measured duration of the receive interrupt, in core clock cycles. The
 * port takes the closing timestamp before calling this, so the cost of
 * recording the statistic is not counted inside the number it records. The
 * GPIO probe pin covers the whole interrupt including this call, which is
 * what makes the scope a genuine cross-check rather than a repeat. */
void engine_record_isr(uint32_t cycles);

/* Every 86.8 us: the transmit line is free for exactly one byte. */
void engine_on_byte_tick(void);

/* Every 135 us: one CAN frame slot has elapsed. */
void engine_on_can_slot(void);

#endif /* R5F_PORT_H */
