#pragma once

#include <deque>
#include <string>
#include <vector>

#include "esphome/core/component.h"
#include "esphome/components/ble_client/ble_client.h"
#include "esphome/components/esp32_ble_tracker/esp32_ble_tracker.h"
#include "esphome/components/sensor/sensor.h"
#include "esphome/components/text_sensor/text_sensor.h"
#include "esphome/components/binary_sensor/binary_sensor.h"

namespace esphome {
namespace centurion_gate {

namespace espbt = esphome::esp32_ble_tracker;

// TriggerInd values from the Vantage default Triggers table (see RECON.md)
static const uint16_t TRIGGER_TRG = 34;               // VERIFIED
static const uint16_t TRIGGER_PED = 35;               // VERIFIED
static const uint16_t TRIGGER_HOLIDAY_LOCK_ON = 2;    // VERIFIED
static const uint16_t TRIGGER_HOLIDAY_LOCK_OFF = 3;   // VERIFIED
static const uint16_t TRIGGER_HOLIDAY_TOGGLE = 1;     // UNTESTED
static const uint16_t TRIGGER_KEEP_ON = 5;            // UNTESTED
static const uint16_t TRIGGER_KEEP_OFF = 6;           // UNTESTED
static const uint16_t TRIGGER_FRX_ON = 40;            // UNTESTED
static const uint16_t TRIGGER_FRX_OFF = 41;           // UNTESTED
static const uint16_t TRIGGER_GATE_LOCK = 20;         // UNTESTED

enum GateStatus : uint8_t {
  GATE_OPEN = 0,
  GATE_CLOSED = 1,
  GATE_PARTLY_OPEN = 2,
  GATE_PARTLY_CLOSED = 3,
  GATE_OPENING = 4,
  GATE_CLOSING = 5,
  GATE_UNKNOWN = 0xFF,
};

#define CG_SENSOR(name) \
 protected: \
  sensor::Sensor *name##_sensor_{nullptr}; \
 public: \
  void set_##name##_sensor(sensor::Sensor *s) { this->name##_sensor_ = s; }
#define CG_TEXT(name) \
 protected: \
  text_sensor::TextSensor *name##_text_sensor_{nullptr}; \
 public: \
  void set_##name##_text_sensor(text_sensor::TextSensor *s) { this->name##_text_sensor_ = s; }
#define CG_BINARY(name) \
 protected: \
  binary_sensor::BinarySensor *name##_binary_sensor_{nullptr}; \
 public: \
  void set_##name##_binary_sensor(binary_sensor::BinarySensor *s) { this->name##_binary_sensor_ = s; }

class CenturionGate : public Component, public ble_client::BLEClientNode {
 public:
  void setup() override;
  void loop() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::DATA; }

  void gattc_event_handler(esp_gattc_cb_event_t event, esp_gatt_if_t gattc_if,
                           esp_ble_gattc_cb_param_t *param) override;

  void set_phone_number(const std::string &p) { this->phone_ = p; }
  void set_cipher_key(uint8_t a, uint8_t b, uint8_t c, uint8_t d) {
    this->key_[0] = a; this->key_[1] = b; this->key_[2] = c; this->key_[3] = d;
    this->key_set_ = true;
  }
  void set_keepalive_interval(uint32_t ms) { this->keepalive_ms_ = ms; }

  // --- public API for YAML lambdas ---
  /// Queue a Trigger command (TriggerInd). Returns false if no session.
  bool send_trigger(uint16_t trigger_ind);
  /// Enable/disable the BLE link (disable frees the gate for the phone app).
  void set_connection_enabled(bool enabled);
  bool is_session_ready() const { return this->state_ == STATE_READY; }
  uint8_t gate_status() const { return this->combined_status_; }
  bool holiday_lock() const { return this->holiday_lock_; }
  /// Re-read version + controller info.
  void request_info();

  CG_SENSOR(battery_voltage)
  CG_SENSOR(master_position)
  CG_SENSOR(slave_position)
  CG_SENSOR(master_encoder)
  CG_SENSOR(slave_encoder)
  CG_SENSOR(trigger_counter)
  CG_SENSOR(last_trigger)
  CG_TEXT(gate_status)
  CG_TEXT(master_status)
  CG_TEXT(slave_status)
  CG_TEXT(comms_version)
  CG_TEXT(core_version)
  CG_TEXT(serial_number)
  CG_TEXT(health_general)
  CG_TEXT(health_controller)
  CG_TEXT(health_battery)
  CG_TEXT(health_power_supply)
  CG_TEXT(health_motors)
  CG_BINARY(holiday_lock)
  CG_BINARY(frx)
  CG_BINARY(keep_open)
  CG_BINARY(mains_fail)
  CG_BINARY(power_low)
  CG_BINARY(irb_armed)
  CG_BINARY(beam_open)
  CG_BINARY(gate_lock)
  CG_BINARY(fault)
  CG_BINARY(connected)

 protected:
  enum State : uint8_t { STATE_IDLE, STATE_HANDSHAKE, STATE_READY };
  enum StepKind : uint8_t { STEP_CMD, STEP_READ };
  enum StepTag : uint8_t {
    TAG_CONN_PARAMS, TAG_AUTH, TAG_KEEPALIVE, TAG_URC_ON, TAG_URC_OFF,
    TAG_OVERVIEW, TAG_TRIGGER, TAG_READ_VERSION, TAG_READ_CTRL,
  };
  struct Step {
    StepKind kind;
    StepTag tag;
    std::vector<uint8_t> frame;
    uint16_t read_size;
  };

  void reset_session_();
  void start_handshake_();
  void enqueue_cmd_(StepTag tag, uint32_t addr, const std::vector<uint8_t> &payload);
  void enqueue_read_(StepTag tag, uint32_t addr, uint16_t size);
  void send_next_();
  bool write_(const std::vector<uint8_t> &frame);
  void on_packet_(const uint8_t *d, uint16_t n);
  void on_reply_(uint8_t status);
  void on_read_(const uint8_t *d, uint16_t n);
  void parse_overview_(const uint8_t *b);
  void publish_disconnected_();
  void derive_key_from_mac_();

  std::string phone_;
  uint8_t key_[4]{0, 0, 0, 0};
  bool key_set_{false};
  uint32_t keepalive_ms_{8000};

  uint16_t notify_handle_{0};
  uint16_t write_handle_{0};

  State state_{STATE_IDLE};
  std::deque<Step> queue_;
  bool in_flight_{false};
  Step current_{};
  uint32_t sent_at_{0};
  uint32_t last_keepalive_{0};
  uint8_t auth_failures_{0};
  bool want_enabled_{true};

  // multi-notify reassembly
  std::vector<uint8_t> rx_;
  uint16_t rx_expected_{0};

  uint8_t combined_status_{GATE_UNKNOWN};
  bool holiday_lock_{false};
  bool have_overview_{false};
  uint64_t last_notif_{~0ULL};
};

}  // namespace centurion_gate
}  // namespace esphome
