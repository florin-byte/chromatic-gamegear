// PARKED, NOT BUILT (owner 2026-10-10: Wi-Fi only). Compiles only with
// CONFIG_BT_ENABLED + NimBLE; see parked/README.md.
// cwl_ble.c: CWL2 over Bluetooth LE (NimBLE). See cwl_radio.h, PROTOCOL.md
// "Local radios".
//
// Both Chromatics run the same GATT server and both advertise it and scan for
// it; the one with the lower address connects (cwl_lx.c tie-break), so two
// identical devices end up with exactly one connection. A Mac (tools/ble_shim)
// connects as central, or advertises as a peripheral for a Chromatic central.
// Frames: central writes (without response) to the peer's C2P, the peripheral
// notifies P2C. Each is one cwl_lx frame (CWL2 packets unchanged).
#include "cwl_ble.h"

#include "esp_bt.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

#include <stdio.h>
#include <string.h>

static const char *TAG = "CWLBLE";

// 43574C32-B1E0-4C49-4E4B-0000000000xx, little-endian byte order
#define CWL_UUID(x) BLE_UUID128_INIT(x, 0x00, 0x00, 0x00, 0x00, 0x00, 0x4b, 0x4e, 0x49, 0x4c, 0xe0, 0xb1, 0x32, 0x4c, 0x57, 0x43)
static const ble_uuid128_t kSvc = CWL_UUID(0x00);
static const ble_uuid128_t kC2p = CWL_UUID(0x01);
static const ble_uuid128_t kP2c = CWL_UUID(0x02);
static const ble_uuid128_t kStat = CWL_UUID(0x03);

enum {
    kNoConn = 0xffff,
    kAdvItvlMin = 32,       // 20 ms
    kAdvItvlMax = 48,       // 30 ms
    kScanItvl = 80,         // 50 ms
    kScanWindow = 48,       // 30 ms: 60 % duty, the rest for advertising
    kConnectTimeout_ms = 3000,
    kSearchKick_ms = 250,
    kStatMax = 512,         // ATT attribute value limit
    // low power: 15 ms interval, the peripheral may skip 19 events (~300 ms,
    // like Wi-Fi modem sleep with listen interval 3)
    kLowItvl = 12, kLowLatency = 19, kLowTimeout = 400,
};

static bool _MemKept = false;
static volatile bool _On = false;          // CwlBle_Start done, not stopped
static volatile bool _Synced = false;
static QueueHandle_t _Rx;
static CwlLxRole_t _Role;
static bool _FixedPeer;
static uint8_t _Fixed[6];
static uint8_t _OwnType;
static volatile uint16_t _Conn = kNoConn;
static volatile bool _Central;
static volatile bool _Ready;
static volatile bool _Initiating;
static uint16_t _PeerC2p, _PeerP2c, _PeerCccd, _SvcStart, _SvcEnd;
static uint16_t _P2cHandle;                // our own P2C value handle
static volatile bool _LowApplied = false;
static uint32_t _SearchStartMs, _LastKickMs;
static uint32_t _StartAtMs;
static uint16_t _FullMin = 6, _FullMax = 6, _FullLat = 0, _FullTmo = 100;   // 7.5 ms, 1 s
static int (*_StatusFn)(char *, int);
static char _StatBuf[kStatMax + 1];
static CwlBleStatus_t _St;

static uint32_t NowMs(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

static int GapEvent(struct ble_gap_event *ev, void *arg);
static void Search(void);

// ---------------------------------------------------------------- helpers

void CwlBle_AddrText(const uint8_t a[6], char *buf, unsigned len)
{
    snprintf(buf, len, "%02X:%02X:%02X:%02X:%02X:%02X", a[5], a[4], a[3], a[2], a[1], a[0]);
}

bool CwlBle_ParseAddr(const char *s, uint8_t out[6])
{
    unsigned v[6];
    if (sscanf(s, "%x:%x:%x:%x:%x:%x", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]) != 6) return false;
    for (int i = 0; i < 6; i++) {
        if (v[i] > 255) return false;
        out[5 - i] = (uint8_t)v[i];
    }
    return true;
}

static void Queue(const uint8_t *src, const struct os_mbuf *om)
{
    CwlRadioRx_t r = {.medium = kCwlMed_Ble, .unicast = 1};
    const uint16_t n = OS_MBUF_PKTLEN(om);
    if (n == 0 || n > sizeof(r.f)) { _St.rx_bad++; return; }
    uint16_t got = 0;
    if (ble_hs_mbuf_to_flat(om, r.f, sizeof(r.f), &got) != 0) { _St.rx_bad++; return; }
    r.len = (uint8_t)got;
    if (src) memcpy(r.src, src, 6);
    _St.rx_frames++;
    if (_Rx == NULL || xQueueSend(_Rx, &r, 0) != pdTRUE) _St.rx_drop++;
}

static void ResetConn(void)
{
    _Conn = kNoConn;
    _Ready = false;
    _Initiating = false;
    _PeerC2p = _PeerP2c = _PeerCccd = 0;
    _LowApplied = false;
    _St.connected = _St.ready = false;
    _St.itvl = _St.latency = _St.timeout = 0;
    _St.mtu = 23;
}

static void NoteConnParams(uint16_t conn)
{
    struct ble_gap_conn_desc d;
    if (ble_gap_conn_find(conn, &d) == 0) {
        _St.itvl = d.conn_itvl;
        _St.latency = d.conn_latency;
        _St.timeout = d.supervision_timeout;
        _St.central = d.role == BLE_GAP_ROLE_MASTER;
        memcpy(_St.peer, d.peer_id_addr.val, 6);
    }
}

static void SetReady(void)
{
    _Ready = true;
    _St.ready = true;
    _St.connect_ms = NowMs() - _SearchStartMs;
    ESP_LOGI(TAG, "ready (%s)", _Central ? "central" : "peripheral");
}

static void Fail(uint16_t conn)
{
    (void)ble_gap_terminate(conn, BLE_ERR_REM_USER_CONN_TERM);
}

// ---------------------------------------------------------------- GATT server

static int ChrAccess(uint16_t conn, uint16_t attr, struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)attr; (void)arg;
    if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR && ble_uuid_cmp(ctxt->chr->uuid, &kC2p.u) == 0) {
        if (conn == _Conn) Queue(_St.peer, ctxt->om);
        return 0;
    }
    if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR && ble_uuid_cmp(ctxt->chr->uuid, &kStat.u) == 0) {
        char own[18], peer[18];
        CwlBle_AddrText(_St.own, own, sizeof(own));
        CwlBle_AddrText(_St.peer, peer, sizeof(peer));
        int n = snprintf(_StatBuf, sizeof(_StatBuf), "CWB own=%s peer=%s role=%s ready=%d itvl=%u lat=%u mtu=%u tx=%lu rx=%lu ",
                         own, peer, _St.central ? "central" : "peripheral", _St.ready, _St.itvl, _St.latency, _St.mtu,
                         (unsigned long)_St.tx_frames, (unsigned long)_St.rx_frames);
        if (n < 0) n = 0;
        if (_StatusFn != NULL && n < kStatMax) {
            const int x = _StatusFn(_StatBuf + n, kStatMax - n);
            if (x > 0 && x <= kStatMax - n) n += x;
        }
        if (n > kStatMax) n = kStatMax;
        return os_mbuf_append(ctxt->om, _StatBuf, (uint16_t)n) == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
    }
    if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR) return 0;   // P2C: empty value
    return BLE_ATT_ERR_UNLIKELY;
}

static const struct ble_gatt_svc_def kSvcs[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &kSvc.u,
        .characteristics = (struct ble_gatt_chr_def[]){
            {.uuid = &kC2p.u, .access_cb = ChrAccess, .flags = BLE_GATT_CHR_F_WRITE_NO_RSP | BLE_GATT_CHR_F_WRITE},
            {.uuid = &kP2c.u, .access_cb = ChrAccess, .flags = BLE_GATT_CHR_F_NOTIFY, .val_handle = &_P2cHandle},
            {.uuid = &kStat.u, .access_cb = ChrAccess, .flags = BLE_GATT_CHR_F_READ},
            {0},
        },
    },
    {0},
};

// ---------------------------------------------------------------- GATT client (central)

static int OnCccdWritten(uint16_t conn, const struct ble_gatt_error *e, struct ble_gatt_attr *a, void *arg)
{
    (void)a; (void)arg;
    if (conn != _Conn) return 0;
    if (e->status != 0) { ESP_LOGW(TAG, "cccd write %d", e->status); Fail(conn); return 0; }
    SetReady();
    return 0;
}

static int OnDsc(uint16_t conn, const struct ble_gatt_error *e, uint16_t chr_val, const struct ble_gatt_dsc *d, void *arg)
{
    (void)chr_val; (void)arg;
    if (conn != _Conn) return 0;
    if (e->status == 0) {
        if (_PeerCccd == 0 && ble_uuid_cmp(&d->uuid.u, BLE_UUID16_DECLARE(BLE_GATT_DSC_CLT_CFG_UUID16)) == 0) _PeerCccd = d->handle;
        return 0;
    }
    if (e->status != BLE_HS_EDONE || _PeerCccd == 0) { Fail(conn); return 0; }
    static const uint8_t on[2] = {1, 0};
    if (ble_gattc_write_flat(conn, _PeerCccd, on, 2, OnCccdWritten, NULL) != 0) Fail(conn);
    return 0;
}

static int OnChr(uint16_t conn, const struct ble_gatt_error *e, const struct ble_gatt_chr *c, void *arg)
{
    (void)arg;
    if (conn != _Conn) return 0;
    if (e->status == 0) {
        if (ble_uuid_cmp(&c->uuid.u, &kC2p.u) == 0) _PeerC2p = c->val_handle;
        else if (ble_uuid_cmp(&c->uuid.u, &kP2c.u) == 0) _PeerP2c = c->val_handle;
        return 0;
    }
    if (e->status != BLE_HS_EDONE || _PeerC2p == 0 || _PeerP2c == 0) { Fail(conn); return 0; }
    if (ble_gattc_disc_all_dscs(conn, _PeerP2c, _SvcEnd, OnDsc, NULL) != 0) Fail(conn);
    return 0;
}

static int OnSvc(uint16_t conn, const struct ble_gatt_error *e, const struct ble_gatt_svc *s, void *arg)
{
    (void)arg;
    if (conn != _Conn) return 0;
    if (e->status == 0) {
        _SvcStart = s->start_handle;
        _SvcEnd = s->end_handle;
        return 0;
    }
    if (e->status != BLE_HS_EDONE || _SvcStart == 0) { Fail(conn); return 0; }
    if (ble_gattc_disc_all_chrs(conn, _SvcStart, _SvcEnd, OnChr, NULL) != 0) Fail(conn);
    return 0;
}

static int OnMtu(uint16_t conn, const struct ble_gatt_error *e, uint16_t mtu, void *arg)
{
    (void)arg;
    if (conn != _Conn) return 0;
    if (e->status == 0) _St.mtu = mtu;
    _SvcStart = _SvcEnd = 0;
    if (ble_gattc_disc_svc_by_uuid(conn, &kSvc.u, OnSvc, NULL) != 0) Fail(conn);
    return 0;
}

// ---------------------------------------------------------------- GAP

static bool AdvHasService(const uint8_t *data, uint8_t len)
{
    struct ble_hs_adv_fields f;
    if (ble_hs_adv_parse_fields(&f, data, len) != 0) return false;
    for (int i = 0; i < f.num_uuids128; i++)
        if (ble_uuid_cmp(&f.uuids128[i].u, &kSvc.u) == 0) return true;
    return false;
}

static void OnAdv(const struct ble_gap_disc_desc *d)
{
    if (_Conn != kNoConn || _Initiating) return;
    if (d->event_type != BLE_HCI_ADV_RPT_EVTYPE_ADV_IND && d->event_type != BLE_HCI_ADV_RPT_EVTYPE_DIR_IND) return;
    if (!AdvHasService(d->data, d->length_data)) return;
    _St.adv_seen++;
    if (_FixedPeer && memcmp(d->addr.val, _Fixed, 6) != 0) { _St.adv_ignored++; return; }
    if (!CwlLx_BleShouldInitiate(_St.own, d->addr.val, _Role)) { _St.adv_ignored++; return; }   // it connects to us
    (void)ble_gap_disc_cancel();
    (void)ble_gap_adv_stop();
    const struct ble_gap_conn_params p = {
        .scan_itvl = 16, .scan_window = 16,
        .itvl_min = _FullMin, .itvl_max = _FullMax, .latency = _FullLat, .supervision_timeout = _FullTmo,
        .min_ce_len = 0, .max_ce_len = 0,
    };
    _Initiating = true;
    if (ble_gap_connect(_OwnType, &d->addr, kConnectTimeout_ms, &p, GapEvent, NULL) != 0) {
        _Initiating = false;
        Search();
    }
}

static void Search(void)
{
    if (!_On || !_Synced || _Conn != kNoConn || _Initiating) return;
    if (!ble_gap_adv_active()) {
        struct ble_gap_adv_params ap = {0};
        ap.conn_mode = BLE_GAP_CONN_MODE_UND;
        ap.disc_mode = BLE_GAP_DISC_MODE_GEN;
        ap.itvl_min = kAdvItvlMin;
        ap.itvl_max = kAdvItvlMax;
        (void)ble_gap_adv_start(_OwnType, NULL, BLE_HS_FOREVER, &ap, GapEvent, NULL);
    }
    if (_Role != kCwlLxRole_Peripheral && !ble_gap_disc_active()) {
        const struct ble_gap_disc_params dp = {
            .itvl = kScanItvl, .window = kScanWindow, .filter_policy = 0, .limited = 0, .passive = 1, .filter_duplicates = 0,
        };
        (void)ble_gap_disc(_OwnType, BLE_HS_FOREVER, &dp, GapEvent, NULL);
    }
}

static int GapEvent(struct ble_gap_event *ev, void *arg)
{
    (void)arg;
    switch (ev->type) {
    case BLE_GAP_EVENT_DISC:
        OnAdv(&ev->disc);
        break;
    case BLE_GAP_EVENT_CONNECT:
        _Initiating = false;
        if (ev->connect.status != 0) {
            Search();
            break;
        }
        (void)ble_gap_disc_cancel();
        (void)ble_gap_adv_stop();
        ResetConn();
        _Conn = ev->connect.conn_handle;
        _St.connected = true;
        _St.connects++;
        NoteConnParams(_Conn);
        _Central = _St.central;
        if (!_Central && _FixedPeer && memcmp(_St.peer, _Fixed, 6) != 0) {   // not the configured partner
            Fail(_Conn);
            break;
        }
        if (_Central) {
            if (ble_gattc_exchange_mtu(_Conn, OnMtu, NULL) != 0) Fail(_Conn);
        } else {
            // Ask the central (a Mac picks its own) for the fast interval.
            const struct ble_gap_upd_params u = {_FullMin, _FullMax, _FullLat, _FullTmo, 0, 0};
            (void)ble_gap_update_params(_Conn, &u);
        }
        break;
    case BLE_GAP_EVENT_DISCONNECT:
        _St.disconnects++;
        _St.last_reason = (uint32_t)ev->disconnect.reason;
        ResetConn();
        _SearchStartMs = NowMs();
        Search();
        break;
    case BLE_GAP_EVENT_CONN_UPDATE:
        if (ev->conn_update.conn_handle == _Conn) {
            NoteConnParams(_Conn);
            _St.param_updates++;
        }
        break;
    case BLE_GAP_EVENT_SUBSCRIBE:
        if (ev->subscribe.conn_handle == _Conn && ev->subscribe.attr_handle == _P2cHandle && !_Central) {
            if (ev->subscribe.cur_notify) SetReady();
            else { _Ready = false; _St.ready = false; }
        }
        break;
    case BLE_GAP_EVENT_MTU:
        if (ev->mtu.conn_handle == _Conn) _St.mtu = ev->mtu.value;
        break;
    case BLE_GAP_EVENT_NOTIFY_RX:
        if (ev->notify_rx.conn_handle == _Conn && _Central && ev->notify_rx.attr_handle == _PeerP2c) Queue(_St.peer, ev->notify_rx.om);
        break;
    case BLE_GAP_EVENT_ADV_COMPLETE:
    case BLE_GAP_EVENT_DISC_COMPLETE:
        break;   // CwlBle_Poll restarts them
    default:
        break;
    }
    return 0;
}

// ---------------------------------------------------------------- host

static void OnSync(void)
{
    (void)ble_hs_util_ensure_addr(0);
    if (ble_hs_id_infer_auto(0, &_OwnType) != 0) _OwnType = BLE_OWN_ADDR_PUBLIC;
    (void)ble_hs_id_copy_addr(_OwnType == BLE_OWN_ADDR_RANDOM ? BLE_ADDR_RANDOM : BLE_ADDR_PUBLIC, _St.own, NULL);
    char name[16];
    snprintf(name, sizeof(name), "CWL2-%02X%02X", _St.own[1], _St.own[0]);
    (void)ble_svc_gap_device_name_set(name);
    struct ble_hs_adv_fields f = {0};
    f.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    f.uuids128 = (ble_uuid128_t *)&kSvc;
    f.num_uuids128 = 1;
    f.uuids128_is_complete = 1;
    (void)ble_gap_adv_set_fields(&f);
    struct ble_hs_adv_fields r = {0};
    r.name = (const uint8_t *)name;
    r.name_len = (uint8_t)strlen(name);
    r.name_is_complete = 1;
    (void)ble_gap_adv_rsp_set_fields(&r);
    _St.start_ms = NowMs() - _StartAtMs;
    _Synced = true;
    _St.up = true;
    _SearchStartMs = NowMs();
    Search();
}

static void OnReset(int reason)
{
    ESP_LOGW(TAG, "host reset %d", reason);
    _Synced = false;
    _St.up = false;
    ResetConn();
}

static void HostTask(void *arg)
{
    (void)arg;
    nimble_port_run();            // returns after nimble_port_stop()
    nimble_port_freertos_deinit();
}

// ---------------------------------------------------------------- API

void CwlBle_BootMemory(bool keep)
{
    _MemKept = keep;
    if (keep) (void)esp_bt_controller_mem_release(ESP_BT_MODE_CLASSIC_BT);   // ~22 KB back: BLE only
    else (void)esp_bt_mem_release(ESP_BT_MODE_BTDM);                         // all 56 KB + host back
}

bool CwlBle_MemoryKept(void) { return _MemKept; }

esp_err_t CwlBle_Start(QueueHandle_t rx, CwlLxRole_t role, const uint8_t *fixed_peer)
{
    if (!_MemKept) return ESP_ERR_INVALID_STATE;
    if (_On) return ESP_OK;
    _Rx = rx;
    _Role = role;
    _FixedPeer = fixed_peer != NULL;
    if (_FixedPeer) memcpy(_Fixed, fixed_peer, 6);
    _Synced = false;
    ResetConn();
    _StartAtMs = NowMs();
    esp_err_t err = nimble_port_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nimble_port_init %d", err);
        return err;
    }
    ble_hs_cfg.sync_cb = OnSync;
    ble_hs_cfg.reset_cb = OnReset;
    ble_svc_gap_init();
    ble_svc_gatt_init();
    int rc = ble_gatts_count_cfg(kSvcs);
    if (rc == 0) rc = ble_gatts_add_svcs(kSvcs);
    if (rc != 0) {
        ESP_LOGE(TAG, "gatts %d", rc);
        (void)nimble_port_deinit();
        return ESP_FAIL;
    }
    (void)ble_att_set_preferred_mtu(kCwlLx_MaxFrame + 3);
    _On = true;
    _St.starts++;
    nimble_port_freertos_init(HostTask);
    return ESP_OK;
}

void CwlBle_Stop(void)
{
    if (!_On) return;
    _On = false;
    if (_Conn != kNoConn) {
        (void)ble_gap_terminate(_Conn, BLE_ERR_REM_USER_CONN_TERM);
        vTaskDelay(pdMS_TO_TICKS(30));   // let the LL_TERMINATE_IND go out
    }
    (void)ble_gap_adv_stop();
    (void)ble_gap_disc_cancel();
    if (nimble_port_stop() == 0) (void)nimble_port_deinit();
    _Synced = false;
    _St.up = false;
    _St.advertising = _St.scanning = false;
    ResetConn();
}

bool CwlBle_On(void) { return _On; }
bool CwlBle_Ready(void) { return _On && _Ready; }

unsigned CwlBle_FrameLimit(void)
{
    const unsigned mtu = _St.mtu ? _St.mtu : 23;
    const unsigned n = mtu > 3 ? mtu - 3 : 20;
    return n > kCwlLx_MaxFrame ? kCwlLx_MaxFrame : n;
}

bool CwlBle_Send(const uint8_t *f, size_t n)
{
    if (!CwlBle_Ready() || n == 0) return false;
    const uint16_t conn = _Conn;
    int rc;
    if (_Central) {
        rc = ble_gattc_write_no_rsp_flat(conn, _PeerC2p, f, (uint16_t)n);
    } else {
        struct os_mbuf *om = ble_hs_mbuf_from_flat(f, (uint16_t)n);
        rc = om ? ble_gatts_notify_custom(conn, _P2cHandle, om) : BLE_HS_ENOMEM;
    }
    if (rc == 0) _St.tx_frames++;
    else _St.tx_fail++;
    return rc == 0;
}

void CwlBle_Poll(uint32_t now_ms, bool low_power)
{
    if (!_On || !_Synced) return;
    _St.advertising = ble_gap_adv_active();
    _St.scanning = ble_gap_disc_active();
    _St.initiating = _Initiating;
    if (_Conn == kNoConn) {
        if (now_ms - _LastKickMs >= kSearchKick_ms) {
            _LastKickMs = now_ms;
            Search();
        }
        return;
    }
    if (_Ready && low_power != _LowApplied) {
        const struct ble_gap_upd_params u = low_power
            ? (struct ble_gap_upd_params){kLowItvl, kLowItvl, kLowLatency, kLowTimeout, 0, 0}
            : (struct ble_gap_upd_params){_FullMin, _FullMax, _FullLat, _FullTmo, 0, 0};
        (void)ble_gap_update_params(_Conn, &u);   // a central (Mac) may refuse: asked once
        _LowApplied = low_power;
        _St.low_power = low_power;
    }
    static uint32_t LastRssi;
    if (now_ms - LastRssi >= 1000) {
        LastRssi = now_ms;
        int8_t r;
        if (ble_gap_conn_rssi(_Conn, &r) == 0) _St.rssi = r;
    }
}

void CwlBle_GetStatus(CwlBleStatus_t *st) { *st = _St; }

void CwlBle_SetParams(uint16_t itvl_min, uint16_t itvl_max, uint16_t latency, uint16_t timeout)
{
    _FullMin = itvl_min;
    _FullMax = itvl_max < itvl_min ? itvl_min : itvl_max;
    _FullLat = latency;
    _FullTmo = timeout;
    if (_On && _Conn != kNoConn) {
        const struct ble_gap_upd_params u = {_FullMin, _FullMax, _FullLat, _FullTmo, 0, 0};
        (void)ble_gap_update_params(_Conn, &u);
        _LowApplied = false;
    }
}

void CwlBle_SetStatusFn(int (*fn)(char *buf, int len)) { _StatusFn = fn; }
