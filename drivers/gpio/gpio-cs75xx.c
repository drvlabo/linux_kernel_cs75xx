#include <linux/init.h>
#include <linux/err.h>
#include <linux/module.h>
#include <linux/interrupt.h>
#include <linux/irq.h>
#include <linux/irqdomain.h>
#include <linux/gpio.h>
#include <linux/io.h>
#include <linux/of.h>
#include <linux/of_irq.h>
#include <linux/of_device.h>
#include <linux/platform_device.h>
#include "gpiolib.h"


#define	DRV_NAME	"cs75xx-gpio"


#define	GPIO_PIN_NUM	(32)


#define	OFFS_CFG	(0x00)
#define	OFFS_OUT	(0x04)
#define	OFFS_IN		(0x08)
#define	OFFS_LVL	(0x0C)
#define	OFFS_EDGE	(0x10)
#define	OFFS_IE		(0x14)
#define	OFFS_INT	(0x18)


#define	VAL_DIR_IN	(0)
#define	VAL_DIR_OUT	(1)


struct cs75xx_gpio {
	int			id;
	void __iomem		*reg_base;
	void __iomem		*mux_reg;
	int			irq;
	struct gpio_chip	gpio_chip;
	spinlock_t		lock;
	u32			toggle_mask;
};

/******************* gpio_chip part ********************/

static int cs75xx_gpio_request(struct gpio_chip *chip, unsigned int offs)
{
	struct cs75xx_gpio *priv = gpiochip_get_data(chip);
	unsigned long flags;
	unsigned int regval;

	spin_lock_irqsave(&priv->lock, flags);

	regval = readl(priv->mux_reg);
	regval |= (0x01UL << offs);
	writel(regval, priv->mux_reg);

	spin_unlock_irqrestore(&priv->lock, flags);
	return 0;
}

static void cs75xx_gpio_free(struct gpio_chip *chip, unsigned int offset)
{
	struct cs75xx_gpio *priv = gpiochip_get_data(chip);
	unsigned long flags;
	unsigned int regval;

	spin_lock_irqsave(&priv->lock, flags);

	regval = readl(priv->mux_reg);
	regval &= ~(0x01UL << offset);
	writel(regval, priv->mux_reg);

	spin_unlock_irqrestore(&priv->lock, flags);
}

static void _gpio_set(struct cs75xx_gpio *priv, unsigned int offset, int val)
{
	unsigned int regval;

	regval = readl(priv->reg_base + OFFS_OUT);
	if (val)
		regval |= 0x01UL << offset;
	else
		regval &= ~(0x01UL << offset);
	writel(regval, priv->reg_base + OFFS_OUT);
}

static void cs75xx_gpio_set(struct gpio_chip *chip, unsigned int offset, int val)
{
	struct cs75xx_gpio *priv = gpiochip_get_data(chip);
	unsigned long flags;

	spin_lock_irqsave(&priv->lock, flags);

	_gpio_set(priv, offset, val);

	spin_unlock_irqrestore(&priv->lock, flags);
}

static int cs75xx_gpio_get(struct gpio_chip *chip, unsigned int offset)
{
	struct cs75xx_gpio *priv = gpiochip_get_data(chip);
	unsigned long flags;
	int val;

	spin_lock_irqsave(&priv->lock, flags);

	val = (readl(priv->reg_base + OFFS_IN) & (0x01UL << offset)) ? 1 : 0;

	spin_unlock_irqrestore(&priv->lock, flags);

	return val;
}

static void _gpio_dir(struct cs75xx_gpio *priv, unsigned int offset, int dir)
{
	unsigned int regval;

	regval = readl(priv->reg_base + OFFS_CFG);
	if (dir == VAL_DIR_OUT)
		regval &= ~(0x01UL << offset);
	else
		regval |= (0x01UL << offset);
	writel(regval, priv->reg_base + OFFS_CFG);
}

static int cs75xx_gpio_dir_in(struct gpio_chip *chip, unsigned int offset)
{
	struct cs75xx_gpio *priv = gpiochip_get_data(chip);
	unsigned long flags;

	spin_lock_irqsave(&priv->lock, flags);

	_gpio_dir(priv, offset, VAL_DIR_IN);

	spin_unlock_irqrestore(&priv->lock, flags);

	return 0;
}

static int cs75xx_gpio_dir_out(struct gpio_chip *chip, unsigned int offset, int val)
{
	struct cs75xx_gpio *priv = gpiochip_get_data(chip);
	unsigned long flags;

	spin_lock_irqsave(&priv->lock, flags);

	_gpio_set(priv, offset, val);
	_gpio_dir(priv, offset, VAL_DIR_OUT);

	spin_unlock_irqrestore(&priv->lock, flags);

	return 0;
}

/******************* irq_chip part *********************/

static void _toggle_irq_edge_trig(struct cs75xx_gpio *priv, unsigned int offs)
{
	unsigned int regval;

	regval = readl(priv->reg_base + OFFS_LVL);
	writel(regval ^ (0x01UL << offs), priv->reg_base + OFFS_LVL);
}

static void cs75xx_gpio_irq_handler(struct irq_desc *desc)
{
	struct cs75xx_gpio *priv;
	unsigned int irq_stat;
	struct irq_chip *irqchip;
	unsigned long flags;

	priv = gpiochip_get_data(irq_desc_get_handler_data(desc));
	irqchip = irq_desc_get_chip(desc);

	chained_irq_enter(irqchip, desc);

	spin_lock_irqsave(&priv->lock, flags);

	irq_stat = readl(priv->reg_base + OFFS_INT);
	irq_stat &= readl(priv->reg_base + OFFS_IE);

	spin_unlock_irqrestore(&priv->lock, flags);

	while (irq_stat) {
		unsigned int offset;

		offset = __fls(irq_stat);

		if (priv->toggle_mask & (0x01UL << offset)) {
			spin_lock_irqsave(&priv->lock, flags);
			_toggle_irq_edge_trig(priv, offset);
			spin_unlock_irqrestore(&priv->lock, flags);
		}
		generic_handle_irq(irq_find_mapping(priv->gpio_chip.irqdomain, offset));
		irq_stat &= ~(0x01UL << offset);
	}

	chained_irq_exit(irqchip, desc);
}

static void cs75xx_gpio_irq_ack(struct irq_data *irq_data)
{
	struct cs75xx_gpio *priv;
	unsigned long flags;

	priv = gpiochip_get_data(irq_data_get_irq_chip_data(irq_data));

	spin_lock_irqsave(&priv->lock, flags);

	writel(0x01UL << irq_data->hwirq, priv->reg_base + OFFS_INT);

	spin_unlock_irqrestore(&priv->lock, flags);
}

static void cs75xx_gpio_irq_mask(struct irq_data *irq_data)
{
	struct cs75xx_gpio *priv;
	unsigned long flags;
	unsigned int regval;

	priv = gpiochip_get_data(irq_data_get_irq_chip_data(irq_data));

	spin_lock_irqsave(&priv->lock, flags);

	regval = readl(priv->reg_base + OFFS_IE);
	regval &= ~(0x01UL << irq_data->hwirq);
	writel(regval, priv->reg_base + OFFS_IE);

	spin_unlock_irqrestore(&priv->lock, flags);
}

static void cs75xx_gpio_irq_unmask(struct irq_data *irq_data)
{
	struct cs75xx_gpio *priv;
	unsigned long flags;
	unsigned int regval;

	priv = gpiochip_get_data(irq_data_get_irq_chip_data(irq_data));

	spin_lock_irqsave(&priv->lock, flags);

	/* clear pending interrupt before enabling */
	writel(0x01UL << irq_data->hwirq, priv->reg_base + OFFS_INT);

	regval = readl(priv->reg_base + OFFS_IE);
	regval |= (0x01UL << irq_data->hwirq);
	writel(regval, priv->reg_base + OFFS_IE);

	spin_unlock_irqrestore(&priv->lock, flags);
}

static int cs75xx_gpio_irq_set_type(struct irq_data *irq_data, unsigned int type)
{
	struct cs75xx_gpio *priv;
	unsigned long flags;
	unsigned int reg_lvl;
	unsigned int reg_edge;
	unsigned int mask;
	int res = 0;

	priv = gpiochip_get_data(irq_data_get_irq_chip_data(irq_data));

	mask = 0x01UL << irq_data->hwirq;

	spin_lock_irqsave(&priv->lock, flags);

	priv->toggle_mask &= ~(0x01UL << irq_data->hwirq);

	reg_lvl = readl(priv->reg_base + OFFS_LVL);
	reg_edge = readl(priv->reg_base + OFFS_EDGE);

	switch (type) {
	case IRQ_TYPE_LEVEL_LOW:
		reg_lvl &= ~mask;
		reg_edge &= ~mask;
		break;
	case IRQ_TYPE_LEVEL_HIGH:
		reg_lvl |= mask;
		reg_edge &= ~mask;
		break;
	case IRQ_TYPE_EDGE_FALLING:
	l_edge_f:
		reg_lvl &= ~mask;
		reg_edge |= mask;
		break;
	case IRQ_TYPE_EDGE_RISING:
	l_edge_r:
		reg_lvl |= mask;
		reg_edge |= mask;
		break;
	case IRQ_TYPE_EDGE_BOTH:
		priv->toggle_mask |= (0x01UL << irq_data->hwirq);
		if (gpiod_is_active_low(gpiochip_get_desc(&priv->gpio_chip, irq_data->hwirq)))
			goto l_edge_f;
		else
			goto l_edge_r;
		break;
	default:
		res = -EINVAL;
		goto l_unlock;
	}

	writel(reg_lvl, priv->reg_base + OFFS_LVL);
	writel(reg_edge, priv->reg_base + OFFS_EDGE);

  l_unlock:;
	spin_unlock_irqrestore(&priv->lock, flags);

	if (res == 0){
		if (type & (IRQ_TYPE_LEVEL_LOW | IRQ_TYPE_LEVEL_HIGH))
			irq_set_handler_locked(irq_data, handle_level_irq);
		else if (type & (IRQ_TYPE_EDGE_FALLING | IRQ_TYPE_EDGE_RISING))
			irq_set_handler_locked(irq_data, handle_edge_irq);
	}

	return res;
}

static struct irq_chip cs75xx_gpio_irqchip = {
	.name		= "g2_gpio",			/* use short name here */
	.irq_ack	= cs75xx_gpio_irq_ack,
	.irq_mask	= cs75xx_gpio_irq_mask,
	.irq_unmask	= cs75xx_gpio_irq_unmask,
	.irq_set_type	= cs75xx_gpio_irq_set_type,
#ifdef NOT_YET
	.irq_set_wake	= NULL,
#endif
#if 0
	.flags		= IRQCHIP_MASK_ON_SUSPEND | IRQCHIP_SET_TYPE_MASKED,
#else
	.flags		= IRQCHIP_MASK_ON_SUSPEND,
#endif
};

static int cs75xx_gpio_probe(struct platform_device *pdev)
{
	struct cs75xx_gpio *priv;
	struct gpio_chip *gpio_chip;
	struct resource *rp;
	void __iomem* reg;
	u32 regval;
	int irq;
	int id;
	int res;

	id = of_alias_get_id(pdev->dev.of_node, "gpio");
	if (id < 0) {
		dev_err(&pdev->dev, "Couldn't get OF id\n");
		return id;
	}

	priv = devm_kzalloc(&pdev->dev, sizeof(*priv), GFP_KERNEL);
	if (priv == NULL)
		return -ENOMEM;

	platform_set_drvdata(pdev, priv);

	priv->id = id;

	rp = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	reg = devm_ioremap_resource(&pdev->dev, rp);
	if (IS_ERR(reg)) {
		dev_err(&pdev->dev, "could not get GPIO register space.\n");
		return PTR_ERR(reg);
	}
	priv->reg_base = reg;

	rp = platform_get_resource(pdev, IORESOURCE_MEM, 1);
	reg = devm_ioremap_resource(&pdev->dev, rp);
	if (IS_ERR(reg)) {
		dev_err(&pdev->dev, "could not get MUX register space.\n");
		return PTR_ERR(reg);
	}
	priv->mux_reg = reg;

	irq = platform_get_irq(pdev, 0);
	if (irq < 0) {
		dev_err(&pdev->dev, "invalid IRQ\n");
		return irq;
	}
	priv->irq = irq;

	if (of_property_read_u32(pdev->dev.of_node, "mux-initval", &regval) == 0) {
		writel(regval, priv->mux_reg);
		dev_info(&pdev->dev, "initialize GPIO_MUX%d = 0x%08X\n", priv->id, regval);
	}

	spin_lock_init(&priv->lock);

	gpio_chip = &priv->gpio_chip;
	gpio_chip->label		= dev_name(&pdev->dev);
	gpio_chip->owner		= THIS_MODULE;
	gpio_chip->parent		= &pdev->dev;
	gpio_chip->request		= cs75xx_gpio_request;
	gpio_chip->free			= cs75xx_gpio_free;
	gpio_chip->set			= cs75xx_gpio_set;
	gpio_chip->get			= cs75xx_gpio_get;
	gpio_chip->direction_input	= cs75xx_gpio_dir_in;
	gpio_chip->direction_output	= cs75xx_gpio_dir_out;
	gpio_chip->base			= id * GPIO_PIN_NUM;
	gpio_chip->ngpio		= GPIO_PIN_NUM;

	res = gpiochip_add_data(gpio_chip, priv);
	if (res) {
		dev_err(&pdev->dev, "failed to add gpiochip.\n");
		return res;
	}

	/* disable all interrupts */
	writel(0x00000000UL, priv->reg_base + OFFS_IE);

	/* clear all pendings */
	writel(0xFFFFFFFFUL, priv->reg_base + OFFS_INT);

	res = gpiochip_irqchip_add(gpio_chip, &cs75xx_gpio_irqchip, 0,
					handle_level_irq, IRQ_TYPE_NONE);
	if (res) {
		dev_err(&pdev->dev, "failed to add irqchip.\n");
		return res;
	}

	gpiochip_set_chained_irqchip(gpio_chip, &cs75xx_gpio_irqchip, priv->irq,
					cs75xx_gpio_irq_handler);

	return 0;
}

static const struct of_device_id cs75xx_gpio_of_match[] = {
	{ .compatible = "cortina,cs75xx-gpio" },
	{},
};

static struct platform_driver cs75xx_gpio_driver = {
	.driver		= {
		.name		= DRV_NAME,
		.of_match_table = cs75xx_gpio_of_match,
	},
	.probe		= cs75xx_gpio_probe,
};

static int __init cs75xx_gpio_init(void)
{
	return platform_driver_register(&cs75xx_gpio_driver);
}
postcore_initcall(cs75xx_gpio_init);
