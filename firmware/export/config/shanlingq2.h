/*
 * This config file is for the Shanling Q2 (hosted on the stock Linux)
 */

/* For Rolo and boot loader */
#define MODEL_NUMBER 126

#define MODEL_NAME   "Shanling Q2"

/* LCD dimensions: the 320x375 panel, used landscape */
#define LCD_WIDTH  375
#define LCD_HEIGHT 320
/* sqrt(375^2 + 320^2) / 3.5 = 141 */
#define LCD_DPI 141

#ifndef SIMULATOR
#define CONFIG_PLATFORM (PLATFORM_HOSTED)
#define PIVOT_ROOT "/mnt/mmc"
#endif

#define HAVE_FPU

#define HW_SAMPR_CAPS (SAMPR_CAP_44 | SAMPR_CAP_48 | SAMPR_CAP_88 | SAMPR_CAP_96 | SAMPR_CAP_176 | SAMPR_CAP_192)

/* define this if you have a colour LCD */
#define HAVE_LCD_COLOR

/* define this if you want album art for this target */
#define HAVE_ALBUMART

/* define this to enable bitmap scaling */
#define HAVE_BMP_SCALING

/* define this to enable JPEG decoding */
#define HAVE_JPEG

/* define this if you have access to the quickscreen */
#define HAVE_QUICKSCREEN

/* define this if you would like tagcache to build on this target */
#define HAVE_TAGCACHE

#define LCD_DEPTH  32
#define LCD_PIXELFORMAT XRGB8888

#define CONFIG_LCD LCD_INGENIC_LINUX

/* The glass hides the panel's corners (about 80 px radius): lists keep clear
 * of them. x, y, width, height, font (1: the user's), and the user's colours;
 * sb_create_from_settings ignores a setting without all seven */
#define DEFAULT_UI_VIEWPORT "16,32,343,256,1,-,-"

/* Define this if the LCD can be toggled */
#define HAVE_LCD_ENABLE

#define HAVE_BACKLIGHT
#define HAVE_BACKLIGHT_BRIGHTNESS
/* the key that wakes the screen (often the wheel's centre) only wakes it */
#define DEFAULT_BL_FILTER_FIRST_KEYPRESS true
/* 1..20 is stock's brightness 5..100 (backlight-q2.c); 10 is stock's default */
#define MIN_BRIGHTNESS_SETTING     1
#define MAX_BRIGHTNESS_SETTING     20
#define DEFAULT_BRIGHTNESS_SETTING 10

/* define this if you have a real-time clock */
#define CONFIG_RTC RTC_HOSTED

/* The number of bytes reserved for loadable codecs */
#define CODEC_SIZE 0x100000

/* The number of bytes reserved for loadable plugins */
#define PLUGIN_BUFFER_SIZE 0x200000

/* KeyPad configuration for plugins: the iPod's buttons and wheel */
#define CONFIG_KEYPAD IPOD_4G_PAD
#define HAVE_SCROLLWHEEL

/* Linux handles USB; Rockbox leaves it alone */
#ifndef SIMULATOR
#define USB_NONE
#endif

#define CONFIG_BATTERY_MEASURE PERCENTAGE_MEASURE

/* Linux controls charging, we can monitor */
#define CONFIG_CHARGING CHARGING_MONITOR

/* Define this if you have a software controlled poweroff */
#define HAVE_SW_POWEROFF

/* "power off" returns to the stock player, so it is fine while charging */
#define HAVE_POWEROFF_WHILE_CHARGING

/* Define this to the CPU frequency */
#define CPU_FREQ            1200000000

/* The microSD card is the only storage */
#define CONFIG_STORAGE STORAGE_HOSTFS
#define HAVE_STORAGE_FLUSH

/* Audio codec */
#define HAVE_SHANLINGQ2_CODEC
#define HAVE_ALSA_32BIT
/* low gain unless asked: safer for sensitive earphones */
#define TARGET_DEFAULT_DAC_POWER_MODE SOUND_LOW_POWER

/* We don't have hardware controls */
#define HAVE_SW_TONE_CONTROLS

/* Battery */
#define BATTERY_CAPACITY_DEFAULT 1500 /* default battery capacity */
#define BATTERY_CAPACITY_MIN 1500  /* min. capacity selectable */
#define BATTERY_CAPACITY_MAX 1500 /* max. capacity selectable */
#define BATTERY_CAPACITY_INC 0   /* capacity increment */

/* ROLO */
#define BOOTFILE_EXT "q2"
#define BOOTFILE     "rockbox." BOOTFILE_EXT
#define BOOTDIR      "/.rockbox"
