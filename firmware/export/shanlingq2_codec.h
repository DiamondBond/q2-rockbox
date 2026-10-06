#ifndef __SHANLINGQ2_CODEC__
#define __SHANLINGQ2_CODEC__

/* "DAC power mode" is the amp's gain: high or low, as stock's Gain */
#define AUDIOHW_CAPS (POWER_MODE_CAP | FILTER_ROLL_OFF_CAP)
#define AUDIOHW_HAVE_SHORT2_ROLL_OFF

/* volume in software, through pcm-alsa's mixer */
AUDIOHW_SETTING(VOLUME, "dB", 0, 1, -100, 0, -30)
AUDIOHW_SETTING(POWER_MODE, "", 0, 1, 0, 1, 1)
AUDIOHW_SETTING(FILTER_ROLL_OFF, "", 0, 1, 0, 3, 0)

#define AUDIOHW_MUTE_ON_STOP

void audiohw_mute(int mute);

#endif
