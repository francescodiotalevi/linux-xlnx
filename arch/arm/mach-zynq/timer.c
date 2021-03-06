/*
 * This file contains driver for the Xilinx PS Timer Counter IP.
 *
 *  Copyright (C) 2011 Xilinx
 *
 * based on arch/mips/kernel/time.c timer driver
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include <linux/clk.h>
#include <linux/clockchips.h>
#include <linux/interrupt.h>
#include <linux/of_irq.h>
#include <linux/of_address.h>
#include <linux/slab.h>
#include <asm/smp_twd.h>
#include "common.h"

/*
 * This driver configures the 2 16-bit count-up timers as follows:
 *
 * T1: Timer 1, clocksource for generic timekeeping
 * T2: Timer 2, clockevent source for hrtimers
 * T3: Timer 3, <unused>
 *
 * The input frequency to the timer module for emulation is 2.5MHz which is
 * common to all the timer channels (T1, T2, and T3). With a pre-scaler of 32,
 * the timers are clocked at 78.125KHz (12.8 us resolution).

 * The input frequency to the timer module in silicon is configurable and
 * obtained from device tree. The pre-scaler of 32 is used.
 */

/*
 * Timer Register Offset Definitions of Timer 1, Increment base address by 4
 * and use same offsets for Timer 2
 */
#define XTTCPS_CLK_CNTRL_OFFSET	0x00 /* Clock Control Reg, RW */
#define XTTCPS_CNT_CNTRL_OFFSET	0x0C /* Counter Control Reg, RW */
#define XTTCPS_COUNT_VAL_OFFSET	0x18 /* Counter Value Reg, RO */
#define XTTCPS_INTR_VAL_OFFSET		0x24 /* Interval Count Reg, RW */
#define XTTCPS_ISR_OFFSET		0x54 /* Interrupt Status Reg, RO */
#define XTTCPS_IER_OFFSET		0x60 /* Interrupt Enable Reg, RW */

#define XTTCPS_CNT_CNTRL_DISABLE_MASK	0x1

/*
 * Setup the timers to use pre-scaling, using a fixed value for now that will
 * work across most input frequency, but it may need to be more dynamic
 */
#define PRESCALE_EXPONENT	11	/* 2 ^ PRESCALE_EXPONENT = PRESCALE */
#define PRESCALE		2048	/* The exponent must match this */
#define CLK_CNTRL_PRESCALE_EN	1
#define CLK_CNTRL_PRESCALE	(((PRESCALE_EXPONENT - 1) << 1) | \
				CLK_CNTRL_PRESCALE_EN)
#define CNT_CNTRL_RESET		(1 << 4)

/**
 * struct xttcps_timer - This definition defines local timer structure
 *
 * @base_addr:	Base address of timer
 * @clk:	Associated clock source
 * @clk_rate_change_nb	Notifier block for clock rate changes
 */
struct xttcps_timer {
	void __iomem *base_addr;
	struct clk *clk;
	struct notifier_block clk_rate_change_nb;
};

#define to_xttcps_timer(x) \
		container_of(x, struct xttcps_timer, clk_rate_change_nb)

struct xttcps_timer_clocksource {
	struct xttcps_timer	xttc;
	struct clocksource	cs;
};

#define to_xttcps_timer_clksrc(x) \
		container_of(x, struct xttcps_timer_clocksource, cs)

struct xttcps_timer_clockevent {
	struct xttcps_timer		xttc;
	struct clock_event_device	ce;
};

#define to_xttcps_timer_clkevent(x) \
		container_of(x, struct xttcps_timer_clockevent, ce)

/**
 * xttcps_set_interval - Set the timer interval value
 *
 * @timer:	Pointer to the timer instance
 * @cycles:	Timer interval ticks
 **/
static void xttcps_set_interval(struct xttcps_timer *timer,
					unsigned long cycles)
{
	u32 ctrl_reg;

	/* Disable the counter, set the counter value  and re-enable counter */
	ctrl_reg = __raw_readl(timer->base_addr + XTTCPS_CNT_CNTRL_OFFSET);
	ctrl_reg |= XTTCPS_CNT_CNTRL_DISABLE_MASK;
	__raw_writel(ctrl_reg, timer->base_addr + XTTCPS_CNT_CNTRL_OFFSET);

	__raw_writel(cycles, timer->base_addr + XTTCPS_INTR_VAL_OFFSET);

	/*
	 * Reset the counter (0x10) so that it starts from 0, one-shot
	 * mode makes this needed for timing to be right.
	 */
	ctrl_reg |= CNT_CNTRL_RESET;
	ctrl_reg &= ~XTTCPS_CNT_CNTRL_DISABLE_MASK;
	__raw_writel(ctrl_reg, timer->base_addr + XTTCPS_CNT_CNTRL_OFFSET);
}

/**
 * xttcps_clock_event_interrupt - Clock event timer interrupt handler
 *
 * @irq:	IRQ number of the Timer
 * @dev_id:	void pointer to the xttcps_timer instance
 *
 * returns: Always IRQ_HANDLED - success
 **/
static irqreturn_t xttcps_clock_event_interrupt(int irq, void *dev_id)
{
	struct xttcps_timer_clockevent *xttce = dev_id;
	struct xttcps_timer *timer = &xttce->xttc;

	/* Acknowledge the interrupt and call event handler */
	__raw_readl(timer->base_addr + XTTCPS_ISR_OFFSET);

	xttce->ce.event_handler(&xttce->ce);

	return IRQ_HANDLED;
}

/**
 * __xttc_clocksource_read - Reads the timer counter register
 *
 * returns: Current timer counter register value
 **/
static cycle_t __xttc_clocksource_read(struct clocksource *cs)
{
	struct xttcps_timer *timer = &to_xttcps_timer_clksrc(cs)->xttc;

	return (cycle_t)__raw_readl(timer->base_addr +
				XTTCPS_COUNT_VAL_OFFSET);
}

/**
 * xttcps_set_next_event - Sets the time interval for next event
 *
 * @cycles:	Timer interval ticks
 * @evt:	Address of clock event instance
 *
 * returns: Always 0 - success
 **/
static int xttcps_set_next_event(unsigned long cycles,
					struct clock_event_device *evt)
{
	struct xttcps_timer_clockevent *xttce = to_xttcps_timer_clkevent(evt);
	struct xttcps_timer *timer = &xttce->xttc;

	xttcps_set_interval(timer, cycles);
	return 0;
}

/**
 * xttcps_set_mode - Sets the mode of timer
 *
 * @mode:	Mode to be set
 * @evt:	Address of clock event instance
 **/
static void xttcps_set_mode(enum clock_event_mode mode,
					struct clock_event_device *evt)
{
	struct xttcps_timer_clockevent *xttce = to_xttcps_timer_clkevent(evt);
	struct xttcps_timer *timer = &xttce->xttc;
	u32 ctrl_reg;

	switch (mode) {
	case CLOCK_EVT_MODE_PERIODIC:
		xttcps_set_interval(timer,
				DIV_ROUND_CLOSEST(clk_get_rate(xttce->xttc.clk),
					PRESCALE * HZ));
		break;
	case CLOCK_EVT_MODE_ONESHOT:
	case CLOCK_EVT_MODE_UNUSED:
	case CLOCK_EVT_MODE_SHUTDOWN:
		ctrl_reg = __raw_readl(timer->base_addr +
					XTTCPS_CNT_CNTRL_OFFSET);
		ctrl_reg |= XTTCPS_CNT_CNTRL_DISABLE_MASK;
		__raw_writel(ctrl_reg,
				timer->base_addr + XTTCPS_CNT_CNTRL_OFFSET);
		break;
	case CLOCK_EVT_MODE_RESUME:
		ctrl_reg = __raw_readl(timer->base_addr +
					XTTCPS_CNT_CNTRL_OFFSET);
		ctrl_reg &= ~XTTCPS_CNT_CNTRL_DISABLE_MASK;
		__raw_writel(ctrl_reg,
				timer->base_addr + XTTCPS_CNT_CNTRL_OFFSET);
		break;
	}
}

static int xttcps_rate_change_clocksource_cb(struct notifier_block *nb,
		unsigned long event, void *data)
{
	struct clk_notifier_data *ndata = data;
	struct xttcps_timer *xttcps = to_xttcps_timer(nb);
	struct xttcps_timer_clocksource *xttccs = container_of(xttcps,
			struct xttcps_timer_clocksource, xttc);

	switch (event) {
	case POST_RATE_CHANGE:
		/*
		 * Do whatever is necessare to maintain a proper time base
		 *
		 * I cannot find a way to adjust the currently used clocksource
		 * to the new frequency. __clocksource_updatefreq_hz() sounds
		 * good, but does not work. Not sure what's that missing.
		 *
		 * This approach works, but triggers two clocksource switches.
		 * The first after unregister to clocksource jiffies. And
		 * another one after the register to the newly registered timer.
		 *
		 * Alternatively we could 'waste' another HW timer to ping pong
		 * between clock sources. That would also use one register and
		 * one unregister call, but only trigger one clocksource switch
		 * for the cost of another HW timer used by the OS.
		 */
		clocksource_unregister(&xttccs->cs);
		clocksource_register_hz(&xttccs->cs,
				ndata->new_rate / PRESCALE);
		/* fall through */
	case PRE_RATE_CHANGE:
	case ABORT_RATE_CHANGE:
	default:
		return NOTIFY_DONE;
	}
}

static void __init zynq_ttc_setup_clocksource(struct clk *clk,
							void __iomem *base)
{
	struct xttcps_timer_clocksource *ttccs;
	int err;

	ttccs = kzalloc(sizeof(*ttccs), GFP_KERNEL);
	if (WARN_ON(!ttccs))
		return;

	ttccs->xttc.clk = clk;

	err = clk_prepare_enable(ttccs->xttc.clk);
	if (WARN_ON(err))
		return;

	ttccs->xttc.clk_rate_change_nb.notifier_call =
		xttcps_rate_change_clocksource_cb;
	ttccs->xttc.clk_rate_change_nb.next = NULL;
	if (clk_notifier_register(ttccs->xttc.clk,
				&ttccs->xttc.clk_rate_change_nb))
		pr_warn("Unable to register clock notifier.\n");

	ttccs->xttc.base_addr = base;
	ttccs->cs.name = "xttcps_clocksource";
	ttccs->cs.rating = 200;
	ttccs->cs.read = __xttc_clocksource_read;
	ttccs->cs.mask = CLOCKSOURCE_MASK(16);
	ttccs->cs.flags = CLOCK_SOURCE_IS_CONTINUOUS;

	/*
	 * Setup the clock source counter to be an incrementing counter
	 * with no interrupt and it rolls over at 0xFFFF. Pre-scale
	 * it by 32 also. Let it start running now.
	 */
	__raw_writel(0x0,  ttccs->xttc.base_addr + XTTCPS_IER_OFFSET);
	__raw_writel(CLK_CNTRL_PRESCALE,
		     ttccs->xttc.base_addr + XTTCPS_CLK_CNTRL_OFFSET);
	__raw_writel(CNT_CNTRL_RESET,
		     ttccs->xttc.base_addr + XTTCPS_CNT_CNTRL_OFFSET);

	err = clocksource_register_hz(&ttccs->cs,
			clk_get_rate(ttccs->xttc.clk) / PRESCALE);
	if (WARN_ON(err))
		return;

}

static int xttcps_rate_change_clockevent_cb(struct notifier_block *nb,
		unsigned long event, void *data)
{
	struct clk_notifier_data *ndata = data;
	struct xttcps_timer *xttcps = to_xttcps_timer(nb);
	struct xttcps_timer_clockevent *xttcce = container_of(xttcps,
			struct xttcps_timer_clockevent, xttc);

	switch (event) {
	case POST_RATE_CHANGE:
	{
		unsigned long flags;

		/*
		 * clockevents_update_freq should be called with IRQ disabled on
		 * the CPU the timer provides events for. The timer we use is
		 * common to both CPUs, not sure if we need to run on both
		 * cores.
		 */
		local_irq_save(flags);
		clockevents_update_freq(&xttcce->ce,
				ndata->new_rate / PRESCALE);
		local_irq_restore(flags);

		/* fall through */
	}
	case PRE_RATE_CHANGE:
	case ABORT_RATE_CHANGE:
	default:
		return NOTIFY_DONE;
	}
}

static void __init zynq_ttc_setup_clockevent(struct clk *clk,
						void __iomem *base, u32 irq)
{
	struct xttcps_timer_clockevent *ttcce;
	int err;

	ttcce = kzalloc(sizeof(*ttcce), GFP_KERNEL);
	if (WARN_ON(!ttcce))
		return;

	ttcce->xttc.clk = clk;

	err = clk_prepare_enable(ttcce->xttc.clk);
	if (WARN_ON(err))
		return;

	ttcce->xttc.clk_rate_change_nb.notifier_call =
		xttcps_rate_change_clockevent_cb;
	ttcce->xttc.clk_rate_change_nb.next = NULL;
	if (clk_notifier_register(ttcce->xttc.clk,
				&ttcce->xttc.clk_rate_change_nb))
		pr_warn("Unable to register clock notifier.\n");

	ttcce->xttc.base_addr = base;
	ttcce->ce.name = "xttcps_clockevent";
	ttcce->ce.features = CLOCK_EVT_FEAT_PERIODIC | CLOCK_EVT_FEAT_ONESHOT;
	ttcce->ce.set_next_event = xttcps_set_next_event;
	ttcce->ce.set_mode = xttcps_set_mode;
	ttcce->ce.rating = 200;
	ttcce->ce.irq = irq;
	ttcce->ce.cpumask = cpu_possible_mask;

	/*
	 * Setup the clock event timer to be an interval timer which
	 * is prescaled by 32 using the interval interrupt. Leave it
	 * disabled for now.
	 */
	__raw_writel(0x23, ttcce->xttc.base_addr + XTTCPS_CNT_CNTRL_OFFSET);
	__raw_writel(CLK_CNTRL_PRESCALE,
		     ttcce->xttc.base_addr + XTTCPS_CLK_CNTRL_OFFSET);
	__raw_writel(0x1,  ttcce->xttc.base_addr + XTTCPS_IER_OFFSET);

	err = request_irq(irq, xttcps_clock_event_interrupt,
			  IRQF_DISABLED | IRQF_TIMER,
			  ttcce->ce.name, ttcce);
	if (WARN_ON(err))
		return;

	clockevents_config_and_register(&ttcce->ce,
			clk_get_rate(ttcce->xttc.clk) / PRESCALE, 1, 0xfffe);
}

/**
 * xttcps_timer_init - Initialize the timer
 *
 * Initializes the timer hardware and register the clock source and clock event
 * timers with Linux kernal timer framework
 */
static void __init xttcps_timer_init(struct device_node *timer)
{
	unsigned int irq;
	void __iomem *timer_baseaddr;
	struct clk *clk;

	/*
	 * Get the 1st Triple Timer Counter (TTC) block from the device tree
	 * and use it. Note that the event timer uses the interrupt and it's the
	 * 2nd TTC hence the irq_of_parse_and_map(,1)
	 */
	timer_baseaddr = of_iomap(timer, 0);
	if (!timer_baseaddr) {
		pr_err("ERROR: invalid timer base address\n");
		BUG();
	}

	irq = irq_of_parse_and_map(timer, 1);
	if (irq <= 0) {
		pr_err("ERROR: invalid interrupt number\n");
		BUG();
	}

	clk = clk_get_sys("CPU_1X_CLK", NULL);
	if (IS_ERR(clk)) {
		pr_err("ERROR: timer input clock not found\n");
		BUG();
	}

	zynq_ttc_setup_clocksource(clk, timer_baseaddr);
	zynq_ttc_setup_clockevent(clk, timer_baseaddr + 4, irq);
#ifdef CONFIG_IPIPE
	if (num_possible_cpus() == 1)
		pr_err("I-pipe: not supported on Zynq without SMP\n");
#endif
#ifdef CONFIG_HAVE_ARM_TWD
	twd_local_timer_of_register();
#endif
	pr_info("%s #0 at %p, irq=%d\n", timer->name, timer_baseaddr, irq);
}

/*
 * This will be replaced in v3.10 by
 * CLOCKSOURCE_OF_DECLARE(zynq, "xlnx,ttc",xttcps_timer_init);
 * or
 * CLOCKSOURCE_OF_DECLARE(zynq, "xlnx,ps7-ttc-1.00.a",xttcps_timer_init);
 */
void __init xttcps_timer_init_old(void)
{
	const char * const timer_list[] = {
		"xlnx,ps7-ttc-1.00.a",
		NULL
	};
	struct device_node *timer;

	timer = of_find_compatible_node(NULL, NULL, timer_list[0]);
	if (!timer) {
		pr_err("ERROR: no compatible timer found\n");
		BUG();
	}

	xttcps_timer_init(timer);
}
