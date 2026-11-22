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
/* the DAC's ioctls are made by a thread of their own: waits, at most ms, until
 * it has made every one queued so far (true if it did); not from SIGIO */
bool shanlingq2_dac_sync(int ms);
/* pcm-alsa: a Bluetooth headset is back, so the writer opens the PCM again;
 * pending until it has settled on a device */
void pcm_alsa_reattach(void);
bool pcm_alsa_reattach_pending(void);
/* misc.c has set the volume of this output (Bluetooth or not): ends the silence */
void shanlingq2_volume_ack(bool bt);
/* to misc.c, which acts for the output in use (data 1 for a Bluetooth
 * headset, 0 the DAC): a MISC id upstream won't reach */
#define SYS_OUTPUT_CHANGED MAKE_SYS_EVENT(SYS_EVENT_CLS_MISC, 0x51)
/* Sound Settings' Control Device Volume: Rockbox's volume sets a Bluetooth
 * headset's own; and whether the headset carries it now (the skin then shows
 * its position) */
void shanlingq2_set_device_volume(bool on);
bool shanlingq2_device_volume(void);
/* button-q2: the latest event for the main thread (SYS_OUTPUT_CHANGED, ...), if any */
bool shanlingq2_take_event(long *id, intptr_t *data);

#endif
