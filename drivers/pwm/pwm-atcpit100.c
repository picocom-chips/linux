#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/err.h>
#include <linux/io.h>
#include <linux/ioport.h>
#include <linux/math64.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/pwm.h>
#include <linux/slab.h>
#include <linux/types.h>

#define CLK_EXTERNAL				32768
#define CLK_INTERNAL				60000000
#define PRESCALE_MIN				0
#define CHANNEL_ENABLE				(0x1c)
#define CHPWMEN(ch)				((1<<3)<<(4*ch))
#define CHANNEL_CONTROL(n)			(0x20 + (n*0x10))
#define CHCLK_APB				(1<<3)
#define CHMODE_PWM				(0x4)
#define RELOAD(n)				(0x24 + (n*0x10))
#define DUTY_CYCLE_HIGH_MIN			(0x00000000)
#define DUTY_CYCLE_HIGH_MAX			(0x0000ffff)
#define PERIOD_COUNT_MIN			(0x00000000)
#define PERIOD_COUNT_MAX			(0x0000ffff)

#define PWM_READL(offset)		\
	readl(ap->base + (offset))

#define PWM_WRITEL(val, offset)	\
	writel((val), ap->base + (offset))

struct atcpit_pwmc {
	struct pwm_chip chip;
	void __iomem *base;
	struct clk *clk;
	u64 rate[2];
	u32 val;
	bool en[2];
	enum pwm_polarity polarity;	/* PWM polarity */
};

static inline struct atcpit_pwmc *to_atcpit_pwmc(struct pwm_chip *_chip)
{
	return container_of(_chip, struct atcpit_pwmc, chip);
}

static void atcpit_pwmc_disable(struct pwm_chip *chip, struct pwm_device *pwm)
{
	struct atcpit_pwmc *ap = to_atcpit_pwmc(chip);
	unsigned int chan = pwm->hwpwm;
	unsigned int value;

	value = PWM_READL(CHANNEL_ENABLE);
	value &= ~(CHPWMEN(pwm->hwpwm));
	PWM_WRITEL(value, CHANNEL_ENABLE);
	ap->en[chan] = 0;
	pwm->state.enabled = 0;
}

static int atcpit_pwmc_config(struct pwm_chip *chip, struct pwm_device *pwm,
			    int duty_ns, int period_ns)
{
	struct atcpit_pwmc *ap = to_atcpit_pwmc(chip);
	u64 val, div, rate;
	unsigned long prescale = PRESCALE_MIN, pc, dc;
	unsigned int value, chan = pwm->hwpwm;

	/*
	 * Find period count, duty count and prescale to suit duty_ns and
	 * period_ns. This is done according to formulas described below:
	 *
	 * period_ns = 10^9 * (PRESCALE + 1) * PC / PWM_CLK_RATE
	 * duty_ns = 10^9  * (PRESCALE + 1) * DC / PWM_CLK_RATE
	 *
	 * PC = (PWM_CLK_RATE * period_ns) / (10^9 * (PRESCALE + 1))
	 * DC = (PWM_CLK_RATE * duty_ns) / (10^9 * (PRESCALE + 1))
	 */

	if (period_ns <= duty_ns)
		return -EINVAL;

	if (ap->en[chan]) {
		while (1) {
			rate = ap->rate[pwm->hwpwm];
			div = 1000000000;
			div *= 1 + prescale;
			val = rate * period_ns;
			pc = div64_u64(val, div);
			val = rate * duty_ns;
			dc = div64_u64(val, div);
			pc -= dc;

			/* If duty_ns or period_ns are not achievable then return */
			if (pc < PERIOD_COUNT_MIN || dc < DUTY_CYCLE_HIGH_MIN) {
				atcpit_pwmc_disable(chip, pwm);
				return -EINVAL;
			}

			/* If pc and dc are in bounds, the calculation is done */
			if (pc <= PERIOD_COUNT_MAX && dc <= DUTY_CYCLE_HIGH_MAX) {
				if(pc > 0)
					pc--;

				if (ap->polarity == PWM_POLARITY_NORMAL) {
					value = pc;
					value |= ((dc) << 16);
					ap->val = value;
				} else {
					value = dc;
					value |= ((pc) << 16);
				}
				PWM_WRITEL(value, RELOAD(pwm->hwpwm));
				break;
			}

			/* Otherwise recalculate pc and dc */
			if (pc > PERIOD_COUNT_MAX || dc > DUTY_CYCLE_HIGH_MAX) {
				atcpit_pwmc_disable(chip, pwm);
				return -EINVAL;
			}
		}
	}

	return 0;
}

static int atcpit_pwmc_enable(struct pwm_chip *chip, struct pwm_device *pwm)
{
	struct atcpit_pwmc *ap = to_atcpit_pwmc(chip);
	int ret;
	unsigned int chan = pwm->hwpwm;
	unsigned int value;

	value = PWM_READL(CHANNEL_CONTROL(chan));
	value |= (CHMODE_PWM);

	if(pwm->hwpwm)
		value |= (CHCLK_APB);

	PWM_WRITEL(value, CHANNEL_CONTROL(chan));
	value = PWM_READL(CHANNEL_ENABLE);
	value |= CHPWMEN(pwm->hwpwm);
	PWM_WRITEL(value, CHANNEL_ENABLE);
	ap->en[chan] = 1;
	pwm->state.enabled = 1;
	ret = atcpit_pwmc_config(chip, pwm, pwm_get_duty_cycle(pwm),
			       pwm_get_period(pwm));

	if (ret < 0) {
		clk_disable_unprepare(ap->clk);
		return ret;
	}

	return 0;
}

static int atcpit_pwmc_set_polarity(struct pwm_chip *chip, struct pwm_device *pwm,
				  enum pwm_polarity polarity)
{
	struct atcpit_pwmc *ap = to_atcpit_pwmc(chip);
	ap->polarity = polarity;

	return 0;
}

static const struct pwm_ops atcpit_pwm_ops = {
	.config = atcpit_pwmc_config,
	.set_polarity = atcpit_pwmc_set_polarity,
	.enable = atcpit_pwmc_enable,
	.disable = atcpit_pwmc_disable,
	.owner = THIS_MODULE,
};

static int atcpit_pwmc_probe(struct platform_device *pdev)
{
	struct atcpit_pwmc *ap;
	struct resource *res;
	int ret = 0;

	ap = devm_kzalloc(&pdev->dev, sizeof(*ap), GFP_KERNEL);

	if (ap == NULL)
		return -ENOMEM;

	platform_set_drvdata(pdev, ap);
	ap->chip.dev = &pdev->dev;
	ap->chip.ops = &atcpit_pwm_ops;
	ap->chip.base = -1;
	ap->chip.npwm = 2;
	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	ap->base = devm_ioremap_resource(&pdev->dev, res);

	if (IS_ERR(ap->base))
		return PTR_ERR(ap->base);

	ap->rate[0] = CLK_EXTERNAL;
	ap->rate[1] = CLK_INTERNAL;

	if (IS_ERR(ap->clk)) {
		dev_err(&pdev->dev, "failed to get clock: %ld\n",
			PTR_ERR(ap->clk));
		return PTR_ERR(ap->clk);
	}

	ret = pwmchip_add_with_polarity(&ap->chip, PWM_POLARITY_NORMAL);
	if (ret < 0)
		dev_err(&pdev->dev, "failed to add PWM chip: %d\n", ret);

	return ret;
}

static int atcpit_pwmc_remove(struct platform_device *pdev)
{
	struct atcpit_pwmc *ap = platform_get_drvdata(pdev);
	int err;

	err = pwmchip_remove(&ap->chip);
	if (err < 0)
		return err;

	dev_dbg(&pdev->dev, "pwm-atcpit100 driver removed\n");

	return 0;

}

static const struct of_device_id atcpit_pwmc_dt[] = {
	{ .compatible = "andestech,atcpit100-pwm" },
	{ },
};
MODULE_DEVICE_TABLE(of, atcpit_pwmc_dt);

static struct platform_driver atcpit_pwmc_driver = {
	.driver = {
		.name = "atcpit100-pwm",
		.of_match_table = atcpit_pwmc_dt,
	},
	.probe = atcpit_pwmc_probe,
	.remove = atcpit_pwmc_remove,
};
module_platform_driver(atcpit_pwmc_driver);
MODULE_AUTHOR("Andestech Corporation <rick@andestech.com>");
MODULE_DESCRIPTION("Andes atcpit100 PWM driver");
MODULE_LICENSE("GPL v2");
