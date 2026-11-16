#include "osd_default.h"

#include "battery.h"
#include "menu_mgr.h"
#include "tab_shared.h"
#include "tab_list.h"
#include "tab_dots.h"
#include "tab_table.h"
#include "status/fw.h"
#include "status/brightness.h"
#include "controls/dpad_ctl.h"
#include "controls/hotkeys.h"
#include "palette/gbc_color_temp.h"
#include "palette/style.h"
#include "display/color_correct_lcd.h"
#include "display/color_correct_usb.h"
#include "display/frameblend.h"
#include "display/low_batt_icon_ctl.h"
#include "display/screen_transit_ctl.h"
#include "system/silent.h"
#include "system/player_num.h"
#include "system/serial_num.h"
#include "system/wifi_link.h"
#include "system/wifi_net.h"
#include "system/network.h"
#include "system/timeset.h"
#include "system/download.h"
#include "system/update.h"
#include "system/custom.h"
#include "system/play.h"
#include "system/games.h"
#include "system/getgames.h"
#include "sdkconfig.h"
#include "system/online.h"
#include "osd.h"
#include "osd_shared.h"
#include "esp_log.h"

#include <stdint.h>
#include <stdio.h>
#include "nw_dev.h"
#include "button.h"
#include "esp_timer.h"

// lane chromshlink1: the room menu, opened for a game (from the nw task).
static esp_timer_handle_t _MenuTapTimer;
static void MenuTapRelease(void *arg)
{
    (void)arg;
    if (!Button_Poke(0))
        printf("[%u ms] osd: MENU release queue failed\n", (unsigned)esp_log_timestamp());
}
void OSD_ReopenCurrent(void)
{
    if (OSD_IsVisible()) return;
    // The FPGA owns the OSD: a MENU tap opens it, as the button does (on the
    // release). The MENU button is poke bit 8 (button_poke.h).
    if (_MenuTapTimer == NULL)
    {
        const esp_timer_create_args_t a = { .callback = MenuTapRelease, .name = "osd_tap" };
        if (esp_timer_create(&a, &_MenuTapTimer) != ESP_OK) return;
    }
    if (!Button_Poke(kButtonPoke_Menu)) return;
    const esp_err_t err = esp_timer_start_once(_MenuTapTimer, 120 * 1000);
    if (err != ESP_OK)
    {
        printf("[%u ms] osd: MENU release timer failed (%d)\n", (unsigned)esp_log_timestamp(), (int)err);
        MenuTapRelease(NULL);
    }
}

static void OpenPage(const char *Item)
{
    MenuMgr_ShowItem(kTabID_System, Item);
    OSD_ReopenCurrent();
}

static void OpenRoomMenu(void)
{
    Network_RequestRoomMenu();
    OpenPage("NETWORK");
}

static const char *TAG = "OSDDef";

LV_IMG_DECLARE(menu_status);
LV_IMG_DECLARE(menu_display);
LV_IMG_DECLARE(menu_controls);
LV_IMG_DECLARE(menu_palette);
LV_IMG_DECLARE(menu_palette_gbc);
LV_IMG_DECLARE(menu_colortemp);
LV_IMG_DECLARE(menu_system);
LV_IMG_DECLARE(menu_games);

static void CreateMenuStatus(lv_obj_t *const pScreen);
static void CreateMenuDisplay(lv_obj_t *const pScreen);
static void CreateMenuControls(lv_obj_t *const pScreen);
static void CreateMenuPalette(lv_obj_t *const pScreen);
static void CreateMenuColorTemp(lv_obj_t *const pScreen);
static void CreateMenuSystem(lv_obj_t *const pScreen);
static void CreateMenuGames(lv_obj_t *const pScreen);
static bool IsPaletteTabActive(void);
static bool IsColorTempTabActive(void);
static bool IsColorTempGBTabActive(void);

void OSD_Default_Init(lv_obj_t *const pScreen)
{
    static OSD_Widget_t MenuMgr = {
        .Name = "MenuMgr",
    };

    OSD_Result_t eResult = MenuMgr_Initialize(&MenuMgr, pScreen);

    if (eResult != kOSD_Result_Ok)
    {
        ESP_LOGE(TAG, "Menu manager init failed: %d", eResult);
        return;
    }

    static OSD_Widget_t Battery = {
        .Name = "Battery",
    };

    if ((eResult = Battery_Init(&Battery)) != kOSD_Result_Ok)
    {
        ESP_LOGE(TAG, "Battery widget init failed %d", eResult);
        return;
    }

    static OSD_Widget_t LinkToast = {
        .Name = "LinkToast",
    };
    (void)LinkToast_Init(&LinkToast);

    CreateMenuStatus(pScreen);
    CreateMenuDisplay(pScreen);
    CreateMenuControls(pScreen);
    CreateMenuPalette(pScreen);
    CreateMenuColorTemp(pScreen);
    CreateMenuSystem(pScreen);
    CreateMenuGames(pScreen);

    OSD_AddWidget(&Battery);
    OSD_AddWidget(&MenuMgr);
    OSD_AddWidget(&LinkToast);   // after the menus: drawn on top

    ESP_LOGI(TAG, "Default OSD init OK");
}

static void CreateMenuStatus(lv_obj_t *const pScreen)
{
    static TabCollection_t StatusList;
    TabCollection_t *pList = &StatusList;
    OSD_Result_t eResult;

    sys_dlist_init(&StatusList.WidgetList);

    static MenuTab_t Tab_Status = {
        .Widget = {
            .Name = "MenuStatus",
            .fnDraw = TabList_Draw,
            .fnOnButton = Tab_OnButton,
            .fnOnTransition =  TabList_OnTransition,
        },
        .pImageDesc = &menu_status,
        .Menu = &StatusList
    };
    Tab_Status.Accent = lv_color_make(0xFF, 0, 0);

    if ((eResult = MenuMgr_AddTab(kTabID_Status, &Tab_Status)) != kOSD_Result_Ok)
    {
        ESP_LOGE(TAG, "%s widget init failed %d", Tab_Status.Widget.Name, eResult);
        return;
    }

    static TabItem_t Brightness = {
        .Widget = {
            .Name = "BRIGHTNESS",
            .fnDraw = Brightness_Draw,
            .fnOnTransition = Brightness_OnTransition,
            .fnOnButton = Brightness_OnButton,
        },
    };

    if ((eResult = Tab_AddItem(pList, &Brightness, pScreen)) != kOSD_Result_Ok)
    {
        ESP_LOGE(TAG, "%s tab item init failed %d", Tab_Status.Widget.Name, eResult);
        return;
    }

    static TabItem_t SilentMode = {
        .Widget = {
            .Name = "SILENT MODE",
            .fnDraw = SilentMode_Draw,
            .fnOnButton = SilentMode_OnButton,
            .fnOnTransition = SilentMode_OnTransition,
        },
    };

    if ((eResult = Tab_AddItem(pList, &SilentMode, pScreen)) != kOSD_Result_Ok)
    {
        ESP_LOGE(TAG, "%s tab item init failed %d", SilentMode.Widget.Name, eResult);
        return;
    }

}

static void CreateMenuDisplay(lv_obj_t *const pScreen)
{
    static TabCollection_t DisplayList;
    TabCollection_t *pList = &DisplayList;
    OSD_Result_t eResult;

    sys_dlist_init(&DisplayList.WidgetList);

    static MenuTab_t Tab_Display = {
        .Widget = {
            .Name = "MenuDisplay",
            .fnDraw = TabList_Draw,
            .fnOnButton = Tab_OnButton,
            .fnOnTransition =  TabList_OnTransition,
        },
        .pImageDesc = &menu_display,
        .Menu = &DisplayList,
    };
    Tab_Display.Accent = lv_color_make(0xFF, 0xFF, 0);

    if ((eResult = MenuMgr_AddTab(kTabID_Display, &Tab_Display)) != kOSD_Result_Ok)
    {
        ESP_LOGE(TAG, "%s widget init failed %d", Tab_Display.Widget.Name, eResult);
        return;
    }

    static TabItem_t FrameBlending = {
        .Widget = {
            .Name = "FRAME BLEND",
            .fnDraw = FrameBlend_Draw,
            .fnOnButton = FrameBlend_OnButton,
            .fnOnTransition = FrameBlend_OnTransition,
        },
    };

    if ((eResult = Tab_AddItem(pList, &FrameBlending, pScreen)) != kOSD_Result_Ok)
    {
        ESP_LOGE(TAG, "%s tab item init failed %d", FrameBlending.Widget.Name, eResult);
        return;
    }

    #ifdef ENABLE_COLOR_CORRECTION
    static TabItem_t ColorCorrection_LCD = {
        .Widget = {
            .Name = "LCD COLOR CORR.",
            .fnDraw = ColorCorrectLCD_Draw,
            .fnOnButton = ColorCorrectLCD_OnButton,
            .fnOnTransition = ColorCorrectLCD_OnTransition,
        },
    };

    if ((eResult = Tab_AddItem(pList, &ColorCorrection_LCD, pScreen)) != kOSD_Result_Ok)
    {
        ESP_LOGE(TAG, "%s tab item init failed %d", ColorCorrection_LCD.Widget.Name, eResult);
        return;
    }
    #endif

    static TabItem_t ColorCorrection_USB = {
        .Widget = {
            .Name = "STREAMING",
            .fnDraw = ColorCorrectUSB_Draw,
            .fnOnButton = ColorCorrectUSB_OnButton,
            .fnOnTransition = ColorCorrectUSB_OnTransition,
        },
    };

    if ((eResult = Tab_AddItem(pList, &ColorCorrection_USB, pScreen)) != kOSD_Result_Ok)
    {
        ESP_LOGE(TAG, "%s tab item init failed %d", ColorCorrection_USB.Widget.Name, eResult);
        return;
    }

    static TabItem_t ScreenTransitCtl = {
        .Widget = {
            .Name = "TRANSITIONS",
            .fnDraw = ScreenTransitCtl_Draw,
            .fnOnButton = ScreenTransitCtl_OnButton,
            .fnOnTransition = ScreenTransitCtl_OnTransition,
        },
    };

    if ((eResult = Tab_AddItem(pList, &ScreenTransitCtl, pScreen)) != kOSD_Result_Ok)
    {
        ESP_LOGE(TAG, "%s tab item init failed %d", ScreenTransitCtl.Widget.Name, eResult);
        return;
    }

    static TabItem_t LowBattIconCtl = {
        .Widget = {
            .Name = "LOW BATTERY",
            .fnDraw = LowBattIconCtl_Draw,
            .fnOnButton = LowBattIconCtl_OnButton,
            .fnOnTransition = LowBattIconCtl_OnTransition,
        },
    };

    if ((eResult = Tab_AddItem(pList, &LowBattIconCtl, pScreen)) != kOSD_Result_Ok)
    {
        ESP_LOGE(TAG, "%s tab item init failed %d", LowBattIconCtl.Widget.Name, eResult);
        return;
    }
}

static void CreateMenuControls(lv_obj_t *const pScreen)
{
    static TabCollection_t ControlsList;
    TabCollection_t *pList = &ControlsList;

    sys_dlist_init(&ControlsList.WidgetList);
    static MenuTab_t Tab_Controls = {
        .Widget = {
            .Name = "MenuControls",
            .fnDraw = TabDot_Draw,
            .fnOnButton = Tab_OnButton,
            .fnOnTransition =  TabDot_OnTransition,
        },
        .Menu = &ControlsList,
        .pImageDesc = &menu_controls,
    };
    Tab_Controls.Accent = lv_color_make(0, 0xCC, 0xFF);

    OSD_Result_t eResult;

    static TabItem_t DPad = {
        .Widget = {
            .Name = "D-PAD",
            .fnDraw = DPadCtl_Draw,
            .fnOnTransition = DPadCtl_OnTransition,
            .fnOnButton = DPadCtl_OnButton,
        },
    };

    if ((eResult = Tab_AddItem(pList, &DPad, pScreen)) != kOSD_Result_Ok)
    {
        ESP_LOGE(TAG, "%s tab item init failed %d", DPad.Widget.Name, eResult);
        return;
    }

    static TabItem_t HotKeysBrightness = {
        .Widget = {
            .Name = "HK: BRIGHTNESS",
            .fnDraw = HotKeys_DrawBrightness,
            .fnOnTransition = HotKeys_OnTransition,
        },
    };

    if ((eResult = Tab_AddItem(pList, &HotKeysBrightness, pScreen)) != kOSD_Result_Ok)
    {
        ESP_LOGE(TAG, "%s tab item init failed %d", HotKeysBrightness.Widget.Name, eResult);
        return;
    }

    static TabItem_t HotKeysColorTemp = {
        .Widget = {
            .Name = "HK: PAL/COLOR TEMP",
            .fnDraw = HotKeys_DrawColorTemp,
            .fnOnTransition = HotKeys_OnTransition,
        },
    };

    if ((eResult = Tab_AddItem(pList, &HotKeysColorTemp, pScreen)) != kOSD_Result_Ok)
    {
        ESP_LOGE(TAG, "%s tab item init failed %d", HotKeysColorTemp.Widget.Name, eResult);
        return;
    }

    static TabItem_t HotKeysResetEmu = {
        .Widget = {
            .Name = "HK: RESET EMU",
            .fnDraw = HotKeys_DrawResetEmulation,
            .fnOnTransition = HotKeys_OnTransition,
        },
    };

    if ((eResult = Tab_AddItem(pList, &HotKeysResetEmu, pScreen)) != kOSD_Result_Ok)
    {
        ESP_LOGE(TAG, "%s tab item init failed %d", HotKeysResetEmu.Widget.Name, eResult);
        return;
    }

    if ((eResult = MenuMgr_AddTab(kTabID_Controls, &Tab_Controls)) != kOSD_Result_Ok)
    {
        ESP_LOGE(TAG, "%s widget init failed %d", Tab_Controls.Widget.Name, eResult);
        return;
    }

}

static void CreateMenuPalette(lv_obj_t *const pScreen)
{
    static TabCollection_t PaletteListGB;
    static TabItem_t PaletteOpts[kNumPalettes];
    static MenuTab_t Tab_Palette = {
        .Widget = {
            .Name = "MenuPalette",
            .fnDraw = TabTable_Draw,
            .fnOnButton = Tab_OnButton,
            .fnOnTransition =  TabList_OnTransition,  // TabTable has same object lifetime behavior as TabList
        },
        .pImageDesc = &menu_palette,
        .Menu = &PaletteListGB,
        .fnIsActive = IsPaletteTabActive,
    };

    OSD_Result_t eResult;

    sys_dlist_init(&PaletteListGB.WidgetList);

    Tab_Palette.Accent = lv_color_make(0x92, 0x4E, 0xD7);  // Unused in actual selection

    const char *PaletteStyleNames[kNumPalettes] = {
        [kPalette_Default]   = "DEFAULT",
        [kPalette_Brown]     = "BROWN",          // Up
        [kPalette_Blue]      = "BLUE",           // Left
        [kPalette_Pastel]    = "PASTEL",         // Down
        [kPalette_Green]     = "GREEN",          // Right
        [kPalette_Red]       = "RED",            // Up + A
        [kPalette_DarkBlue]  = "DARK BLUE",      // Left + A 
        [kPalette_Orange]    = "ORANGE",         // Down + A
        [kPalette_DarkGreen] = "DARK GREEN",     // Right + A
        [kPalette_DarkBrown] = "DARK BROWN",     // Up + B
        [kPalette_Grayscale] = "GRAYSCALE",      // Left + B
        [kPalette_Yellow]    = "YELLOW",         // Down + B
        [kPalette_Negative]  = "NEGATIVE",       // Right + B
        [kPalette_DMG1]      = "DMG1: GAMEBOY",  // Right + A + B
        [kPalette_DMG2]      = "DMG2: GAMEBOY"   // Left + A + B
    };

    for (StyleID_t ID = 0; ID < kNumPalettes; ID++)
    {
        PaletteOpts[ID] = (TabItem_t){
            .Widget = {
                .Name = PaletteStyleNames[ID],
                .fnDraw = Style_Draw,
                .fnOnButton = Style_OnButton,
                .fnOnTransition = Style_OnTransition,
            },
        };

        if ((eResult = Tab_AddItem(&PaletteListGB, &PaletteOpts[ID], pScreen)) != kOSD_Result_Ok)
        {
            ESP_LOGE(TAG, "%s tab item init failed %d", PaletteOpts[ID].Widget.Name, eResult);
            return;
        }
    }

    if ((eResult = MenuMgr_AddTab(kTabID_Palette, &Tab_Palette)) != kOSD_Result_Ok)
    {
        ESP_LOGE(TAG, "%s widget init failed %d", Tab_Palette.Widget.Name, eResult);
        return;
    }
}

// One COLOR TEMP setting for every game. A Game Boy Color game has the stock page (no PALETTE of its own, so the
// picture's header says PALETTE); a Game Boy game has PALETTE and, right after it, this page under its own header
// text COLOR TEMP (menu_colortemp). Only one of the two is ever active, so they share the page's widget.
static void AddColorTempTab(lv_obj_t *const pScreen, TabCollection_t *const pList, TabItem_t *const pItem,
                            MenuTab_t *const pTab, const TabID_t eID)
{
    OSD_Result_t eResult;

    sys_dlist_init(&pList->WidgetList);

    pTab->Accent = lv_color_make(0x92, 0x4E, 0xD7);

    *pItem = (TabItem_t){
        .Widget = {
            .Name = "COLOR TEMP",
            .fnDraw = GBCColorTemp_Draw,
            .fnOnButton = GBCColorTemp_OnButton,
            .fnOnTransition = GBCColorTemp_OnTransition,
        },
    };

    if ((eResult = Tab_AddItem(pList, pItem, pScreen)) != kOSD_Result_Ok)
    {
        ESP_LOGE(TAG, "%s tab item init failed %d", pItem->Widget.Name, eResult);
        return;
    }

    if ((eResult = MenuMgr_AddTab(eID, pTab)) != kOSD_Result_Ok)
    {
        ESP_LOGE(TAG, "%s widget init failed %d", pTab->Widget.Name, eResult);
        return;
    }
}

static void CreateMenuColorTemp(lv_obj_t *const pScreen)
{
    static TabCollection_t ColorTempList;
    static TabItem_t ColorTempItem;
    static MenuTab_t Tab_ColorTemp = {
        .Widget = {
            .Name = "MenuColorTemp",
            .fnDraw = TabList_Draw,
            .fnOnButton = Tab_OnButton,
            .fnOnTransition = TabList_OnTransition,
        },
        .pImageDesc = &menu_palette_gbc,
        .Menu = &ColorTempList,
        .fnIsActive = IsColorTempTabActive,
    };
    AddColorTempTab(pScreen, &ColorTempList, &ColorTempItem, &Tab_ColorTemp, kTabID_ColorTemp);

    static TabCollection_t ColorTempListGB;
    static TabItem_t ColorTempItemGB;
    static MenuTab_t Tab_ColorTempGB = {
        .Widget = {
            .Name = "MenuColorTempGB",
            .fnDraw = TabList_Draw,
            .fnOnButton = Tab_OnButton,
            .fnOnTransition = TabList_OnTransition,
        },
        .pImageDesc = &menu_colortemp,
        .Menu = &ColorTempListGB,
        .fnIsActive = IsColorTempGBTabActive,
    };
    AddColorTempTab(pScreen, &ColorTempListGB, &ColorTempItemGB, &Tab_ColorTempGB, kTabID_ColorTempGB);
}

static bool IsPaletteTabActive(void)
{
    return !Style_IsGBCMode();
}

static bool IsColorTempTabActive(void)
{
    return Style_IsGBCMode();
}

static bool IsColorTempGBTabActive(void)
{
    return !Style_IsGBCMode();
}

static void CreateMenuSystem(lv_obj_t *const pScreen)
{
    static TabCollection_t SystemList;
    TabCollection_t *pList = &SystemList;
    OSD_Result_t eResult;

    sys_dlist_init(&SystemList.WidgetList);

    static MenuTab_t Tab_System = {
        .Widget = {
            .Name = "MenuSystem",
            .fnDraw = TabList_Draw,
            .fnOnButton = Tab_OnButton,
            .fnOnTransition = TabList_OnTransition,
        },
        .pImageDesc = &menu_system,
        .Menu = &SystemList,
    };
    Tab_System.Accent = lv_color_make(0xFF, 0x33, 0x99);

    static TabItem_t Firmware = {
        .Widget = {
            .Name = "FIRMWARE",
            .fnDraw = Firmware_Draw,
            .fnOnTransition = Firmware_OnTransition,
            .fnOnButton = Firmware_OnButton,
        },
    };

    if ((eResult = Tab_AddItem(pList, &Firmware, pScreen)) != kOSD_Result_Ok)
    {
        ESP_LOGE(TAG, "%s tab item init failed %d", Firmware.Widget.Name, eResult);
        return;
    }

    static TabItem_t PlayerNum = {
        .Widget = {
            .Name = "PLAYER #",
            .fnDraw = PlayerNum_Draw,
            .fnOnButton = PlayerNum_OnButton,
            .fnOnTransition = PlayerNum_OnTransition,
        },
    };

    if ((eResult = Tab_AddItem(pList, &PlayerNum, pScreen)) != kOSD_Result_Ok)
    {
        ESP_LOGE(TAG, "%s tab item init failed %d", PlayerNum.Widget.Name, eResult);
        return;
    }

    static TabItem_t WifiLink = {
        .Widget = {
            .Name = "LINK  CABLE",
            .fnDraw = WifiLink_Draw,
            .fnOnButton = WifiLink_OnButton,
            .fnOnTransition = WifiLink_OnTransition,
        },
    };

    if ((eResult = Tab_AddItem(pList, &WifiLink, pScreen)) != kOSD_Result_Ok)
    {
        ESP_LOGE(TAG, "%s tab item init failed %d", WifiLink.Widget.Name, eResult);
        return;
    }

    static TabItem_t Online = {
        .Widget = {
            .Name = "ONLINE",
            .fnDraw = Online_Draw,
            .fnOnButton = Online_OnButton,
            .fnOnTransition = Online_OnTransition,
        },
    };

    if ((eResult = Tab_AddItem(pList, &Online, pScreen)) != kOSD_Result_Ok)
    {
        ESP_LOGE(TAG, "%s tab item init failed %d", Online.Widget.Name, eResult);
        return;
    }

    static TabItem_t WifiNet = {
        .Widget = {
            .Name = "WI-FI",
            .fnDraw = WifiNet_Draw,
            .fnOnButton = WifiNet_OnButton,
            .fnOnTransition = WifiNet_OnTransition,
        },
    };

    if ((eResult = Tab_AddItem(pList, &WifiNet, pScreen)) != kOSD_Result_Ok)
    {
        ESP_LOGE(TAG, "%s tab item init failed %d", WifiNet.Widget.Name, eResult);
        return;
    }

    // 1.1.7: the date and time by hand when Wi-Fi gave none (the game clocks are caught up by it)
    static TabItem_t TimeSet = {
        .Widget = {
            .Name = "CLOCK",
            .fnDraw = TimeSet_Draw,
            .fnOnButton = TimeSet_OnButton,
            .fnOnTransition = TimeSet_OnTransition,
        },
    };

    if ((eResult = Tab_AddItem(pList, &TimeSet, pScreen)) != kOSD_Result_Ok)
    {
        ESP_LOGE(TAG, "%s tab item init failed %d", TimeSet.Widget.Name, eResult);
        return;
    }

    static TabItem_t Network = {
        .Widget = {
            .Name = "NETWORK",
            .fnDraw = Network_Draw,
            .fnOnButton = Network_OnButton,
            .fnOnTransition = Network_OnTransition,
        },
    };

    if ((eResult = Tab_AddItem(pList, &Network, pScreen)) != kOSD_Result_Ok)
    {
        ESP_LOGE(TAG, "%s tab item init failed %d", Network.Widget.Name, eResult);
        return;
    }
#ifdef CONFIG_NW_CARTFLASH_WIFI
    // lane ui110dev (DESIGN.md): GAMES (every game, one list: the cartridge, ON CHROMATIC, the card; the old card GAMES
    // page and the PLAY page merged), GET GAMES (the free games from our server), DOWNLOAD (the receive code), UPDATE
    static TabItem_t Games = {
        .Widget = {
            .Name = "GAMES",
            .fnDraw = Games_Draw,
            .fnOnButton = Games_OnButton,
            .fnOnTransition = Games_OnTransition,
        },
    };

    if ((eResult = Tab_AddItem(pList, &Games, pScreen)) != kOSD_Result_Ok)
    {
        ESP_LOGE(TAG, "%s tab item init failed %d", Games.Widget.Name, eResult);
        return;
    }
    static TabItem_t Core = {
        .Widget = {
            .Name = "CORE",
            .fnDraw = Core_Draw,
            .fnOnButton = Core_OnButton,
            .fnOnTransition = Games_OnTransition,
        },
    };
    if ((eResult = Tab_AddItem(pList, &Core, pScreen)) != kOSD_Result_Ok) {
        ESP_LOGE(TAG, "%s tab item init failed %d", Core.Widget.Name, eResult);
        return;
    }
    Games_Register();
    Play_Register();                     // `ppage`: a game loading / its result

    static TabItem_t GetGames = {
        .Widget = {
            .Name = "GET  GAMES",
            .fnDraw = GetGames_Draw,
            .fnOnButton = GetGames_OnButton,
            .fnOnTransition = GetGames_OnTransition,
        },
    };

    if ((eResult = Tab_AddItem(pList, &GetGames, pScreen)) != kOSD_Result_Ok)
    {
        ESP_LOGE(TAG, "%s tab item init failed %d", GetGames.Widget.Name, eResult);
        return;
    }
    GetGames_Register();

    // wifirecv1: one action: RECEIVE GAME ("Send to my Chromatic" by a code)
    static TabItem_t Download = {
        .Widget = {
            .Name = "DOWNLOAD",
            .fnDraw = Download_Draw,
            .fnOnButton = Download_OnButton,
            .fnOnTransition = Download_OnTransition,
        },
    };

    if ((eResult = Tab_AddItem(pList, &Download, pScreen)) != kOSD_Result_Ok)
    {
        ESP_LOGE(TAG, "%s tab item init failed %d", Download.Widget.Name, eResult);
        return;
    }
    // 1.1.0: Shiny Relay's own update over Wi-Fi (A: CHECK FOR UPDATE)
    static TabItem_t Update = {
        .Widget = {
            .Name = "UPDATE",
            .fnDraw = Update_Draw,
            .fnOnButton = Update_OnButton,
            .fnOnTransition = Update_OnTransition,
        },
    };

    if ((eResult = Tab_AddItem(pList, &Update, pScreen)) != kOSD_Result_Ok)
    {
        ESP_LOGE(TAG, "%s tab item init failed %d", Update.Widget.Name, eResult);
        return;
    }
    // 1.1.12: custom firmware from shinyrelay.com/custom-firmware (UPDATE is the official releases only)
    static TabItem_t Custom = {
        .Widget = {
            .Name = "CUSTOM",
            .fnDraw = Custom_Draw,
            .fnOnButton = Custom_OnButton,
            .fnOnTransition = Custom_OnTransition,
        },
    };

    if ((eResult = Tab_AddItem(pList, &Custom, pScreen)) != kOSD_Result_Ok)
    {
        ESP_LOGE(TAG, "%s tab item init failed %d", Custom.Widget.Name, eResult);
        return;
    }
    Custom_Register();                   // `cpage`
#endif
    // A game's "play online" (SHLINK lk_ask, lane chromshlink1) opens the OSD on this item.
    NW_SetRoomMenuOpener(OpenRoomMenu);
    // 1.0.2: a game's own screens open SYSTEM > WI-FI (LINK_UI)
    NW_SetPageOpener(OpenPage);

    if (SerialNum_IsPresent())
    {
        static TabItem_t SerialNumber = {
            .Widget = {
                .Name = "SERIAL #",
                .fnDraw = SerialNum_Draw,
                .fnOnTransition = SerialNum_OnTransition,
            },
        };

        if ((eResult = Tab_AddItem(pList, &SerialNumber, pScreen)) != kOSD_Result_Ok)
        {
            ESP_LOGE(TAG, "%s tab item init failed %d", SerialNumber.Widget.Name, eResult);
            return;
        }
    }

    if ((eResult = MenuMgr_AddTab(kTabID_System, &Tab_System)) != kOSD_Result_Ok)
    {
        ESP_LOGE(TAG, "%s widget init failed %d", Tab_System.Widget.Name, eResult);
        return;
    }
}

// fr1: GAMES, a tab of its own (the last: Left from the first tab reaches it). Its page is SYSTEM > GAMES's (games.c);
// A on it, or SELECT anywhere in the menu, opens the library over the whole screen.
#ifdef CONFIG_NW_CARTFLASH_WIFI
static void OpenGames(void)
{
    Games_OpenLibrary();
    MenuMgr_ShowItem(kTabID_Games, "");
}
#endif
static void CreateMenuGames(lv_obj_t *const pScreen)
{
    (void)pScreen;
#ifdef CONFIG_NW_CARTFLASH_WIFI
    static MenuTab_t Tab_Games = {
        .Widget = {
            .Name = "MenuGames",
            .fnDraw = GamesTab_Draw,
            .fnOnButton = GamesTab_OnButton,
            .fnOnTransition = GamesTab_OnTransition,
        },
        .pImageDesc = &menu_games,
        .Menu = NULL,
    };
    Tab_Games.Accent = lv_color_make(0x30, 0xE0, 0x70);
    OSD_Result_t eResult;
    if ((eResult = MenuMgr_AddTab(kTabID_Games, &Tab_Games)) != kOSD_Result_Ok)
    {
        ESP_LOGE(TAG, "MenuGames tab init failed %d", eResult);
        return;
    }
    MenuMgr_SetSelectHandler(OpenGames);
#endif
}
