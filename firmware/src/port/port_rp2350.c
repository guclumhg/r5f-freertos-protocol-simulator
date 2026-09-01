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
#include "hardware/dma.h"
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

/* =========================================================================
 * Baud sweep support
 *
 * The PL011 oversamples by 16, so its top speed is clk_peri/16 and a byte
 * can never arrive faster than one per 160 core clocks. Raising the system
 * clock raises both sides of that ratio, so it does not move. That is why
 * there is a synthetic mode here as well as a real one: the real sweep says
 * whether a UART bridge works, and the synthetic sweep finds where the
 * per-byte architecture actually stops.
 * ========================================================================= */

#define SLICE_SYNTH   8u
#define RX_DMA_BLOCK  256u      /* bytes per DMA receive interrupt */

static rx_mode_t s_rx_mode = RX_MODE_PER_BYTE;
static int  s_dma_tx_a = -1, s_dma_tx_b = -1, s_dma_rx = -1;
/* Channel B writes this value into channel A's read-address trigger, which
 * is what makes the transmit stream loop forever without the CPU. It has to
 * outlive the setup function, so it lives here. */
static const uint8_t *s_tx_buf_ptr;
static bool s_synth_running;
static volatile uint32_t s_synth_budget;

static void uart_quiesce(void)
{
    uart_set_irq_enables(uart0, false, false);
    hw_clear_bits(&uart_get_hw(uart0)->cr, UART_UARTCR_UARTEN_BITS);
    /* Drain anything the receiver is still holding. */
    while (uart_is_readable(uart0)) {
        (void)uart_get_hw(uart0)->dr;
    }
    uart_get_hw(uart0)->rsr = 0;
}

static void uart_resume(void)
{
    hw_set_bits(&uart_get_hw(uart0)->cr, UART_UARTCR_UARTEN_BITS);
}

uint32_t port_set_baud(uint32_t want)
{
    uart_quiesce();
    s_actual_baud = uart_set_baudrate(uart0, want);
    /* uart_set_baudrate rewrites the line control register, which clears the
     * loopback bit and the frame format. Put them back. */
    uart_set_format(uart0, 8, 1, UART_PARITY_NONE);
    if (s_mode == LOOPBACK_INTERNAL) {
        hw_set_bits(&uart_get_hw(uart0)->cr, UART_UARTCR_LBE_BITS);
    }
    port_set_rx_mode(s_rx_mode);
    uart_resume();
    return s_actual_baud;
}

/* ------------------------------------------------------------- DMA receive */

static void __not_in_flash_func(on_rx_dma)(void)
{
    gpio_put(PIN_PROBE_ISR, 1);
    uint32_t t0 = DWT_CYCCNT;

    dma_hw->ints1 = 1u << (uint32_t)s_dma_rx;
    engine_on_rx_block(RX_DMA_BLOCK);
    /* The write address wraps by itself; only the count needs rearming. */
    dma_channel_set_trans_count((uint)s_dma_rx, RX_DMA_BLOCK, true);

    uint32_t t1 = DWT_CYCCNT;
    engine_record_isr(t1 - t0);
    gpio_put(PIN_PROBE_ISR, 0);
}

static void rx_dma_start(void)
{
    uint8_t *buf = NULL;
    uint32_t size = 0;
    engine_rx_ring_storage(&buf, &size);

    if (s_dma_rx < 0) {
        s_dma_rx = dma_claim_unused_channel(true);
    }
    dma_channel_config c = dma_channel_get_default_config((uint)s_dma_rx);
    channel_config_set_transfer_data_size(&c, DMA_SIZE_8);
    channel_config_set_read_increment(&c, false);
    channel_config_set_write_increment(&c, true);
    /* Wrap the write address inside the ring, so the hardware never walks
     * off the end and the handler only has to move an index. */
    channel_config_set_ring(&c, true, 12);      /* 2^12 = 4096 */
    channel_config_set_dreq(&c, DREQ_UART0_RX);

    dma_channel_configure((uint)s_dma_rx, &c, buf,
                          &uart_get_hw(uart0)->dr, RX_DMA_BLOCK, false);

    dma_channel_set_irq1_enabled((uint)s_dma_rx, true);
    irq_set_exclusive_handler(DMA_IRQ_1, on_rx_dma);
    irq_set_priority(DMA_IRQ_1, PRIO_UART_RX);
    irq_set_enabled(DMA_IRQ_1, true);
    dma_channel_start((uint)s_dma_rx);
}

static void rx_dma_stop(void)
{
    if (s_dma_rx < 0) {
        return;
    }
    dma_channel_abort((uint)s_dma_rx);
    dma_channel_set_irq1_enabled((uint)s_dma_rx, false);
    irq_set_enabled(DMA_IRQ_1, false);
}

void port_set_rx_mode(rx_mode_t mode)
{
    uart_set_irq_enables(uart0, false, false);
    rx_dma_stop();

    switch (mode) {
    case RX_MODE_PER_BYTE:
        uart_set_fifo_enabled(uart0, false);
        uart_set_irq_enables(uart0, true, false);
        break;

    case RX_MODE_FIFO_TH:
        uart_set_fifo_enabled(uart0, true);
        /* Receive interrupt at half full: 16 of the 32 byte FIFO. */
        hw_write_masked(&uart_get_hw(uart0)->ifls,
                        2u << UART_UARTIFLS_RXIFLSEL_LSB,
                        UART_UARTIFLS_RXIFLSEL_BITS);
        uart_set_irq_enables(uart0, true, false);
        break;

    case RX_MODE_DMA:
        uart_set_fifo_enabled(uart0, true);
        rx_dma_start();
        break;
    }
    s_rx_mode = mode;
}

rx_mode_t port_rx_mode(void) { return s_rx_mode; }

/* ------------------------------------------------------------ DMA transmit */

void port_tx_dma_start(const uint8_t *buf, uint32_t len)
{
    s_tx_buf_ptr = buf;

    if (s_dma_tx_a < 0) {
        s_dma_tx_a = dma_claim_unused_channel(true);
        s_dma_tx_b = dma_claim_unused_channel(true);
    }

    /* Channel A streams the buffer at the UART's pace. Channel B does one
     * write - resetting A's read pointer, which retriggers it - and chains
     * back. Between them the transmitter runs forever at zero CPU cost, which
     * is the whole point: the stimulus must not compete with the interrupt
     * being measured. */
    dma_channel_config a = dma_channel_get_default_config((uint)s_dma_tx_a);
    channel_config_set_transfer_data_size(&a, DMA_SIZE_8);
    channel_config_set_read_increment(&a, true);
    channel_config_set_write_increment(&a, false);
    channel_config_set_dreq(&a, DREQ_UART0_TX);
    channel_config_set_chain_to(&a, (uint)s_dma_tx_b);
    dma_channel_configure((uint)s_dma_tx_a, &a,
                          &uart_get_hw(uart0)->dr, buf, len, false);

    dma_channel_config b = dma_channel_get_default_config((uint)s_dma_tx_b);
    channel_config_set_transfer_data_size(&b, DMA_SIZE_32);
    channel_config_set_read_increment(&b, false);
    channel_config_set_write_increment(&b, false);
    dma_channel_configure((uint)s_dma_tx_b, &b,
                          &dma_hw->ch[(uint)s_dma_tx_a].al3_read_addr_trig,
                          &s_tx_buf_ptr, 1, false);

    dma_channel_start((uint)s_dma_tx_a);
}

void port_tx_dma_stop(void)
{
    if (s_dma_tx_a < 0) {
        return;
    }
    dma_channel_abort((uint)s_dma_tx_b);
    dma_channel_abort((uint)s_dma_tx_a);
}

void port_stop_ticks(void)
{
    pwm_set_enabled(SLICE_BYTE_TICK, false);
    pwm_set_enabled(SLICE_CAN_SLOT, false);
}

/* ------------------------------------------------- synthetic handler load */

static void __not_in_flash_func(on_synth)(void)
{
    gpio_put(PIN_PROBE_ISR, 1);
    uint32_t t0 = DWT_CYCCNT;

    pwm_clear_irq(SLICE_SYNTH);
    engine_on_synth_tick();

    uint32_t t1 = DWT_CYCCNT;
    engine_record_isr(t1 - t0);

    if (s_synth_budget && --s_synth_budget == 0u) {
        /* Switch the source off from inside the interrupt it is driving.
         * Above saturation this is the only code that still gets to run. */
        pwm_set_enabled(SLICE_SYNTH, false);
        pwm_set_irq0_enabled(SLICE_SYNTH, false);
    }
    gpio_put(PIN_PROBE_ISR, 0);
}

void port_synth_start(uint32_t cycles, uint32_t budget)
{
    port_synth_stop();
    if (cycles < 8u || cycles > 65535u || budget == 0u) {
        return;
    }
    s_synth_budget = budget;

    pwm_config c = pwm_get_default_config();
    pwm_config_set_clkdiv_int(&c, 1);
    pwm_config_set_wrap(&c, (uint16_t)(cycles - 1u));
    pwm_init(SLICE_SYNTH, &c, false);
    pwm_clear_irq(SLICE_SYNTH);
    pwm_set_irq0_enabled(SLICE_SYNTH, true);
    /* Shares PWM_IRQ_WRAP_0 with the byte tick, which the sweep has stopped.
     * The old handler has to be removed first: setting an exclusive handler
     * over an existing one is a panic, not an overwrite. */
    irq_remove_handler(PWM_IRQ_WRAP_0, on_byte_tick);
    irq_set_exclusive_handler(PWM_IRQ_WRAP_0, on_synth);
    irq_set_priority(PWM_IRQ_WRAP_0, PRIO_UART_RX);
    irq_set_enabled(PWM_IRQ_WRAP_0, true);
    pwm_set_enabled(SLICE_SYNTH, true);
    s_synth_running = true;
}

bool port_synth_done(void)
{
    return s_synth_budget == 0u;
}

void port_synth_stop(void)
{
    if (!s_synth_running) {
        return;
    }
    s_synth_budget = 0;
    pwm_set_enabled(SLICE_SYNTH, false);
    pwm_set_irq0_enabled(SLICE_SYNTH, false);
    irq_set_enabled(PWM_IRQ_WRAP_0, false);
    /* Give the byte tick its handler back, same rule in reverse. */
    irq_remove_handler(PWM_IRQ_WRAP_0, on_synth);
    irq_set_exclusive_handler(PWM_IRQ_WRAP_0, on_byte_tick);
    irq_set_priority(PWM_IRQ_WRAP_0, PRIO_BYTE_TICK);
    irq_set_enabled(PWM_IRQ_WRAP_0, true);
    s_synth_running = false;
}

/* FreeRTOS run time statistics counter: 1 us, free running, one load. */
unsigned long r5f_runtime_counter(void)
{
    return (unsigned long)time_us_32();
}
