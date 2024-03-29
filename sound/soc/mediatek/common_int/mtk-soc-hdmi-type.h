


#ifndef _AUDIO_HDMI_TYPE_H
#define _AUDIO_HDMI_TYPE_H

#include <linux/list.h>

enum hdmi_display_type {
	HDMI_DISPLAY_MHL = 0,
	HDMI_DISPLAY_SILMPORT = 1,
};

struct audio_hdmi_format {
	unsigned char mHDMI_DisplayType;
	unsigned int mI2Snum;
	unsigned int mI2S_MCKDIV;
	unsigned int mI2S_BCKDIV;

	unsigned int mHDMI_Samplerate;
	unsigned int mHDMI_Channels; /* channel number HDMI transmitted */
	unsigned int mHDMI_Data_Lens;
	unsigned int mTDM_Data_Lens;
	unsigned int mClock_Data_Lens;
	unsigned int mTDM_LRCK;
	unsigned int msDATA_Channels; /* channel number per sdata */
	unsigned int mMemIfFetchFormatPerSample;
	bool mSdata0;
	bool mSdata1;
	bool mSdata2;
	bool mSdata3;
};

#endif
