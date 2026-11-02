#ifndef __SHANLINGQ2_CODEC__
#define __SHANLINGQ2_CODEC__

/* "DAC power mode" is the amp's gain: high or low, as stock's Gain */
#define AUDIOHW_CAPS (POWER_MODE_CAP | FILTER_ROLL_OFF_CAP)
#define AUDIOHW_HAVE_SHORT2_ROLL_OFF

/* volume in software, through pcm-alsa's mixer */
AUDIOHW_SETTING(VOLUME, "dB", 0, 1, -100, 0, -30)
AUDIOHW_SETTING(POWER_MODE, "", 0, 1, 0, 1, 1)
/* default 3: stock's Low Delay Slow, shown as "Short Slow" */
AUDIOHW_SETTING(FILTER_ROLL_OFF, "", 0, 1, 0, 3, 3)

#define AUDIOHW_MUTE_ON_STOP

void audiohw_mute(int mute);
/* a Bluetooth headset is the output, not the DAC */
bool shanlingq2_bt_output(void);
/* pcm-alsa: playback is about to start, or has stopped (maybe in SIGIO) */
void shanlingq2_dac_wake(void);
void shanlingq2_dac_idle(void);

#endif
