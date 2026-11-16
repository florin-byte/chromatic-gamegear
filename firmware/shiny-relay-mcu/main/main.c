#include "cwl.h"
#include "nw_dev.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "driver/uart.h"
#include "esp_console.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_vfs.h"
#include "esp_vfs_fat.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lvgl.h"
#include "gfx.h"
#include "board.h"
#include "brightness.h"
#include "color_correct_lcd.h"
#include "color_correct_usb.h"
#include "dpad_ctl.h"
#include "fpga_rx.h"
#include "fpga_tx.h"
#include "frameblend.h"
#include "fw.h"
#include "gbc_color_temp.h"
#include "osd.h"
#include "osd_default.h"
#include "low_batt_icon_ctl.h"
#include "style.h"
#include "player_num.h"
#include "screen_transit_ctl.h"
#include "mutex.h"
#include "serial_num.h"
#include "settings.h"
#include "silent.h"
#include "pwrmgr.h"
#include "sd.h"

enum {
    kLVGL_TickPeriod_us = 1000u, // 1 millisecond
};

enum {
    // 1.0.3: the stacks the tasks really use, measured with `cart stacks` (least left) on unit 2 after boot,
    // receive mode and a 2 MiB express install, and again after the release gates: fpga_tx used 748 of 8 KiB,
    // fpga_rx 1,196, lvgl Timer 2,336, pwr_mgr 520. The 12 KiB given back are heap for the TLS handshakes
    // (the lowest free heap during a receive was ~2.3-4 KB with the stock 8 KiB stacks).
    kFPGATxTask_StackDepth = 3*1024,   // [bytes]
    kFPGARxTask_StackDepth = 4*1024,   // [bytes]
    kSleepTask_StackDepth  = 3*1024,   // [bytes]
    kTimerTask_StackDepth  = 6*1024,   // [bytes]

    kFPGATxTask_Priority = configMAX_PRIORITIES - 10,
    kFPGARxTask_Priority = configMAX_PRIORITIES - 10,
    kSleepTask_Priority  = configMAX_PRIORITIES - 9,
};

static const char *TAG = "main";
static volatile uint8_t fpga_hsync = 0;

static void lvgl_tick(void *arg);

static void persist_storage_init(void);
static void example_lvgl_flush_cb(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *color_map);
static void lvglTimerTask(void* param);
static void send_lines(spi_device_handle_t spi, int ypos, uint16_t *linedata);

static spi_device_handle_t spi;
static lv_disp_draw_buf_t disp_buf; // contains internal graphic buffer(s) called draw buffer(s)
static lv_disp_drv_t disp_drv;      // contains callback functions
static lv_disp_t *disp;

// Having some issue with floating point here so scale 9.6 10x
#define ROWS_PER_XFER_X10 32
#define NUMTRANS 1440/ROWS_PER_XFER_X10
#define PIXELSPERXFER 16*ROWS_PER_XFER_X10
#define BYTESPERXFER PIXELSPERXFER*2

// cartspeed1: transactions of the express cart ring carry this tag (OSD_QspiCartWrite); they are
// not OSD rows.
#define kQspiCartTag ((void *)0x43415254)
// The OSD queues its rows while the express cart ring sends POLLING transactions on the same
// device; the driver refuses a queue_trans while a polling one is under way (ESP_ERR_INVALID_STATE,
// send_lines' assert: unit 2, 2026-10-15). One mutex keeps the two apart.
static SemaphoreHandle_t _QspiMtx;
static StaticSemaphore_t _QspiMtxBuf;
static SemaphoreHandle_t QspiMtx(void)
{
    if (_QspiMtx == NULL) _QspiMtx = xSemaphoreCreateMutexStatic(&_QspiMtxBuf);
    return _QspiMtx;
}

static void notify_lvgl_flush_ready(spi_transaction_t *pTransaction)
{
    static int rownum = 0;

    if ((pTransaction != NULL) && (pTransaction->user == kQspiCartTag))
    {
        return;
    }

    rownum = rownum + ROWS_PER_XFER_X10;
    if(rownum >= 1440)
    {
        rownum = 0;
        lv_disp_flush_ready(&disp_drv);
    }
}

// `osdshot`: the flush callback copies a complete frame here on request.
static uint16_t *volatile pOsdShotCopy = NULL;
static volatile bool OsdShotWanted = false;

static void example_lvgl_flush_cb(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *color_map)
{
    if (OsdShotWanted && (pOsdShotCopy != NULL))
    {
        memcpy(pOsdShotCopy, drv->draw_buf->buf1, sizeof(lv_color_t) * kOSD_NumPixels);
        OsdShotWanted = false;
    }
    // copy a buffer's content to a specific area of the display
    send_lines(spi, 0, drv->draw_buf->buf1);
}

static void lvgl_tick(void *arg)
{
    /* Tell LVGL how many milliseconds has elapsed. lv_tick_inc() takes milliseconds: passing the period in
       microseconds ran LVGL's clock 1000x fast, so a scrolling label (LV_LABEL_LONG_SCROLL_CIRCULAR) never
       showed its text moving (fr1). */
    lv_tick_inc(kLVGL_TickPeriod_us / 1000u);
}

static void lvglTimerTask(void* param)
{
    while(1)
    {
        // The task running lv_timer_handler should have lower priority than that running `lv_tick_inc`
        lv_timer_handler();
        vTaskDelay(pdMS_TO_TICKS(30));
    }
}

// fwshrink1: in .bss, not DMA_ATTR's .dram1 (= .data): the same DMA-capable internal DRAM, but no 46,080
// bytes of zeros stored in the app image (.data is copied from flash at boot; .bss is zeroed)
static WORD_ALIGNED_ATTR lv_color_t buffy[160*144];

static void send_lines(spi_device_handle_t spi, int ypos, uint16_t *linedata)
{
    esp_err_t ret;

    int x;
    //Transaction descriptors. Declared static so they're not allocated on the stack; we need this memory even when this
    //function is finished because the SPI driver needs access to it even while we're already calculating the next line.
    static spi_transaction_t trans[NUMTRANS];

    //In theory, it's better to initialize trans and data only once and hang on to the initialized
    //variables. We allocate them on the stack, so we need to re-init them each call.
    for (x=0; x<NUMTRANS; x++) {
        memset(&trans[x], 0, sizeof(spi_transaction_t));
        trans[x].tx_buffer = &buffy[x*PIXELSPERXFER];//finally send the line datai
        trans[x].length = BYTESPERXFER * 8;  //Data length, in bits
        trans[x].flags = SPI_TRANS_MODE_QIO; //undo SPI_TRANS_USE_TXDATA flag
        trans[x].cmd = 0x400 | 1023;//write
        trans[x].addr = x*BYTESPERXFER;
    }

    //Queue all transactions (never while a cart ring write polls the bus: OSD_QspiCartWrite).
    (void)xSemaphoreTake(QspiMtx(), portMAX_DELAY);
    for (x=0; x<NUMTRANS; x++) {
        ret=spi_device_queue_trans(spi, &trans[x], portMAX_DELAY);
        assert(ret==ESP_OK);
    }
    xSemaphoreGive(QspiMtx());

    return;
    //When we are here, the SPI driver is busy (in the background) getting the transactions sent. That happens
    //mostly using DMA, so the CPU doesn't have much to do here. We're not going to wait for the transaction to
    //finish because we may as well spend the time calculating the next line. When that is done, we can call
    //send_line_finish, which will wait for the transfers to be done and check their status.
}

// gamecomp1 (bench): the task holding the QSPI bus lock, or "none" (a load that hangs: cf_rc_dev's watch prints it)
const char *OSD_QspiHolder(void)
{
    TaskHandle_t h = xSemaphoreGetMutexHolder(QspiMtx());
    return h ? pcTaskGetName(h) : "none";
}

// cartspeed1: bytes for the FPGA's express cart ring (cart_xs.v, FPGA 18.26+). The OSD's QSPI
// device, one POLLING transaction: it waits for the OSD rows queued before it (the bus lock), puts
// the bytes on the bus (40 MHz, 4 lines: ~52 us per KiB) and returns when they are out. Address
// bit 31 makes the FPGA put them in the ring at offset Off, never in PSRAM (mm_burst_write skips
// them). pData must be DMA-capable (internal RAM). 0 = sent.
int OSD_QspiCartWrite(uint16_t Off, const uint8_t *pData, uint16_t Len)
{
    if ((spi == NULL) || (pData == NULL) || (Len == 0) || (Len > 4096))
    {
        return -1;
    }
    (void)xSemaphoreTake(QspiMtx(), portMAX_DELAY);
    spi_transaction_t t;
    memset(&t, 0, sizeof t);
    t.tx_buffer = pData;
    t.length = (size_t)Len * 8;
    t.flags = SPI_TRANS_MODE_QIO;
    t.cmd = 0x400 | ((Len - 1) & 0x3FF);
    t.addr = 0x80000000u | Off;
    t.user = kQspiCartTag;
    const esp_err_t e = spi_device_polling_transmit(spi, &t);
    xSemaphoreGive(QspiMtx());
    return (e == ESP_OK) ? 0 : -1;
}

// Shiny Relay 1.1.0: 1024 bytes into PSRAM at byte address Addr (a RAM cart game or its save RAM:
// ramcart.v). The FPGA always writes a 1 KiB burst per transaction (mm_burst_write), so a write is
// exactly that; Addr 1 KiB aligned, below 8 MiB, never in the frame buffers' first 128 KiB.
int OSD_QspiPsramWrite(uint32_t Addr, const uint8_t *pData)
{
    if ((spi == NULL) || (pData == NULL) || (Addr & 0x3FFu) || (Addr < 0x20000u) || (Addr >= 0x800000u))
    {
        return -1;
    }
    (void)xSemaphoreTake(QspiMtx(), portMAX_DELAY);
    spi_transaction_t t;
    memset(&t, 0, sizeof t);
    t.tx_buffer = pData;
    t.length = 1024 * 8;
    t.flags = SPI_TRANS_MODE_QIO;
    t.cmd = 0x400 | 1023;
    t.addr = Addr;
    t.user = kQspiCartTag;
    const esp_err_t e = spi_device_polling_transmit(spi, &t);
    xSemaphoreGive(QspiMtx());
    return (e == ESP_OK) ? 0 : -1;
}

// Shiny Relay 1.1.0: Len bytes (even, <= 1024) back from the RAM cart's read window at byte offset
// Off (FPGA 18.30 qspi_slave.v: command bit 10 clear = a read, address bit 30 = the window). The
// window is filled first by a cart_xs RREAD. pData must be DMA-capable. 0 = read.
uint8_t OSD_QspiWinDummy = 4;   /* the read's dummy clocks (bench: rc dummy N) */
int OSD_QspiWinRead(uint16_t Off, uint8_t *pData, uint16_t Len)
{
    if ((spi == NULL) || (pData == NULL) || (Len == 0) || (Len > 1024) || (Len & 1u) || (Off & 1u) || (Off + Len > 1024))
    {
        return -1;
    }
    (void)xSemaphoreTake(QspiMtx(), portMAX_DELAY);
    // 4 dummy clocks, not the device's 3: the bus turns around (the FPGA drives from clock 47)
    spi_transaction_ext_t x;
    memset(&x, 0, sizeof x);
    x.base.rx_buffer = pData;
    x.base.rxlength = (size_t)Len * 8;
    x.base.length = 0;
    x.base.flags = SPI_TRANS_MODE_QIO | SPI_TRANS_VARIABLE_DUMMY;
    x.base.cmd = (Len - 1) & 0x3FF;
    x.base.addr = 0x40000000u | Off;
    x.base.user = kQspiCartTag;
    x.dummy_bits = OSD_QspiWinDummy;
    const esp_err_t e = spi_device_polling_transmit(spi, &x.base);
    xSemaphoreGive(QspiMtx());
    return (e == ESP_OK) ? 0 : -1;
}

static void register_update_callbacks(void)
{
    Brightness_RegisterOnUpdateCb(FPGA_Tx_WriteBrightness);
    PlayerNum_RegisterOnUpdateCb(FPGA_Tx_SendSysCtl);
    SilentMode_RegisterOnUpdateCb(FPGA_Tx_SendSysCtl);
    ColorCorrectLCD_RegisterOnUpdateCb(FPGA_Tx_SendSysCtl);
    ColorCorrectUSB_RegisterOnUpdateCb(FPGA_Tx_SendSysCtl);
    FrameBlend_RegisterOnUpdateCb(FPGA_Tx_SendSysCtl);
    ScreenTransitCtl_RegisterOnUpdateCb(FPGA_Tx_SendSysCtl);
    DPadCtl_RegisterOnUpdateCb(FPGA_Tx_SendSysCtl);
    LowBattIconCtl_RegisterOnUpdateCb(FPGA_Tx_SendSysCtl);
    Button_RegisterOnButtonPokeCb(FPGA_Tx_PokeButtons);
    Style_RegisterOnUpdateCb(FPGA_Tx_WritePaletteStyle);
    GBCColorTemp_RegisterOnUpdateCb(FPGA_Tx_WriteColorTemp);
}

static void persist_storage_init(void)
{
    Settings_Initialize();
    Firmware_Initialize();
    SerialNum_Initialize();
    CWL_Initialize();
    NW_Initialize();
    PwrMgr_RegisterCommands();
    Button_RegisterCommands();
    Sd_Register();

    const fnSettingApply_t fnApplySetting[kNumSettingKeys] = {
        [kSettingKey_FrameBlend]       = FrameBlend_ApplySetting,
        [kSettingKey_ColorCorrectLCD]  = ColorCorrectLCD_ApplySetting,
        [kSettingKey_ColorCorrectUSB]  = ColorCorrectUSB_ApplySetting,
        [kSettingKey_PlayerNum]        = PlayerNum_ApplySetting,
        [kSettingKey_SilentMode]       = SilentMode_ApplySetting,
        [kSettingKey_Brightness]       = Brightness_ApplySetting,
        [kSettingKey_ScreenTransitCtl] = ScreenTransitCtl_ApplySetting,
        [kSettingKey_DPadCtl]          = DPadCtl_ApplySetting,
        [kSettingKey_LowBattIconCtl]   = LowBattIconCtl_ApplySetting,
        [kSettingKey_PaletteStyleID]   = Style_ApplySetting,
        [kSettingKey_GBCColorTemp]     = GBCColorTemp_ApplySetting,
    };

    OSD_Result_t eResult;
    for(SettingKey_t k = kSettingKey_FirstKey; k < kNumSettingKeys; k++)
    {
        SettingValue_t Value = {
            .eType = kSettingDataType_U32,
            .U32 = 0,
        };

        const char* pSettingName = Settings_KeyToName(k);

        // Set up the data type eType for each key
        if ((eResult = Settings_RetrieveDefault(k, &Value)) != kOSD_Result_Ok)
        {
            ESP_LOGE(TAG, "Failed to setup %s with its default, err = %d", pSettingName, eResult);
            continue;
        }

        if ((eResult = Settings_Retrieve(k, &Value.U32)) != kOSD_Result_Ok)
        {
            ESP_LOGE(TAG, "Failed to load setting %s, err = %d. Using default value.", pSettingName, eResult);

            // Create the entry if not existent, or store the default
            if ((eResult = Settings_Update(k, Value.U32)) != kOSD_Result_Ok)
            {
                ESP_LOGE(TAG, "Failed to update setting %s on init, err = %d.", pSettingName, eResult);
            }
        }
        else
        {
            ESP_LOGI(TAG, "Setting loaded, %s = %lu", pSettingName, Value.U32);
        }

        // Send over the setting to the module
        if (fnApplySetting[k] != NULL)
        {
            if ((eResult = fnApplySetting[k](&Value)) != kOSD_Result_Ok)
            {
                ESP_LOGE(TAG, "Failed to apply setting %s to %lu, err = %d.", pSettingName, Value.U32, eResult);
            }
        }
    }
}

// `osdshot`: dump the OSD framebuffer (what the MCU overlays on the LCD; it is
// not in the USB video) as hex, one row per line: OSD <row> <160 x RGB565>.
static int osdshot_command(int argc, char **argv)
{
    (void)argc; (void)argv;
    // Copy a complete frame first: LVGL keeps redrawing the live buffer, and
    // a copy taken mid-render shows half a menu. The flush callback copies
    // right after a frame is rendered.
    uint16_t *pCopy = malloc(sizeof(buffy));
    if (pCopy != NULL)
    {
        pOsdShotCopy = pCopy;
        OsdShotWanted = true;
        for (int i = 0; (i < 50) && OsdShotWanted; i++)
        {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
        if (OsdShotWanted)
        {
            OsdShotWanted = false;                     // no flush: copy live
            memcpy(pCopy, buffy, sizeof(buffy));
        }
        pOsdShotCopy = NULL;
    }
    const uint16_t *pSrc = (pCopy != NULL) ? pCopy : (const uint16_t *)buffy;
    printf("OSDVISIBLE %d\n", OSD_IsVisible() ? 1 : 0);
    printf("OSDSHOT %u %u\n", (unsigned)kOSD_Width_px, (unsigned)kOSD_Height_px);
    char Line[8 + 160 * 4 + 2];
    for (unsigned y = 0; y < kOSD_Height_px; y++)
    {
        int n = snprintf(Line, sizeof(Line), "OSD %u ", y);
        const uint16_t *pRow = &pSrc[y * kOSD_Width_px];
        for (unsigned x = 0; x < kOSD_Width_px; x++)
        {
            n += snprintf(Line + n, sizeof(Line) - n, "%04x", pRow[x]);
        }
        printf("%s\n", Line);
        vTaskDelay(pdMS_TO_TICKS(2));
    }
    printf("OSDSHOT END\n");
    free(pCopy);
    return 0;
}

static esp_console_repl_t *pRepl = NULL;
static esp_console_repl_config_t ReplConfig = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
static esp_console_dev_uart_config_t ReplHWConfig = ESP_CONSOLE_DEV_UART_CONFIG_DEFAULT();

void app_main(void)
{
    printf("diag: boot fw=%s\n", CONFIG_APP_PROJECT_VER);
    persist_storage_init();
    Mutex_Init();

    // Set up the system management UART to/from the FPGA
    ESP_LOGI(TAG, "Initialize FPGA UART");
    const uart_config_t uart_config = {
        .baud_rate = 115200,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    uart_driver_install(UART_NUM_1, kFPGA_TxConsts_BufferSize, kFPGA_RxConsts_BufferSize, 0, NULL, 0);
    uart_param_config(UART_NUM_1, &uart_config);
    uart_set_pin(UART_NUM_1, PIN_NUM_UART_TO_FPGA, PIN_NUM_UART_FROM_FPGA, -1, -1);
    // linkperf1: hand received bytes to the RX task 1 symbol (~87 us) after the
    // line goes quiet instead of the driver's default 10 (~870 us): every
    // link-port event (6 bytes) reached the link task ~0.8 ms sooner.
    uart_set_rx_timeout(UART_NUM_1, 1);

    gpio_sleep_set_direction(PIN_NUM_UART_FROM_FPGA, GPIO_MODE_INPUT);
    gpio_sleep_set_pull_mode(PIN_NUM_UART_FROM_FPGA, GPIO_PULLUP_ONLY);

    // Task is created earlier than the others to apply the settings ASAP
    xTaskCreate(FPGA_TxTask, "fpga_tx_task", kFPGATxTask_StackDepth, NULL, kFPGATxTask_Priority, FPGA_GetTxTaskHandle());

    // Task is created early in order to recieve fast messages (e.g hotkey palette data)
    // These tasks are started in the "paused" state
    xTaskCreate(FPGA_RxTask, "fpga_rx_task", kFPGARxTask_StackDepth, NULL, kFPGARxTask_Priority, FPGA_GetRxTaskHandle());

    // Let the tasks run for a bit to transmit config data to the FPGA core
    const size_t kTransmitCfgCounts = 6;
    for(size_t i = 0; i < kTransmitCfgCounts; i++)
    {
        FPGA_Tx_SendAll();
        vTaskDelay( pdMS_TO_TICKS(10) );
    }

    ESP_LOGI(TAG, "Initialize SPI Master");
    (void)QspiMtx();   // cartspeed1: made here, before the OSD or a cart job can race to make it

    spi_bus_config_t buscfg={
        .miso_io_num=PIN_NUM_QSPI_MISO,
        .mosi_io_num=PIN_NUM_QSPI_MOSI,
        .sclk_io_num=PIN_NUM_QSPI_CLK,
        .quadwp_io_num=PIN_NUM_QSPI_WP,
        .quadhd_io_num=PIN_NUM_QSPI_HD,
        .max_transfer_sz=(BYTESPERXFER*8)+46,
        .flags=SPICOMMON_BUSFLAG_QUAD | SPICOMMON_BUSFLAG_MASTER
    };
    spi_device_interface_config_t devcfg={
        .clock_speed_hz=40*1000*1000,           //Clock out at 40 MHz
        .mode=0,                                //SPI mode 0
        .spics_io_num=PIN_NUM_QSPI_CS,
        .queue_size=NUMTRANS,
        .pre_cb=NULL,
        .flags=SPI_DEVICE_HALFDUPLEX,
        .cs_ena_pretrans=3,
        .cs_ena_posttrans=3,
        .command_bits=11,
        .address_bits=32,
        .dummy_bits=3,
        .post_cb=notify_lvgl_flush_ready
    };

    //Initialize the SPI bus
    esp_err_t ret=spi_bus_initialize(VSPI_HOST, &buscfg, SPI_DMA_CH_AUTO);
    ESP_ERROR_CHECK(ret);
    ret=spi_bus_add_device(VSPI_HOST, &devcfg, &spi);
    ESP_ERROR_CHECK(ret);

    ESP_LOGI(TAG, "Initialize LVGL library");
    lv_init();
    // alloc draw buffers used by LVGL
    // it's recommended to choose the size of the draw buffer(s) to be at least 1/10 screen sized
    lv_color_t *buf1 = (lv_color_t*)&buffy;
    assert(buf1);
    lv_disp_draw_buf_init(&disp_buf, buf1, NULL, kOSD_NumPixels);

    ESP_LOGI(TAG, "Register display driver to LVGL");
    lv_disp_drv_init(&disp_drv);
    disp_drv.hor_res = kOSD_Width_px;
    disp_drv.ver_res = kOSD_Height_px;
    disp_drv.direct_mode = 1;
    disp_drv.full_refresh = 1;
    disp_drv.flush_cb = example_lvgl_flush_cb;
    disp_drv.draw_buf = &disp_buf;
    disp = lv_disp_drv_register(&disp_drv);

    //Configuration is completed.

    ESP_LOGI(TAG, "Install LVGL tick timer");
    // Tick interface for LVGL (using esp_timer to generate 2ms periodic event)
    const esp_timer_create_args_t lvgl_tick_timer_args = {
        .callback = &lvgl_tick,
        .name = "lvgl_tick"
    };
    esp_timer_handle_t lvgl_tick_timer = NULL;
    ESP_ERROR_CHECK(esp_timer_create(&lvgl_tick_timer_args, &lvgl_tick_timer));
    ESP_ERROR_CHECK(esp_timer_start_periodic(lvgl_tick_timer, kLVGL_TickPeriod_us));

    ESP_LOGI(TAG, "Display LVGL animation");
    lv_obj_t *scr = lv_disp_get_scr_act(disp);

    OSD_Initialize();
    OSD_Default_Init(scr);
    register_update_callbacks();

    Gfx_Start(scr);

    xTaskCreatePinnedToCore(lvglTimerTask, "lvgl Timer", kTimerTask_StackDepth, NULL, 4, NULL, 1);

    // Prompt to be printed before each line.
    ReplConfig.prompt = "mcu> ";
    ReplConfig.max_cmdline_length = 1024;

    ReplHWConfig.rx_gpio_num = GPIO_NUM_35;
    ReplHWConfig.tx_gpio_num = GPIO_NUM_36;

    ESP_ERROR_CHECK(esp_console_new_repl_uart(&ReplHWConfig, &ReplConfig, &pRepl));
    esp_console_register_help_command();
    const esp_console_cmd_t OsdShotCmd = {
        .command = "osdshot",
        .help = "Dump the OSD framebuffer as hex (debug screenshots)",
        .func = &osdshot_command,
    };
    (void) esp_console_cmd_register(&OsdShotCmd);
    // All commands must be registered prior to starting the REPL
    ESP_ERROR_CHECK(esp_console_start_repl(pRepl));

    // Before the power manager exists, so it never sleeps under a starting link.
    CWL_AutoStart();
    NW_Start();

    vTaskDelay( pdMS_TO_TICKS(2000) );
    xTaskCreate(PwrMgr_Task, "pwr_mgr_task", kSleepTask_StackDepth, NULL, kSleepTask_Priority, NULL);
}
