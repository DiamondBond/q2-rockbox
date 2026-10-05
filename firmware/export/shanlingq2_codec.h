#ifndef __SHANLINGQ2_CODEC__
#define __SHANLINGQ2_CODEC__

/* volume in software, through pcm-alsa's mixer */
AUDIOHW_SETTING(VOLUME, "dB", 0, 1, -100, 0, -30)

#define AUDIOHW_MUTE_ON_STOP

void audiohw_mute(int mute);

#endif
