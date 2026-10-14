// ble-hid-bridge — Pico W as a Bluetooth keyboard -> USB keyboard bridge.
//
// The Pico W connects to a Bluetooth keyboard and re-emits every key
// as a plain USB HID keyboard, so a BLE-only keyboard works on machines with no
// Bluetooth (servers, BIOS/boot menus, KVMs).
//
// LED:  solid = connected | fast blink = reconnecting to saved keyboard
//       slow blink = pairing mode (put the keyboard in pairing mode)
// Re-pair: hold BOOTSEL while plugging in (or while it's reconnecting).
// Pair at a desktop the first time so you can read the serial log; the bond is
// saved in flash and later reconnects need no serial at all.

#include "src/HIDMaster.h"  // vendored arduino-pico BluetoothHIDMaster, bigger descriptor buffer
#include <Keyboard.h>
#include <Mouse.h>
#include <EEPROM.h>
#include <BluetoothLock.h>

// Most cheap "Bluetooth 5.x" keyboards (e.g. Samsers foldables) pair over
// Classic Bluetooth even though the chip supports BLE. Set true for a keyboard
// that only advertises over BLE (HID-over-GATT).
const bool USE_BLE = false;

BluetoothHIDMaster hid;

const char *MAGIC = "BLEHIDBRIDGE v1";  // 15 chars + \0 = 16

struct Saved {
  char magic[16];
  uint8_t addr[6];
  uint8_t addrType;
};

uint8_t savedAddr[6] = {};
uint8_t savedType = 0;

static const char *mac(const uint8_t *a, uint8_t t) {
  static char buf[24];
  snprintf(buf, sizeof(buf), "%02x:%02x:%02x:%02x:%02x:%02x/%d", a[0], a[1], a[2], a[3], a[4], a[5], t);
  return buf;
}

static bool haveSaved() {
  for (int i = 0; i < 6; i++) {
    if (savedAddr[i]) {
      return true;
    }
  }
  return false;
}

void loadBond() {
  Saved s;
  EEPROM.begin(sizeof(Saved));
  EEPROM.get(0, s);
  EEPROM.end();
  if (memcmp(s.magic, MAGIC, sizeof(s.magic)) == 0) {
    memcpy(savedAddr, s.addr, 6);
    savedType = s.addrType;
    Serial.printf("Saved keyboard: %s\n", mac(savedAddr, savedType));
  } else {
    Serial.println("No saved keyboard");
  }
}

void saveBond() {
  Saved s;
  memcpy(s.magic, MAGIC, sizeof(s.magic));
  memcpy(s.addr, savedAddr, 6);
  s.addrType = savedType;
  EEPROM.begin(sizeof(Saved));
  EEPROM.put(0, s);
  EEPROM.commit();
  EEPROM.end();
  Serial.printf("Saved keyboard %s to flash\n", mac(savedAddr, savedType));
}

void blink(void *periodMs) {
  digitalWrite(LED_BUILTIN, (millis() / (uint32_t)periodMs) & 1);
}

// --- key forwarding --------------------------------------------------------
// BluetoothHIDMaster hands us raw HID usage codes (page 0x07). The Keyboard
// library takes raw usages as (usage + 136) and modifiers as 128..135, so the
// report reaching the USB host is the same one the BLE keyboard sent.

static int toKeyboardCode(int usage) {
  if (usage >= 0xE0 && usage <= 0xE7) {
    return 128 + (usage - 0xE0);  // LCtrl LShift LAlt LGUI RCtrl RShift RAlt RGUI
  }
  if (usage >= 0x04 && usage + 136 <= 255) {
    return usage + 136;           // letters, numbers, F1-F24, arrows, keypad...
  }
  return -1;                      // usages > 0x77 (rare intl/system keys) not representable
}

void keyDown(void *, int usage) {
  int k = toKeyboardCode(usage);
  if (k >= 0) {
    Keyboard.press(k);
  }
}

void keyUp(void *, int usage) {
  int k = toKeyboardCode(usage);
  if (k >= 0) {
    Keyboard.release(k);
  }
}

void consumerDown(void *, int usage) {
  Keyboard.consumerPress(usage);  // volume, play/pause, brightness...
}

void consumerUp(void *, int) {
  Keyboard.consumerRelease();
}

// --- trackpad forwarding ---------------------------------------------------
// Touchpads on combo keyboards report as a mouse (relative X/Y, wheel, buttons).

void mouseMove(void *, int dx, int dy, int wheel) {
  // USB mouse reports carry int8 deltas; split big jumps instead of clipping.
  while (dx || dy || wheel) {
    int sx = constrain(dx, -127, 127), sy = constrain(dy, -127, 127);
    int sw = constrain(wheel, -127, 127);
    Mouse.move(sx, sy, sw);
    dx -= sx;
    dy -= sy;
    wheel -= sw;
  }
}

void mouseButton(void *, int button, bool down) {
  if (button > 2) {
    return;  // left/right/middle only
  }
  uint8_t b = 1 << button;  // 0->MOUSE_LEFT, 1->MOUSE_RIGHT, 2->MOUSE_MIDDLE
  if (down) {
    Mouse.press(b);
  } else {
    Mouse.release(b);
  }
}

// Some keyboards pair with "type this passkey": we show it on serial and the
// user types it on the BLE keyboard followed by Enter.
void showPasskey(uint32_t passkey) {
  Serial.printf("\n*** Type %06lu then Enter on the Bluetooth keyboard ***\n\n", (unsigned long)passkey);
}

// --- pairing -----------------------------------------------------------------
// The library asks for MITM-protected pairing (a code typed on the keyboard)
// and never answers the SSP confirmation request. Keyboards that only do
// "Just Works" (IO capability NoInputNoOutput, e.g. Samsers foldables) then
// time out with HCI auth failure 0x05. Match them: no MITM, NoInputNoOutput,
// and accept the confirmation request.
static btstack_packet_callback_registration_t hciLog;

static void logHci(uint8_t type, uint16_t, uint8_t *packet, uint16_t) {
  if (type != HCI_EVENT_PACKET) {
    return;
  }
  switch (hci_event_packet_get_type(packet)) {
  case HCI_EVENT_IO_CAPABILITY_RESPONSE:
    Serial.printf("Keyboard IO capability %u, auth req %u\n", packet[8], packet[10]);
    break;
  case HCI_EVENT_USER_CONFIRMATION_REQUEST: {
    // Just Works: nothing for a human to compare, accept it.
    bd_addr_t addr;
    hci_event_user_confirmation_request_get_bd_addr(packet, addr);
    Serial.println("Accepting pairing request");
    gap_ssp_confirmation_response(addr);
    break;
  }
  case HCI_EVENT_AUTHENTICATION_COMPLETE:
    Serial.printf("Pairing result 0x%02x (0 = paired)\n", hci_event_authentication_complete_get_status(packet));
    break;
  }
}

void relaxPairing() {
  BluetoothLock lock;
  gap_ssp_set_io_capability(SSP_IO_CAPABILITY_NO_INPUT_NO_OUTPUT);
  gap_ssp_set_authentication_requirement(SSP_IO_AUTHREQ_MITM_PROTECTION_NOT_REQUIRED_GENERAL_BONDING);
  gap_ssp_set_auto_accept(1);
  hciLog.callback = logHci;
  hci_add_event_handler(&hciLog);
}

// --- connection management -------------------------------------------------

// Start a connect attempt and give it `ms` to come up (the keyboard may also
// reconnect to us on its own; the library accepts incoming connections).
bool tryReconnect(uint32_t ms) {
  if (USE_BLE) {
    hid.connectBLE(savedAddr, savedType, blink, (void *)125);
  } else {
    hid.connect(savedAddr);
  }
  uint32_t start = millis();
  while (!hid.connected() && millis() - start < ms && !BOOTSEL) {
    blink((void *)125);
    delay(10);
  }
  return hid.connected();
}

void connectOrPair() {
  Keyboard.releaseAll();  // never leave keys/buttons stuck down across a disconnect
  Mouse.release(MOUSE_ALL);
  if (haveSaved() && !BOOTSEL) {
    Serial.printf("Reconnecting to %s (hold BOOTSEL to pair a new keyboard)\n", mac(savedAddr, savedType));
    while (!BOOTSEL) {
      if (tryReconnect(5000)) {
        Serial.println("Connected");
        return;
      }
    }
  }
  Serial.println("Pairing mode: put the keyboard into Bluetooth pairing mode now");
  while (!hid.connected()) {
    if (USE_BLE) {
      hid.connectBLE(blink, (void *)600);  // scans ~5 s for HID-over-GATT devices
    } else {
      // Inquiry scan for keyboard class-of-device, then connect + wait for the
      // HID descriptor (up to 60 s, enough to type a passkey).
      hid.connectKeyboard();
    }
    blink((void *)600);
    delay(10);
  }
  memcpy(savedAddr, hid.lastConnectedAddress(), 6);
  savedType = hid.lastConnectedAddressType();
  Serial.printf("Paired with %s\n", mac(savedAddr, savedType));
  saveBond();
}

void setup() {
  pinMode(LED_BUILTIN, OUTPUT);
  Serial.begin(115200);
  Keyboard.begin();
  Mouse.begin();
  delay(1500);  // give a desktop time to open the serial port for the log

  loadBond();
  hid.onKeyDown(keyDown);
  hid.onKeyUp(keyUp);
  hid.onConsumerKeyDown(consumerDown);
  hid.onConsumerKeyUp(consumerUp);
  hid.onMouseMove(mouseMove);
  hid.onMouseButton(mouseButton);
  hid.setPasskeyCB(showPasskey);
  hid.begin(USE_BLE);
  if (!USE_BLE) {
    relaxPairing();
  }

  connectOrPair();
  digitalWrite(LED_BUILTIN, HIGH);
}

void loop() {

  if (!hid.connected()) {
    Serial.println("Keyboard disconnected");
    connectOrPair();
    digitalWrite(LED_BUILTIN, HIGH);
  }
  delay(20);
}
