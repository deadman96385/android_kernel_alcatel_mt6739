



#include <linux/delay.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/pm_runtime.h>
#include <sound/core.h>
#include <sound/soc.h>
#include <sound/soc-dapm.h>
#include <sound/pcm.h>

#if defined(CONFIG_SND_SOC_CS43130)
static int cs43130_startup(struct snd_pcm_substream *substream)
{
	return 0;
}


static int cs43130_hw_params(struct snd_pcm_substream *substream,
				struct snd_pcm_hw_params *params)
{
	struct snd_soc_pcm_runtime *rtd = substream->private_data;
	struct snd_soc_codec *codec = rtd[0].codec;
	struct snd_soc_dai *codec_dai = rtd[0].codec_dai;
	struct snd_soc_dai_link *dai_link = rtd[0].dai_link;
	int ret;

	dev_info(codec_dai->dev, "%s:%s:%d+ on codec_dai %s\n",
		__FILE__, __func__, __LINE__, codec_dai->name);

	ret = snd_soc_dai_set_fmt(codec_dai, dai_link->dai_fmt);
	if (ret != 0) {
		dev_err(codec_dai->dev, "Failed to set %s's dai_fmt: ret = %d",
			codec_dai->name, ret);
		return ret;
	}

	ret = snd_soc_dai_set_sysclk(codec_dai, 0, 3072000, SND_SOC_CLOCK_IN);
	if (ret != 0) {
		dev_err(codec_dai->dev, "Failed to set %s's clock: ret = %d\n",
			codec_dai->name, ret);
		return ret;
	}

	ret = snd_soc_codec_set_sysclk(codec, 0, 0, 22579200, SND_SOC_CLOCK_IN);
	if (ret != 0) {
		dev_err(codec_dai->dev, "Failed to set %s's clock: ret = %d\n",
			codec_dai->name, ret);
		return ret;
	}

	dev_info(codec_dai->dev, "%s:%s:%d- with ret %d\n",
		__FILE__, __func__, __LINE__, ret);

	return ret;
}


static int cs43130_free(struct snd_pcm_substream *substream)
{
	return 0;
}

const struct snd_soc_ops cs43130_ops = {
	.startup = cs43130_startup,
	.hw_params = cs43130_hw_params,
	.hw_free = cs43130_free,
};
EXPORT_SYMBOL_GPL(cs43130_ops);

#endif /* #if defined(CONFIG_SND_SOC_CS43130) */


