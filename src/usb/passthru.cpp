#include "passthru.h"

#include <atomic>
#include <string.h>

#include <driver/gpio.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "esp_private/usb_phy.h"
#include "hal/usb_serial_jtag_ll.h"
#include "hal/usb_wrap_ll.h"

// The Arduino core links a prebuilt TinyUSB for the P4 (rhport 0 is the full-speed OTG 1.1
// controller, rhport 1 the high-speed one), but the core rebuilt by custom_sdkconfig loses the
// TinyUSB options its tusb_config.h keys off. Only the device stack's core API is used here,
// so these two are all the headers need.
#define CONFIG_TINYUSB_ENABLED 1
#define CFG_TUSB_MCU OPT_MCU_ESP32P4
#include "tusb.h"
#include "class/hid/hid.h" // hid_report_type_t, for the stubs below
#include "device/usbd_pvt.h"

#include "config.h"
#include "m8/protocol.h"
#include "m8_usb.h"

static const char *TAG = "passthru";

#define U16(x) (uint8_t)((x) & 0xFF), (uint8_t)(((x) >> 8) & 0xFF)
#define U32(x) U16((x) & 0xFFFF), U16(((uint32_t)(x) >> 16) & 0xFFFF)

namespace passthru {
namespace {

constexpr uint8_t kRhport = 0; // the P4's full-speed controller

// Interfaces and endpoints are numbered as on the M8 where the hardware allows, so tools that
// address them directly (m8.run's WebUSB mode uses interface 1 and endpoint 3) find them. The
// full-speed controller has TX FIFOs 0-4 only, and TinyUSB gives each IN endpoint the FIFO of
// its own number, so IN endpoints must be 1-4: audio moves from the M8's 0x85 to 0x81. That
// also leaves no room for the M8's audio input (its interface 4), whose feedback endpoint would
// be a fifth IN endpoint; MIDI moves up to interface 4.
constexpr uint8_t kItfComm = 0;
constexpr uint8_t kItfData = 1;
constexpr uint8_t kItfAudioControl = 2;
constexpr uint8_t kItfAudioStream = 3;
constexpr uint8_t kItfMidi = 4;
constexpr uint8_t kEpNotify = 0x82;
constexpr uint8_t kEpDataOut = 0x03;
constexpr uint8_t kEpDataIn = 0x83;
constexpr uint8_t kEpMidiOut = 0x04;
constexpr uint8_t kEpMidiIn = 0x84;
constexpr uint8_t kEpAudioIn = 0x81;
constexpr uint8_t kClockId = 0x3A; // the M8's IDs
constexpr uint8_t kInputTerminalId = 0x01;
constexpr uint8_t kOutputTerminalId = 0x02;

constexpr int kBulkSize = 64;
constexpr uint32_t kSampleRate = 44100;
// One packet per 1 ms frame: 44.1 frames nominal, with room to catch up.
constexpr int kAudioMaxFrames = 49;
// Frames kept buffered for the computer: the M8's audio arrives in 4 ms bursts (one host
// transfer), so aim for about 10 ms and correct by a frame per packet outside +-5 ms.
constexpr size_t kAudioTarget = 441;
constexpr size_t kAudioBand = 220;

// Time for the computer to notice one device leave before the other appears.
constexpr TickType_t kSwitchGap = pdMS_TO_TICKS(200);

// ---------------------------------------------------------------------------
// Descriptors
// ---------------------------------------------------------------------------

constexpr uint16_t kAcTotal = 9 + 8 + 17 + 12;
constexpr uint16_t kMsTotal = 7 + 6 + 6 + 9 + 9 + 9 + 5 + 9 + 5;
constexpr uint16_t kConfigTotal = 9 + 66 + 118 + 9 + kMsTotal;
constexpr uint16_t kAudioPacketMax = kAudioMaxFrames * 4;

const uint8_t kConfig[] = {
    // Configuration: 5 interfaces, self-powered (the Tab5 has its battery), 100 mA like the M8
    9, TUSB_DESC_CONFIGURATION, U16(kConfigTotal), 5, 1, 0, 0xC0, 50,

    // --- Serial port (CDC ACM): display protocol and keys -----------------------------------
    8, TUSB_DESC_INTERFACE_ASSOCIATION, kItfComm, 2, 0x02, 0x02, 0x01, 0,
    9, TUSB_DESC_INTERFACE, kItfComm, 0, 1, 0x02, 0x02, 0x01, 0,
    5, 0x24, 0x00, U16(0x0110),        // header
    5, 0x24, 0x01, 0x01, kItfData,     // call management
    4, 0x24, 0x02, 0x06,               // ACM: line coding, break
    5, 0x24, 0x06, kItfComm, kItfData, // union
    7, TUSB_DESC_ENDPOINT, kEpNotify, TUSB_XFER_INTERRUPT, U16(16), 16,
    9, TUSB_DESC_INTERFACE, kItfData, 0, 2, 0x0A, 0x00, 0x00, 0,
    7, TUSB_DESC_ENDPOINT, kEpDataOut, TUSB_XFER_BULK, U16(kBulkSize), 0,
    7, TUSB_DESC_ENDPOINT, kEpDataIn, TUSB_XFER_BULK, U16(kBulkSize), 0,

    // --- Audio (UAC 2.0), M8 to computer: 44.1 kHz, 16-bit stereo ----------------------------
    8, TUSB_DESC_INTERFACE_ASSOCIATION, kItfAudioControl, 2, 0x01, 0x00, 0x20, 0,
    9, TUSB_DESC_INTERFACE, kItfAudioControl, 0, 0, 0x01, 0x01, 0x20, 0,
    9, 0x24, 0x01, U16(0x0200), 0x09 /* musical instrument */, U16(kAcTotal), 0,
    // Clock source: internal, fixed; frequency readable
    8, 0x24, 0x0A, kClockId, 0x01, 0x01, 0, 0,
    // Input terminal: the M8's audio (digital audio interface, as on the M8), stereo
    17, 0x24, 0x02, kInputTerminalId, U16(0x0602), 0, kClockId, 2, U32(0x3), 0, U16(0), 0,
    // Output terminal: USB streaming
    12, 0x24, 0x03, kOutputTerminalId, U16(0x0101), 0, kInputTerminalId, kClockId, U16(0), 0,
    // Streaming interface: alt 0 idle, alt 1 streaming
    9, TUSB_DESC_INTERFACE, kItfAudioStream, 0, 0, 0x01, 0x02, 0x20, 0,
    9, TUSB_DESC_INTERFACE, kItfAudioStream, 1, 1, 0x01, 0x02, 0x20, 0,
    16, 0x24, 0x01, kOutputTerminalId, 0, 0x01 /* type I */, U32(0x1) /* PCM */, 2, U32(0x3), 0,
    6, 0x24, 0x02, 0x01, 2, 16, // type I: 2-byte subslots, 16 bits
    7, TUSB_DESC_ENDPOINT, kEpAudioIn, 0x05 /* iso, async */, U16(kAudioPacketMax), 1,
    8, 0x25, 0x01, 0, 0, 0, U16(0),

    // --- USB MIDI, a lone streaming interface as on the M8 (Teensy) -------------------------
    9, TUSB_DESC_INTERFACE, kItfMidi, 0, 2, 0x01, 0x03, 0x00, 0,
    7, 0x24, 0x01, U16(0x0100), U16(kMsTotal),
    6, 0x24, 0x02, 0x01, 1, 0,          // IN jack 1, embedded
    6, 0x24, 0x02, 0x02, 2, 0,          // IN jack 2, external
    9, 0x24, 0x03, 0x01, 3, 1, 2, 1, 0, // OUT jack 3, embedded, from jack 2
    9, 0x24, 0x03, 0x02, 4, 1, 1, 1, 0, // OUT jack 4, external, from jack 1
    9, TUSB_DESC_ENDPOINT, kEpMidiOut, TUSB_XFER_BULK, U16(kBulkSize), 0, 0, 0,
    5, 0x25, 0x01, 1, 1,
    9, TUSB_DESC_ENDPOINT, kEpMidiIn, TUSB_XFER_BULK, U16(kBulkSize), 0, 0, 0,
    5, 0x25, 0x01, 1, 3,
};
static_assert(sizeof(kConfig) == kConfigTotal, "configuration length");

uint8_t device_desc[18];
char manufacturer[32], product[32], serial[32];
uint16_t string_desc[32];

// ---------------------------------------------------------------------------
// Buffers between the M8's USB task and the computer
// ---------------------------------------------------------------------------

// Single producer, single consumer. N is a power of two.
template <typename T, size_t N> class Ring {
public:
  size_t size() const {
    return head_.load(std::memory_order_acquire) - tail_.load(std::memory_order_acquire);
  }
  size_t push(const T *src, size_t n) { // producer
    const uint32_t head = head_.load(std::memory_order_relaxed);
    const size_t space = N - (head - tail_.load(std::memory_order_acquire));
    if (n > space) n = space;
    for (size_t i = 0; i < n; i++) buf_[(head + i) & (N - 1)] = src[i];
    head_.store(head + n, std::memory_order_release);
    return n;
  }
  size_t pop(T *dst, size_t n) { // consumer; null dst discards
    const uint32_t tail = tail_.load(std::memory_order_relaxed);
    const size_t avail = head_.load(std::memory_order_acquire) - tail;
    if (n > avail) n = avail;
    if (dst) {
      for (size_t i = 0; i < n; i++) dst[i] = buf_[(tail + i) & (N - 1)];
    }
    tail_.store(tail + n, std::memory_order_release);
    return n;
  }
  void clear() { pop(nullptr, N); } // consumer

private:
  static_assert((N & (N - 1)) == 0, "N must be a power of two");
  T buf_[N];
  std::atomic<uint32_t> head_{0}, tail_{0};
};

Ring<uint32_t, 4096> audio_ring; // stereo frames, ~93 ms
Ring<uint8_t, 4096> serial_ring;
Ring<uint8_t, 1024> midi_ring;

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------

usb_phy_handle_t phy;
bool stack_up;     // TinyUSB initialized (it stays up between M8 connections)
bool attached;     // the full-speed controller owns USB-C
// A computer asked for Serial/JTAG back (to flash, or for logs): USB-C stays Serial/JTAG
// until the M8 disconnects or the Tab5 restarts.
std::atomic<bool> want_serial{false};
bool serial_held;
bool attach_failed; // don't retry until the M8 reconnects

std::atomic<bool> is_mounted{false};
std::atomic<uint8_t> keys{0};
std::atomic<bool> pump_queued{false};
volatile bool streaming; // the computer selected the audio interface's alt 1
volatile bool primed;    // enough audio buffered to start sending
uint8_t audio_alt;
uint32_t audio_acc; // fractional frames carried between packets, in 1/1000ths
const tusb_desc_endpoint_t *audio_ep_desc;

uint8_t data_rx[kBulkSize];
uint8_t data_tx[kBulkSize * 8];
uint8_t midi_rx[kBulkSize];
uint8_t midi_tx[kBulkSize];
uint32_t audio_tx[kAudioMaxFrames];
uint8_t line_coding[7] = {0x00, 0xC2, 0x01, 0x00, 0x00, 0x00, 0x08}; // 115200 8N1
uint8_t ctrl_buf[14];
bool serial_zlp, midi_zlp;  // the last IN transfer ended on a packet boundary
uint16_t midi_pending;      // MIDI from the computer waiting for room in the M8's queue

// The computer's commands to the M8, split up so its key state can be merged with the touch
// keys. M8 clients send: C mask, K note velocity (or K FF), E, R, D, X.
uint8_t cmd[3];
uint8_t cmd_len, cmd_need;
uint32_t cmds_dropped;

// Audio diagnostics, read from a computer with a vendor request (tools/passthru.py):
// the Tab5's log isn't reachable while USB-C is the M8.
struct Stats {
  uint32_t alt_sets, isr_calls, isr_failed, packets, empty_packets, frames_sent;
  uint32_t short_packets; // fewer frames available than the packet wanted
  uint32_t frames_pushed, frames_dropped;
  uint32_t level, alt, streaming, primed;
} stats;

// ---------------------------------------------------------------------------
// Data from the computer
// ---------------------------------------------------------------------------

void dispatch_command() {
  switch (cmd[0]) {
  case m8proto::kCmdController:
    keys = cmd[1]; // the touch task merges it with the touch keys
    break;
  case m8proto::kCmdDisconnect:
    // The M8 would stop sending its screen, and the Tab5's display with it.
    ESP_LOGI(TAG, "computer disconnected from the M8 (not forwarded)");
    keys = 0;
    break;
  default:
    if (!m8usb::send(cmd, cmd_len)) cmds_dropped++;
    break;
  }
}

void from_computer(const uint8_t *data, uint32_t n) {
  for (uint32_t i = 0; i < n; i++) {
    const uint8_t b = data[i];
    if (cmd_len == 0) {
      cmd_need = (b == m8proto::kCmdController || b == m8proto::kCmdKeyjazz) ? 2 : 1;
    } else if (cmd_len == 1 && cmd[0] == m8proto::kCmdKeyjazz && b != 0xFF) {
      cmd_need = 3; // K note velocity; K FF alone ends keyjazz
    }
    cmd[cmd_len++] = b;
    if (cmd_len >= cmd_need) {
      dispatch_command();
      cmd_len = 0;
    }
  }
}

// Forwards MIDI from the computer, then re-arms the endpoint. If the M8's queue is full the
// endpoint stays un-armed (the computer waits) and the task loop retries.
void forward_midi() {
  if (midi_pending && !m8usb::send_midi(midi_rx, midi_pending)) return;
  midi_pending = 0;
  usbd_edpt_xfer(kRhport, kEpMidiOut, midi_rx, sizeof(midi_rx), false);
}

// ---------------------------------------------------------------------------
// Data to the computer (TinyUSB task)
// ---------------------------------------------------------------------------

// Sends what's buffered; a transfer of whole packets is followed by a zero-length packet once
// the buffer runs dry, so the computer's read completes.
template <typename R>
void pump(R &ring, uint8_t ep, uint8_t *buf, size_t size, bool &zlp) {
  if (!is_mounted || usbd_edpt_busy(kRhport, ep)) return;
  const size_t n = ring.pop(buf, size);
  if (n == 0) {
    if (zlp) {
      zlp = false;
      usbd_edpt_xfer(kRhport, ep, buf, 0, false);
    }
    return;
  }
  zlp = n % kBulkSize == 0;
  usbd_edpt_xfer(kRhport, ep, buf, (uint16_t)n, false);
}

void pump_all(void *) {
  pump_queued = false;
  pump(serial_ring, kEpDataIn, data_tx, sizeof(data_tx), serial_zlp);
  pump(midi_ring, kEpMidiIn, midi_tx, sizeof(midi_tx), midi_zlp);
}

// Wakes the TinyUSB task to send newly buffered data. Any task.
void kick() {
  if (is_mounted && !pump_queued.exchange(true)) usbd_defer_func(pump_all, nullptr, false);
}

// ---------------------------------------------------------------------------
// Control requests
// ---------------------------------------------------------------------------

void put32(uint8_t *p, uint32_t v) {
  p[0] = v;
  p[1] = v >> 8;
  p[2] = v >> 16;
  p[3] = v >> 24;
}

bool cdc_request(uint8_t rhport, const tusb_control_request_t *req) {
  switch (req->bRequest) {
  case 0x20: // SET_LINE_CODING (meaningless over USB; kept for GET_LINE_CODING)
  case 0x21: // GET_LINE_CODING
    return tud_control_xfer(rhport, req, line_coding, sizeof(line_coding));
  case 0x22: // SET_CONTROL_LINE_STATE
    if (!(req->wValue & 1)) keys = 0; // port closed: release the computer's keys
    return tud_control_status(rhport, req);
  case 0x23: // SEND_BREAK
    return tud_control_status(rhport, req);
  default:
    return false;
  }
}

// UAC2 requests: only the clock has controls (its frequency, and whether it's valid).
bool audio_request(uint8_t rhport, const tusb_control_request_t *req) {
  const uint8_t entity = tu_u16_high(req->wIndex);
  const uint8_t selector = tu_u16_high(req->wValue);
  const bool get = req->bmRequestType_bit.direction == TUSB_DIR_IN;
  constexpr uint8_t kCur = 0x01, kRange = 0x02;
  constexpr uint8_t kSamFreq = 0x01, kClockValid = 0x02;
  if (entity != kClockId) return false;

  if (selector == kSamFreq && req->bRequest == kCur) {
    put32(ctrl_buf, kSampleRate); // a SET is accepted and ignored: the rate is fixed
    return tud_control_xfer(rhport, req, ctrl_buf, 4);
  }
  if (selector == kSamFreq && req->bRequest == kRange && get) {
    ctrl_buf[0] = 1; // one subrange: 44100..44100, step 0
    ctrl_buf[1] = 0;
    put32(ctrl_buf + 2, kSampleRate);
    put32(ctrl_buf + 6, kSampleRate);
    put32(ctrl_buf + 10, 0);
    return tud_control_xfer(rhport, req, ctrl_buf, 14);
  }
  if (selector == kClockValid && req->bRequest == kCur && get) {
    ctrl_buf[0] = 1;
    return tud_control_xfer(rhport, req, ctrl_buf, 1);
  }
  return false;
}

bool set_audio_alt(uint8_t rhport, uint8_t alt) {
  stats.alt_sets++;
  if (alt > 1 || !audio_ep_desc) return false;
  streaming = false;
  audio_alt = alt;
  if (alt == 1) {
    audio_ring.clear();
    primed = false;
    audio_acc = 0;
    if (!usbd_edpt_iso_activate(rhport, audio_ep_desc)) return false;
    streaming = true;
    // A zero-length packet starts the chain; each completion queues the next packet.
    usbd_edpt_xfer(rhport, kEpAudioIn, (uint8_t *)audio_tx, 0, false);
  }
  ESP_LOGI(TAG, "computer audio %s", alt ? "on" : "off");
  return true;
}

// ---------------------------------------------------------------------------
// Class driver: one driver owns the whole configuration
// ---------------------------------------------------------------------------

void drv_init() {}

bool drv_deinit() { return true; }

void drv_reset(uint8_t) {
  streaming = false;
  audio_alt = 0;
  keys = 0;
  cmd_len = 0;
  midi_pending = 0;
  serial_zlp = midi_zlp = false;
}

uint16_t drv_open(uint8_t rhport, const tusb_desc_interface_t *itf, uint16_t max_len) {
  if (itf->bInterfaceNumber != kItfComm) return 0; // everything is claimed with interface 0

  const uint8_t *p = (const uint8_t *)itf;
  const uint8_t *end = p + max_len;
  while (p < end) {
    if (tu_desc_type(p) == TUSB_DESC_ENDPOINT) {
      const auto *ep = (const tusb_desc_endpoint_t *)p;
      if (ep->bmAttributes.xfer == TUSB_XFER_ISOCHRONOUS) {
        TU_ASSERT(usbd_edpt_iso_alloc(rhport, ep->bEndpointAddress, tu_edpt_packet_size(ep)), 0);
        audio_ep_desc = ep;
      } else {
        TU_ASSERT(usbd_edpt_open(rhport, ep), 0);
      }
    }
    p = tu_desc_next(p);
  }
  usbd_edpt_xfer(rhport, kEpDataOut, data_rx, sizeof(data_rx), false);
  usbd_edpt_xfer(rhport, kEpMidiOut, midi_rx, sizeof(midi_rx), false);
  return max_len;
}

bool drv_control(uint8_t rhport, uint8_t stage, const tusb_control_request_t *req) {
  if (stage != CONTROL_STAGE_SETUP) return true;
  if (req->bmRequestType_bit.recipient != TUSB_REQ_RCPT_INTERFACE) return false;
  const uint8_t itf = tu_u16_low(req->wIndex);

  if (req->bmRequestType_bit.type == TUSB_REQ_TYPE_STANDARD) {
    if (itf != kItfAudioStream) return false; // usbd answers the rest (alt 0 only)
    if (req->bRequest == TUSB_REQ_SET_INTERFACE) {
      if (!set_audio_alt(rhport, tu_u16_low(req->wValue))) return false;
      return tud_control_status(rhport, req);
    }
    if (req->bRequest == TUSB_REQ_GET_INTERFACE) {
      return tud_control_xfer(rhport, req, &audio_alt, 1);
    }
    return false;
  }
  if (req->bmRequestType_bit.type != TUSB_REQ_TYPE_CLASS) return false;
  // The M8 (and m8.run's WebUSB mode) take CDC requests on either serial interface.
  if (itf == kItfComm || itf == kItfData) return cdc_request(rhport, req);
  if (itf == kItfAudioControl) return audio_request(rhport, req);
  return false;
}

bool drv_xfer(uint8_t rhport, uint8_t ep, xfer_result_t result, uint32_t bytes) {
  switch (ep) {
  case kEpDataOut:
    if (result == XFER_RESULT_SUCCESS) from_computer(data_rx, bytes);
    usbd_edpt_xfer(rhport, kEpDataOut, data_rx, sizeof(data_rx), false);
    break;
  case kEpMidiOut:
    midi_pending = result == XFER_RESULT_SUCCESS ? bytes : 0;
    forward_midi();
    break;
  case kEpDataIn:
  case kEpMidiIn:
    pump_all(nullptr);
    break;
  default:
    break;
  }
  return true;
}

// Audio runs from the interrupt, so a packet is queued for every frame however busy the task
// is. Each packet carries 44 or 45 frames (44.1 per ms), plus or minus one to keep about 10 ms
// buffered: the M8's clock, not the computer's, sets the rate, so this is an asynchronous
// endpoint and the computer adapts to the packet sizes.
bool drv_xfer_isr(uint8_t rhport, uint8_t ep, xfer_result_t result, uint32_t) {
  if (ep != kEpAudioIn) return false; // the rest run in the task
  stats.isr_calls++;
  if (result != XFER_RESULT_SUCCESS) stats.isr_failed++;
  if (!streaming) return true;

  audio_acc += kSampleRate;
  int frames = audio_acc / 1000;
  audio_acc %= 1000;
  const size_t level = audio_ring.size();
  if (!primed) {
    primed = level >= kAudioTarget;
    if (!primed) frames = 0;
  } else if (level > kAudioTarget + kAudioBand) {
    frames++;
  } else if (level < kAudioTarget - kAudioBand) {
    frames--;
  }
  const int wanted = frames;
  frames = (int)audio_ring.pop(audio_tx, frames);
  stats.packets++;
  stats.frames_sent += frames;
  if (frames == 0) stats.empty_packets++;
  if (frames < wanted) stats.short_packets++;
  usbd_edpt_xfer(rhport, kEpAudioIn, (uint8_t *)audio_tx, (uint16_t)(frames * 4), true);
  return true;
}

const usbd_class_driver_t kDriver = {
    .name = "M8",
    .init = drv_init,
    .deinit = drv_deinit,
    .reset = drv_reset,
    .open = drv_open,
    .control_xfer_cb = drv_control,
    .xfer_cb = drv_xfer,
    .xfer_isr = drv_xfer_isr,
    .sof = nullptr,
};

// ---------------------------------------------------------------------------
// Switching USB-C between Serial/JTAG and the full-speed controller
// ---------------------------------------------------------------------------
//
// The P4 has two full-speed PHYs: PHY 0 on GPIO 24/25 (the Tab5's USB-C) and PHY 1 on GPIO
// 26/27 (the Tab5's I2S BCLK and DOUT). Serial/JTAG normally has PHY 0 and the OTG 1.1
// controller PHY 1. Swapping them puts OTG 1.1 on USB-C and Serial/JTAG on the I2S pins, so
// Serial/JTAG's pads are disabled first: each controller's pad enable follows it to its PHY.

void copy_or(char *dst, const char *src, const char *fallback) {
  strlcpy(dst, src[0] ? src : fallback, 32);
}

bool take_usb_c() {
  usb_serial_jtag_ll_phy_enable_pad(false); // the computer sees Serial/JTAG leave
  vTaskDelay(kSwitchGap);
  usb_wrap_ll_phy_select(&USB_WRAP, 0);

  if (!phy) {
    // usb_new_phy() gives the pads it assumes OTG 1.1 has (26/27, the I2S pins here) full
    // drive strength. Put those back and give it to USB-C's pads instead.
    gpio_drive_cap_t cap26 = GPIO_DRIVE_CAP_DEFAULT, cap27 = GPIO_DRIVE_CAP_DEFAULT;
    gpio_get_drive_capability(GPIO_NUM_26, &cap26);
    gpio_get_drive_capability(GPIO_NUM_27, &cap27);
    usb_phy_config_t cfg = {};
    cfg.controller = USB_PHY_CTRL_OTG;
    cfg.target = USB_PHY_TARGET_INT;
    cfg.otg_mode = USB_OTG_MODE_DEVICE;
    cfg.otg_speed = USB_PHY_SPEED_FULL;
    const esp_err_t err = usb_new_phy(&cfg, &phy);
    gpio_set_drive_capability(GPIO_NUM_26, cap26);
    gpio_set_drive_capability(GPIO_NUM_27, cap27);
    if (err != ESP_OK) {
      ESP_LOGE(TAG, "usb_new_phy: %s", esp_err_to_name(err));
      phy = nullptr;
      usb_wrap_ll_phy_select(&USB_WRAP, 1);
      usb_serial_jtag_ll_phy_enable_pad(true);
      return false;
    }
    gpio_set_drive_capability(GPIO_NUM_24, GPIO_DRIVE_CAP_3);
    gpio_set_drive_capability(GPIO_NUM_25, GPIO_DRIVE_CAP_3);
  }
  usb_wrap_ll_phy_enable_pad(&USB_WRAP, true);
  return true;
}

void release_usb_c() {
  usb_wrap_ll_phy_enable_pad(&USB_WRAP, false);
  usb_wrap_ll_phy_select(&USB_WRAP, 1); // the default mapping
  usb_serial_jtag_ll_phy_enable_pad(true);
}

void attach() {
  const m8usb::Identity id = m8usb::identity();
  copy_or(manufacturer, id.manufacturer, "Dirtywave");
  copy_or(product, id.product, "M8");
  strlcpy(serial, id.serial, sizeof(serial));
  const uint8_t dev[18] = {18, TUSB_DESC_DEVICE, U16(0x0200),
                           TUSB_CLASS_MISC, MISC_SUBCLASS_COMMON, MISC_PROTOCOL_IAD,
                           CFG_TUD_ENDPOINT0_SIZE, U16(id.vid), U16(id.pid), U16(id.bcd_device),
                           1, 2, (uint8_t)(serial[0] ? 3 : 0), 1};
  memcpy(device_desc, dev, sizeof(dev));

  keys = 0;
  serial_ring.clear();
  midi_ring.clear();
  if (!take_usb_c()) {
    attach_failed = true;
    return;
  }
  if (!stack_up) {
    const tusb_rhport_init_t init = {.role = TUSB_ROLE_DEVICE, .speed = TUSB_SPEED_FULL};
    if (!tusb_rhport_init(kRhport, &init)) {
      ESP_LOGE(TAG, "TinyUSB init failed");
      release_usb_c();
      attach_failed = true;
      return;
    }
    stack_up = true;
  } else {
    tud_connect();
  }
  attached = true;
  ESP_LOGI(TAG, "USB-C is now the M8 (%04X:%04X \"%s\")", id.vid, id.pid, product);
}

void detach() {
  attached = false;
  is_mounted = false;
  streaming = false;
  keys = 0;
  tud_disconnect();
  vTaskDelay(kSwitchGap);
  release_usb_c();
  ESP_LOGI(TAG, "USB-C is Serial/JTAG again");
}

void task(void *) {
  // The PHY mapping lives in the LP domain and may survive a software reset; with it swapped,
  // Serial/JTAG would sit on the I2S pins. Start from the default.
  release_usb_c();

  for (;;) {
    if (want_serial.exchange(false) && attached) {
      vTaskDelay(pdMS_TO_TICKS(50)); // let the request's status stage complete
      serial_held = true;
      detach();
    }
    const bool want = m8usb::state() == m8usb::State::kConnected;
    if (!want) attach_failed = serial_held = false;
    if (want && !attached && !attach_failed && !serial_held) {
      attach();
    } else if (!want && attached) {
      detach();
    }

    if (stack_up) {
      tud_task_ext(10, false);
      if (attached) {
        if (midi_pending) forward_midi();
        pump_all(nullptr);
      }
    } else {
      vTaskDelay(pdMS_TO_TICKS(50));
    }
  }
}

} // namespace

bool start() {
  return xTaskCreatePinnedToCore(task, "passthru", 4096, nullptr, 16, nullptr, USB_CORE) == pdPASS;
}

void push_audio(const uint8_t *pcm, size_t bytes) {
  if (!streaming) return;
  uint32_t frames[64];
  size_t n = bytes / 4;
  while (n > 0) {
    const size_t chunk = n < 64 ? n : 64;
    memcpy(frames, pcm, chunk * 4);
    const size_t pushed = audio_ring.push(frames, chunk); // drops what doesn't fit
    stats.frames_pushed += pushed;
    stats.frames_dropped += chunk - pushed;
    pcm += chunk * 4;
    n -= chunk;
  }
}

void push_serial(const uint8_t *data, size_t bytes) {
  if (!is_mounted) return;
  serial_ring.push(data, bytes);
  kick();
}

void push_midi(const uint8_t *data, size_t bytes) {
  if (!is_mounted) return;
  midi_ring.push(data, bytes);
  kick();
}

uint8_t remote_keys() { return keys; }

bool mounted() { return is_mounted; }

} // namespace passthru

// ---------------------------------------------------------------------------
// TinyUSB callbacks
// ---------------------------------------------------------------------------

extern "C" {

usbd_class_driver_t const *usbd_app_driver_get_cb(uint8_t *driver_count) {
  *driver_count = 1;
  return &passthru::kDriver;
}

uint8_t const *tud_descriptor_device_cb(void) { return passthru::device_desc; }

uint8_t const *tud_descriptor_configuration_cb(uint8_t) { return passthru::kConfig; }

uint16_t const *tud_descriptor_string_cb(uint8_t index, uint16_t) {
  using namespace passthru;
  const char *s;
  switch (index) {
  case 0:
    string_desc[0] = (TUSB_DESC_STRING << 8) | 4;
    string_desc[1] = 0x0409; // English (US)
    return string_desc;
  case 1: s = manufacturer; break;
  case 2: s = product; break;
  case 3: s = serial; break;
  default: return nullptr;
  }
  size_t n = strlen(s);
  if (n > 31) n = 31;
  for (size_t i = 0; i < n; i++) string_desc[1 + i] = (uint8_t)s[i];
  string_desc[0] = (uint16_t)((TUSB_DESC_STRING << 8) | (2 + 2 * n));
  return string_desc;
}

// Vendor requests (tools/passthru.py): 0x01 (IN) reads the audio diagnostics above; 0x02 (OUT)
// gives USB-C back to Serial/JTAG until the M8 reconnects or the Tab5 restarts, so the Tab5 can
// be flashed (esptool's usual reset into the bootloader and back) or its log read.
bool tud_vendor_control_xfer_cb(uint8_t rhport, uint8_t stage, tusb_control_request_t const *req) {
  using namespace passthru;
  if (stage != CONTROL_STAGE_SETUP) return true;
  if (req->bRequest == 0x02 && req->bmRequestType_bit.direction == TUSB_DIR_OUT) {
    want_serial = true;
    return tud_control_status(rhport, req);
  }
  if (req->bRequest != 0x01 || req->bmRequestType_bit.direction != TUSB_DIR_IN) return false;
  static Stats snapshot;
  snapshot = stats;
  snapshot.level = audio_ring.size();
  snapshot.alt = audio_alt;
  snapshot.streaming = streaming;
  snapshot.primed = primed;
  return tud_control_xfer(rhport, req, &snapshot, sizeof(snapshot));
}

void tud_mount_cb(void) {
  passthru::is_mounted = true;
  ESP_LOGI(TAG, "computer connected");
}

void tud_umount_cb(void) {
  passthru::is_mounted = false;
  passthru::keys = 0;
  ESP_LOGI(TAG, "computer disconnected");
}

// The prebuilt TinyUSB links every class driver it was built with, and these have no default.
// None of those drivers ever opens: the driver above claims the whole configuration.
int32_t tud_msc_read10_cb(uint8_t, uint32_t, uint32_t, void *, uint32_t) { return -1; }
int32_t tud_msc_write10_cb(uint8_t, uint32_t, uint32_t, uint8_t *, uint32_t) { return -1; }
void tud_msc_capacity_cb(uint8_t, uint32_t *count, uint16_t *size) { *count = 0, *size = 512; }
int32_t tud_msc_scsi_cb(uint8_t, uint8_t const[16], void *, uint16_t) { return -1; }
bool tud_msc_test_unit_ready_cb(uint8_t) { return false; }
uint8_t const *tud_hid_descriptor_report_cb(uint8_t) { return nullptr; }
uint16_t tud_hid_get_report_cb(uint8_t, uint8_t, hid_report_type_t, uint8_t *, uint16_t) {
  return 0;
}
void tud_hid_set_report_cb(uint8_t, uint8_t, hid_report_type_t, uint8_t const *, uint16_t) {}
void tud_dfu_download_cb(uint8_t, uint16_t, uint8_t const *, uint16_t) {}
uint32_t tud_dfu_get_timeout_cb(uint8_t, uint8_t) { return 0; }
void tud_dfu_manifest_cb(uint8_t) {}
void tud_dfu_runtime_reboot_to_dfu_cb(void) {}
bool tud_network_recv_cb(const uint8_t *, uint16_t) { return false; }
uint16_t tud_network_xmit_cb(uint8_t *, void *, uint16_t) { return 0; }

} // extern "C"
