#pragma once

/* -----------------------------------------------------------------------
 * ps5-autoloader — version & config
 * ----------------------------------------------------------------------- */

#define AUTOLOADER_VERSION "0.1.4"

/* Port that elfldr (socksrv) listens on for incoming ELF payloads */
#define ELFLDR_PORT 9021

/* Config file looked for inside each search base */
#define AUTOLOAD_CONFIG_SUBPATH "ps5_autoloader/autoload.txt"

/* Fallback search base when no USB config is found */
#define DATA_BASE "/data"

/* USB mount bases — searched in order, highest priority first */
static const char * const USB_BASES[] = {
    "/mnt/usb0", "/mnt/usb1", "/mnt/usb2", "/mnt/usb3",
    "/mnt/usb4", "/mnt/usb5", "/mnt/usb6", "/mnt/usb7"
};
#define USB_COUNT 8

/* Applications that can be auto-killed directly via SIGKILL on eboot.bin */
static const char * const AUTOKILL_EBOOT_TITLE_IDS[] = {
    /* YouTube app title IDs (PPSA01650–01652 cover all regional variants) */
    "PPSA01650", "PPSA01651", "PPSA01652",

    /* Artemis Lua games title IDs */
    "CUSA17122", /* Aerial Life */
    "CUSA17068", /* Aibeya */
    "CUSA19556", /* Aikagi 2 */
    "CUSA16229", /* Aikagi Kimi to Issho ni Pack */
    "CUSA29745", /* Fuyu Kiss */
    "CUSA29746", /* Fuyu Kiss Demo */
    "CUSA27389", /* Hamidashi Creative */
    "CUSA27390", /* Hamidashi Creative Demo */
    "CUSA14324", /* Haruoto Alice Gram Snow Drop */
    "CUSA17112", /* IxSHE Tell */
    "CUSA17126", /* IxSHE Tell Demo */
    "CUSA25179", /* Jinki Resurrection */
    "CUSA25180", /* Jinki Resurrection Demo */
    "CUSA11481", /* Mikagami Sumika no Seifuku Katsudou */
    "CUSA13586", /* Nora Princess and Crying Cat 2 */
    "CUSA13303", /* Nora Princess and Stray Cat Heart HD */
    "CUSA16074", /* Raspberry Cube */
    "CUSA11977", /* Tonari ni Kanojo no Iru Shiawase Winter Guest */

    /* Yarpe games title IDs */
    "CUSA30428", /* A YEAR OF SPRINGS PS4 */
    "CUSA30429", /* A YEAR OF SPRINGS PS4 */
    "CUSA30430", /* A YEAR OF SPRINGS PS4 */
    "CUSA30431", /* A YEAR OF SPRINGS PS4 */
    "CUSA32096", /* Arcade Spirits: The New Challengers PS4 */
    "CUSA32097", /* Arcade Spirits: The New Challengers PS4 */
    "PPSA06409", /* Arcade Spirits: The New Challengers PS5 */
    "PPSA06410", /* Arcade Spirits: The New Challengers PS5 */

    /* mast1c0re games title IDs */
    "CUSA02199", /* Okage: Shadow King (US) */
    "CUSA02282", /* Okage: Shadow King (EU) */
    "CUSA03474", /* Star Wars Racer Revenge (US) */
    "CUSA03492", /* Star Wars Racer Revenge (EU) */
    "CUSA07842", /* Jak X: Combat Racing (US) */
    "CUSA07992"  /* Jak X: Combat Racing (EU) */
};
#define AUTOKILL_EBOOT_TITLE_ID_COUNT (sizeof(AUTOKILL_EBOOT_TITLE_IDS) / sizeof(AUTOKILL_EBOOT_TITLE_IDS[0]))

/* BD Disc Player title ID and process name */
#define DISC_PLAYER_TITLE_ID "NPXS40140"
#define DISC_PLAYER_PROCESS  "SceDiscPlayer"

/* WebKit web process name */
#define BROWSER_PROCESS      "SceNKWebProcess"

/* How long to poll for elfldr readiness before giving up */
#define ELFLDR_WAIT_RETRIES  50
#define ELFLDR_WAIT_DELAY_US 200000   /* 200 ms */
