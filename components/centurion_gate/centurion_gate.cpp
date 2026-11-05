#include "centurion_gate.h"

#include <cinttypes>
#include "esphome/core/log.h"
#include "esphome/core/helpers.h"

namespace esphome {
namespace centurion_gate {

static const char *const TAG = "centurion_gate";

static const espbt::ESPBTUUID SERVICE_UUID = espbt::ESPBTUUID::from_raw("57b53956-da15-4005-8a1f-fcacde09af51");
static const espbt::ESPBTUUID NOTIFY_UUID = espbt::ESPBTUUID::from_raw("57b5db3b-da15-4005-8a1f-fcacde09af51");
static const espbt::ESPBTUUID WRITE_UUID = espbt::ESPBTUUID::from_raw("57b5f92c-da15-4005-8a1f-fcacde09af51");

// Transfer types (phone -> gate, byte 0 of the 10-byte header)
static const uint8_t TT_READ_REQUEST = 0x00;
static const uint8_t TT_COMMAND = 0x02;

// Command logical addresses (BE on the wire)
static const uint32_t CMD_AUTH_A = 0x08000004;
static const uint32_t CMD_KEEPALIVE = 0x08000020;
static const uint32_t CMD_OVERVIEW_ENTER = 0x08000022;
static const uint32_t CMD_TRIGGER = 0x08000024;
static const uint32_t CMD_URC_ENABLE = 0x0800002C;
static const uint32_t CMD_URC_DISABLE = 0x0800002D;
static const uint32_t CMD_CONN_PARAMS = 0x0FFFFFFC;
// RAM objects
static const uint32_t LA_VERSION_INFO = 0x00900400;
static const uint16_t SZ_VERSION_INFO = 38;
static const uint32_t LA_CONTROLLER_INFO = 0x00900A00;
static const uint16_t SZ_CONTROLLER_INFO = 40;

// Gate -> phone status byte
static const uint8_t TS_COMPLETE = 0;
static const uint8_t TS_READ_REPLY = 1;
static const uint8_t TS_ACK = 2;
static const uint8_t TS_NACK = 3;
static const uint8_t TS_TIMEOUT = 4;

static const uint8_t URC_OVERVIEW_VX = 0xA2;
static const uint8_t OVERVIEW_LEN = 64;
static const uint32_t REPLY_TIMEOUT_MS = 5000;

static inline uint16_t le16(const uint8_t *p) { return p[0] | (p[1] << 8); }
static inline uint32_t le32(const uint8_t *p) {
  return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}

static const char *gate_status_str(uint8_t s) {
  switch (s) {
    case GATE_OPEN: return "Open";
    case GATE_CLOSED: return "Closed";
    case GATE_PARTLY_OPEN: return "Partly Open";
    case GATE_PARTLY_CLOSED: return "Partly Closed";
    case GATE_OPENING: return "Opening";
    case GATE_CLOSING: return "Closing";
    default: return "Unknown";
  }
}

// INFRALOG_NOTIF_IND bit names (notif64 = notif2<<32 | notif1)
static const char *notif_name(int bit) {
  switch (bit) {
    case 0: return "Mains failure";
    case 1: return "Mains low";
    case 2: return "PSU hardware fault";
    case 3: return "Battery low";
    case 4: return "Replace battery";
    case 5: return "No batteries";
    case 6: return "Battery usage";
    case 7: return "File config busy";
    case 8: return "Invalid files";
    case 9: return "Encoder while running";
    case 10: return "PSU comms unhealthy";
    case 11: return "IO config error";
    case 12: return "IO4 overcurrent";
    case 13: return "IO5 overcurrent";
    case 14: return "IO6 overcurrent";
    case 15: return "Master collision";
    case 16: return "Slave collision";
    case 17: return "Max collisions";
    case 18: return "Master stalled";
    case 19: return "Slave stalled";
    case 20: return "Gate lost";
    case 21: return "Limits not set";
    case 22: return "Aux 12V failure";
    case 23: return "H-bridge temperature";
    case 24: return "Master bridge/fuse";
    case 25: return "Slave bridge/fuse";
    case 26: return "CPU temperature";
    case 27: return "Temperature warning";
    case 28: return "Master motor disconnected";
    case 29: return "Slave motor disconnected";
    case 30: return "Emergency stop";
    case 31: return "Holiday lock";
    case 32: return "IRBO test fail";
    case 33: return "IRBC test fail";
    case 34: return "IRBO alarm";
    case 35: return "IRBC alarm";
    case 36: return "Beams open";
    case 37: return "Photons BLE disconnected";
    case 38: return "Photons battery low";
    default: return nullptr;
  }
}

#define B(n) (1ULL << (n))
// Category masks (HealthService / ProductTypeToHealthCategoryMapper)
static const uint64_t MASK_BATTERY = B(3) | B(4) | B(5);
static const uint64_t MASK_CONTROLLER = B(6) | B(7) | B(8) | B(9) | B(11) | B(12) | B(13) | B(14) | B(23) | B(26) |
                                        B(27) | B(32) | B(33);
static const uint64_t MASK_POWER = B(0) | B(1) | B(2) | B(10);
static const uint64_t MASK_GENERAL = B(22) | B(30) | B(31) | B(36) | B(37) | B(38);
static const uint64_t MASK_MOTORS = B(15) | B(16) | B(17) | B(18) | B(19) | B(20) | B(21) | B(24) | B(25) | B(28) |
                                    B(29) | B(34) | B(35);
// Swing-gate ignore list: the app never shows these as faults
static const uint64_t MASK_IGNORED = B(20) | B(21) | B(31) | B(34) | B(35);
#undef B

static std::string health_text(uint64_t notif, uint64_t mask) {
  uint64_t bits = notif & mask & ~MASK_IGNORED;
  if (bits == 0)
    return "Ok";
  std::string out;
  for (int i = 0; i < 64; i++) {
    if (!(bits & (1ULL << i)))
      continue;
    if (!out.empty())
      out += ", ";
    const char *n = notif_name(i);
    out += n != nullptr ? n : ("Bit " + to_string(i)).c_str();
  }
  return out;
}

// ---------------------------------------------------------------------------

void CenturionGate::setup() { this->publish_disconnected_(); }

void CenturionGate::dump_config() {
  ESP_LOGCONFIG(TAG, "Centurion gate (MyCentsys BLE):");
  ESP_LOGCONFIG(TAG, "  Phone number: %s", this->phone_.empty() ? "NOT SET" : "<set>");
  ESP_LOGCONFIG(TAG, "  Cipher key: %s", this->key_set_ ? "configured" : "from MAC (last 4 bytes)");
  ESP_LOGCONFIG(TAG, "  Keep-alive: %" PRIu32 " ms", this->keepalive_ms_);
}

void CenturionGate::set_connection_enabled(bool enabled) {
  this->want_enabled_ = enabled;
  this->auth_failures_ = 0;
  ESP_LOGI(TAG, "BLE connection %s", enabled ? "enabled" : "disabled (gate free for phone app)");
  this->parent()->set_enabled(enabled);
  if (!enabled)
    this->publish_disconnected_();
}

void CenturionGate::derive_key_from_mac_() {
  if (this->key_set_)
    return;
  // The 4-byte key in the advert (mfr 0x58, bytes 1..4) equals the last 4 bytes of the BLE MAC
  // (FC:B0:E9:61:70:09 -> e9 61 70 09).
  uint64_t mac = this->parent()->get_address();
  this->key_[0] = (mac >> 24) & 0xFF;
  this->key_[1] = (mac >> 16) & 0xFF;
  this->key_[2] = (mac >> 8) & 0xFF;
  this->key_[3] = mac & 0xFF;
}

void CenturionGate::reset_session_() {
  this->state_ = STATE_IDLE;
  this->queue_.clear();
  this->in_flight_ = false;
  this->rx_.clear();
  this->rx_expected_ = 0;
}

void CenturionGate::publish_disconnected_() {
  if (this->connected_binary_sensor_ != nullptr)
    this->connected_binary_sensor_->publish_state(false);
  if (this->gate_status_text_sensor_ != nullptr)
    this->gate_status_text_sensor_->publish_state("Unknown");
  this->combined_status_ = GATE_UNKNOWN;
  this->have_overview_ = false;
  this->last_notif_ = ~0ULL;
}

void CenturionGate::gattc_event_handler(esp_gattc_cb_event_t event, esp_gatt_if_t gattc_if,
                                        esp_ble_gattc_cb_param_t *param) {
  switch (event) {
    case ESP_GATTC_OPEN_EVT:
      if (param->open.status == ESP_GATT_OK)
        ESP_LOGI(TAG, "Connected to gate, discovering services");
      break;
    case ESP_GATTC_DISCONNECT_EVT:
    case ESP_GATTC_CLOSE_EVT:
      if (this->state_ != STATE_IDLE)
        ESP_LOGW(TAG, "Disconnected from gate");
      this->reset_session_();
      this->notify_handle_ = 0;
      this->write_handle_ = 0;
      this->publish_disconnected_();
      break;
    case ESP_GATTC_SEARCH_CMPL_EVT: {
      auto *n = this->parent()->get_characteristic(SERVICE_UUID, NOTIFY_UUID);
      auto *w = this->parent()->get_characteristic(SERVICE_UUID, WRITE_UUID);
      if (n == nullptr || w == nullptr) {
        ESP_LOGE(TAG, "Centurion serial service/characteristics not found");
        break;
      }
      this->notify_handle_ = n->handle;
      this->write_handle_ = w->handle;
      auto err = this->parent()->register_for_notify(this->notify_handle_);
      if (err != ESP_OK)
        ESP_LOGW(TAG, "register_for_notify failed: %d", err);
      break;
    }
    case ESP_GATTC_REG_FOR_NOTIFY_EVT:
      if (param->reg_for_notify.handle != this->notify_handle_)
        break;
      if (param->reg_for_notify.status != ESP_GATT_OK) {
        ESP_LOGW(TAG, "Notify registration failed: %d", param->reg_for_notify.status);
        break;
      }
      this->node_state = espbt::ClientState::ESTABLISHED;
      this->start_handshake_();
      break;
    case ESP_GATTC_NOTIFY_EVT:
      if (param->notify.handle == this->notify_handle_)
        this->on_packet_(param->notify.value, param->notify.value_len);
      break;
    default:
      break;
  }
}

// ---------------------------------------------------------------------------
// framing

void CenturionGate::enqueue_cmd_(StepTag tag, uint32_t addr, const std::vector<uint8_t> &payload) {
  Step s{STEP_CMD, tag, {}, 0};
  uint32_t len = payload.size();
  s.frame = {TT_COMMAND,
             uint8_t(addr >> 24), uint8_t(addr >> 16), uint8_t(addr >> 8), uint8_t(addr),
             uint8_t(len >> 24), uint8_t(len >> 16), uint8_t(len >> 8), uint8_t(len),
             0x00};
  s.frame.insert(s.frame.end(), payload.begin(), payload.end());
  this->queue_.push_back(std::move(s));
}

void CenturionGate::enqueue_read_(StepTag tag, uint32_t addr, uint16_t size) {
  Step s{STEP_READ, tag, {}, size};
  s.frame = {TT_READ_REQUEST,
             uint8_t(addr >> 24), uint8_t(addr >> 16), uint8_t(addr >> 8), uint8_t(addr),
             0, 0, uint8_t(size >> 8), uint8_t(size),
             0x00};
  this->queue_.push_back(std::move(s));
}

bool CenturionGate::write_(const std::vector<uint8_t> &frame) {
  if (this->write_handle_ == 0)
    return false;
  auto err = esp_ble_gattc_write_char(this->parent()->get_gattc_if(), this->parent()->get_conn_id(),
                                      this->write_handle_, frame.size(), const_cast<uint8_t *>(frame.data()),
                                      ESP_GATT_WRITE_TYPE_NO_RSP, ESP_GATT_AUTH_REQ_NONE);
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "write failed: %d", err);
    return false;
  }
  return true;
}

void CenturionGate::start_handshake_() {
  this->reset_session_();
  this->derive_key_from_mac_();
  if (this->phone_.empty()) {
    ESP_LOGE(TAG, "No phone number configured");
    return;
  }
  this->state_ = STATE_HANDSHAKE;
  // ChangeBLEConnectionParameters: BE u16 min=10, max=12, latency=0, supervision=4000 (as the Android app)
  this->enqueue_cmd_(TAG_CONN_PARAMS, CMD_CONN_PARAMS, {0x00, 0x0A, 0x00, 0x0C, 0x00, 0x00, 0x0F, 0xA0});
  // AuthMobilePhoneA: phone bytes XOR key[i%4], then nibble swap
  std::vector<uint8_t> enc;
  for (size_t i = 0; i < this->phone_.size(); i++) {
    uint8_t b = uint8_t(this->phone_[i]) ^ this->key_[i % 4];
    enc.push_back(uint8_t((b << 4) | (b >> 4)));
  }
  this->enqueue_cmd_(TAG_AUTH, CMD_AUTH_A, enc);
  this->enqueue_cmd_(TAG_KEEPALIVE, CMD_KEEPALIVE, {});
  this->enqueue_cmd_(TAG_URC_ON, CMD_URC_ENABLE, {});
  this->enqueue_read_(TAG_READ_VERSION, LA_VERSION_INFO, SZ_VERSION_INFO);
  this->enqueue_read_(TAG_READ_CTRL, LA_CONTROLLER_INFO, SZ_CONTROLLER_INFO);
  this->enqueue_cmd_(TAG_KEEPALIVE, CMD_KEEPALIVE, {});
  this->enqueue_cmd_(TAG_URC_OFF, CMD_URC_DISABLE, {});
  this->enqueue_cmd_(TAG_OVERVIEW, CMD_OVERVIEW_ENTER, {});
  this->last_keepalive_ = millis();
  ESP_LOGI(TAG, "Starting authentication");
}

void CenturionGate::send_next_() {
  if (this->in_flight_ || this->queue_.empty())
    return;
  this->current_ = std::move(this->queue_.front());
  this->queue_.pop_front();
  if (!this->write_(this->current_.frame)) {
    this->parent()->disconnect();
    return;
  }
  this->in_flight_ = true;
  this->sent_at_ = millis();
  if (this->current_.tag == TAG_KEEPALIVE)
    this->last_keepalive_ = this->sent_at_;
}

void CenturionGate::loop() {
  if (this->state_ == STATE_IDLE)
    return;
  const uint32_t now = millis();
  if (this->in_flight_ && now - this->sent_at_ > REPLY_TIMEOUT_MS) {
    ESP_LOGW(TAG, "No reply to step %u, reconnecting", this->current_.tag);
    this->in_flight_ = false;
    this->parent()->disconnect();
    return;
  }
  if (this->state_ == STATE_READY && !this->in_flight_ && this->queue_.empty() &&
      now - this->last_keepalive_ >= this->keepalive_ms_) {
    this->enqueue_cmd_(TAG_KEEPALIVE, CMD_KEEPALIVE, {});
  }
  this->send_next_();
}

bool CenturionGate::send_trigger(uint16_t trigger_ind) {
  if (this->state_ != STATE_READY) {
    ESP_LOGW(TAG, "Trigger %u ignored: no authenticated session", trigger_ind);
    return false;
  }
  ESP_LOGI(TAG, "Sending trigger %u", trigger_ind);
  // INFRATP_TRIGGER { u16 flags; u16 trigger; } little-endian
  this->enqueue_cmd_(TAG_TRIGGER, CMD_TRIGGER, {0x00, 0x00, uint8_t(trigger_ind & 0xFF), uint8_t(trigger_ind >> 8)});
  this->send_next_();
  return true;
}

void CenturionGate::request_info() {
  if (this->state_ != STATE_READY)
    return;
  this->enqueue_read_(TAG_READ_VERSION, LA_VERSION_INFO, SZ_VERSION_INFO);
  this->enqueue_read_(TAG_READ_CTRL, LA_CONTROLLER_INFO, SZ_CONTROLLER_INFO);
}

// ---------------------------------------------------------------------------
// receive

void CenturionGate::on_packet_(const uint8_t *d, uint16_t n) {
  // Reassemble if a gate packet (2-byte header + psz) spans several notifications (small MTU).
  if (this->rx_expected_ > 0) {
    this->rx_.insert(this->rx_.end(), d, d + n);
    if (this->rx_.size() < this->rx_expected_)
      return;
    std::vector<uint8_t> pkt;
    pkt.swap(this->rx_);
    this->rx_expected_ = 0;
    this->on_packet_(pkt.data(), pkt.size());
    return;
  }
  if (n < 2)
    return;
  uint16_t total = 2 + d[1];
  if (n < total) {
    this->rx_.assign(d, d + n);
    this->rx_expected_ = total;
    return;
  }
  const uint8_t status = d[0];
  const uint8_t psz = d[1];

  // Overview URC: 00 42 A2 40 <64-byte INFRATP_OVERVIEW_VX>
  if (psz >= 2 + OVERVIEW_LEN && d[2] == URC_OVERVIEW_VX && d[3] == OVERVIEW_LEN) {
    this->parse_overview_(d + 4);
    return;
  }
  if (this->in_flight_ && this->current_.kind == STEP_READ && psz == this->current_.read_size &&
      (status == TS_COMPLETE || status == TS_READ_REPLY)) {
    this->on_read_(d + 2, psz);
    this->in_flight_ = false;
    this->send_next_();
    return;
  }
  if (psz == 0 && this->in_flight_) {
    this->on_reply_(status);
    return;
  }
  ESP_LOGD(TAG, "Unhandled packet: %s", format_hex_pretty(d, n).c_str());
}

void CenturionGate::on_reply_(uint8_t status) {
  const StepTag tag = this->current_.tag;
  this->in_flight_ = false;
  const bool ok = status == TS_COMPLETE || status == TS_ACK;
  if (tag == TAG_AUTH) {
    if (!ok) {
      this->auth_failures_++;
      ESP_LOGE(TAG, "Authentication REJECTED (status %u) - phone number not enrolled on this gate?", status);
      this->reset_session_();
      this->parent()->disconnect();
      if (this->auth_failures_ >= 3) {
        // Stop hammering the gate; retry in 10 minutes.
        ESP_LOGE(TAG, "3 auth failures, pausing connection for 10 min");
        this->parent()->set_enabled(false);
        this->set_timeout("auth_backoff", 600000, [this]() {
          if (this->want_enabled_)
            this->parent()->set_enabled(true);
        });
        this->auth_failures_ = 0;
      }
      return;
    }
    this->auth_failures_ = 0;
    ESP_LOGI(TAG, "Authenticated");
    if (this->connected_binary_sensor_ != nullptr)
      this->connected_binary_sensor_->publish_state(true);
  } else if (!ok) {
    ESP_LOGW(TAG, "Step %u got status %u", tag, status);
    if (tag == TAG_TRIGGER)
      ESP_LOGW(TAG, "Trigger was not accepted");
  }
  if (tag == TAG_OVERVIEW && ok) {
    this->state_ = STATE_READY;
    ESP_LOGI(TAG, "Status overview stream active");
  }
  this->send_next_();
}

void CenturionGate::on_read_(const uint8_t *d, uint16_t n) {
  char buf[40];
  if (this->current_.tag == TAG_READ_VERSION && n >= 25) {
    snprintf(buf, sizeof(buf), "%u.%u.%u.%u", d[4], d[5], d[6], d[7]);
    ESP_LOGI(TAG, "Comms firmware %s", buf);
    if (this->comms_version_text_sensor_ != nullptr)
      this->comms_version_text_sensor_->publish_state(buf);
    snprintf(buf, sizeof(buf), "%u.%u.%u.%u", d[21], d[22], d[23], d[24]);
    ESP_LOGI(TAG, "Core firmware %s", buf);
    if (this->core_version_text_sensor_ != nullptr)
      this->core_version_text_sensor_->publish_state(buf);
  } else if (this->current_.tag == TAG_READ_CTRL && n >= 16) {
    // u32 flags, then core SN1, SN2, SN3 (LE); app shows SN3 SN2 SN1 as %08X
    snprintf(buf, sizeof(buf), "%08" PRIX32 "%08" PRIX32 "%08" PRIX32, le32(d + 12), le32(d + 8), le32(d + 4));
    ESP_LOGI(TAG, "Serial %s", buf);
    if (this->serial_number_text_sensor_ != nullptr)
      this->serial_number_text_sensor_->publish_state(buf);
  }
}

void CenturionGate::parse_overview_(const uint8_t *b) {
  const float batt = le16(b + 2) / 100.0f;
  const uint32_t n1 = le32(b + 4), n2 = le32(b + 8), cond = le32(b + 12);
  const uint8_t m_status = b[20], s_status = b[28], combined = b[36];
  const int16_t m_pos = int16_t(le16(b + 22)), s_pos = int16_t(le16(b + 30));
  const int32_t m_enc = int32_t(le32(b + 24)), s_enc = int32_t(le32(b + 32));
  const uint8_t irbo = b[37], irbc = b[38], gate_lock = b[39], power = b[42];
  const uint8_t trig_counter = b[44];
  const uint16_t last_trig = le16(b + 48);

  ESP_LOGD(TAG, "Overview: %s batt=%.2fV n1=%08" PRIX32 " n2=%08" PRIX32 " cond=%08" PRIX32 " m=%d s=%d",
           gate_status_str(combined), batt, n1, n2, cond, m_pos, s_pos);
  if (combined != this->combined_status_)
    ESP_LOGI(TAG, "Gate %s", gate_status_str(combined));
  this->combined_status_ = combined;
  this->holiday_lock_ = cond & (1u << 0);
  this->have_overview_ = true;

  if (this->battery_voltage_sensor_) this->battery_voltage_sensor_->publish_state(batt);
  if (this->master_position_sensor_) this->master_position_sensor_->publish_state(m_pos);
  if (this->slave_position_sensor_) this->slave_position_sensor_->publish_state(s_pos);
  if (this->master_encoder_sensor_) this->master_encoder_sensor_->publish_state(m_enc);
  if (this->slave_encoder_sensor_) this->slave_encoder_sensor_->publish_state(s_enc);
  if (this->trigger_counter_sensor_) this->trigger_counter_sensor_->publish_state(trig_counter);
  if (this->last_trigger_sensor_) this->last_trigger_sensor_->publish_state(last_trig);

  if (this->gate_status_text_sensor_) this->gate_status_text_sensor_->publish_state(gate_status_str(combined));
  if (this->master_status_text_sensor_) this->master_status_text_sensor_->publish_state(gate_status_str(m_status));
  if (this->slave_status_text_sensor_) this->slave_status_text_sensor_->publish_state(gate_status_str(s_status));

  if (this->holiday_lock_binary_sensor_) this->holiday_lock_binary_sensor_->publish_state(cond & (1u << 0));
  if (this->frx_binary_sensor_) this->frx_binary_sensor_->publish_state(cond & (1u << 5));
  if (this->keep_open_binary_sensor_) this->keep_open_binary_sensor_->publish_state(cond & (1u << 13));
  if (this->irb_armed_binary_sensor_) this->irb_armed_binary_sensor_->publish_state(cond & (1u << 9));
  if (this->mains_fail_binary_sensor_) this->mains_fail_binary_sensor_->publish_state(n1 & (1u << 0));
  if (this->power_low_binary_sensor_) this->power_low_binary_sensor_->publish_state(power != 0);
  // irbo/irbc: 0 = DISABLED seen live; non-zero meaning per enum is unverified -> notif bit 36 BEAMS_OPEN used too
  if (this->beam_open_binary_sensor_)
    this->beam_open_binary_sensor_->publish_state(n2 & (1u << (36 - 32)));
  if (this->gate_lock_binary_sensor_) this->gate_lock_binary_sensor_->publish_state(gate_lock != 0);
  (void) irbo;
  (void) irbc;

  const uint64_t notif = (uint64_t(n2) << 32) | n1;
  if (notif != this->last_notif_) {
    this->last_notif_ = notif;
    if (this->health_general_text_sensor_)
      this->health_general_text_sensor_->publish_state(health_text(notif, MASK_GENERAL));
    if (this->health_controller_text_sensor_)
      this->health_controller_text_sensor_->publish_state(health_text(notif, MASK_CONTROLLER));
    if (this->health_battery_text_sensor_)
      this->health_battery_text_sensor_->publish_state(health_text(notif, MASK_BATTERY));
    if (this->health_power_supply_text_sensor_)
      this->health_power_supply_text_sensor_->publish_state(health_text(notif, MASK_POWER));
    if (this->health_motors_text_sensor_)
      this->health_motors_text_sensor_->publish_state(health_text(notif, MASK_MOTORS));
    if (this->fault_binary_sensor_)
      this->fault_binary_sensor_->publish_state((notif & ~MASK_IGNORED) != 0);
  }
}

}  // namespace centurion_gate
}  // namespace esphome
