#include "m8_usb.h"

#include <esp_intr_alloc.h>
#include <esp_log.h>
#include <freertos/queue.h>
#include <freertos/task.h>
#include <string.h>
#include <usb/usb_helpers.h>
#include <usb/usb_host.h>

#include "soc/rtc_cntl_struct.h"
#include "soc/usb_dwc_struct.h"
#include "soc/usb_wrap_struct.h"

#include "config.h"

static const char *TAG = "m8usb";

namespace m8usb {
namespace {

constexpr uint16_t kPjrcVid = 0x16C0;
constexpr uint16_t kM8PidStereo = 0x048A;
constexpr uint16_t kM8PidMultichannel = 0x048B;

constexpr uint8_t kClassAudio = 0x01;
constexpr uint8_t kClassCdcComm = 0x02;
constexpr uint8_t kClassCdcData = 0x0A;
constexpr uint8_t kSubclassAudioStreaming = 0x02;

constexpr uint8_t kCdcSetLineCoding = 0x20;
constexpr uint8_t kCdcSetControlLineState = 0x22;
constexpr uint16_t kCdcDtrRts = 0x03;

constexpr int kBulkInSize = 512; // multiple of the 64-byte FS bulk MPS
constexpr int kBulkInTransfers = 2;
constexpr int kIsoTransfers = 4;
constexpr int kIsoPacketsPerTransfer = 8; // 8 ms per transfer at FS
constexpr int kOutMaxLen = 8;

// If no device has enumerated for this long, power-cycle the root port. This is the host-side
// equivalent of replugging: useful because the M8 powers this board through the same cable,
// so it can't actually be unplugged without rebooting both.
constexpr TickType_t kPortRetryInterval = pdMS_TO_TICKS(5000);

struct OutMsg {
  uint8_t len;
  uint8_t data[kOutMaxLen];
};

struct Endpoints {
  int comm_iface = -1;
  int data_iface = -1;
  uint8_t bulk_in = 0, bulk_out = 0;
  uint16_t bulk_in_mps = 64, bulk_out_mps = 64;

  int audio_iface = -1;
  uint8_t audio_alt = 0;
  uint8_t iso_in = 0;
  uint16_t iso_mps = 0;
};

usb_host_client_handle_t client;
usb_device_handle_t dev;
Endpoints eps;

StreamBufferHandle_t rx_stream;
AudioSink audio_sink;
QueueHandle_t out_queue;

volatile State cur_state = State::kWaiting;
volatile uint32_t connections;
volatile bool audio_on;

// Event flags set by the client callback, consumed in the client task loop.
uint8_t pending_new_addr;
bool pending_gone;

// Transfers. Only touched from the client task (callbacks run inside
// usb_host_client_handle_events on that task).
usb_transfer_t *bulk_in_xfer[kBulkInTransfers];
usb_transfer_t *iso_xfer[kIsoTransfers];
usb_transfer_t *out_xfer;
usb_transfer_t *ctrl_xfer;
int inflight;
bool out_busy;
bool tearing_down;
uint32_t rx_dropped;
AudioStats iso_stats; // written by the client task, read elsewhere as a snapshot

// ---------------------------------------------------------------------------
// Callbacks
// ---------------------------------------------------------------------------

void client_event_cb(const usb_host_client_event_msg_t *msg, void *) {
  switch (msg->event) {
  case USB_HOST_CLIENT_EVENT_NEW_DEV:
    pending_new_addr = msg->new_dev.address;
    break;
  case USB_HOST_CLIENT_EVENT_DEV_GONE:
    if (dev && msg->dev_gone.dev_hdl == dev) pending_gone = true;
    break;
  default:
    break;
  }
}

// The Arduino core's sdkconfig enables CONFIG_USB_HOST_ENABLE_ENUM_FILTER_CALLBACK, and with it
// enabled ESP-IDF aborts enumeration of every device unless a filter approves it. Accept all
// devices here; open_device() decides whether one is an M8.
bool enum_filter_cb(const usb_device_desc_t *dev_desc, uint8_t *bConfigurationValue) {
  ESP_LOGI(TAG, "enumerating VID %04X PID %04X (%u configuration%s)", dev_desc->idVendor,
           dev_desc->idProduct, dev_desc->bNumConfigurations,
           dev_desc->bNumConfigurations == 1 ? "" : "s");
  (void)bConfigurationValue; // keep ESP-IDF's default (the first configuration)
  return true;
}

void resubmit(usb_transfer_t *t) {
  if (tearing_down) return;
  if (usb_host_transfer_submit(t) == ESP_OK) {
    inflight++;
  } else {
    ESP_LOGW(TAG, "resubmit failed on EP 0x%02X", t->bEndpointAddress);
  }
}

void bulk_in_cb(usb_transfer_t *t) {
  inflight--;
  if (t->status == USB_TRANSFER_STATUS_COMPLETED) {
    if (t->actual_num_bytes > 0) {
      const size_t sent = xStreamBufferSend(rx_stream, t->data_buffer, t->actual_num_bytes, 0);
      if (sent < (size_t)t->actual_num_bytes) rx_dropped += t->actual_num_bytes - sent;
    }
    resubmit(t);
  } else if (t->status != USB_TRANSFER_STATUS_NO_DEVICE &&
             t->status != USB_TRANSFER_STATUS_CANCELED) {
    ESP_LOGW(TAG, "bulk IN status %d", t->status);
    resubmit(t);
  }
}

void iso_in_cb(usb_transfer_t *t) {
  inflight--;
  if (t->status == USB_TRANSFER_STATUS_NO_DEVICE || t->status == USB_TRANSFER_STATUS_CANCELED) {
    return;
  }
  // On a failed URB only the overall status is updated; per-packet results are stale.
  if (t->status == USB_TRANSFER_STATUS_COMPLETED) {
    iso_stats.urbs_ok++;
    int offset = 0;
    for (int i = 0; i < t->num_isoc_packets; i++) {
      const usb_isoc_packet_desc_t &p = t->isoc_packet_desc[i];
      if (p.status != USB_TRANSFER_STATUS_COMPLETED) {
        iso_stats.packets_failed++;
        iso_stats.last_packet_status = p.status;
      } else if (p.actual_num_bytes == 0) {
        iso_stats.packets_empty++;
      } else {
        iso_stats.packets_ok++;
        iso_stats.bytes += p.actual_num_bytes;
        if (audio_sink) audio_sink(t->data_buffer + offset, p.actual_num_bytes);
      }
      offset += p.num_bytes;
    }
  } else {
    iso_stats.urbs_failed++;
    iso_stats.last_urb_status = t->status;
  }
  resubmit(t);
}

void out_cb(usb_transfer_t *t) {
  inflight--;
  out_busy = false;
  if (t->status != USB_TRANSFER_STATUS_COMPLETED) ESP_LOGW(TAG, "bulk OUT status %d", t->status);
}

volatile bool ctrl_done;
void ctrl_cb(usb_transfer_t *t) {
  inflight--;
  ctrl_done = true;
}

// ---------------------------------------------------------------------------
// Diagnostics: watch the root port's physical state
// ---------------------------------------------------------------------------

const char *line_state_name(uint32_t lnsts) {
  // PrtLnSts bit 0 is D+, bit 1 is D-.
  static const char *names[] = {"SE0 (D+ low, D- low)", "J (D+ high)", "K (D- high)",
                                "SE1 (both high)"};
  return names[lnsts & 3];
}

const char *speed_name(uint32_t spd) {
  static const char *names[] = {"high", "full", "low", "?"};
  return names[spd & 3];
}

void log_phy_routing() {
  usb_wrap_otg_conf_reg_t otg;
  otg.val = USB_WRAP.otg_conf.val;
  ESP_LOGI(TAG, "phy: rtc sw_hw_sel=%d sw_sel=%d | otg phy_sel=%d pad_en=%d pull_ovr=%d "
                "dp_pu=%d dp_pd=%d dm_pu=%d dm_pd=%d",
           (int)RTCCNTL.usb_conf.sw_hw_usb_phy_sel, (int)RTCCNTL.usb_conf.sw_usb_phy_sel,
           (int)otg.phy_sel, (int)otg.pad_enable, (int)otg.pad_pull_override, (int)otg.dp_pullup,
           (int)otg.dp_pulldown, (int)otg.dm_pullup, (int)otg.dm_pulldown);
  ESP_LOGI(TAG, "phy: usb-serial-jtag pull_ovr=%d dp_pu=%d",
           (int)RTCCNTL.usb_conf.usb_pad_pull_override, (int)RTCCNTL.usb_conf.usb_dp_pullup);
}

// Logs the root port's state when power, device presence, enable or speed change. The line
// state (D+/D-) is included for context but doesn't trigger a log line: it toggles constantly
// once traffic is flowing. A full-speed device that is attached and pulling D+ up shows
// "connected=1 lines=J" before enumeration.
void watch_port() {
  static uint32_t last = UINT32_MAX;
  usb_dwc_hprt_reg_t hprt;
  hprt.val = USB_DWC.hprt_reg.val;

  usb_dwc_hprt_reg_t key = {};
  key.prtpwr = hprt.prtpwr;
  key.prtconnsts = hprt.prtconnsts;
  key.prtena = hprt.prtena;
  key.prtspd = hprt.prtspd;
  key.prtovrcurract = hprt.prtovrcurract;
  if (key.val == last) return;
  last = key.val;

  ESP_LOGI(TAG, "port: power=%d connected=%d enabled=%d reset=%d lines=%s speed=%s",
           (int)hprt.prtpwr, (int)hprt.prtconnsts, (int)hprt.prtena, (int)hprt.prtrst,
           line_state_name(hprt.prtlnsts), speed_name(hprt.prtspd));
}

void retry_port_if_idle() {
  static TickType_t idle_since = xTaskGetTickCount();
  if (dev) {
    idle_since = xTaskGetTickCount();
    return;
  }
  usb_host_lib_info_t info;
  if (usb_host_lib_info(&info) == ESP_OK && info.num_devices > 0) {
    idle_since = xTaskGetTickCount();
    return;
  }
  if (xTaskGetTickCount() - idle_since < kPortRetryInterval) return;

  ESP_LOGW(TAG, "no device enumerated; power-cycling the root port");
  usb_host_lib_set_root_port_power(false);
  vTaskDelay(pdMS_TO_TICKS(200));
  usb_host_lib_set_root_port_power(true);
  idle_since = xTaskGetTickCount();
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

// Synchronous control transfer, run from the client task by pumping its own events.
esp_err_t control(uint8_t bm_request_type, uint8_t b_request, uint16_t w_value, uint16_t w_index,
                  const uint8_t *data, uint16_t len) {
  usb_setup_packet_t *setup = (usb_setup_packet_t *)ctrl_xfer->data_buffer;
  setup->bmRequestType = bm_request_type;
  setup->bRequest = b_request;
  setup->wValue = w_value;
  setup->wIndex = w_index;
  setup->wLength = len;
  if (len) memcpy(ctrl_xfer->data_buffer + sizeof(usb_setup_packet_t), data, len);

  ctrl_xfer->device_handle = dev;
  ctrl_xfer->bEndpointAddress = 0;
  ctrl_xfer->callback = ctrl_cb;
  ctrl_xfer->num_bytes = sizeof(usb_setup_packet_t) + len;
  ctrl_xfer->timeout_ms = 1000;
  ctrl_done = false;

  esp_err_t err = usb_host_transfer_submit_control(client, ctrl_xfer);
  if (err != ESP_OK) return err;
  inflight++;

  const TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(1000);
  while (!ctrl_done) {
    usb_host_client_handle_events(client, pdMS_TO_TICKS(10));
    if (xTaskGetTickCount() > deadline) return ESP_ERR_TIMEOUT;
  }
  return ctrl_xfer->status == USB_TRANSFER_STATUS_COMPLETED ? ESP_OK : ESP_FAIL;
}

void parse_config(const usb_config_desc_t *cfg) {
  eps = Endpoints{};
  int offset = 0;
  const usb_standard_desc_t *d = (const usb_standard_desc_t *)cfg;
  const usb_intf_desc_t *intf = nullptr;

  while ((d = usb_parse_next_descriptor(d, cfg->wTotalLength, &offset)) != nullptr) {
    if (d->bDescriptorType == USB_B_DESCRIPTOR_TYPE_INTERFACE) {
      intf = (const usb_intf_desc_t *)d;
      if (intf->bInterfaceClass == kClassCdcComm && eps.comm_iface < 0) {
        eps.comm_iface = intf->bInterfaceNumber;
      }
      continue;
    }
    if (d->bDescriptorType != USB_B_DESCRIPTOR_TYPE_ENDPOINT || !intf) continue;

    const usb_ep_desc_t *ep = (const usb_ep_desc_t *)d;
    const uint8_t type = ep->bmAttributes & USB_BM_ATTRIBUTES_XFERTYPE_MASK;
    const bool in = ep->bEndpointAddress & USB_B_ENDPOINT_ADDRESS_EP_DIR_MASK;
    const uint16_t mps = USB_EP_DESC_GET_MPS(ep);

    if (intf->bInterfaceClass == kClassCdcData && type == USB_BM_ATTRIBUTES_XFER_BULK) {
      if (eps.data_iface < 0) eps.data_iface = intf->bInterfaceNumber;
      if (intf->bInterfaceNumber != eps.data_iface) continue;
      if (in && !eps.bulk_in) {
        eps.bulk_in = ep->bEndpointAddress;
        eps.bulk_in_mps = mps;
      } else if (!in && !eps.bulk_out) {
        eps.bulk_out = ep->bEndpointAddress;
        eps.bulk_out_mps = mps;
      }
    } else if (intf->bInterfaceClass == kClassAudio &&
               intf->bInterfaceSubClass == kSubclassAudioStreaming &&
               type == USB_BM_ATTRIBUTES_XFER_ISOC && in && eps.audio_iface < 0) {
      eps.audio_iface = intf->bInterfaceNumber;
      eps.audio_alt = intf->bAlternateSetting;
      eps.iso_in = ep->bEndpointAddress;
      eps.iso_mps = mps;
    }
  }
}

bool start_audio() {
  if (eps.audio_iface < 0 || eps.iso_mps == 0) {
    ESP_LOGW(TAG, "no isochronous audio endpoint found; audio disabled");
    return false;
  }

  esp_err_t err = usb_host_interface_claim(client, dev, eps.audio_iface, eps.audio_alt);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "claim audio iface %d alt %d: %s", eps.audio_iface, eps.audio_alt,
             esp_err_to_name(err));
    eps.audio_iface = -1;
    return false;
  }
  err = control(USB_BM_REQUEST_TYPE_DIR_OUT | USB_BM_REQUEST_TYPE_TYPE_STANDARD |
                    USB_BM_REQUEST_TYPE_RECIP_INTERFACE,
                USB_B_REQUEST_SET_INTERFACE, eps.audio_alt, eps.audio_iface, nullptr, 0);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "SET_INTERFACE audio: %s", esp_err_to_name(err));
    return false;
  }

  for (int i = 0; i < kIsoTransfers; i++) {
    if (!iso_xfer[i]) {
      if (usb_host_transfer_alloc(eps.iso_mps * kIsoPacketsPerTransfer, kIsoPacketsPerTransfer,
                                  &iso_xfer[i]) != ESP_OK) {
        ESP_LOGE(TAG, "iso transfer alloc failed");
        return false;
      }
    }
    usb_transfer_t *t = iso_xfer[i];
    t->device_handle = dev;
    t->bEndpointAddress = eps.iso_in;
    t->callback = iso_in_cb;
    t->num_bytes = eps.iso_mps * kIsoPacketsPerTransfer;
    for (int p = 0; p < kIsoPacketsPerTransfer; p++) t->isoc_packet_desc[p].num_bytes = eps.iso_mps;
    if (usb_host_transfer_submit(t) != ESP_OK) {
      ESP_LOGE(TAG, "iso submit failed");
      return false;
    }
    inflight++;
  }
  ESP_LOGI(TAG, "audio streaming: iface %d alt %d EP 0x%02X mps %u", eps.audio_iface,
           eps.audio_alt, eps.iso_in, eps.iso_mps);
  return true;
}

void release_all() {
  tearing_down = true;

  const uint8_t endpoints[] = {eps.bulk_in, eps.bulk_out, eps.iso_in};
  for (uint8_t ep : endpoints) {
    if (!ep) continue;
    usb_host_endpoint_halt(dev, ep);
    usb_host_endpoint_flush(dev, ep);
    usb_host_endpoint_clear(dev, ep);
  }

  // Let cancelled/failed transfers drain back through their callbacks.
  const TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(500);
  while (inflight > 0 && xTaskGetTickCount() < deadline) {
    usb_host_client_handle_events(client, pdMS_TO_TICKS(10));
  }
  if (inflight > 0) ESP_LOGW(TAG, "%d transfers still in flight at teardown", inflight);

  if (eps.audio_iface >= 0) usb_host_interface_release(client, dev, eps.audio_iface);
  if (eps.data_iface >= 0) usb_host_interface_release(client, dev, eps.data_iface);
  if (eps.comm_iface >= 0) usb_host_interface_release(client, dev, eps.comm_iface);
  usb_host_device_close(client, dev);

  dev = nullptr;
  eps = Endpoints{};
  inflight = 0;
  out_busy = false;
  audio_on = false;
  tearing_down = false;
  xQueueReset(out_queue);
}

void open_device(uint8_t addr) {
  if (dev) {
    ESP_LOGW(TAG, "ignoring device %u: already connected", addr);
    return;
  }
  if (usb_host_device_open(client, addr, &dev) != ESP_OK) {
    ESP_LOGE(TAG, "failed to open device %u", addr);
    dev = nullptr;
    return;
  }

  const usb_device_desc_t *dd;
  usb_host_get_device_descriptor(dev, &dd);
  ESP_LOGI(TAG, "device %u: VID %04X PID %04X", addr, dd->idVendor, dd->idProduct);

  const bool known_m8 = dd->idVendor == kPjrcVid &&
                        (dd->idProduct == kM8PidStereo || dd->idProduct == kM8PidMultichannel);

  const usb_config_desc_t *cfg;
  usb_host_get_active_config_descriptor(dev, &cfg);
  parse_config(cfg);

  if (dd->idVendor != kPjrcVid || eps.data_iface < 0 || !eps.bulk_in || !eps.bulk_out) {
    ESP_LOGW(TAG, "not an M8 (no PJRC CDC interface); ignoring");
    usb_host_device_close(client, dev);
    dev = nullptr;
    cur_state = State::kUnsupported;
    return;
  }
  if (!known_m8) ESP_LOGW(TAG, "unrecognised PJRC PID %04X, trying anyway", dd->idProduct);

  esp_err_t err = ESP_OK;
  if (eps.comm_iface >= 0) err = usb_host_interface_claim(client, dev, eps.comm_iface, 0);
  if (err == ESP_OK) err = usb_host_interface_claim(client, dev, eps.data_iface, 0);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "claim CDC: %s", esp_err_to_name(err));
    release_all();
    cur_state = State::kError;
    return;
  }

  // Line state (DTR|RTS) and 115200 8N1, as m8c does. The baud rate is irrelevant over USB but
  // the Teensy core expects the request.
  const uint16_t comm = eps.comm_iface >= 0 ? eps.comm_iface : 0;
  const uint8_t class_out = USB_BM_REQUEST_TYPE_DIR_OUT | USB_BM_REQUEST_TYPE_TYPE_CLASS |
                            USB_BM_REQUEST_TYPE_RECIP_INTERFACE;
  err = control(class_out, kCdcSetControlLineState, kCdcDtrRts, comm, nullptr, 0);
  if (err == ESP_OK) {
    static const uint8_t coding[7] = {0x00, 0xC2, 0x01, 0x00, 0x00, 0x00, 0x08};
    err = control(class_out, kCdcSetLineCoding, 0, comm, coding, sizeof(coding));
  }
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "CDC line setup: %s", esp_err_to_name(err));
    release_all();
    cur_state = State::kError;
    return;
  }

  for (int i = 0; i < kBulkInTransfers; i++) {
    usb_transfer_t *t = bulk_in_xfer[i];
    t->device_handle = dev;
    t->bEndpointAddress = eps.bulk_in;
    t->callback = bulk_in_cb;
    t->num_bytes = usb_round_up_to_mps(kBulkInSize, eps.bulk_in_mps);
    if (usb_host_transfer_submit(t) == ESP_OK) inflight++;
  }

  audio_on = start_audio();

  ESP_LOGI(TAG, "M8 connected: CDC iface %d (IN 0x%02X, OUT 0x%02X)", eps.data_iface, eps.bulk_in,
           eps.bulk_out);
  xStreamBufferReset(rx_stream);
  cur_state = State::kConnected;
  connections = connections + 1;
}

void pump_out_queue() {
  if (!dev || out_busy || tearing_down) return;
  OutMsg msg;
  if (xQueueReceive(out_queue, &msg, 0) != pdTRUE) return;

  memcpy(out_xfer->data_buffer, msg.data, msg.len);
  out_xfer->num_bytes = msg.len;
  out_xfer->device_handle = dev;
  out_xfer->bEndpointAddress = eps.bulk_out;
  out_xfer->callback = out_cb;
  if (usb_host_transfer_submit(out_xfer) == ESP_OK) {
    inflight++;
    out_busy = true;
  } else {
    ESP_LOGW(TAG, "bulk OUT submit failed");
  }
}

// ---------------------------------------------------------------------------
// Tasks
// ---------------------------------------------------------------------------

void host_lib_task(void *) {
  for (;;) {
    uint32_t flags;
    usb_host_lib_handle_events(portMAX_DELAY, &flags);
    if (flags & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS) usb_host_device_free_all();
  }
}

void client_task(void *) {
  const usb_host_client_config_t cfg = {
      .is_synchronous = false,
      .max_num_event_msg = 5,
      .async = {.client_event_callback = client_event_cb, .callback_arg = nullptr},
  };
  ESP_ERROR_CHECK(usb_host_client_register(&cfg, &client));

  for (int i = 0; i < kBulkInTransfers; i++) {
    ESP_ERROR_CHECK(usb_host_transfer_alloc(kBulkInSize, 0, &bulk_in_xfer[i]));
  }
  ESP_ERROR_CHECK(usb_host_transfer_alloc(64, 0, &out_xfer));
  ESP_ERROR_CHECK(usb_host_transfer_alloc(sizeof(usb_setup_packet_t) + 64, 0, &ctrl_xfer));

  log_phy_routing();

  for (;;) {
    usb_host_client_handle_events(client, pdMS_TO_TICKS(20));
    watch_port();
    retry_port_if_idle();

    if (pending_gone) {
      pending_gone = false;
      ESP_LOGI(TAG, "M8 disconnected");
      release_all();
      cur_state = State::kWaiting;
    }
    if (pending_new_addr) {
      const uint8_t addr = pending_new_addr;
      pending_new_addr = 0;
      open_device(addr);
    }
    pump_out_queue();
  }
}

} // namespace

bool start(StreamBufferHandle_t rx, AudioSink audio) {
  rx_stream = rx;
  audio_sink = audio;
  out_queue = xQueueCreate(32, sizeof(OutMsg));

  usb_host_config_t host_cfg = {};
  host_cfg.skip_phy_setup = false;
  host_cfg.intr_flags = ESP_INTR_FLAG_LEVEL1;
  host_cfg.enum_filter_cb = enum_filter_cb;
  // The prebuilt Arduino sdkconfig biases the 256-line (4-byte) FIFO towards periodic OUT.
  // We need the opposite: room for ~180-byte isochronous IN audio packets alongside bulk IN.
  host_cfg.fifo_settings_custom.rx_fifo_lines = 136;  // 544 B: two iso packets + status words
  host_cfg.fifo_settings_custom.nptx_fifo_lines = 32; // 128 B: two 64 B bulk/control OUT packets
  host_cfg.fifo_settings_custom.ptx_fifo_lines = 16;  // no periodic OUT endpoints in use
  esp_err_t err = usb_host_install(&host_cfg);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "usb_host_install: %s", esp_err_to_name(err));
    return false;
  }

  xTaskCreatePinnedToCore(host_lib_task, "usb_lib", 4096, nullptr, 20, nullptr, USB_CORE);
  xTaskCreatePinnedToCore(client_task, "usb_m8", 6144, nullptr, 19, nullptr, USB_CORE);
  ESP_LOGI(TAG, "USB host started");
  return true;
}

State state() { return cur_state; }
uint32_t connection_count() { return connections; }
bool audio_streaming() { return audio_on; }
AudioStats audio_stats() { return iso_stats; }

bool send(const uint8_t *data, size_t len) {
  if (len == 0 || len > kOutMaxLen || cur_state != State::kConnected) return false;
  OutMsg msg;
  msg.len = (uint8_t)len;
  memcpy(msg.data, data, len);
  if (xQueueSend(out_queue, &msg, 0) != pdTRUE) return false;
  if (client) usb_host_client_unblock(client);
  return true;
}

} // namespace m8usb
