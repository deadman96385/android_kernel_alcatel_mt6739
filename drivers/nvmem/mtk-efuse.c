
#include <linux/device.h>
#include <linux/module.h>
#include <linux/nvmem-provider.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>

static struct regmap_config mtk_regmap_config = {
	.reg_bits = 32,
	.val_bits = 32,
	.reg_stride = 4,
};

static int mtk_efuse_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct resource *res;
	struct nvmem_device *nvmem;
	struct nvmem_config *econfig;
	struct regmap *regmap;
	void __iomem *base;

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	base = devm_ioremap_resource(dev, res);
	if (IS_ERR(base))
		return PTR_ERR(base);

	econfig = devm_kzalloc(dev, sizeof(*econfig), GFP_KERNEL);
	if (!econfig)
		return -ENOMEM;

	mtk_regmap_config.max_register = resource_size(res) - 1;

	regmap = devm_regmap_init_mmio(dev, base, &mtk_regmap_config);
	if (IS_ERR(regmap)) {
		dev_err(dev, "regmap init failed\n");
		return PTR_ERR(regmap);
	}

	econfig->dev = dev;
	econfig->owner = THIS_MODULE;
	nvmem = nvmem_register(econfig);
	if (IS_ERR(nvmem))
		return PTR_ERR(nvmem);

	platform_set_drvdata(pdev, nvmem);

	return 0;
}

static int mtk_efuse_remove(struct platform_device *pdev)
{
	struct nvmem_device *nvmem = platform_get_drvdata(pdev);

	return nvmem_unregister(nvmem);
}

static const struct of_device_id mtk_efuse_of_match[] = {
	{ .compatible = "mediatek,mt8173-efuse",},
	{ .compatible = "mediatek,efuse",},
	{/* sentinel */},
};
MODULE_DEVICE_TABLE(of, mtk_efuse_of_match);

static struct platform_driver mtk_efuse_driver = {
	.probe = mtk_efuse_probe,
	.remove = mtk_efuse_remove,
	.driver = {
		.name = "mediatek,efuse",
		.of_match_table = mtk_efuse_of_match,
	},
};

static int __init mtk_efuse_init(void)
{
	int ret;

	ret = platform_driver_register(&mtk_efuse_driver);
	if (ret) {
		pr_err("Failed to register efuse driver\n");
		return ret;
	}

	return 0;
}

static void __exit mtk_efuse_exit(void)
{
	return platform_driver_unregister(&mtk_efuse_driver);
}

subsys_initcall(mtk_efuse_init);
module_exit(mtk_efuse_exit);

MODULE_AUTHOR("Andrew-CT Chen <andrew-ct.chen@mediatek.com>");
MODULE_DESCRIPTION("Mediatek EFUSE driver");
MODULE_LICENSE("GPL v2");
