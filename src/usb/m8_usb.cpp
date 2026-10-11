#include "m8_usb.h"

#include <esp_intr_alloc.h>
#include <esp_log.h>
#include <freertos/queue.h>
#include <freertos/task.h>
#include <stdio.h>
#include <string.h>
#include <usb/usb_helpers.h>
#include <usb/usb_host.h>

#include "soc/usb_dwc_struct.h"

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
constexpr uint8_t kSubclassMidiStreaming = 0x03;

constexpr uint8_t kCdcSetLineCoding = 0x20;
constexpr uint8_t kCdcSetControlLineState = 0x22;
constexpr uint16_t kCdcDtrRts = 0x03;

constexpr int kBulkInSize = 512; // a multiple of the bulk MPS at both full (64) and high (512) speed
constexpr int kBulkInTransfers = 2;
constexpr int kIsoTransfers = 4;
// The M8's audio endpoints use bInterval 2: one packet per 250 us at high speed. 16 packets
// make a 4 ms transfer; with 4 in flight there's 16 ms of slack for the client task.
constexpr int kIsoPacketsPerTransfer = 16;
// Audio to the M8 (its USB input): the same 250 us packets, sized from the M8's feedback.
constexpr int kIsoOutTransfers = 4;
constexpr int kIsoOutPackets = 16;
// Feedback from the M8: one 4-byte value per ms.
constexpr int kFeedbackTransfers = 2;
constexpr int kFeedbackPackets = 4;
// High-speed feedback is 16.16 samples per microframe: 44.1 kHz is 5.5125.
constexpr uint32_t kFeedbackNominal = (uint32_t)(44100.0 / 8000.0 * 65536.0);
constexpr uint32_t kFeedbackMin = kFeedbackNominal * 98 / 100;
constexpr uint32_t kFeedbackMax = kFeedbackNominal * 102 / 100;
constexpr int kOutMicroframesPerPacket = 2; // bInterval 2
constexpr int kOutMaxLen = 8;
// USB MIDI, forwarded for passthrough: event packets in 64-byte transfers.
constexpr int kMidiInTransfers = 2;
constexpr int kMidiMaxLen = 64;

// If no device has enumerated for this long, power-cycle the root port: the host-side
// equivalent of replugging, in case a device that attached during boot was missed.
constexpr TickType_t kPortRetryInterval = pdMS_TO_TICKS(5000);

struct OutMsg {
  uint8_t len;
  uint8_t data[kOutMaxLen];
};

struct MidiMsg {
  uint8_t len;
  uint8_t data[kMidiMaxLen];
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

  // The M8's audio input
  int input_iface = -1;
  uint8_t input_alt = 0;
  uint8_t iso_out = 0;
  uint16_t iso_out_mps = 0;
  uint8_t feedback_in = 0;
  uint16_t feedback_mps = 0;

  int midi_iface = -1;
  uint8_t midi_in = 0, midi_out = 0;
  uint16_t midi_in_mps = 64;
};

usb_host_client_handle_t client;
usb_device_handle_t dev;
Endpoints eps;

StreamBufferHandle_t rx_stream;
AudioSink audio_sink;
AudioSource audio_source;
DataSink serial_tap;
DataSink midi_tap;
Identity ident;
volatile bool want_input; // set from any task
bool input_on;            // client task only
uint32_t feedback = kFeedbackNominal;
uint32_t out_acc;         // fractional samples carried between packets, 16.16
QueueHandle_t out_queue;
QueueHandle_t midi_queue;

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
usb_transfer_t *iso_out_xfer[kIsoOutTransfers];
usb_transfer_t *feedback_xfer[kFeedbackTransfers];
int input_inflight; // of the transfers above
usb_transfer_t *midi_in_xfer[kMidiInTransfers];
usb_transfer_t *out_xfer;
usb_transfer_t *midi_out_xfer;
usb_transfer_t *ctrl_xfer;
int inflight;
bool out_busy;
bool midi_out_busy;
bool tearing_down;
uint32_t rx_dropped;
AudioStats iso_stats; // written by the client task, read elsewhere as a snapshot
InputStats input_stats;

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
      if (serial_tap) serial_tap(t->data_buffer, t->actual_num_bytes);
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

// Fills an OUT transfer with audio for the M8: each packet carries the samples the M8's clock
// has advanced by (from its feedback), 11 or 12 frames.
void fill_out(usb_transfer_t *t) {
  const int max_frames = eps.iso_out_mps / 4;
  int offset = 0;
  for (int p = 0; p < kIsoOutPackets; p++) {
    out_acc += feedback * kOutMicroframesPerPacket;
    int frames = (int)(out_acc >> 16);
    out_acc &= 0xFFFF;
    if (frames > max_frames) frames = max_frames;
    if (audio_source) {
      audio_source((int16_t *)(t->data_buffer + offset), frames);
    } else {
      memset(t->data_buffer + offset, 0, frames * 4);
    }
    t->isoc_packet_desc[p].num_bytes = frames * 4;
    offset += frames * 4;
  }
  t->num_bytes = offset;
}

void submit_input(usb_transfer_t *t) {
  if (tearing_down || !input_on) return;
  if (usb_host_transfer_submit(t) == ESP_OK) {
    input_inflight++;
  } else {
    ESP_LOGW(TAG, "resubmit failed on EP 0x%02X", t->bEndpointAddress);
  }
}

void iso_out_cb(usb_transfer_t *t) {
  input_inflight--;
  if (t->status == USB_TRANSFER_STATUS_NO_DEVICE || t->status == USB_TRANSFER_STATUS_CANCELED) {
    return;
  }
  if (t->status == USB_TRANSFER_STATUS_COMPLETED) {
    for (int i = 0; i < t->num_isoc_packets; i++) {
      if (t->isoc_packet_desc[i].status == USB_TRANSFER_STATUS_COMPLETED) {
        input_stats.packets_ok++;
      } else {
        input_stats.packets_failed++;
      }
    }
  } else {
    input_stats.packets_failed += t->num_isoc_packets;
  }
  fill_out(t);
  submit_input(t);
}

// A feedback report as 16.16 samples per microframe, or 0 if it doesn't look like 44.1 kHz in
// any of the units devices use. The M8 reports 16.16 samples per data packet (2 microframes:
// 11.025). The spec's high-speed unit is per microframe, and some devices report per frame.
uint32_t parse_feedback(const uint8_t *b, int bytes) {
  if (bytes < 3) return 0;
  const uint32_t v = b[0] | (b[1] << 8) | (b[2] << 16) | (bytes >= 4 ? (uint32_t)b[3] << 24 : 0);
  const uint32_t candidates[] = {
      v / kOutMicroframesPerPacket, // per data packet (the M8)
      v,                            // per microframe
      v / 8,                        // per frame (1 ms)
  };
  for (uint32_t c : candidates) {
    if (c >= kFeedbackMin && c <= kFeedbackMax) return c;
  }
  return 0;
}

void feedback_cb(usb_transfer_t *t) {
  input_inflight--;
  if (t->status == USB_TRANSFER_STATUS_NO_DEVICE || t->status == USB_TRANSFER_STATUS_CANCELED) {
    return;
  }
  if (t->status == USB_TRANSFER_STATUS_COMPLETED) {
    int offset = 0;
    for (int i = 0; i < t->num_isoc_packets; i++) {
      const usb_isoc_packet_desc_t &p = t->isoc_packet_desc[i];
      if (p.status != USB_TRANSFER_STATUS_COMPLETED) {
        input_stats.feedback_failed++;
      } else if (p.actual_num_bytes == 0) {
        input_stats.feedback_empty++;
      } else {
        const uint8_t *b = t->data_buffer + offset;
        input_stats.feedback_raw = 0;
        for (int k = 0; k < p.actual_num_bytes && k < 4; k++) {
          input_stats.feedback_raw |= (uint32_t)b[k] << (8 * k);
        }
        input_stats.feedback_bytes = p.actual_num_bytes;
        const uint32_t v = parse_feedback(b, p.actual_num_bytes);
        if (v) {
          feedback = v;
          input_stats.feedback = v;
          input_stats.feedback_count++;
        }
      }
      offset += p.num_bytes;
    }
  } else {
    input_stats.feedback_failed += t->num_isoc_packets;
  }
  submit_input(t);
}

void midi_in_cb(usb_transfer_t *t) {
  inflight--;
  if (t->status == USB_TRANSFER_STATUS_COMPLETED) {
    if (t->actual_num_bytes > 0 && midi_tap) midi_tap(t->data_buffer, t->actual_num_bytes);
    resubmit(t);
  } else if (t->status != USB_TRANSFER_STATUS_NO_DEVICE &&
             t->status != USB_TRANSFER_STATUS_CANCELED) {
    ESP_LOGW(TAG, "MIDI IN status %d", t->status);
    resubmit(t);
  }
}

void out_cb(usb_transfer_t *t) {
  inflight--;
  out_busy = false;
  if (t->status != USB_TRANSFER_STATUS_COMPLETED) ESP_LOGW(TAG, "bulk OUT status %d", t->status);
}

void midi_out_cb(usb_transfer_t *t) {
  inflight--;
  midi_out_busy = false;
  if (t->status != USB_TRANSFER_STATUS_COMPLETED) ESP_LOGW(TAG, "MIDI OUT status %d", t->status);
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

// Logs the root port's state when power, device presence, enable or speed change. The line
// state (D+/D-) is included for context but doesn't trigger a log line: it toggles constantly
// once traffic is flowing. An attached device shows "connected=1" before enumeration, and the
// M8 should then come up at speed=high.
void watch_port() {
  static uint32_t last = UINT32_MAX;
  usb_dwc_hprt_reg_t hprt;
  hprt.val = USB_DWC_HS.hprt_reg.val; // the M8 is on the high-speed controller (USB-A)

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
               intf->bInterfaceSubClass == kSubclassMidiStreaming &&
               type == USB_BM_ATTRIBUTES_XFER_BULK) {
      if (eps.midi_iface < 0) eps.midi_iface = intf->bInterfaceNumber;
      if (intf->bInterfaceNumber != eps.midi_iface) continue;
      if (in && !eps.midi_in) {
        eps.midi_in = ep->bEndpointAddress;
        eps.midi_in_mps = mps;
      } else if (!in && !eps.midi_out) {
        eps.midi_out = ep->bEndpointAddress;
      }
    } else if (intf->bInterfaceClass == kClassAudio &&
               intf->bInterfaceSubClass == kSubclassAudioStreaming &&
               type == USB_BM_ATTRIBUTES_XFER_ISOC) {
      const bool feedback_ep = ((ep->bmAttributes >> 4) & 3) == 1;
      if (in && !feedback_ep && eps.audio_iface < 0) { // M8 audio out
        eps.audio_iface = intf->bInterfaceNumber;
        eps.audio_alt = intf->bAlternateSetting;
        eps.iso_in = ep->bEndpointAddress;
        eps.iso_mps = mps;
      } else if (!in && eps.input_iface < 0) { // M8 audio in
        eps.input_iface = intf->bInterfaceNumber;
        eps.input_alt = intf->bAlternateSetting;
        eps.iso_out = ep->bEndpointAddress;
        eps.iso_out_mps = mps;
      } else if (in && feedback_ep && intf->bInterfaceNumber == eps.input_iface) {
        eps.feedback_in = ep->bEndpointAddress;
        eps.feedback_mps = mps;
      }
    }
  }
}

// ---------------------------------------------------------------------------
// Diagnostics: describe the device's interfaces and endpoints
// ---------------------------------------------------------------------------

// Raw hex for class-specific descriptors in ESP-IDF's full descriptor printout.
void print_class_desc_hex(const usb_standard_desc_t *d) {
  const uint8_t *b = (const uint8_t *)d;
  printf("\t*** class-specific (type 0x%02X, %u bytes):", b[1], b[0]);
  for (int i = 0; i < b[0]; i++) printf(" %02X", b[i]);
  printf("\n");
}

// Logs every interface and endpoint, decoding USB Audio Class 1.0 streaming formats, so we
// can see which audio directions, formats and sync modes the M8 offers.
void log_descriptor_summary(const usb_config_desc_t *cfg) {
  static const char *types[] = {"control", "iso", "bulk", "interrupt"};
  static const char *syncs[] = {"none", "async", "adaptive", "sync"};
  static const char *usages[] = {"data", "feedback", "implicit-fb", "reserved"};

  ESP_LOGI(TAG, "config: %u interfaces, %u bytes", cfg->bNumInterfaces, cfg->wTotalLength);
  int offset = 0;
  const usb_standard_desc_t *d = (const usb_standard_desc_t *)cfg;
  const usb_intf_desc_t *intf = nullptr;
  while ((d = usb_parse_next_descriptor(d, cfg->wTotalLength, &offset)) != nullptr) {
    const uint8_t *b = (const uint8_t *)d;
    if (d->bDescriptorType == USB_B_DESCRIPTOR_TYPE_INTERFACE) {
      intf = (const usb_intf_desc_t *)d;
      ESP_LOGI(TAG, "  iface %u alt %u: class %02X sub %02X proto %02X, %u endpoints",
               intf->bInterfaceNumber, intf->bAlternateSetting, intf->bInterfaceClass,
               intf->bInterfaceSubClass, intf->bInterfaceProtocol, intf->bNumEndpoints);
    } else if (d->bDescriptorType == USB_B_DESCRIPTOR_TYPE_ENDPOINT) {
      const usb_ep_desc_t *ep = (const usb_ep_desc_t *)d;
      const uint8_t attr = ep->bmAttributes;
      const bool iso = (attr & 3) == 1;
      char extra[64] = "";
      if (iso) {
        snprintf(extra, sizeof(extra), " sync=%s usage=%s", syncs[(attr >> 2) & 3],
                 usages[(attr >> 4) & 3]);
        if (b[0] >= 9) { // audio class endpoints carry bRefresh and bSynchAddress
          snprintf(extra + strlen(extra), sizeof(extra) - strlen(extra),
                   " refresh %u synch-addr 0x%02X", b[7], b[8]);
        }
      }
      ESP_LOGI(TAG, "    EP 0x%02X %s %s mps %u (raw 0x%04X) interval %u%s", ep->bEndpointAddress,
               (ep->bEndpointAddress & 0x80) ? "IN " : "OUT", types[attr & 3],
               USB_EP_DESC_GET_MPS(ep), ep->wMaxPacketSize, ep->bInterval, extra);
    } else if (intf && intf->bInterfaceClass == kClassAudio &&
               intf->bInterfaceSubClass == kSubclassAudioStreaming && b[1] == 0x24 && b[0] >= 3) {
      const bool uac2 = intf->bInterfaceProtocol == 0x20;
      if (b[2] == 0x01 && uac2 && b[0] >= 16) { // AS_GENERAL (UAC 2.0)
        const unsigned long formats = b[6] | (b[7] << 8) | (b[8] << 16) | ((uint32_t)b[9] << 24);
        const unsigned long channels =
            b[11] | (b[12] << 8) | (b[13] << 16) | ((uint32_t)b[14] << 24);
        ESP_LOGI(TAG, "    AS general (UAC2): terminal link %u, format type %u, formats 0x%08lX, "
                      "%u ch, channel config 0x%08lX",
                 b[3], b[5], formats, b[10], channels);
      } else if (b[2] == 0x02 && uac2 && b[0] >= 6 && b[3] == 0x01) { // FORMAT_TYPE I (UAC 2.0)
        ESP_LOGI(TAG, "    format (UAC2): %u-byte subslots, %u-bit; rate set by the clock source",
                 b[4], b[5]);
      } else if (b[2] == 0x01 && !uac2 && b[0] >= 7) { // AS_GENERAL (UAC 1.0)
        ESP_LOGI(TAG, "    AS general (UAC1): terminal link %u, delay %u, format tag 0x%04X", b[3],
                 b[4], b[5] | (b[6] << 8));
      } else if (b[2] == 0x02 && !uac2 && b[0] >= 8 && b[3] == 0x01) { // FORMAT_TYPE I (UAC 1.0)
        char rates[64] = "";
        for (int i = 0; i < b[7] && 8 + i * 3 + 2 < b[0]; i++) {
          const uint32_t hz = b[8 + i * 3] | (b[9 + i * 3] << 8) | (b[10 + i * 3] << 16);
          snprintf(rates + strlen(rates), sizeof(rates) - strlen(rates), "%s%lu", i ? "," : "",
                   (unsigned long)hz);
        }
        ESP_LOGI(TAG, "    format (UAC1): %u ch, %u-byte subframes, %u-bit, rates %s Hz%s", b[4],
                 b[5], b[6], rates, b[7] == 0 ? " (continuous)" : "");
      }
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

// Halts, flushes and clears endpoints, so their transfers come back cancelled.
void cancel_endpoints(const uint8_t *endpoints, int n) {
  for (int i = 0; i < n; i++) {
    if (!endpoints[i]) continue;
    usb_host_endpoint_halt(dev, endpoints[i]);
    usb_host_endpoint_flush(dev, endpoints[i]);
    usb_host_endpoint_clear(dev, endpoints[i]);
  }
}

// Starts streaming audio to the M8's USB input: selects the input's streaming alternate
// setting, then keeps feedback and audio transfers in flight.
bool start_input() {
  if (eps.input_iface < 0 || !eps.iso_out || !eps.iso_out_mps) {
    ESP_LOGW(TAG, "the M8 has no USB audio input");
    return false;
  }
  esp_err_t err = usb_host_interface_claim(client, dev, eps.input_iface, eps.input_alt);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "claim input iface %d alt %d: %s", eps.input_iface, eps.input_alt,
             esp_err_to_name(err));
    return false;
  }
  err = control(USB_BM_REQUEST_TYPE_DIR_OUT | USB_BM_REQUEST_TYPE_TYPE_STANDARD |
                    USB_BM_REQUEST_TYPE_RECIP_INTERFACE,
                USB_B_REQUEST_SET_INTERFACE, eps.input_alt, eps.input_iface, nullptr, 0);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "SET_INTERFACE input: %s", esp_err_to_name(err));
    usb_host_interface_release(client, dev, eps.input_iface);
    return false;
  }

  input_on = true;
  feedback = kFeedbackNominal;
  out_acc = 0;
  if (eps.feedback_in) {
    for (int i = 0; i < kFeedbackTransfers; i++) {
      if (!feedback_xfer[i] &&
          usb_host_transfer_alloc(eps.feedback_mps * kFeedbackPackets, kFeedbackPackets,
                                  &feedback_xfer[i]) != ESP_OK) {
        break;
      }
      usb_transfer_t *t = feedback_xfer[i];
      t->device_handle = dev;
      t->bEndpointAddress = eps.feedback_in;
      t->callback = feedback_cb;
      t->num_bytes = eps.feedback_mps * kFeedbackPackets;
      for (int p = 0; p < kFeedbackPackets; p++) {
        t->isoc_packet_desc[p].num_bytes = eps.feedback_mps;
      }
      submit_input(t);
    }
  }
  for (int i = 0; i < kIsoOutTransfers; i++) {
    if (!iso_out_xfer[i] &&
        usb_host_transfer_alloc(eps.iso_out_mps * kIsoOutPackets, kIsoOutPackets,
                                &iso_out_xfer[i]) != ESP_OK) {
      ESP_LOGE(TAG, "iso OUT transfer alloc failed");
      break;
    }
    usb_transfer_t *t = iso_out_xfer[i];
    t->device_handle = dev;
    t->bEndpointAddress = eps.iso_out;
    t->callback = iso_out_cb;
    fill_out(t);
    submit_input(t);
  }
  ESP_LOGI(TAG, "audio input streaming: iface %d alt %d EP 0x%02X mps %u, feedback EP 0x%02X",
           eps.input_iface, eps.input_alt, eps.iso_out, eps.iso_out_mps, eps.feedback_in);
  return true;
}

void stop_input() {
  if (!input_on) return;
  input_on = false; // callbacks stop resubmitting
  const uint8_t endpoints[] = {eps.iso_out, eps.feedback_in};
  cancel_endpoints(endpoints, 2);
  const TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(500);
  while (input_inflight > 0 && xTaskGetTickCount() < deadline) {
    usb_host_client_handle_events(client, pdMS_TO_TICKS(10));
  }
  if (input_inflight > 0) ESP_LOGW(TAG, "%d input transfers still in flight", input_inflight);
  input_inflight = 0;
  // Back to the zero-bandwidth alternate setting.
  control(USB_BM_REQUEST_TYPE_DIR_OUT | USB_BM_REQUEST_TYPE_TYPE_STANDARD |
              USB_BM_REQUEST_TYPE_RECIP_INTERFACE,
          USB_B_REQUEST_SET_INTERFACE, 0, eps.input_iface, nullptr, 0);
  usb_host_interface_release(client, dev, eps.input_iface);
  ESP_LOGI(TAG, "audio input stopped");
}

void release_all() {
  tearing_down = true;

  const bool had_input = input_on;
  input_on = false;
  const uint8_t endpoints[] = {eps.bulk_in, eps.bulk_out, eps.iso_in, eps.iso_out,
                               eps.feedback_in, eps.midi_in, eps.midi_out};
  cancel_endpoints(endpoints, sizeof(endpoints));

  // Let cancelled/failed transfers drain back through their callbacks.
  const TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(500);
  while ((inflight > 0 || input_inflight > 0) && xTaskGetTickCount() < deadline) {
    usb_host_client_handle_events(client, pdMS_TO_TICKS(10));
  }
  if (inflight > 0 || input_inflight > 0) {
    ESP_LOGW(TAG, "%d transfers still in flight at teardown", inflight + input_inflight);
  }
  if (had_input) usb_host_interface_release(client, dev, eps.input_iface);

  if (eps.audio_iface >= 0) usb_host_interface_release(client, dev, eps.audio_iface);
  if (eps.midi_iface >= 0) usb_host_interface_release(client, dev, eps.midi_iface);
  if (eps.data_iface >= 0) usb_host_interface_release(client, dev, eps.data_iface);
  if (eps.comm_iface >= 0) usb_host_interface_release(client, dev, eps.comm_iface);
  usb_host_device_close(client, dev);

  dev = nullptr;
  eps = Endpoints{};
  inflight = 0;
  input_inflight = 0;
  out_busy = false;
  midi_out_busy = false;
  audio_on = false;
  tearing_down = false;
  xQueueReset(out_queue);
  xQueueReset(midi_queue);
}

// ASCII copy of a UTF-16LE string descriptor (non-ASCII characters become '?').
void copy_string(char *out, size_t size, const usb_str_desc_t *desc) {
  size_t n = 0;
  if (desc && desc->bLength >= 2) {
    const int chars = (desc->bLength - 2) / 2;
    for (int i = 0; i < chars && n + 1 < size; i++) {
      const uint16_t c = desc->wData[i];
      out[n++] = c >= 0x20 && c < 0x7F ? (char)c : '?';
    }
  }
  out[n] = '\0';
}

// Claims the M8's USB MIDI interface and starts reading it, for passthrough. Optional: the M8
// works without it.
void start_midi() {
  if (eps.midi_iface < 0 || !eps.midi_in || !eps.midi_out) return;
  if (usb_host_interface_claim(client, dev, eps.midi_iface, 0) != ESP_OK) {
    ESP_LOGW(TAG, "claim MIDI iface %d failed", eps.midi_iface);
    eps.midi_iface = -1;
    return;
  }
  for (int i = 0; i < kMidiInTransfers; i++) {
    usb_transfer_t *t = midi_in_xfer[i];
    t->device_handle = dev;
    t->bEndpointAddress = eps.midi_in;
    t->callback = midi_in_cb;
    t->num_bytes = usb_round_up_to_mps(kMidiMaxLen, eps.midi_in_mps);
    if (usb_host_transfer_submit(t) == ESP_OK) inflight++;
  }
  ESP_LOGI(TAG, "MIDI: iface %d (IN 0x%02X, OUT 0x%02X)", eps.midi_iface, eps.midi_in,
           eps.midi_out);
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

  if (dd->idVendor == kPjrcVid) {
    log_descriptor_summary(cfg);
    usb_print_device_descriptor(dd);
    usb_print_config_descriptor(cfg, print_class_desc_hex);
  }

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
  start_midi();

  ident.vid = dd->idVendor;
  ident.pid = dd->idProduct;
  ident.bcd_device = dd->bcdDevice;
  usb_device_info_t info = {};
  usb_host_device_info(dev, &info);
  copy_string(ident.manufacturer, sizeof(ident.manufacturer), info.str_desc_manufacturer);
  copy_string(ident.product, sizeof(ident.product), info.str_desc_product);
  copy_string(ident.serial, sizeof(ident.serial), info.str_desc_serial_num);
  ESP_LOGI(TAG, "M8 identity: \"%s\" \"%s\" serial \"%s\"", ident.manufacturer, ident.product,
           ident.serial);

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

void pump_midi_queue() {
  if (!dev || midi_out_busy || tearing_down || eps.midi_iface < 0) return;
  MidiMsg msg;
  if (xQueueReceive(midi_queue, &msg, 0) != pdTRUE) return;

  memcpy(midi_out_xfer->data_buffer, msg.data, msg.len);
  midi_out_xfer->num_bytes = msg.len;
  midi_out_xfer->device_handle = dev;
  midi_out_xfer->bEndpointAddress = eps.midi_out;
  midi_out_xfer->callback = midi_out_cb;
  if (usb_host_transfer_submit(midi_out_xfer) == ESP_OK) {
    inflight++;
    midi_out_busy = true;
  } else {
    ESP_LOGW(TAG, "MIDI OUT submit failed");
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
  for (int i = 0; i < kMidiInTransfers; i++) {
    // Room for a high-speed bulk packet, in case the M8 reports a 512-byte MPS.
    ESP_ERROR_CHECK(usb_host_transfer_alloc(512, 0, &midi_in_xfer[i]));
  }
  ESP_ERROR_CHECK(usb_host_transfer_alloc(64, 0, &out_xfer));
  ESP_ERROR_CHECK(usb_host_transfer_alloc(kMidiMaxLen, 0, &midi_out_xfer));
  ESP_ERROR_CHECK(usb_host_transfer_alloc(sizeof(usb_setup_packet_t) + 64, 0, &ctrl_xfer));

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
    pump_midi_queue();

    // Audio to the M8's input follows the mic switch.
    if (cur_state == State::kConnected && want_input != input_on) {
      if (want_input) {
        if (!start_input()) want_input = false; // don't retry every loop
      } else {
        stop_input();
      }
    }
  }
}

} // namespace

bool start(StreamBufferHandle_t rx, AudioSink audio, AudioSource input) {
  rx_stream = rx;
  audio_sink = audio;
  audio_source = input;
  out_queue = xQueueCreate(32, sizeof(OutMsg));
  midi_queue = xQueueCreate(8, sizeof(MidiMsg));

  usb_host_config_t host_cfg = {};
  host_cfg.skip_phy_setup = false;
  host_cfg.intr_flags = ESP_INTR_FLAG_LEVEL1;
  host_cfg.enum_filter_cb = enum_filter_cb;
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
InputStats input_stats_snapshot() { return input_stats; }

void set_input_enabled(bool on) {
  want_input = on;
  if (client) usb_host_client_unblock(client);
}

bool send(const uint8_t *data, size_t len) {
  if (len == 0 || len > kOutMaxLen || cur_state != State::kConnected) return false;
  OutMsg msg;
  msg.len = (uint8_t)len;
  memcpy(msg.data, data, len);
  if (xQueueSend(out_queue, &msg, 0) != pdTRUE) return false;
  if (client) usb_host_client_unblock(client);
  return true;
}

bool send_midi(const uint8_t *data, size_t len) {
  if (len == 0 || len > kMidiMaxLen || cur_state != State::kConnected) return false;
  MidiMsg msg;
  msg.len = (uint8_t)len;
  memcpy(msg.data, data, len);
  if (xQueueSend(midi_queue, &msg, 0) != pdTRUE) return false;
  if (client) usb_host_client_unblock(client);
  return true;
}

void set_taps(DataSink serial, DataSink midi) {
  serial_tap = serial;
  midi_tap = midi;
}

Identity identity() { return ident; }

} // namespace m8usb
