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


struct cs75xx_gpio {
	struct gpio_chip	gpio_chip;
	void __iomem		*reg_base;
	void __iomem		*mux_reg;
	int			irq;
	int			id;
};


static irqreturn_t cs75xx_gpio_irq_handler(int irq, void *data)
{
	struct cs75xx_gpio *priv = data;

	return IRQ_HANDLED;
}

static struct irq_chip cs75xx_gpio_irqchip = {
	.name	= DRV_NAME,
#ifdef NOT_YET
	.irq_mask	= NULL,
	.irq_unmask	= NULL,
	.irq_set_type	= NULL,
	.irq_set_wake	= NULL,
	.flags		= 0,
#endif
};

static int cs75xx_gpio_probe(struct platform_device *pdev)
{
	struct cs75xx_gpio *priv;
	struct gpio_chip *gpio_chip;
	struct resource *rp;
	void __iomem* reg;
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

printk("%s:%d: exit\n", __func__, __LINE__);
return -ENOMEM;

	gpio_chip = &priv->gpio_chip;
	gpio_chip->label	= dev_name(&pdev->dev);
	gpio_chip->owner	= THIS_MODULE;
	gpio_chip->parent	= &pdev->dev;
#ifdef NOT_YET
	gpio_chip->request		= NULL;
	gpio_chip->free			= NULL;
	gpio_chip->set			= NULL;
	gpio_chip->get			= NULL;
	gpio_chip->direction_input	= NULL;
	gpio_chip->direction_output	= NULL;
#endif
	gpio_chip->base		= -1;
	gpio_chip->ngpio	= GPIO_PIN_NUM;

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

printk("%s:%d: exit\n", __func__, __LINE__);
return -ENOMEM;

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
builtin_platform_driver(cs75xx_gpio_driver);
