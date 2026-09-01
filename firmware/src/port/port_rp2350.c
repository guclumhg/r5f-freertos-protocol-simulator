/*
 * port_rp2350.c - port.h implemented on the RP2350.
 *
 * This is the only file that knows what chip we are on. Three things in here
 * are worth reading twice:
 *
 * 1. The two periodic ticks come from PWM slices, not from the microsecond
 *    alarm timer. 86.8 us and 135 us are 13020 and 20250 core clock cycles at
 *    150 MHz - both whole numbers, both under the 16 bit wrap limit. The
 *    alarm timer only has 1 us resolution and would have had to alternate
 *    between 86 and 87 us.
 *
 * 2. The UART FIFOs are switched off. That is not an oversight: with the
 *    FIFOs on, the hardware would batch bytes and the receive interrupt would
 *    fire once per burst. We want it to fire once per byte, because the once
 *    per byte path is the thing that has to fit inside 86.8 us on the target.
 *
 * 3. The receive interrupt runs at NVIC priority 0, above the kernel's
 *    configMAX_SYSCALL_INTERRUPT_PRIORITY of 16. FreeRTOS cannot mask it,
 *    delay it or schedule around it. In exchange it calls no FreeRTOS API.
 */
#include "port/port.h"

#include "hardware/clocks.h"
#include "hardware/gpio.h"
#include "hardware/irq.h"
#include "hardware/pwm.h"
#include "hardware/regs/intctrl.h"
#include "hardware/structs/uart.h"
#include "hardware/sync.h"
#include "hardware/uart.h"
#include "pico/stdlib.h"
#include "pico/time.h"

#include "config.h"

/* config.h carries its own clock constant because it has to stay free of any
 * SDK dependency. The SDK states the same number independently, so check the
 * two against each other here rather than trusting either alone. */
_Static_assert(R5F_SYS_CLK_HZ == SYS_CLK_HZ,
               "config.h disagrees with the SDK about the RP2350 system clock");

/* ------------------------------------------------------- DWT cycle counter
 *
 * Addressed directly rather than through CMSIS so this file has no dependency
 * beyond the SDK. The equivalent on the R5F is the PMU cycle counter, which
 * is enabled in much the same three writes.
 */
#define DWT_CTRL    (*(volatile uint32_t *)0xE0001000u)
#define DWT_CYCCNT  (*(volatile uint32_t *)0xE0001004u)
#define DWT_LAR     (*(volatile uint32_t *)0xE0001FB0u)
#define SCB_DEMCR   (*(volatile uint32_t *)0xE000EDFCu)
#define DEMCR_TRCENA      (1u << 24)
#define DWT_CTRL_CYCCNTENA (1u << 0)

/* --------------------------------------------------------- IRQ priorities
 *
 * RP2350 implements 4 NVIC priority bits, so only the top nibble matters and
 * usable levels step by 16. Lower is more urgent.
 */
#define PRIO_UART_RX   0x00u   /* above the kernel ceiling of 16 */
#define PRIO_BYTE_TICK 0x40u
#define PRIO_CAN_SLOT  0x80u

/* PWM slices 6 and 7 are used purely as timers; no GPIO is attached. */
#define SLICE_BYTE_TICK 6u
#define SLICE_CAN_SLOT  7u

static loopback_mode_t s_mode;
static uint32_t        s_actual_baud;
static uint32_t        s_clk_hz;
static bool            s_cycles_ok;
static uint32_t        s_overrun_hw;

/* ------------------------------------------------------------- interrupts */

static void __not_in_flash_func(on_uart_rx)(void)
{
    gpio_put(PIN_PROBE_ISR, 1);
    uint32_t t0 = DWT_CYCCNT;

    /* One read takes the byte and its error flags together. With the FIFOs
     * off there is at most one byte waiting. */
    uint32_t dr = uart_get_hw(uart0)->dr;
    engine_on_rx_byte((uint8_t)(dr & 0xFFu), dr);

    uint32_t t1 = DWT_CYCCNT;

    /* Outside the measured window on purpose - see port.h. */
    engine_record_isr(t1 - t0);
    gpio_put(PIN_PROBE_ISR, 0);
}

static void __not_in_flash_func(on_byte_tick)(void)
{
    pwm_clear_irq(SLICE_BYTE_TICK);
    engine_on_byte_tick();
}

static void __not_in_flash_func(on_can_slot)(void)
{
    pwm_clear_irq(SLICE_CAN_SLOT);
    engine_on_can_slot();
}

/* ------------------------------------------------------------------- init */

static void cycles_init(void)
{
    SCB_DEMCR |= DEMCR_TRCENA;
    DWT_LAR    = 0xC5ACCE55u;   /* harmless if the lock is not implemented */
    DWT_CYCCNT = 0;
    DWT_CTRL  |= DWT_CTRL_CYCCNTENA;

    /* A silently dead cycle counter would turn every timing number in this
     * project into a zero, so prove it is running before trusting it. */
    uint32_t a = DWT_CYCCNT;
    for (volatile int i = 0; i < 100; i++) { }
    uint32_t b = DWT_CYCCNT;
    s_cycles_ok = (b != a);
}

static void uart_init_loopback(loopback_mode_t mode)
{
    s_actual_baud = uart_init(uart0, UART_BAUD);

    gpio_set_function(PIN_UART_TX, GPIO_FUNC_UART);
    gpio_set_function(PIN_UART_RX, GPIO_FUNC_UART);

    uart_set_format(uart0, 8, 1, UART_PARITY_NONE);
    uart_set_hw_flow(uart0, false, false);

    /* One interrupt per byte. See the note at the top of the file. */
    uart_set_fifo_enabled(uart0, false);

    if (mode == LOOPBACK_INTERNAL) {
        /* UARTCR.LBE. The PL011 still serialises and deserialises at the
         * configured baud rate, so a byte still occupies the line for one
         * byte time and still raises exactly one receive interrupt. The
         * hardware_uart API does not expose this bit. */
        hw_set_bits(&uart_get_hw(uart0)->cr, UART_UARTCR_LBE_BITS);
    }

    uart_set_irq_enables(uart0, true, false);
    irq_set_exclusive_handler(UART0_IRQ, on_uart_rx);
    irq_set_priority(UART0_IRQ, PRIO_UART_RX);
    irq_set_enabled(UART0_IRQ, true);
}

static void tick_init(uint slice, uint16_t cycles, uint irq_num,
                      irq_handler_t handler, uint8_t priority, bool use_irq1)
{
    pwm_config c = pwm_get_default_config();
    pwm_config_set_clkdiv_int(&c, 1);        /* count core clock cycles */
    pwm_config_set_wrap(&c, cycles - 1u);    /* period is wrap + 1 */
    pwm_init(slice, &c, false);

    pwm_clear_irq(slice);
    if (use_irq1) {
        pwm_set_irq1_enabled(slice, true);
    } else {
        pwm_set_irq0_enabled(slice, true);
    }
    irq_set_exclusive_handler(irq_num, handler);
    irq_set_priority(irq_num, priority);
    irq_set_enabled(irq_num, true);
}

void port_init(loopback_mode_t mode)
{
    s_mode   = mode;
    s_clk_hz = clock_get_hz(clk_sys);

    gpio_init(PIN_PROBE_ISR);
    gpio_set_dir(PIN_PROBE_ISR, GPIO_OUT);
    gpio_put(PIN_PROBE_ISR, 0);
    gpio_init(PIN_PROBE_BURST);
    gpio_set_dir(PIN_PROBE_BURST, GPIO_OUT);
    gpio_put(PIN_PROBE_BURST, 0);

    cycles_init();
    uart_init_loopback(mode);

    tick_init(SLICE_BYTE_TICK, BYTE_TIME_CYCLES, PWM_IRQ_WRAP_0,
              on_byte_tick, PRIO_BYTE_TICK, false);
    tick_init(SLICE_CAN_SLOT, CAN_SLOT_CYCLES, PWM_IRQ_WRAP_1,
              on_can_slot, PRIO_CAN_SLOT, true);
}

void port_start_ticks(void)
{
    pwm_set_enabled(SLICE_BYTE_TICK, true);
    pwm_set_enabled(SLICE_CAN_SLOT, true);
}

/* ------------------------------------------------------------- accessors */

uint32_t port_cycles(void)      { return DWT_CYCCNT; }
bool     port_cycles_ok(void)   { return s_cycles_ok; }
uint32_t port_clk_hz(void)      { return s_clk_hz; }
uint32_t port_actual_baud(void) { return s_actual_baud; }
loopback_mode_t port_loopback_mode(void) { return s_mode; }

void __not_in_flash_func(port_probe_burst)(bool on)
{
    gpio_put(PIN_PROBE_BURST, on);
}

bool __not_in_flash_func(port_uart_tx)(uint8_t b)
{
    if (!uart_is_writable(uart0)) {
        return false;
    }
    uart_get_hw(uart0)->dr = b;
    return true;
}

uint32_t port_uart_overrun(void)
{
    /* Independent of the per-byte flag the interrupt sees: this catches an
     * overrun even if the interrupt never ran at all. */
    uint32_t rsr = uart_get_hw(uart0)->rsr;
    if (rsr & UART_UARTRSR_OE_BITS) {
        s_overrun_hw++;
        uart_get_hw(uart0)->rsr = 0;   /* any write clears the flags */
    }
    return s_overrun_hw;
}

uint32_t port_irq_save(void)          { return save_and_disable_interrupts(); }
void     port_irq_restore(uint32_t s) { restore_interrupts(s); }

/* FreeRTOS run time statistics counter: 1 us, free running, one load. */
unsigned long r5f_runtime_counter(void)
{
    return (unsigned long)time_us_32();
}
