#include <Adafruit_PN532.h>
#include <SoftwareSerial.h>
#include "BGLib.h"

// DEBUG serial output switch
#define DEBUG

// READER pins
#define SCK  (2)
#define MOSI (3)
#define SS   (4) // rx
#define MISO (5) // tx

// BLUETOOTH pins
#define BLE_RX_PIN  6
#define BLE_TX_PIN  7
#define BLE_WAKEUP_PIN  8   // BLE wake-up pin
#define BLE_RESET_PIN   9   // BLE reset pin (active-low)

// ================================================================
// BLE STATE TRACKING (UNIVERSAL TO JUST ABOUT ANY BLE PROJECT)
// ================================================================

// BLE state machine definitions
#define BLE_STATE_STANDBY           0
#define BLE_STATE_SCANNING          1
#define BLE_STATE_ADVERTISING       2
#define BLE_STATE_CONNECTING        3
#define BLE_STATE_CONNECTED_MASTER  4
#define BLE_STATE_CONNECTED_SLAVE   5

// BLE state/link status tracker
uint8_t ble_state = BLE_STATE_STANDBY;
uint8_t ble_encrypted = 0;  // 0 = not encrypted, otherwise = encrypted
uint8_t ble_bonding = 0xFF; // 0xFF = no bonding, otherwise = bonding handle

#define GATT_HANDLE_C_RX_DATA   17  // 0x11, supports "write" operation
#define GATT_HANDLE_C_TX_DATA   20  // 0x14, supports "read" and "indicate" operations

// use SoftwareSerial on pins D2/D3 for RX/TX (Arduino side)
SoftwareSerial bleSerialPort(BLE_RX_PIN, BLE_TX_PIN);

// create BGLib object:
//  - use SoftwareSerial por for module comms
//  - use nothing for passthrough comms (0 = null pointer)
//  - enable packet mode on API protocol since flow control is unavailable
BGLib ble112((HardwareSerial *)&bleSerialPort, 0, 1);

#define BGAPI_GET_RESPONSE(v, dType) dType *v = (dType *)ble112.getLastRXPayload()

// READER lib
Adafruit_PN532 nfc(SCK, MISO, MOSI, SS);

void setup() {
    // open Arduino USB serial (and wait, if we're using Leonardo)
    // use 38400 since it works at 8MHz as well as 16MHz
    Serial.begin(38400);
    while (!Serial);
    
    #ifdef DEBUG
      Serial.println("Starting");
    #endif
    
    // initialize BLE reset pin (active-low)
    pinMode(BLE_RESET_PIN, OUTPUT);
    digitalWrite(BLE_RESET_PIN, HIGH);

    // initialize BLE wake-up pin to allow (not force) sleep mode (assumes active-high)
    pinMode(BLE_WAKEUP_PIN, OUTPUT);
    digitalWrite(BLE_WAKEUP_PIN, LOW);
    
    // set up internal status handlers (these are technically optional)
    ble112.onBusy = onBusy;
    ble112.onIdle = onIdle;
    ble112.onTimeout = onTimeout;

    // ONLY enable these if you are using the <wakeup_pin> parameter in your firmware's hardware.xml file
    // BLE module must be woken up before sending any UART data
    ble112.onBeforeTXCommand = onBeforeTXCommand;
    ble112.onTXCommandComplete = onTXCommandComplete;

    // set up BGLib event handlers
    ble112.ble_evt_system_boot = my_ble_evt_system_boot;
    ble112.ble_evt_connection_status = my_ble_evt_connection_status;
    ble112.ble_evt_connection_disconnected = my_ble_evt_connection_disconnect;
    ble112.ble_evt_attributes_value = my_ble_evt_attributes_value;
    
    
    // open BLE software serial port
    bleSerialPort.begin(38400);
    
    // Setup NFC
    nfc.begin();

    uint32_t versiondata = nfc.getFirmwareVersion();
    if (! versiondata) {
      Serial.print("Didn't find PN53x board");
      while (1); // halt
    }
    
    #ifdef DEBUG
      Serial.print("Found chip PN5"); Serial.println((versiondata>>24) & 0xFF, HEX); 
      Serial.print("Firmware ver. "); Serial.print((versiondata>>16) & 0xFF, DEC); 
      Serial.print('.'); Serial.println((versiondata>>8) & 0xFF, DEC);
    #endif
    
    // configure board to read RFID tags
    nfc.SAMConfig();
    
    #ifdef DEBUG
//      Serial.println("Waiting for an ISO14443A Card ...");
        Serial.println("NFC Ready");
    #endif
}


void loop() {
    uint8_t success;
    uint8_t uid[] = { 0, 0, 0, 0, 0, 0, 0 };  // Buffer to store the returned UID
    uint8_t uidLength;                        // Length of the UID (4 or 7 bytes depending on ISO14443A card type)
    
    success = nfc.readPassiveTargetID(PN532_MIFARE_ISO14443A, uid, &uidLength);
    
    if (success) {
      nfc.PrintHex(uid, uidLength);
      
      String message = String(45, HEX); //"hi fred";
      byte bytes[message.length() + 1];
      message.getBytes(bytes, message.length() + 1);
  
      uint8 data_len_var = message.length();
      const uint8 *data_var = bytes;
      
      Serial.println(*data_var);
      //ble112.ble_cmd_attributes_write(GATT_HANDLE_C_TX_DATA, 0, uid, uidLength);
      ble112.ble_cmd_attributes_write(GATT_HANDLE_C_TX_DATA, 0, data_len_var, data_var);
    }
    
    // keep polling for new data from BLE
    ble112.checkActivity();
    
}


// ================================================================
// INTERNAL BGLIB CLASS CALLBACK FUNCTIONS
// ================================================================

// called when the module begins sending a command
void onBusy() {
    // turn LED on when we're busy
    //digitalWrite(LED_PIN, HIGH);
}

// called when the module receives a complete response or "system_boot" event
void onIdle() {
    // turn LED off when we're no longer busy
    //digitalWrite(LED_PIN, LOW);
}

// called when the parser does not read the expected response in the specified time limit
void onTimeout() {
    // reset module (might be a bit drastic for a timeout condition though)
    digitalWrite(BLE_RESET_PIN, LOW);
    delay(5); // wait 5ms
    digitalWrite(BLE_RESET_PIN, HIGH);
}

// called immediately before beginning UART TX of a command
void onBeforeTXCommand() {
    // wake module up (assuming here that digital pin 5 is connected to the BLE wake-up pin)
    digitalWrite(BLE_WAKEUP_PIN, HIGH);

    // wait for "hardware_io_port_status" event to come through, and parse it (and otherwise ignore it)
    uint8_t *last;
    while (1) {
        ble112.checkActivity();
        last = ble112.getLastEvent();
        if (last[0] == 0x07 && last[1] == 0x00) break;
    }

    // give a bit of a gap between parsing the wake-up event and allowing the command to go out
    delayMicroseconds(1000);
}

// called immediately after finishing UART TX
void onTXCommandComplete() {
    // allow module to return to sleep (assuming here that digital pin 5 is connected to the BLE wake-up pin)
    digitalWrite(BLE_WAKEUP_PIN, LOW);
}



// ================================================================
// APPLICATION EVENT HANDLER FUNCTIONS
// ================================================================

void my_ble_evt_system_boot(const ble_msg_system_boot_evt_t *msg) {
    #ifdef DEBUG
        Serial.print("###\tsystem_boot: { ");
        Serial.print("major: "); Serial.print(msg -> major, HEX);
        Serial.print(", minor: "); Serial.print(msg -> minor, HEX);
        Serial.print(", patch: "); Serial.print(msg -> patch, HEX);
        Serial.print(", build: "); Serial.print(msg -> build, HEX);
        Serial.print(", ll_version: "); Serial.print(msg -> ll_version, HEX);
        Serial.print(", protocol_version: "); Serial.print(msg -> protocol_version, HEX);
        Serial.print(", hw: "); Serial.print(msg -> hw, HEX);
        Serial.println(" }");
    #endif

    // system boot means module is in standby state
    //ble_state = BLE_STATE_STANDBY;
    // ^^^ skip above since we're going right back into advertising below

    // set advertisement interval to 200-300ms, use all advertisement channels
    // (note min/max parameters are in units of 625 uSec)
    ble112.ble_cmd_gap_set_adv_parameters(320, 480, 7);
    while (ble112.checkActivity(1000));

    // USE THE FOLLOWING TO LET THE BLE STACK HANDLE YOUR ADVERTISEMENT PACKETS
    // ========================================================================
    // start advertising general discoverable / undirected connectable
    //ble112.ble_cmd_gap_set_mode(BGLIB_GAP_GENERAL_DISCOVERABLE, BGLIB_GAP_UNDIRECTED_CONNECTABLE);
    //while (ble112.checkActivity(1000));

    // USE THE FOLLOWING TO HANDLE YOUR OWN CUSTOM ADVERTISEMENT PACKETS
    // =================================================================

    // build custom advertisement data
    // default BLE stack value: 0201061107e4ba94c3c9b7cdb09b487a438ae55a19
    uint8 adv_data[] = {
        0x02, // field length
        BGLIB_GAP_AD_TYPE_FLAGS, // field type (0x01)
        BGLIB_GAP_AD_FLAG_GENERAL_DISCOVERABLE | BGLIB_GAP_AD_FLAG_BREDR_NOT_SUPPORTED, // data (0x02 | 0x04 = 0x06)
        0x11, // field length
        BGLIB_GAP_AD_TYPE_SERVICES_128BIT_ALL, // field type (0x07)
        0xe4, 0xba, 0x94, 0xc3, 0xc9, 0xb7, 0xcd, 0xb0, 0x9b, 0x48, 0x7a, 0x43, 0x8a, 0xe5, 0x5a, 0x19
    };

    // set custom advertisement data
    ble112.ble_cmd_gap_set_adv_data(0, 0x15, adv_data);
    while (ble112.checkActivity(1000));

    // build custom scan response data (i.e. the Device Name value)
    // default BLE stack value: 140942474c69622055314131502033382e344e4657
    uint8 sr_data[] = {
        0x14, // field length
        BGLIB_GAP_AD_TYPE_LOCALNAME_COMPLETE, // field type
        'M', 'y', ' ', 'A', 'r', 'd', 'u', 'i', 'n', 'o', ' ', '0', '0', ':', '0', '0', ':', '0', '0'
    };

    // get BLE MAC address
    ble112.ble_cmd_system_address_get();
    while (ble112.checkActivity(1000));
    BGAPI_GET_RESPONSE(r0, ble_msg_system_address_get_rsp_t);

    // assign last three bytes of MAC address to ad packet friendly name (instead of 00:00:00 above)
    sr_data[13] = (r0 -> address.addr[2] / 0x10) + 48 + ((r0 -> address.addr[2] / 0x10) / 10 * 7); // MAC byte 4 10's digit
    sr_data[14] = (r0 -> address.addr[2] & 0xF)  + 48 + ((r0 -> address.addr[2] & 0xF ) / 10 * 7); // MAC byte 4 1's digit
    sr_data[16] = (r0 -> address.addr[1] / 0x10) + 48 + ((r0 -> address.addr[1] / 0x10) / 10 * 7); // MAC byte 5 10's digit
    sr_data[17] = (r0 -> address.addr[1] & 0xF)  + 48 + ((r0 -> address.addr[1] & 0xF ) / 10 * 7); // MAC byte 5 1's digit
    sr_data[19] = (r0 -> address.addr[0] / 0x10) + 48 + ((r0 -> address.addr[0] / 0x10) / 10 * 7); // MAC byte 6 10's digit
    sr_data[20] = (r0 -> address.addr[0] & 0xF)  + 48 + ((r0 -> address.addr[0] & 0xF ) / 10 * 7); // MAC byte 6 1's digit

    // set custom scan response data (i.e. the Device Name value)
    ble112.ble_cmd_gap_set_adv_data(1, 0x15, sr_data);
    while (ble112.checkActivity(1000));

    // put module into discoverable/connectable mode (with user-defined advertisement data)
    ble112.ble_cmd_gap_set_mode(BGLIB_GAP_USER_DATA, BGLIB_GAP_UNDIRECTED_CONNECTABLE);
    while (ble112.checkActivity(1000));

    // set state to ADVERTISING
    ble_state = BLE_STATE_ADVERTISING;
}

void my_ble_evt_connection_status(const ble_msg_connection_status_evt_t *msg) {
    #ifdef DEBUG
        Serial.print("###\tconnection_status: { ");
        Serial.print("connection: "); Serial.print(msg -> connection, HEX);
        Serial.print(", flags: "); Serial.print(msg -> flags, HEX);
        Serial.print(", address: ");
        // this is a "bd_addr" data type, which is a 6-byte uint8_t array
        for (uint8_t i = 0; i < 6; i++) {
            if (msg -> address.addr[i] < 16) Serial.write('0');
            Serial.print(msg -> address.addr[i], HEX);
        }
        Serial.print(", address_type: "); Serial.print(msg -> address_type, HEX);
        Serial.print(", conn_interval: "); Serial.print(msg -> conn_interval, HEX);
        Serial.print(", timeout: "); Serial.print(msg -> timeout, HEX);
        Serial.print(", latency: "); Serial.print(msg -> latency, HEX);
        Serial.print(", bonding: "); Serial.print(msg -> bonding, HEX);
        Serial.println(" }");
    #endif

    // "flags" bit description:
    //  - bit 0: connection_connected
    //           Indicates the connection exists to a remote device.
    //  - bit 1: connection_encrypted
    //           Indicates the connection is encrypted.
    //  - bit 2: connection_completed
    //           Indicates that a new connection has been created.
    //  - bit 3; connection_parameters_change
    //           Indicates that connection parameters have changed, and is set
    //           when parameters change due to a link layer operation.

    // check for new connection established
    if ((msg -> flags & 0x05) == 0x05) {
        // track state change based on last known state, since we can connect two ways
        if (ble_state == BLE_STATE_ADVERTISING) {
            ble_state = BLE_STATE_CONNECTED_SLAVE;
        } else {
            ble_state = BLE_STATE_CONNECTED_MASTER;
        }
    }

    // update "encrypted" status
    ble_encrypted = msg -> flags & 0x02;
    
    // update "bonded" status
    ble_bonding = msg -> bonding;
    
    //    ble_cmd_attclient_attribute_write(uint8 connection, uint16 atthandle, uint8 data_len, const uint8 *data_data)
    
    
}

void my_ble_evt_connection_disconnect(const struct ble_msg_connection_disconnected_evt_t *msg) {
    #ifdef DEBUG
        Serial.print("###\tconnection_disconnect: { ");
        Serial.print("connection: "); Serial.print(msg -> connection, HEX);
        Serial.print(", reason: "); Serial.print(msg -> reason, HEX);
        Serial.println(" }");
    #endif

    // set state to DISCONNECTED
    //ble_state = BLE_STATE_DISCONNECTED;
    // ^^^ skip above since we're going right back into advertising below

    // after disconnection, resume advertising as discoverable/connectable
    //ble112.ble_cmd_gap_set_mode(BGLIB_GAP_GENERAL_DISCOVERABLE, BGLIB_GAP_UNDIRECTED_CONNECTABLE);
    //while (ble112.checkActivity(1000));

    // after disconnection, resume advertising as discoverable/connectable (with user-defined advertisement data)
    ble112.ble_cmd_gap_set_mode(BGLIB_GAP_USER_DATA, BGLIB_GAP_UNDIRECTED_CONNECTABLE);
    while (ble112.checkActivity(1000));

    // set state to ADVERTISING
    ble_state = BLE_STATE_ADVERTISING;

    // clear "encrypted" and "bonding" info
    ble_encrypted = 0;
    ble_bonding = 0xFF;
}

//void my_ble_evt_attributes_write(const struct ble_msg_attributes_value_evt_t *msg) {
//  Serial.print("msg");
//}

void my_ble_evt_attributes_value(const struct ble_msg_attributes_value_evt_t *msg) {
    #ifdef DEBUG
        Serial.print("###\tattributes_value: { ");
        Serial.print("connection: "); Serial.print(msg -> connection, HEX);
        Serial.print(", reason: "); Serial.print(msg -> reason, HEX);
        Serial.print(", handle: "); Serial.print(msg -> handle, HEX);
        Serial.print(", offset: "); Serial.print(msg -> offset, HEX);
        Serial.print(", value_len: "); Serial.print(msg -> value.len, HEX);
        Serial.print(", value_data: ");
        // this is a "uint8array" data type, which is a length byte and a uint8_t* pointer
        for (uint8_t i = 0; i < msg -> value.len; i++) {
            if (msg -> value.data[i] < 16) Serial.write('0');
            Serial.print(msg -> value.data[i]);
        }
        Serial.println(" }");
    #endif

    // check for data written to "c_rx_data" handle
    if (msg -> handle == GATT_HANDLE_C_RX_DATA && msg -> value.len > 0) {
        // set ping 8, 9, and 10 to three lower-most bits of first byte of RX data
        // (nice for controlling RGB LED or something)
        digitalWrite(8, msg -> value.data[0] & 0x01);
        digitalWrite(9, msg -> value.data[0] & 0x02);
        digitalWrite(10, msg -> value.data[0] & 0x04);
    }
    
    
}
