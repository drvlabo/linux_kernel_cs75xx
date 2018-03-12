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


#define	DRV_NAME	"cs75xx-gpio"


#define	GPIO_PIN_NUM	(32)


#define	OFFS_CFG	(0x00)
#define	OFFS_OUT	(0x04)
#define	OFFS_IN		(0x08)
#define	OFFS_LVL	(0x0C)
#define	OFFS_EDGE	(0x10)
#define	OFFS_IE		(0x14)
#define	OFFS_INT	(0x18)
#if 0
#define	OFFS_STAT	(0x1C)
#endif


#define	VAL_DIR_IN	(0)
#define	VAL_DIR_OUT	(1)


struct cs75xx_gpio {
	struct gpio_chip	gpio_chip;
	spinlock_t		lock;
	void __iomem		*reg_base;
	void __iomem		*mux_reg;
	int			irq;
	int			id;
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

	_gpio_dir(priv, offset, VAL_DIR_OUT);
	_gpio_set(priv, offset, val);

	spin_unlock_irqrestore(&priv->lock, flags);

	return 0;
}

/******************* irq_chip part *********************/

static irqreturn_t cs75xx_gpio_irq_handler(int irq, void *data)
{
	struct cs75xx_gpio *priv = data;

	return IRQ_HANDLED;
}

static void cs75xx_gpio_irq_mask(struct irq_data *irq_data)
{
}

static void cs75xx_gpio_irq_unmask(struct irq_data *irq_data)
{
}

static int cs75xx_gpio_irq_set_type(struct irq_data *irq_data, unsigned int type)
{
	return 0;
}

static struct irq_chip cs75xx_gpio_irqchip = {
	.name	= DRV_NAME,
	.irq_mask	= cs75xx_gpio_irq_mask,
	.irq_unmask	= cs75xx_gpio_irq_unmask,
	.irq_set_type	= cs75xx_gpio_irq_set_type,
#ifdef NOT_YET
	.irq_set_wake	= NULL,
#endif
	.flags		= IRQCHIP_MASK_ON_SUSPEND,
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

	res = gpiochip_irqchip_add(gpio_chip, &cs75xx_gpio_irqchip, 0,
					handle_bad_irq, IRQ_TYPE_NONE);
	if (res) {
		dev_err(&pdev->dev, "failed to add irqchip.\n");
		return res;
	}

	gpiochip_set_chained_irqchip(gpio_chip, &cs75xx_gpio_irqchip, priv->irq, NULL);

	res = devm_request_irq(&pdev->dev, priv->irq, cs75xx_gpio_irq_handler,
			0, dev_name(&pdev->dev), priv);
	if (res) {
		dev_err(&pdev->dev, "failed to request_irq.\n");
		return res;
	}

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
