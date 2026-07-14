#include "dynamic_sprinkler.h"

#include "esphome/components/api/api_server.h"
#include "esphome/core/log.h"
#include "esphome/core/string_ref.h"
#include <esp_heap_caps.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <memory>

#ifdef USE_ESP32
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#endif

namespace esphome::dynamic_sprinkler {

static const char *const TAG = "dynamic_sprinkler";

struct WebRequestJob {
  bool post{false};
  std::string path;
  std::string id;
  std::string data;
  std::string action;
  std::string setting;
  std::string value;
  std::string index;
  std::string key;
  std::string type;
  std::string offset;
  int response_code{500};
  std::string response_body{"{\"error\":\"request was not processed\"}"};
  std::atomic<uint8_t> state{0};  // 0 queued, 1 processing, 2 cancelled.
#ifdef USE_ESP32
  SemaphoreHandle_t done{xSemaphoreCreateBinary()};
  ~WebRequestJob() {
    if (this->done != nullptr) vSemaphoreDelete(this->done);
  }
#endif
};

namespace {
static constexpr uint32_t SCHEDULE_BLOB_MAGIC = 0x44535052;  // "RPSD" in little-endian storage.

uint32_t storage_crc32(const uint8_t *data, size_t length) {
  uint32_t crc = 0xFFFFFFFFU;
  for (size_t i = 0; i < length; i++) {
    crc ^= data[i];
    for (uint8_t bit = 0; bit < 8; bit++) crc = (crc >> 1U) ^ (0xEDB88320U & (0U - (crc & 1U)));
  }
  return ~crc;
}

class StorageWriter {
 public:
  StorageWriter(uint8_t *data, size_t capacity) : data_(data), capacity_(capacity) {}
  void u8(uint8_t value) { this->write_(&value, 1); }
  void u16(uint16_t value) {
    uint8_t bytes[]{(uint8_t) value, (uint8_t) (value >> 8U)};
    this->write_(bytes, sizeof(bytes));
  }
  void u32(uint32_t value) {
    uint8_t bytes[]{(uint8_t) value, (uint8_t) (value >> 8U), (uint8_t) (value >> 16U), (uint8_t) (value >> 24U)};
    this->write_(bytes, sizeof(bytes));
  }
  void i16(int16_t value) { this->u16((uint16_t) value); }
  void i64(int64_t value) {
    const uint64_t raw = (uint64_t) value;
    for (uint8_t shift = 0; shift < 64; shift += 8) this->u8(raw >> shift);
  }
  void bytes(const void *value, size_t length) { this->write_((const uint8_t *) value, length); }
  size_t size() const { return position_; }
  bool ok() const { return ok_; }

 protected:
  void write_(const uint8_t *value, size_t length) {
    if (!ok_ || position_ + length > capacity_) {
      ok_ = false;
      return;
    }
    std::memcpy(data_ + position_, value, length);
    position_ += length;
  }
  uint8_t *data_;
  size_t capacity_;
  size_t position_{0};
  bool ok_{true};
};

class StorageReader {
 public:
  StorageReader(const uint8_t *data, size_t length) : data_(data), length_(length) {}
  uint8_t u8() { uint8_t value{}; this->read_(&value, 1); return value; }
  uint16_t u16() {
    const uint16_t low = this->u8();
    const uint16_t high = this->u8();
    return low | (high << 8U);
  }
  uint32_t u32() {
    const uint32_t low = this->u16();
    const uint32_t high = this->u16();
    return low | (high << 16U);
  }
  int16_t i16() { return (int16_t) this->u16(); }
  int64_t i64() {
    uint64_t value = 0;
    for (uint8_t shift = 0; shift < 64; shift += 8) value |= (uint64_t) this->u8() << shift;
    return (int64_t) value;
  }
  void bytes(void *value, size_t length) { this->read_((uint8_t *) value, length); }
  bool ok() const { return ok_; }

 protected:
  void read_(uint8_t *value, size_t length) {
    if (!ok_ || position_ + length > length_) {
      ok_ = false;
      std::memset(value, 0, length);
      return;
    }
    std::memcpy(value, data_ + position_, length);
    position_ += length;
  }
  const uint8_t *data_;
  size_t length_;
  size_t position_{0};
  bool ok_{true};
};

template<size_t N> bool finish_blob(std::array<uint8_t, N> &blob, const StorageWriter &writer) {
  if (!writer.ok() || writer.size() < 12 || writer.size() - 12 > UINT16_MAX) return false;
  const uint16_t payload_length = writer.size() - 12;
  blob[6] = payload_length & 0xFF;
  blob[7] = payload_length >> 8U;
  const uint32_t crc = storage_crc32(blob.data() + 12, payload_length);
  for (uint8_t i = 0; i < 4; i++) blob[8 + i] = crc >> (8U * i);
  return true;
}

template<size_t N>
bool open_blob(const std::array<uint8_t, N> &blob, uint32_t magic, uint16_t version, StorageReader &payload) {
  StorageReader header(blob.data(), 12);
  if (header.u32() != magic || header.u16() != version) return false;
  const uint16_t payload_length = header.u16();
  const uint32_t expected_crc = header.u32();
  if (!header.ok() || payload_length > blob.size() - 12 ||
      storage_crc32(blob.data() + 12, payload_length) != expected_crc) return false;
  payload = StorageReader(blob.data() + 12, payload_length);
  return true;
}

ESPTime local_date_offset(ESPTime reference, int8_t offset) {
  reference.hour = 12;
  reference.minute = 0;
  reference.second = 0;
  reference.recalc_timestamp_local();
  if (!reference.is_valid()) return {};
  return ESPTime::from_epoch_local(reference.timestamp + (int32_t) offset * 86400L);
}

}  // namespace

void DynamicWeatherSensor::setup() {
  api::global_api_server->subscribe_home_assistant_state(
      this->entity_id_.c_str(), nullptr, [this](StringRef state) {
        auto value = parse_number<float>(state.c_str());
        if (!value.has_value() || !std::isfinite(*value)) {
          ESP_LOGW(TAG, "Weather entity '%s' returned non-numeric state '%s'", this->entity_id_.c_str(),
                   state.c_str());
          this->publish_state(NAN);
          return;
        }
        this->publish_state(*value);
      });
}

void DynamicWeatherSensor::dump_config() {
  LOG_SENSOR("  ", "Weather input", this);
  ESP_LOGCONFIG(TAG, "    Home Assistant entity: %s", this->entity_id_.c_str());
}

void DynamicZoneButton::press_action() {
  if (this->parent_ != nullptr) this->parent_->start_single_zone(this->zone_, this->parent_->quick_manual_seconds());
}

void DynamicSprinkler::setup() {
  this->load_store_();
  this->update_zone_button_names_();
  auto track = [this](uint8_t index, sensor::Sensor *source) {
    if (source == nullptr) return;
    source->add_on_state_callback([this, index](float value) {
      if (!std::isnan(value)) this->weather_updated_ms_[index] = millis();
    });
  };
  track(0, this->wind_speed_);
  track(1, this->wind_gust_);
  track(2, this->rain_24h_);
  track(3, this->rain_rate_);
  track(4, this->daily_temp_);
  web_server_base::global_web_server_base->add_handler(this);
  this->turn_all_off_();
  this->publish_selected_enabled_();
  this->set_decision_("Dynamic scheduler ready");
  this->publish_status_(true);
}

void DynamicSprinkler::dump_config() {
  ESP_LOGCONFIG(TAG, "Dynamic sprinkler scheduler:");
  ESP_LOGCONFIG(TAG, "  Configured zones: %u (maximum %u)", this->relays_.size(), MAX_ZONES);
  ESP_LOGCONFIG(TAG, "  Capacity: %u schedules, %u rows, %u triggers per schedule, %u zones per row", MAX_SCHEDULES,
                MAX_ROWS, MAX_START_TIMES, MAX_ZONES_PER_ROW);
}

void DynamicSprinkler::load_store_() {
  this->serialized_meta_pref_ =
      global_preferences->make_preference<std::array<uint8_t, META_BLOB_SIZE>>(PREF_BASE + 0x600);
  if (!this->load_serialized_meta_()) {
    this->meta_ = {};
    this->meta_.marker = META_MARKER;
    this->meta_.next_schedule_id = 1;
    this->meta_.next_run_id = 1;
  }
  this->meta_.history_head %= HISTORY_SIZE;
  if (this->meta_.next_schedule_id == 0) this->meta_.next_schedule_id = 1;
  if (this->meta_.next_run_id == 0) this->meta_.next_run_id = 1;
  for (uint8_t i = 0; i < MAX_SCHEDULES; i++) {
    this->serialized_schedule_prefs_[i] =
        global_preferences->make_preference<std::array<uint8_t, SCHEDULE_BLOB_SIZE>>(SERIALIZED_SCHEDULE_PREF_BASE + i);
    if (!this->load_serialized_schedule_(i, this->schedules_[i])) this->schedules_[i] = ScheduleRecord{};
    if (!this->schedule_is_usable_(this->schedules_[i])) {
      if (this->schedules_[i].marker == SCHEDULE_MARKER)
        ESP_LOGW(TAG, "Ignoring invalid persisted schedule slot %u", i);
      this->schedules_[i] = ScheduleRecord{};
    }
  }
  for (uint8_t i = 0; i < HISTORY_SIZE; i++) {
    this->serialized_history_prefs_[i] =
        global_preferences->make_preference<std::array<uint8_t, HISTORY_BLOB_SIZE>>(PREF_BASE + 0x700 + i);
    if (!this->load_serialized_history_(i)) this->history_[i] = {};
  }
  this->serialized_zone_names_pref_ =
      global_preferences->make_preference<std::array<uint8_t, ZONE_NAMES_BLOB_SIZE>>(PREF_BASE + 0x800);
  const bool names_loaded = this->load_serialized_zone_names_();
  if (!names_loaded) {
    this->zone_name_store_ = {};
    this->zone_name_store_.marker = ZONE_NAMES_MARKER;
  }
  bool names_changed = !names_loaded;
  for (size_t i = 0; i < this->zone_names_.size(); i++) {
    const uint8_t stored_index = this->zone_ids_[i] - 1;
    if (this->zone_name_store_.names[stored_index][0] == '\0') {
      std::strncpy(this->zone_name_store_.names[stored_index], this->zone_names_[i].c_str(),
                   sizeof(this->zone_name_store_.names[stored_index]) - 1);
      names_changed = true;
    } else {
      this->zone_names_[i] = this->zone_name_store_.names[stored_index];
    }
  }
  if (names_changed && (!this->save_serialized_zone_names_() || !this->sync_preferences_()))
    ESP_LOGE(TAG, "Failed to persist default zone names");

  this->serialized_weather_pref_ =
      global_preferences->make_preference<std::array<uint8_t, WEATHER_BLOB_SIZE>>(PREF_BASE + 0x900);
  if (!this->load_serialized_weather_()) {
    this->weather_settings_ = {};
    this->weather_settings_.marker = WEATHER_SETTINGS_MARKER;
  }
  this->weather_settings_.enabled_mask &= this->weather_capabilities_();
}

bool DynamicSprinkler::save_meta_() { return this->save_serialized_meta_(); }
bool DynamicSprinkler::save_schedule_(uint8_t slot) {
  return this->save_serialized_schedule_(slot, this->schedules_[slot]);
}
bool DynamicSprinkler::save_history_(uint8_t slot) { return this->save_serialized_history_(slot); }
bool DynamicSprinkler::save_weather_settings_() { return this->save_serialized_weather_(); }

bool DynamicSprinkler::sync_preferences_() {
  if (global_preferences != nullptr && global_preferences->sync()) return true;
  ESP_LOGE(TAG, "Failed to commit sprinkler preferences to flash");
  return false;
}

bool DynamicSprinkler::save_serialized_meta_() {
  std::array<uint8_t, META_BLOB_SIZE> blob{};
  StorageWriter writer(blob.data(), blob.size());
  writer.u32(0x44534D54); writer.u16(1); writer.u16(0); writer.u32(0);
  writer.u32(this->meta_.marker);
  writer.u32(this->meta_.next_schedule_id);
  writer.u32(this->meta_.next_run_id);
  writer.u8(this->meta_.history_head);
  return finish_blob(blob, writer) && this->serialized_meta_pref_.save(&blob);
}

bool DynamicSprinkler::load_serialized_meta_() {
  std::array<uint8_t, META_BLOB_SIZE> blob{};
  if (!this->serialized_meta_pref_.load(&blob)) return false;
  StorageReader reader(nullptr, 0);
  if (!open_blob(blob, 0x44534D54, 1, reader)) return false;
  StoreMeta decoded{};
  decoded.marker = reader.u32();
  decoded.next_schedule_id = reader.u32();
  decoded.next_run_id = reader.u32();
  decoded.history_head = reader.u8();
  if (!reader.ok() || decoded.marker != META_MARKER) return false;
  decoded.history_head %= HISTORY_SIZE;
  this->meta_ = decoded;
  return true;
}

bool DynamicSprinkler::save_serialized_history_(uint8_t slot) {
  if (slot >= HISTORY_SIZE) return false;
  const auto &entry = this->history_[slot];
  std::array<uint8_t, HISTORY_BLOB_SIZE> blob{};
  StorageWriter writer(blob.data(), blob.size());
  writer.u32(0x44534849); writer.u16(1); writer.u16(0); writer.u32(0);
  writer.u32(entry.marker); writer.u32(entry.run_id); writer.u32(entry.schedule_id);
  writer.u32(entry.started_at); writer.u32(entry.ended_at); writer.u32(entry.watered_seconds);
  writer.u8(entry.manual); writer.bytes(entry.name, sizeof(entry.name)); writer.bytes(entry.result, sizeof(entry.result));
  return finish_blob(blob, writer) && this->serialized_history_prefs_[slot].save(&blob);
}

bool DynamicSprinkler::load_serialized_history_(uint8_t slot) {
  if (slot >= HISTORY_SIZE) return false;
  std::array<uint8_t, HISTORY_BLOB_SIZE> blob{};
  if (!this->serialized_history_prefs_[slot].load(&blob)) return false;
  StorageReader reader(nullptr, 0);
  if (!open_blob(blob, 0x44534849, 1, reader)) return false;
  HistoryRecord decoded{};
  decoded.marker = reader.u32(); decoded.run_id = reader.u32(); decoded.schedule_id = reader.u32();
  decoded.started_at = reader.u32(); decoded.ended_at = reader.u32(); decoded.watered_seconds = reader.u32();
  decoded.manual = reader.u8(); reader.bytes(decoded.name, sizeof(decoded.name)); reader.bytes(decoded.result, sizeof(decoded.result));
  decoded.name[sizeof(decoded.name) - 1] = '\0'; decoded.result[sizeof(decoded.result) - 1] = '\0';
  if (!reader.ok() || (decoded.marker != 0 && decoded.marker != HISTORY_MARKER)) return false;
  this->history_[slot] = decoded;
  return true;
}

bool DynamicSprinkler::save_serialized_zone_names_() {
  std::array<uint8_t, ZONE_NAMES_BLOB_SIZE> blob{};
  StorageWriter writer(blob.data(), blob.size());
  writer.u32(0x44535A4E); writer.u16(1); writer.u16(0); writer.u32(0);
  writer.u32(this->zone_name_store_.marker);
  writer.bytes(this->zone_name_store_.names, sizeof(this->zone_name_store_.names));
  return finish_blob(blob, writer) && this->serialized_zone_names_pref_.save(&blob);
}

bool DynamicSprinkler::load_serialized_zone_names_() {
  std::array<uint8_t, ZONE_NAMES_BLOB_SIZE> blob{};
  if (!this->serialized_zone_names_pref_.load(&blob)) return false;
  StorageReader reader(nullptr, 0);
  if (!open_blob(blob, 0x44535A4E, 1, reader)) return false;
  ZoneNameStore decoded{};
  decoded.marker = reader.u32(); reader.bytes(decoded.names, sizeof(decoded.names));
  if (!reader.ok() || decoded.marker != ZONE_NAMES_MARKER) return false;
  for (auto &name : decoded.names) name[sizeof(name) - 1] = '\0';
  this->zone_name_store_ = decoded;
  return true;
}

bool DynamicSprinkler::save_serialized_weather_() {
  std::array<uint8_t, WEATHER_BLOB_SIZE> blob{};
  StorageWriter writer(blob.data(), blob.size());
  writer.u32(0x44535745); writer.u16(1); writer.u16(0); writer.u32(0);
  writer.u32(this->weather_settings_.marker); writer.u8(this->weather_settings_.enabled_mask);
  return finish_blob(blob, writer) && this->serialized_weather_pref_.save(&blob);
}

bool DynamicSprinkler::load_serialized_weather_() {
  std::array<uint8_t, WEATHER_BLOB_SIZE> blob{};
  if (!this->serialized_weather_pref_.load(&blob)) return false;
  StorageReader reader(nullptr, 0);
  if (!open_blob(blob, 0x44535745, 1, reader)) return false;
  WeatherSettingsStore decoded{};
  decoded.marker = reader.u32(); decoded.enabled_mask = reader.u8();
  if (!reader.ok() || decoded.marker != WEATHER_SETTINGS_MARKER) return false;
  this->weather_settings_ = decoded;
  return true;
}

bool DynamicSprinkler::save_serialized_schedule_(uint8_t slot, const ScheduleRecord &schedule) {
  if (slot >= MAX_SCHEDULES) return false;
  std::array<uint8_t, SCHEDULE_BLOB_SIZE> blob{};
  StorageWriter writer(blob.data(), blob.size());
  writer.u32(SCHEDULE_BLOB_MAGIC);
  writer.u16(SCHEDULE_FORMAT_VERSION);
  writer.u16(0);
  writer.u32(0);
  writer.u32(schedule.marker);
  writer.u32(schedule.id);
  writer.u32(schedule.revision);
  for (const auto key : schedule.last_run_keys) writer.i64(key);
  writer.bytes(schedule.name, sizeof(schedule.name));
  writer.u8(schedule.enabled);
  writer.u8(schedule.start_time_count);
  for (const auto &trigger : schedule.start_triggers) {
    writer.u8((uint8_t) trigger.type);
    writer.i16(trigger.value);
  }
  writer.u8(schedule.rounds);
  writer.u8(schedule.days_mask);
  writer.u16(schedule.max_wait_minutes);
  writer.i16(schedule.base_temp_deci);
  writer.i16(schedule.temp_adjust_deci);
  writer.u8(schedule.row_count);
  for (const auto &row : schedule.rows) {
    writer.u8(row.zone_count);
    for (uint8_t zone : row.zones) writer.u8(zone);
    writer.u16(row.duration_seconds);
    writer.u16(row.delay_after_seconds);
    writer.i16(row.max_wind_deci);
    writer.i16(row.max_gust_deci);
    writer.i16(row.rain_target_deci);
  }
  if (!writer.ok()) return false;
  const uint16_t payload_length = writer.size() - 12;
  blob[6] = payload_length & 0xFF;
  blob[7] = payload_length >> 8U;
  const uint32_t crc = storage_crc32(blob.data() + 12, payload_length);
  for (uint8_t i = 0; i < 4; i++) blob[8 + i] = crc >> (8U * i);
  return this->serialized_schedule_prefs_[slot].save(&blob);
}

bool DynamicSprinkler::load_serialized_schedule_(uint8_t slot, ScheduleRecord &schedule) {
  if (slot >= MAX_SCHEDULES) return false;
  std::array<uint8_t, SCHEDULE_BLOB_SIZE> blob{};
  if (!this->serialized_schedule_prefs_[slot].load(&blob)) return false;
  StorageReader reader(nullptr, 0);
  if (!open_blob(blob, SCHEDULE_BLOB_MAGIC, SCHEDULE_FORMAT_VERSION, reader)) return false;
  ScheduleRecord decoded{};
  decoded.marker = reader.u32();
  decoded.id = reader.u32();
  decoded.revision = reader.u32();
  for (auto &key : decoded.last_run_keys) key = reader.i64();
  reader.bytes(decoded.name, sizeof(decoded.name));
  decoded.name[sizeof(decoded.name) - 1] = '\0';
  decoded.enabled = reader.u8();
  decoded.start_time_count = reader.u8();
  for (auto &trigger : decoded.start_triggers) {
    trigger.type = (TriggerType) reader.u8();
    trigger.value = reader.i16();
  }
  decoded.rounds = reader.u8();
  decoded.days_mask = reader.u8();
  decoded.max_wait_minutes = reader.u16();
  decoded.base_temp_deci = reader.i16();
  decoded.temp_adjust_deci = reader.i16();
  decoded.row_count = reader.u8();
  for (auto &row : decoded.rows) {
    row.zone_count = reader.u8();
    for (auto &zone : row.zones) zone = reader.u8();
    row.duration_seconds = reader.u16();
    row.delay_after_seconds = reader.u16();
    row.max_wind_deci = reader.i16();
    row.max_gust_deci = reader.i16();
    row.rain_target_deci = reader.i16();
  }
  if (!reader.ok() || decoded.start_time_count > MAX_START_TIMES || decoded.row_count > MAX_ROWS) return false;
  for (uint8_t i = 0; i < decoded.row_count; i++)
    if (decoded.rows[i].zone_count > MAX_ZONES_PER_ROW) return false;
  schedule = decoded;
  return true;
}

uint8_t DynamicSprinkler::weather_capabilities_() const {
  uint8_t capabilities = 0;
  if (this->wind_speed_ != nullptr) capabilities |= WEATHER_WIND;
  if (this->wind_gust_ != nullptr) capabilities |= WEATHER_GUST;
  if (this->rain_24h_ != nullptr) capabilities |= WEATHER_RAIN_24H;
  if (this->rain_rate_ != nullptr) capabilities |= WEATHER_RAIN_RATE;
  if (this->daily_temp_ != nullptr) capabilities |= WEATHER_TEMPERATURE;
  return capabilities;
}

bool DynamicSprinkler::weather_enabled_(uint8_t flag) const {
  return (this->weather_capabilities_() & flag) && (this->weather_settings_.enabled_mask & flag);
}

bool DynamicSprinkler::set_weather_protection_(const std::string &key, bool enabled) {
  uint8_t flag = 0;
  if (key == "wind") flag = WEATHER_WIND;
  else if (key == "gust") flag = WEATHER_GUST;
  else if (key == "rain_24h") flag = WEATHER_RAIN_24H;
  else if (key == "rain_rate") flag = WEATHER_RAIN_RATE;
  else if (key == "temperature") flag = WEATHER_TEMPERATURE;
  if (flag == 0 || !(this->weather_capabilities_() & flag)) return false;
  if (enabled) this->weather_settings_.enabled_mask |= flag;
  else this->weather_settings_.enabled_mask &= ~flag;
  return this->save_weather_settings_();
}

int DynamicSprinkler::zone_index_(uint8_t zone_id) const {
  const auto found = std::find(this->zone_ids_.begin(), this->zone_ids_.end(), zone_id);
  return found == this->zone_ids_.end() ? -1 : std::distance(this->zone_ids_.begin(), found);
}

bool DynamicSprinkler::schedule_is_usable_(const ScheduleRecord &schedule) const {
  if (schedule.marker != SCHEDULE_MARKER || schedule.id == 0 || schedule.start_time_count == 0 ||
      schedule.start_time_count > MAX_START_TIMES || schedule.days_mask == 0 || schedule.days_mask > 0x7F ||
      schedule.rounds == 0 || schedule.rounds > 10 || schedule.max_wait_minutes > 600 || schedule.row_count == 0 ||
      schedule.row_count > MAX_ROWS)
    return false;
  if (schedule.base_temp_deci < -200 || schedule.base_temp_deci > 600 || schedule.temp_adjust_deci < -200 ||
      schedule.temp_adjust_deci > 200)
    return false;
  for (uint8_t i = 0; i < schedule.start_time_count; i++) {
    const auto &trigger = schedule.start_triggers[i];
    if (trigger.type == TriggerType::CLOCK) {
      if (trigger.value < 0 || trigger.value >= 24 * 60) return false;
    } else if (trigger.type == TriggerType::SUNRISE || trigger.type == TriggerType::SUNSET) {
      if (trigger.value < -360 || trigger.value > 360) return false;
    } else {
      return false;
    }
  }
  for (uint8_t i = 0; i < schedule.row_count; i++) {
    const auto &row = schedule.rows[i];
    if (row.zone_count == 0 || row.zone_count > MAX_ZONES_PER_ROW || row.duration_seconds < schedule.rounds ||
        row.duration_seconds > 3600 || row.delay_after_seconds > 7200 || row.max_wind_deci < 0 ||
        row.max_gust_deci < 0 || row.rain_target_deci < 0 || row.max_wind_deci > 1500 || row.max_gust_deci > 1500 ||
        row.rain_target_deci > 1000)
      return false;
    for (uint8_t z = 0; z < row.zone_count; z++) {
      if (this->zone_index_(row.zones[z]) < 0) return false;
      for (uint8_t previous = 0; previous < z; previous++)
        if (row.zones[previous] == row.zones[z]) return false;
    }
  }
  return true;
}

bool DynamicSprinkler::set_zone_name_(uint8_t zone_id, const std::string &name) {
  const int index = this->zone_index_(zone_id);
  if (index < 0 || name.empty() ||
      name.size() >= sizeof(this->zone_name_store_.names[0]))
    return false;
  auto &stored = this->zone_name_store_.names[zone_id - 1];
  std::memset(stored, 0, sizeof(this->zone_name_store_.names[0]));
  std::strncpy(stored, name.c_str(), sizeof(this->zone_name_store_.names[0]) - 1);
  this->zone_names_[index] = stored;
  this->update_zone_button_names_();
  return this->save_serialized_zone_names_();
}

void DynamicSprinkler::update_zone_button_names_() {
  for (size_t i = 0; i < this->zone_buttons_.size(); i++)
    if (this->zone_buttons_[i] != nullptr) this->zone_buttons_[i]->set_dynamic_name(this->zone_names_[i]);
}

uint16_t DynamicSprinkler::quick_manual_seconds() const {
  const float minutes = this->manual_duration_ == nullptr || std::isnan(this->manual_duration_->state)
                            ? 2.0f
                            : this->manual_duration_->state;
  return (uint16_t) lroundf(std::max(1.0f, std::min(60.0f, minutes)) * 60.0f);
}

int DynamicSprinkler::find_schedule_slot_(uint32_t id) const {
  for (uint8_t i = 0; i < MAX_SCHEDULES; i++)
    if (this->schedules_[i].marker == SCHEDULE_MARKER && this->schedules_[i].id == id) return i;
  return -1;
}

int DynamicSprinkler::free_schedule_slot_() const {
  for (uint8_t i = 0; i < MAX_SCHEDULES; i++)
    if (this->schedules_[i].marker != SCHEDULE_MARKER) return i;
  return -1;
}

uint8_t DynamicSprinkler::schedule_count() const {
  uint8_t count = 0;
  for (const auto &schedule : this->schedules_)
    if (schedule.marker == SCHEDULE_MARKER) count++;
  return count;
}

int DynamicSprinkler::schedule_slot_by_ordinal_(uint8_t ordinal) const {
  if (ordinal < 1) return -1;
  uint8_t current = 0;
  for (uint8_t slot = 0; slot < MAX_SCHEDULES; slot++) {
    if (this->schedules_[slot].marker != SCHEDULE_MARKER) continue;
    if (++current == ordinal) return slot;
  }
  return -1;
}

void DynamicSprinkler::select_schedule_ordinal(uint8_t ordinal) {
  this->selected_schedule_ordinal_ = std::max<uint8_t>(1, std::min<uint8_t>(MAX_SCHEDULES, ordinal));
  this->publish_selected_enabled_();
}

void DynamicSprinkler::publish_selected_enabled_() {
  if (this->selected_enabled_switch_ != nullptr)
    this->selected_enabled_switch_->publish_state(this->selected_schedule_enabled());
}

bool DynamicSprinkler::run_selected_schedule() {
  const int slot = this->schedule_slot_by_ordinal_(this->selected_schedule_ordinal_);
  if (slot < 0) {
    this->set_decision_("Selected schedule position is empty");
    return false;
  }
  if (!this->start_schedule_(slot, false)) {
    this->set_decision_("Controller busy; selected schedule not started");
    return false;
  }
  return true;
}

bool DynamicSprinkler::selected_schedule_enabled() const {
  const int slot = this->schedule_slot_by_ordinal_(this->selected_schedule_ordinal_);
  return slot >= 0 && this->schedules_[slot].enabled;
}

void DynamicSprinkler::set_selected_schedule_enabled(bool enabled) {
  const int slot = this->schedule_slot_by_ordinal_(this->selected_schedule_ordinal_);
  if (slot < 0) {
    this->set_decision_("Selected schedule position is empty");
    this->publish_selected_enabled_();
    return;
  }
  if (this->schedules_[slot].enabled == enabled) {
    this->publish_selected_enabled_();
    return;
  }
  const ScheduleRecord previous = this->schedules_[slot];
  this->schedules_[slot].enabled = enabled;
  this->schedules_[slot].revision++;
  this->invalidate_next_schedule_cache_();
  if (!this->save_schedule_(slot) || !this->sync_preferences_()) {
    ESP_LOGE(TAG, "Failed to persist enabled state for schedule %u", this->schedules_[slot].id);
    this->schedules_[slot] = previous;
    if (!this->save_schedule_(slot) || !this->sync_preferences_())
      ESP_LOGE(TAG, "Failed to restore enabled state after persistence error");
    this->invalidate_next_schedule_cache_();
  }
  this->publish_selected_enabled_();
}

std::string DynamicSprinkler::selected_schedule_name() const {
  const int slot = this->schedule_slot_by_ordinal_(this->selected_schedule_ordinal_);
  return slot < 0 ? "No schedule in this position" : this->schedules_[slot].name;
}

std::string DynamicSprinkler::selected_schedule_details() const {
  const int slot = this->schedule_slot_by_ordinal_(this->selected_schedule_ordinal_);
  if (slot < 0) return "Empty";
  const auto &schedule = this->schedules_[slot];
  const std::string additional = schedule.start_time_count > 1
                                     ? str_sprintf(" +%u", schedule.start_time_count - 1)
                                     : "";
  return str_sprintf("%s%s | %u rows x %u | %s", this->trigger_label_(schedule.start_triggers[0]).c_str(),
                     additional.c_str(), schedule.row_count, schedule.rounds,
                     schedule.enabled ? "enabled" : "disabled");
}

bool DynamicSprinkler::resolve_trigger_(const StartTrigger &trigger, ESPTime base_date, ESPTime &target) const {
  if (trigger.type == TriggerType::CLOCK) {
    if (trigger.value < 0 || trigger.value >= 24 * 60) return false;
    target = base_date;
    target.hour = trigger.value / 60;
    target.minute = trigger.value % 60;
    target.second = 0;
    target.recalc_timestamp_local();
    return target.is_valid();
  }
  if (trigger.type != TriggerType::SUNRISE && trigger.type != TriggerType::SUNSET) return false;
  if (this->sun_ == nullptr) return false;
  base_date.hour = 0;
  base_date.minute = 0;
  base_date.second = 0;
  base_date.recalc_timestamp_local();
  if (!base_date.is_valid()) return false;
  optional<ESPTime> event = trigger.type == TriggerType::SUNRISE
                                ? this->sun_->sunrise(base_date, -0.83333)
                                : this->sun_->sunset(base_date, -0.83333);
  if (!event.has_value()) return false;
  target = ESPTime::from_epoch_local(event->timestamp + (int32_t) trigger.value * 60L);
  return target.is_valid();
}

int64_t DynamicSprinkler::trigger_run_key_(const ESPTime &base_date, uint8_t trigger_index) const {
  const int64_t date = (int64_t) base_date.year * 10000LL + base_date.month * 100LL + base_date.day_of_month;
  return date * 100LL + trigger_index;
}

std::string DynamicSprinkler::trigger_label_(const StartTrigger &trigger, const ESPTime *base_date) const {
  if (trigger.type == TriggerType::CLOCK)
    return str_sprintf("%02d:%02d", trigger.value / 60, trigger.value % 60);
  const char *name = trigger.type == TriggerType::SUNRISE ? "Sunrise" : "Sunset";
  std::string label = trigger.value == 0 ? name : str_sprintf("%s %c%dm", name, trigger.value > 0 ? '+' : '-',
                                                              std::abs(trigger.value));
  if (base_date != nullptr) {
    ESPTime resolved{};
    if (this->resolve_trigger_(trigger, *base_date, resolved))
      label += str_sprintf(" (%02u:%02u)", resolved.hour, resolved.minute);
  }
  return label;
}

DynamicSprinkler::NextScheduleInfo DynamicSprinkler::next_schedule_info_() const {
  const uint32_t cache_now_ms = millis();
  if (this->next_schedule_cache_valid_ && cache_now_ms - this->next_schedule_cache_ms_ < 60000UL) {
    auto cached = this->next_schedule_cache_;
    const uint32_t elapsed = (cache_now_ms - this->next_schedule_cache_ms_) / 1000UL;
    cached.seconds = cached.seconds > elapsed ? cached.seconds - elapsed : 0;
    return cached;
  }
  NextScheduleInfo best{};
  if (this->time_ == nullptr) return best;
  const auto now = this->time_->now();
  if (!now.is_valid()) return best;
  for (uint8_t slot = 0; slot < MAX_SCHEDULES; slot++) {
    const auto &schedule = this->schedules_[slot];
    if (!this->schedule_is_usable_(schedule) || !schedule.enabled) continue;
    for (int8_t offset = -1; offset <= 8; offset++) {
      ESPTime base_date = local_date_offset(now, offset);
      if (!base_date.is_valid() || base_date.day_of_week < 1 || base_date.day_of_week > 7) continue;
      if (!(schedule.days_mask & (1U << (base_date.day_of_week - 1)))) continue;
      for (uint8_t trigger_index = 0; trigger_index < schedule.start_time_count; trigger_index++) {
        ESPTime target{};
        if (!this->resolve_trigger_(schedule.start_triggers[trigger_index], base_date, target)) continue;
        if (!target.is_valid() || target.day_of_week < 1 || target.day_of_week > 7) continue;
        const int64_t delta = (int64_t) target.timestamp - now.timestamp;
        if (delta < 0 || schedule.last_run_keys[trigger_index] == this->trigger_run_key_(base_date, trigger_index))
          continue;
        if (best.slot < 0 || (uint32_t) delta < best.seconds) {
          best.slot = slot;
          best.seconds = delta;
          const char *label = "today";
          static const char *const days[] = {"", "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
          const int64_t now_date = (int64_t) now.year * 10000 + now.month * 100 + now.day_of_month;
          const int64_t target_date = (int64_t) target.year * 10000 + target.month * 100 + target.day_of_month;
          ESPTime tomorrow = local_date_offset(now, 1);
          if (!tomorrow.is_valid() || tomorrow.day_of_week < 1 || tomorrow.day_of_week > 7) continue;
          const int64_t tomorrow_date = (int64_t) tomorrow.year * 10000 + tomorrow.month * 100 + tomorrow.day_of_month;
          if (target_date == tomorrow_date) label = "tomorrow";
          else if (target_date != now_date) label = days[target.day_of_week];
          snprintf(best.start, sizeof(best.start), "%s %02u:%02u", label, target.hour, target.minute);
        }
      }
    }
  }
  this->next_schedule_cache_ = best;
  this->next_schedule_cache_ms_ = cache_now_ms;
  this->next_schedule_cache_valid_ = true;
  return best;
}

void DynamicSprinkler::invalidate_next_schedule_cache_() { this->next_schedule_cache_valid_ = false; }

std::string DynamicSprinkler::next_schedule_name() const {
  const auto next = this->next_schedule_info_();
  return next.slot < 0 ? "No enabled schedules" : this->schedules_[next.slot].name;
}

std::string DynamicSprinkler::next_schedule_start() const {
  const auto next = this->next_schedule_info_();
  return next.slot < 0 ? "-" : next.start;
}

uint32_t DynamicSprinkler::next_schedule_seconds() const { return this->next_schedule_info_().seconds; }

void DynamicSprinkler::loop() {
  const uint32_t now_ms = millis();
  if (this->last_loop_call_ms_ != 0)
    this->current_max_loop_ms_ = std::max(this->current_max_loop_ms_, now_ms - this->last_loop_call_ms_);
  this->last_loop_call_ms_ = now_ms;
  if (this->loop_window_started_ms_ == 0) this->loop_window_started_ms_ = now_ms;
  if (now_ms - this->loop_window_started_ms_ >= 30000) {
    this->reported_max_loop_ms_ = this->current_max_loop_ms_;
    this->current_max_loop_ms_ = 0;
    this->loop_window_started_ms_ = now_ms;
  }
  if (now_ms - this->last_evaluation_ms_ >= 30000) {
    this->last_evaluation_ms_ = now_ms;
    this->evaluate_schedules_();
  }
  if (now_ms - this->last_status_ms_ >= 1000) this->publish_status_();
  this->check_safety();

  switch (this->state_) {
    case RunState::WEATHER_WAIT:
      if ((int32_t) (now_ms - this->weather_retry_ms_) >= 0) {
        if (this->active_row_index_ >= this->active_program_.row_count) {
          this->prepare_next_row_();
          break;
        }
        const auto weather = this->prepare_weather_();
        if (weather == WeatherResult::READY) {
          this->begin_row_();
        } else if (weather == WeatherResult::SKIP) {
          this->active_row_index_++;
          this->prepare_next_row_();
        }
      }
      break;
    case RunState::STARTING: {
      if ((int32_t) (now_ms - this->state_deadline_ms_) < 0) break;
      const auto &row = this->active_program_.rows[this->active_row_index_];
      if (this->starting_zone_index_ < row.zone_count) {
        this->turn_zone_on_(row.zones[this->starting_zone_index_++]);
        this->state_deadline_ms_ = now_ms + 250;
      } else {
        this->state_ = RunState::RUNNING;
        this->state_started_ms_ = now_ms;
        this->state_deadline_ms_ = now_ms + this->paused_remaining_ms_;
        this->resuming_ = false;
        this->publish_status_(true);
      }
      break;
    }
    case RunState::RUNNING:
      if ((int32_t) (now_ms - this->state_deadline_ms_) >= 0) this->finish_row_();
      break;
    case RunState::DELAY:
      if ((int32_t) (now_ms - this->state_deadline_ms_) >= 0) {
        this->active_row_index_++;
        this->prepare_next_row_();
      }
      break;
    default:
      break;
  }
}

void DynamicSprinkler::evaluate_schedules_() {
  if (this->is_active() || this->time_ == nullptr) return;
  const auto now = this->time_->now();
  if (!now.is_valid()) return;
  if (now.day_of_week < 1 || now.day_of_week > 7 || now.hour > 23 || now.minute > 59 || now.second > 59) return;
  for (uint8_t i = 0; i < MAX_SCHEDULES; i++) {
    auto &schedule = this->schedules_[i];
    // Records are fully validated when loaded or saved. Keep the periodic
    // path bounded to scalar checks so a corrupt NVS record cannot invoke a
    // vector lookup or other heavier work from the scheduler loop.
    if (schedule.marker != SCHEDULE_MARKER || !schedule.enabled || schedule.id == 0 || schedule.row_count == 0 ||
        schedule.row_count > MAX_ROWS || schedule.start_time_count == 0 || schedule.start_time_count > MAX_START_TIMES ||
        schedule.days_mask == 0 || schedule.days_mask > 0x7F || schedule.rounds == 0 || schedule.rounds > 10)
      continue;
    // Solar offsets may move an event across midnight, and the five-minute
    // grace window may cross midnight for a 23:xx clock trigger. Evaluate the
    // adjacent base dates while keeping weekday selection tied to the base
    // clock/solar event date.
    for (int8_t date_offset = -1; date_offset <= 1; date_offset++) {
      ESPTime base_date = local_date_offset(now, date_offset);
      if (!base_date.is_valid() || base_date.day_of_week < 1 || base_date.day_of_week > 7) continue;
      if (!(schedule.days_mask & (1U << (base_date.day_of_week - 1)))) continue;
      for (uint8_t trigger_index = 0; trigger_index < schedule.start_time_count; trigger_index++) {
        ESPTime target{};
        if (!this->resolve_trigger_(schedule.start_triggers[trigger_index], base_date, target) || !target.is_valid())
          continue;
        const int64_t seconds_late = (int64_t) now.timestamp - (int64_t) target.timestamp;
        if (seconds_late < 0 || seconds_late > TRIGGER_GRACE_SECONDS) continue;
        const int64_t key = this->trigger_run_key_(base_date, trigger_index);
        if (schedule.last_run_keys[trigger_index] == key) continue;
        // Claim and durably persist this occurrence before a relay can start.
        // This prevents reset-within-grace duplicates and keeps independent
        // triggers from overwriting one another's idempotency state.
        const int64_t previous_key = schedule.last_run_keys[trigger_index];
        schedule.last_run_keys[trigger_index] = key;
        if (!this->save_schedule_(i) || !this->sync_preferences_()) {
          schedule.last_run_keys[trigger_index] = previous_key;
          ESP_LOGE(TAG, "Automatic trigger claim could not be persisted for schedule %u", schedule.id);
          continue;
        }
        if (this->start_schedule_(i, true)) {
          this->invalidate_next_schedule_cache_();
          return;
        }
        schedule.last_run_keys[trigger_index] = previous_key;
        if (!this->save_schedule_(i) || !this->sync_preferences_())
          ESP_LOGE(TAG, "Failed to roll back unused trigger claim for schedule %u", schedule.id);
      }
    }
  }
}

bool DynamicSprinkler::start_schedule_(uint8_t slot, bool enforce_window) {
  if (slot >= MAX_SCHEDULES || this->schedules_[slot].marker != SCHEDULE_MARKER) return false;
  return this->start_program_(this->schedules_[slot], false, enforce_window);
}

bool DynamicSprinkler::start_program_(const ScheduleRecord &program, bool manual, bool enforce_window) {
  if (this->is_active() || program.row_count == 0) return false;
  this->active_program_ = program;
  this->active_schedule_id_ = manual ? 0 : program.id;
  this->active_row_index_ = 0;
  this->active_round_index_ = 0;
  this->run_started_ms_ = millis();
  if (++this->run_sequence_ == 0) this->run_sequence_ = 1;
  this->watered_seconds_ = 0;
  this->manual_run_ = manual;
  this->enforce_window_ = enforce_window;
  this->state_ = RunState::WEATHER_WAIT;
  this->set_decision_(manual ? "One-off manual program started" : "Schedule started");
  this->prepare_next_row_();
  return true;
}

void DynamicSprinkler::prepare_next_row_() {
  this->turn_all_off_();
  while (true) {
    if (this->active_row_index_ >= this->active_program_.row_count) {
      if (this->active_round_index_ + 1 >= this->active_program_.rounds) break;
      this->active_round_index_++;
      this->active_row_index_ = 0;
    }
    const auto &row = this->active_program_.rows[this->active_row_index_];
    if (row.zone_count > 0 && row.duration_seconds > 0) {
      this->state_ = RunState::WEATHER_WAIT;
      const auto weather = this->prepare_weather_();
      if (weather == WeatherResult::READY) {
        this->begin_row_();
        this->publish_status_(true);
        return;
      }
      if (weather == WeatherResult::WAIT) {
        this->publish_status_(true);
        return;
      }
      this->active_row_index_++;
      // Bound weather-skip work to one row occurrence per loop turn. A
      // worst-case 32-row/10-round program must not publish 320 decisions in a
      // single call stack or monopolize the ESPHome loop.
      this->weather_retry_ms_ = millis();
      return;
    }
    this->active_row_index_++;
  }
  this->finish_program_("Completed");
}

DynamicSprinkler::WeatherResult DynamicSprinkler::prepare_weather_() {
  auto &row = this->active_program_.rows[this->active_row_index_];
  float adjusted = row.duration_seconds;
  const uint32_t now_ms = millis();
  const bool automatic = !this->manual_run_;
  const uint32_t stale_ms = 30UL * 60UL * 1000UL;
  auto fresh = [&](uint8_t index, sensor::Sensor *source) {
    return source != nullptr && this->weather_updated_ms_[index] != 0 &&
           now_ms - this->weather_updated_ms_[index] <= stale_ms && std::isfinite(source->state) &&
           (index == 4 || source->state >= 0.0f);
  };
  const uint32_t elapsed_ms = now_ms - this->run_started_ms_;
  const uint32_t wait_limit_ms = (uint32_t) this->active_program_.max_wait_minutes * 60000UL;
  const bool window_open = this->enforce_window_ && wait_limit_ms > 0 && elapsed_ms < wait_limit_ms;
  const char *wait_reason = nullptr;
  if (automatic && this->weather_enabled_(WEATHER_RAIN_RATE) && fresh(3, this->rain_rate_) &&
      this->rain_rate_->state > 0.0f)
    wait_reason = "currently raining";
  if (automatic && this->weather_enabled_(WEATHER_WIND) && row.max_wind_deci > 0 &&
      fresh(0, this->wind_speed_) &&
      this->wind_speed_->state > row.max_wind_deci / 10.0f)
    wait_reason = "wind above row limit";
  if (automatic && this->weather_enabled_(WEATHER_GUST) && row.max_gust_deci > 0 &&
      fresh(1, this->wind_gust_) &&
      this->wind_gust_->state > row.max_gust_deci / 10.0f)
    wait_reason = "gust above row limit";
  if (wait_reason != nullptr) {
    if (window_open) {
      this->state_ = RunState::WEATHER_WAIT;
      this->weather_retry_ms_ = now_ms + std::min<uint32_t>(60000UL, wait_limit_ms - elapsed_ms);
      this->set_decision_(std::string("Waiting: ") + wait_reason);
      return WeatherResult::WAIT;
    }
    this->set_decision_(std::string("Row skipped: ") + wait_reason);
    return WeatherResult::SKIP;
  }
  if (automatic && this->weather_enabled_(WEATHER_RAIN_24H) && row.rain_target_deci > 0 &&
      fresh(2, this->rain_24h_)) {
    const float target = row.rain_target_deci / 10.0f;
    if (this->rain_24h_->state >= target) {
      this->set_decision_("Row skipped: 24h rain target reached");
      return WeatherResult::SKIP;
    }
    adjusted *= std::max(0.0f, 1.0f - this->rain_24h_->state / target);
  }
  if (automatic && this->weather_enabled_(WEATHER_TEMPERATURE) &&
      this->active_program_.temp_adjust_deci != 0 && fresh(4, this->daily_temp_)) {
    const float delta = this->daily_temp_->state - this->active_program_.base_temp_deci / 10.0f;
    const float multiplier = 1.0f + delta * (this->active_program_.temp_adjust_deci / 10.0f) / 100.0f;
    adjusted *= std::max(0.0f, std::min(2.0f, multiplier));
  }
  const uint32_t rounds = std::max<uint8_t>(1, this->active_program_.rounds);
  const uint32_t adjusted_ms = (uint32_t) std::max((float) rounds * 1000.0f,
                                                   std::min(3600000.0f, adjusted * 1000.0f));
  this->paused_remaining_ms_ = adjusted_ms / rounds +
                               (this->active_round_index_ < adjusted_ms % rounds ? 1 : 0);
  return WeatherResult::READY;
}

void DynamicSprinkler::begin_row_() {
  this->turn_all_off_();
  this->starting_zone_index_ = 0;
  this->state_ = RunState::STARTING;
  this->state_deadline_ms_ = millis();
  this->set_decision_("Row starting");
}

void DynamicSprinkler::finish_row_() {
  if (this->state_ == RunState::RUNNING) {
    this->watered_seconds_ += (millis() - this->state_started_ms_) / 1000UL;
  }
  this->turn_all_off_();
  const auto &row = this->active_program_.rows[this->active_row_index_];
  const bool more_work = this->active_row_index_ + 1 < this->active_program_.row_count ||
                         this->active_round_index_ + 1 < this->active_program_.rounds;
  if (row.delay_after_seconds > 0 && more_work) {
    this->state_ = RunState::DELAY;
    this->state_started_ms_ = millis();
    this->state_deadline_ms_ = millis() + (uint32_t) row.delay_after_seconds * 1000UL;
    this->set_decision_("Delay before next row");
  } else {
    this->active_row_index_++;
    this->prepare_next_row_();
  }
  this->publish_status_(true);
}

void DynamicSprinkler::finish_program_(const char *result) {
  this->turn_all_off_();
  this->add_history_(result);
  this->state_ = RunState::IDLE;
  this->active_schedule_id_ = 0;
  this->active_row_index_ = 0;
  this->active_round_index_ = 0;
  this->manual_run_ = false;
  this->set_decision_(result);
  this->publish_status_(true);
}

void DynamicSprinkler::turn_all_off_() {
  for (auto *relay : this->relays_)
    if (relay != nullptr && relay->state) relay->turn_off();
}

void DynamicSprinkler::turn_zone_on_(uint8_t zone) {
  const int index = this->zone_index_(zone);
  if (index >= 0 && this->relays_[index] != nullptr) this->relays_[index]->turn_on();
}

bool DynamicSprinkler::relay_expected_(uint8_t zone) const {
  if (this->state_ != RunState::STARTING && this->state_ != RunState::RUNNING) return false;
  const auto &row = this->active_program_.rows[this->active_row_index_];
  for (uint8_t i = 0; i < row.zone_count; i++)
    if (row.zones[i] == zone) return true;
  return false;
}

void DynamicSprinkler::check_safety() {
  uint8_t active = 0;
  for (size_t i = 0; i < this->relays_.size(); i++) {
    if (this->relays_[i] != nullptr && this->relays_[i]->state) {
      active++;
      if (!this->relay_expected_(this->zone_ids_[i])) {
        this->emergency_stop("Emergency stop: unexpected relay active");
        return;
      }
    }
  }
  if (active > MAX_ZONES_PER_ROW) this->emergency_stop("Emergency stop: more than three relays active");
  if (this->state_ == RunState::RUNNING && (int32_t) (millis() - (this->state_deadline_ms_ + 5000)) >= 0)
    this->emergency_stop("Emergency stop: row runtime exceeded");
}

void DynamicSprinkler::emergency_stop(const std::string &reason) {
  if (this->state_ == RunState::RUNNING)
    this->watered_seconds_ += (millis() - this->state_started_ms_) / 1000UL;
  if (this->state_ != RunState::IDLE) this->add_history_(reason.c_str());
  this->turn_all_off_();
  this->state_ = RunState::IDLE;
  this->active_schedule_id_ = 0;
  this->active_round_index_ = 0;
  this->manual_run_ = false;
  this->set_decision_(reason);
  this->publish_status_(true);
}

bool DynamicSprinkler::pause() {
  if (this->state_ != RunState::RUNNING) return false;
  const uint32_t now_ms = millis();
  this->paused_remaining_ms_ = (int32_t) (this->state_deadline_ms_ - now_ms) > 0
                                   ? this->state_deadline_ms_ - now_ms
                                   : 0;
  if (this->paused_remaining_ms_ == 0) return false;
  this->watered_seconds_ += (now_ms - this->state_started_ms_) / 1000UL;
  this->turn_all_off_();
  this->state_ = RunState::PAUSED;
  this->set_decision_("Paused");
  this->publish_status_(true);
  return true;
}

bool DynamicSprinkler::resume() {
  if (this->state_ != RunState::PAUSED || this->paused_remaining_ms_ == 0) return false;
  this->resuming_ = true;
  this->begin_row_();
  this->set_decision_("Resuming");
  return true;
}

bool DynamicSprinkler::skip_row() {
  if (this->state_ == RunState::IDLE) return false;
  if (this->state_ == RunState::RUNNING)
    this->watered_seconds_ += (millis() - this->state_started_ms_) / 1000UL;
  this->turn_all_off_();
  this->active_row_index_++;
  this->set_decision_("Row skipped manually");
  this->prepare_next_row_();
  return true;
}

bool DynamicSprinkler::start_single_zone(uint8_t zone, uint16_t seconds) {
  if (this->zone_index_(zone) < 0) return false;
  ScheduleRecord program{};
  program.marker = SCHEDULE_MARKER;
  std::strncpy(program.name, "Quick manual run", sizeof(program.name) - 1);
  program.row_count = 1;
  program.rows[0].zone_count = 1;
  program.rows[0].zones[0] = zone;
  program.rows[0].duration_seconds = std::max<uint16_t>(1, std::min<uint16_t>(3600, seconds));
  return this->start_program_(program, true, false);
}

uint32_t DynamicSprinkler::remaining_seconds() const {
  if (this->state_ == RunState::PAUSED) return (this->paused_remaining_ms_ + 999UL) / 1000UL;
  if ((this->state_ == RunState::RUNNING || this->state_ == RunState::DELAY) &&
      (int32_t) (this->state_deadline_ms_ - millis()) > 0)
    return (this->state_deadline_ms_ - millis() + 999UL) / 1000UL;
  return 0;
}

uint32_t DynamicSprinkler::elapsed_seconds() const {
  return this->is_active() ? (millis() - this->run_started_ms_) / 1000UL : 0;
}

void DynamicSprinkler::publish_status_(bool force) {
  const uint32_t now_ms = millis();
  if (!force && now_ms - this->last_status_ms_ < 900) return;
  this->last_status_ms_ = now_ms;
  std::string text = "Idle";
  if (this->state_ == RunState::PAUSED) text = "Paused";
  else if (this->state_ == RunState::WEATHER_WAIT) text = "Waiting for weather";
  else if (this->state_ == RunState::DELAY) text = "Delay before next row";
  else if (this->state_ == RunState::STARTING || this->state_ == RunState::RUNNING) {
    char buffer[96];
    snprintf(buffer, sizeof(buffer), "%s round %u/%u row %u", this->manual_run_ ? "Manual" : "Schedule",
             this->active_round_index_ + 1, this->active_program_.rounds, this->active_row_index_ + 1);
    text = buffer;
  }
  const uint32_t remaining = this->remaining_seconds();
  if (this->status_text_ != nullptr && (force || text != this->published_status_)) {
    this->status_text_->publish_state(text);
    this->published_status_ = text;
  }
  if (this->remaining_sensor_ != nullptr && (force || remaining != this->published_remaining_seconds_)) {
    this->remaining_sensor_->publish_state(remaining);
    this->published_remaining_seconds_ = remaining;
  }
}

void DynamicSprinkler::set_decision_(const std::string &message) {
  ESP_LOGI(TAG, "%s", message.c_str());
  if (this->decision_text_ != nullptr) this->decision_text_->publish_state(message);
}

void DynamicSprinkler::add_history_(const char *result) {
  const uint8_t slot = this->meta_.history_head % HISTORY_SIZE;
  this->meta_.history_head = (this->meta_.history_head + 1) % HISTORY_SIZE;
  auto &entry = this->history_[slot];
  entry = {};
  entry.marker = HISTORY_MARKER;
  entry.run_id = this->meta_.next_run_id++;
  entry.schedule_id = this->active_schedule_id_;
  auto now = this->time_->now();
  entry.ended_at = now.is_valid() ? now.timestamp : 0;
  entry.started_at = entry.ended_at > this->elapsed_seconds() ? entry.ended_at - this->elapsed_seconds() : 0;
  entry.watered_seconds = this->watered_seconds_;
  entry.manual = this->manual_run_;
  std::strncpy(entry.name, this->active_program_.name, sizeof(entry.name) - 1);
  std::strncpy(entry.result, result, sizeof(entry.result) - 1);
  bool queued = this->save_history_(slot);
  queued = this->save_meta_() && queued;
  const bool persisted = this->sync_preferences_();
  if (!queued || !persisted)
    ESP_LOGE(TAG, "Failed to persist completed run %u", entry.run_id);
}

bool DynamicSprinkler::parse_program_(const std::string &data, ScheduleRecord &out, bool require_identity,
                                     std::string &error) {
  out = ScheduleRecord{};
  out.marker = SCHEDULE_MARKER;
  bool ok = json::parse_json(data, [&](JsonObject root) {
    out.id = root["id"] | 0U;
    out.revision = root["revision"] | 0U;
    const char *name = root["name"] | (require_identity ? "Schedule" : "Manual run");
    if (name == nullptr || name[0] == '\0' || std::strlen(name) >= sizeof(out.name)) {
      error = "program name must be 1-31 UTF-8 bytes";
      return false;
    }
    std::strncpy(out.name, name, sizeof(out.name) - 1);
    out.enabled = root["enabled"] | false;
    const int days_mask = root["days_mask"] | 0x7F;
    const int max_wait_minutes = root["max_wait_minutes"] | 240;
    const int rounds = root["rounds"] | 1;
    const float base_temp = root["base_temp"] | 22.0f;
    const float temp_adjust = root["temp_adjust"] | 0.0f;
    if (days_mask <= 0 || days_mask > 0x7F || max_wait_minutes < 0 || max_wait_minutes > 600 ||
        rounds < 1 || rounds > 10 || !std::isfinite(base_temp) ||
        base_temp < -20.0f || base_temp > 60.0f || !std::isfinite(temp_adjust) || temp_adjust < -20.0f ||
        temp_adjust > 20.0f) {
      error = "schedule settings are out of range";
      return false;
    }
    JsonArray start_triggers = root["start_triggers"].as<JsonArray>();
    if (start_triggers.size() > MAX_START_TIMES) {
      error = "maximum 8 start triggers exceeded";
      return false;
    }
    out.start_time_count = 0;
    for (JsonObject source : start_triggers) {
      const std::string type = source["type"] | "clock";
      StartTrigger trigger{};
      if (type == "clock") {
        trigger.type = TriggerType::CLOCK;
        trigger.value = source["minute"] | 360;
        if (trigger.value < 0 || trigger.value >= 24 * 60) {
          error = "clock trigger must be between 00:00 and 23:59";
          return false;
        }
      } else if (type == "sunrise" || type == "sunset") {
        if (this->sun_ == nullptr) {
          error = "solar triggers require sun coordinates";
          return false;
        }
        trigger.type = type == "sunrise" ? TriggerType::SUNRISE : TriggerType::SUNSET;
        trigger.value = source["offset"] | 0;
        if (trigger.value < -360 || trigger.value > 360) {
          error = "solar offset must be between -360 and 360 minutes";
          return false;
        }
      } else {
        error = "unknown start trigger type";
        return false;
      }
      for (uint8_t i = 0; i < out.start_time_count; i++) {
        if (out.start_triggers[i].type == trigger.type && out.start_triggers[i].value == trigger.value) {
          error = "duplicate schedule start trigger";
          return false;
        }
      }
      out.start_triggers[out.start_time_count++] = trigger;
    }
    if (out.start_time_count == 0) {
      JsonArray start_times = root["start_times"].as<JsonArray>();
      for (JsonVariant value : start_times) {
        const int minute = value.as<int>();
        if (minute < 0 || minute >= 24 * 60 || out.start_time_count >= MAX_START_TIMES) {
          error = "start time must be between 00:00 and 23:59";
          return false;
        }
        out.start_triggers[out.start_time_count++] = {TriggerType::CLOCK, (int16_t) minute};
      }
    }
    if (out.start_time_count == 0) {
      const int start_hour = root["start_hour"] | 6;
      const int start_minute = root["start_minute"] | 0;
      if (start_hour < 0 || start_hour > 23 || start_minute < 0 || start_minute > 59) return false;
      out.start_time_count = 1;
      out.start_triggers[0] = {TriggerType::CLOCK, (int16_t) (start_hour * 60 + start_minute)};
    }
    out.rounds = rounds;
    out.days_mask = days_mask;
    out.max_wait_minutes = max_wait_minutes;
    out.base_temp_deci = (int16_t) lroundf(base_temp * 10.0f);
    out.temp_adjust_deci = (int16_t) lroundf(temp_adjust * 10.0f);
    JsonArray rows = root["rows"].as<JsonArray>();
    if (rows.size() > MAX_ROWS) {
      error = "maximum 32 rows exceeded";
      return false;
    }
    for (JsonObject source : rows) {
      auto &row = out.rows[out.row_count];
      JsonArray zones = source["zones"].as<JsonArray>();
      if (zones.size() < 1 || zones.size() > MAX_ZONES_PER_ROW) {
        error = "each row requires one to three zones";
        return false;
      }
      for (JsonVariant value : zones) {
        const int zone = value.as<int>();
        if (zone < 1 || zone > MAX_ZONES || this->zone_index_(zone) < 0) {
          error = "row references an unavailable zone_id";
          return false;
        }
        for (uint8_t i = 0; i < row.zone_count; i++) {
          if (row.zones[i] == zone) {
            error = "duplicate zone in row";
            return false;
          }
        }
        row.zones[row.zone_count++] = zone;
      }
      const int duration_seconds = source["duration_seconds"] | 60;
      const int delay_after_seconds = source["delay_after_seconds"] | 0;
      const float max_wind = source["max_wind"] | 0.0f;
      const float max_gust = source["max_gust"] | 0.0f;
      const float rain_target = source["rain_target"] | 0.0f;
      if (duration_seconds < rounds || duration_seconds > 3600 || delay_after_seconds < 0 ||
          delay_after_seconds > 7200 || !std::isfinite(max_wind) || max_wind < 0.0f || max_wind > 150.0f ||
          !std::isfinite(max_gust) || max_gust < 0.0f || max_gust > 150.0f || !std::isfinite(rain_target) ||
          rain_target < 0.0f || rain_target > 100.0f) {
        error = "row duration must allow at least one second per round; delay or weather limit is out of range";
        return false;
      }
      row.duration_seconds = duration_seconds;
      row.delay_after_seconds = delay_after_seconds;
      row.max_wind_deci = (int16_t) lroundf(max_wind * 10.0f);
      row.max_gust_deci = (int16_t) lroundf(max_gust * 10.0f);
      row.rain_target_deci = (int16_t) lroundf(rain_target * 10.0f);
      out.row_count++;
    }
    return out.row_count > 0;
  });
  if (!ok && error.empty()) error = "invalid program JSON or no rows";
  return ok;
}

std::string DynamicSprinkler::schedules_json_() const {
  return json::build_json([&](JsonObject root) {
    root["max_schedules"] = MAX_SCHEDULES;
    root["max_rows"] = MAX_ROWS;
    JsonArray items = root["schedules"].to<JsonArray>();
    for (const auto &schedule : this->schedules_) {
      if (!this->schedule_is_usable_(schedule)) continue;
      JsonObject item = items.add<JsonObject>();
      item["id"] = schedule.id;
      item["revision"] = schedule.revision;
      item["name"] = schedule.name;
      item["enabled"] = schedule.enabled != 0;
      const std::string additional = schedule.start_time_count > 1
                                         ? str_sprintf(" +%u", schedule.start_time_count - 1)
                                         : "";
      item["start"] = this->trigger_label_(schedule.start_triggers[0]) + additional;
      item["time_count"] = schedule.start_time_count;
      item["row_count"] = schedule.row_count;
      item["rounds"] = schedule.rounds;
    }
  });
}

std::string DynamicSprinkler::schedule_json_(const ScheduleRecord &schedule) const {
  return json::build_json([&](JsonObject root) {
    if (!this->schedule_is_usable_(schedule)) return;
    root["id"] = schedule.id;
    root["revision"] = schedule.revision;
    root["name"] = schedule.name;
    root["enabled"] = schedule.enabled != 0;
    JsonArray start_triggers = root["start_triggers"].to<JsonArray>();
    ESPTime today = this->time_ == nullptr ? ESPTime{} : this->time_->now();
    for (uint8_t i = 0; i < schedule.start_time_count; i++) {
      const auto &source = schedule.start_triggers[i];
      JsonObject trigger = start_triggers.add<JsonObject>();
      if (source.type == TriggerType::CLOCK) {
        trigger["type"] = "clock";
        trigger["minute"] = source.value;
      } else {
        trigger["type"] = source.type == TriggerType::SUNRISE ? "sunrise" : "sunset";
        trigger["offset"] = source.value;
        ESPTime resolved{};
        if (today.is_valid() && this->resolve_trigger_(source, today, resolved))
          trigger["resolved"] = str_sprintf("%02u:%02u", resolved.hour, resolved.minute);
        else trigger["resolved"] = nullptr;
      }
    }
    root["rounds"] = schedule.rounds;
    root["days_mask"] = schedule.days_mask;
    root["max_wait_minutes"] = schedule.max_wait_minutes;
    root["base_temp"] = schedule.base_temp_deci / 10.0f;
    root["temp_adjust"] = schedule.temp_adjust_deci / 10.0f;
    JsonArray rows = root["rows"].to<JsonArray>();
    for (uint8_t i = 0; i < schedule.row_count; i++) {
      const auto &source = schedule.rows[i];
      JsonObject row = rows.add<JsonObject>();
      JsonArray zones = row["zones"].to<JsonArray>();
      for (uint8_t z = 0; z < source.zone_count; z++) zones.add(source.zones[z]);
      row["duration_seconds"] = source.duration_seconds;
      row["delay_after_seconds"] = source.delay_after_seconds;
      row["max_wind"] = source.max_wind_deci / 10.0f;
      row["max_gust"] = source.max_gust_deci / 10.0f;
      row["rain_target"] = source.rain_target_deci / 10.0f;
    }
  });
}

std::string DynamicSprinkler::status_json_() const {
  return json::build_json([&](JsonObject root) {
    const uint32_t status_now_ms = millis();
    const char *state = "idle";
    if (this->state_ == RunState::WEATHER_WAIT) state = "weather_wait";
    else if (this->state_ == RunState::STARTING) state = "starting";
    else if (this->state_ == RunState::RUNNING) state = "running";
    else if (this->state_ == RunState::DELAY) state = "delay";
    else if (this->state_ == RunState::PAUSED) state = "paused";
    root["state"] = state;
    root["active"] = this->is_active();
    root["manual"] = this->manual_run_;
    root["schedule_id"] = this->active_schedule_id_;
    root["name"] = this->is_active() ? this->active_program_.name : "";
    root["row"] = this->is_active()
                      ? std::min<uint8_t>(this->active_row_index_ + 1, this->active_program_.row_count)
                      : 0;
    root["row_count"] = this->is_active() ? this->active_program_.row_count : 0;
    root["round"] = this->is_active() ? this->active_round_index_ + 1 : 0;
    root["round_count"] = this->is_active() ? this->active_program_.rounds : 0;
    root["run_sequence"] = this->is_active() ? this->run_sequence_ : 0;
    uint32_t remaining_ms = 0;
    if (this->state_ == RunState::PAUSED) {
      remaining_ms = this->paused_remaining_ms_;
    } else if ((this->state_ == RunState::RUNNING || this->state_ == RunState::DELAY) &&
               (int32_t) (this->state_deadline_ms_ - status_now_ms) > 0) {
      remaining_ms = this->state_deadline_ms_ - status_now_ms;
    }
    const uint32_t elapsed_ms = this->is_active() ? status_now_ms - this->run_started_ms_ : 0;
    root["remaining_ms"] = remaining_ms;
    root["remaining_seconds"] = (remaining_ms + 999UL) / 1000UL;
    root["elapsed_ms"] = elapsed_ms;
    root["elapsed_seconds"] = elapsed_ms / 1000UL;
    const int8_t wifi_rssi = wifi::global_wifi_component == nullptr
                                 ? wifi::WIFI_RSSI_DISCONNECTED
                                 : wifi::global_wifi_component->wifi_rssi();
    if (wifi_rssi == wifi::WIFI_RSSI_DISCONNECTED) root["wifi_rssi"] = nullptr;
    else root["wifi_rssi"] = wifi_rssi;
    JsonArray zones = root["zones"].to<JsonArray>();
    if ((this->state_ == RunState::STARTING || this->state_ == RunState::RUNNING) &&
        this->active_row_index_ < this->active_program_.row_count) {
      const auto &row = this->active_program_.rows[this->active_row_index_];
      for (uint8_t i = 0; i < row.zone_count; i++) zones.add(row.zones[i]);
    }
    JsonArray zone_names = root["zone_names"].to<JsonArray>();
    for (const auto &name : this->zone_names_) zone_names.add(name);
    JsonArray configured_zones = root["configured_zones"].to<JsonArray>();
    for (size_t i = 0; i < this->zone_names_.size(); i++) {
      JsonObject zone = configured_zones.add<JsonObject>();
      zone["id"] = this->zone_ids_[i];
      zone["name"] = this->zone_names_[i];
    }
    const auto next = this->next_schedule_info_();
    JsonObject next_json = root["next"].to<JsonObject>();
    next_json["id"] = next.slot < 0 ? 0 : this->schedules_[next.slot].id;
    next_json["name"] = next.slot < 0 ? "" : this->schedules_[next.slot].name;
    next_json["seconds"] = next.seconds;
    next_json["start"] = next.slot < 0 ? "" : next.start;
    JsonObject weather = root["weather"].to<JsonObject>();
    JsonObject capabilities = weather["capabilities"].to<JsonObject>();
    JsonObject enabled = weather["enabled"].to<JsonObject>();
    auto add_weather = [&](const char *key, uint8_t index, uint8_t flag, sensor::Sensor *source) {
      capabilities[key] = source != nullptr;
      enabled[key] = this->weather_enabled_(flag);
      JsonObject item = weather[key].to<JsonObject>();
      item["configured"] = source != nullptr;
      if (source == nullptr || std::isnan(source->state)) item["value"] = nullptr;
      else item["value"] = source->state;
      if (this->weather_updated_ms_[index] == 0) item["age_seconds"] = nullptr;
      else item["age_seconds"] = (millis() - this->weather_updated_ms_[index]) / 1000UL;
    };
    add_weather("wind", 0, WEATHER_WIND, this->wind_speed_);
    add_weather("gust", 1, WEATHER_GUST, this->wind_gust_);
    add_weather("rain_24h", 2, WEATHER_RAIN_24H, this->rain_24h_);
    add_weather("rain_rate", 3, WEATHER_RAIN_RATE, this->rain_rate_);
    add_weather("temperature", 4, WEATHER_TEMPERATURE, this->daily_temp_);
    root["solar_available"] = this->sun_ != nullptr;
  });
}

std::string DynamicSprinkler::system_json_() const {
  return json::build_json([&](JsonObject root) {
    root["max_loop_ms"] = this->reported_max_loop_ms_;
    root["free_heap"] = heap_caps_get_free_size(MALLOC_CAP_8BIT);
    root["largest_heap_block"] = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
    root["uptime_seconds"] = millis() / 1000UL;
    root["timezone"] = this->timezone_select_ == nullptr ? "" : this->timezone_select_->current_option().str();
    root["manual_duration_minutes"] = this->manual_duration_ == nullptr || std::isnan(this->manual_duration_->state)
                                           ? 2.0f
                                           : this->manual_duration_->state;
    root["solar_available"] = this->sun_ != nullptr;
    JsonObject weather = root["weather"].to<JsonObject>();
    JsonObject capabilities = weather["capabilities"].to<JsonObject>();
    JsonObject enabled = weather["enabled"].to<JsonObject>();
    capabilities["wind"] = this->wind_speed_ != nullptr;
    capabilities["gust"] = this->wind_gust_ != nullptr;
    capabilities["rain_24h"] = this->rain_24h_ != nullptr;
    capabilities["rain_rate"] = this->rain_rate_ != nullptr;
    capabilities["temperature"] = this->daily_temp_ != nullptr;
    enabled["wind"] = this->weather_enabled_(WEATHER_WIND);
    enabled["gust"] = this->weather_enabled_(WEATHER_GUST);
    enabled["rain_24h"] = this->weather_enabled_(WEATHER_RAIN_24H);
    enabled["rain_rate"] = this->weather_enabled_(WEATHER_RAIN_RATE);
    enabled["temperature"] = this->weather_enabled_(WEATHER_TEMPERATURE);
    JsonArray zone_names = root["zone_names"].to<JsonArray>();
    for (const auto &name : this->zone_names_) zone_names.add(name);
    JsonArray configured_zones = root["configured_zones"].to<JsonArray>();
    for (size_t i = 0; i < this->zone_names_.size(); i++) {
      JsonObject zone = configured_zones.add<JsonObject>();
      zone["id"] = this->zone_ids_[i];
      zone["name"] = this->zone_names_[i];
    }
    auto now = this->time_ == nullptr ? ESPTime{} : this->time_->now();
    if (!now.is_valid()) root["device_time"] = "Time unavailable";
    else root["device_time"] = str_sprintf("%04d-%02d-%02d %02d:%02d:%02d", now.year, now.month,
                                           now.day_of_month, now.hour, now.minute, now.second);
  });
}

std::string DynamicSprinkler::history_json_() const {
  return json::build_json([&](JsonObject root) {
    JsonArray items = root["history"].to<JsonArray>();
    for (uint8_t offset = 0; offset < HISTORY_SIZE; offset++) {
      const uint8_t slot = (this->meta_.history_head + HISTORY_SIZE - 1 - offset) % HISTORY_SIZE;
      const auto &entry = this->history_[slot];
      if (entry.marker != HISTORY_MARKER) continue;
      JsonObject item = items.add<JsonObject>();
      item["id"] = entry.run_id;
      item["schedule_id"] = entry.schedule_id;
      item["name"] = entry.name;
      item["manual"] = entry.manual != 0;
      item["started_at"] = entry.started_at;
      item["ended_at"] = entry.ended_at;
      item["watered_seconds"] = entry.watered_seconds;
      item["result"] = entry.result;
    }
  });
}

bool DynamicSprinkler::canHandle(AsyncWebServerRequest *request) const {
  char buffer[AsyncWebServerRequest::URL_BUF_SIZE];
  const auto url = request->url_to(buffer);
  static constexpr char prefix[] = "/sprinkler/api/";
  return url.size() >= sizeof(prefix) - 1 && std::equal(prefix, prefix + sizeof(prefix) - 1, url.begin());
}

void DynamicSprinkler::handleRequest(AsyncWebServerRequest *request) {
  char buffer[AsyncWebServerRequest::URL_BUF_SIZE];
  if (request->method() == HTTP_POST) {
    const auto marker = request->get_header("X-Sprinkler-Request");
    if (!marker.has_value() || *marker != "1") {
      // ESPHome's ESP-IDF response wrapper currently maps unsupported status
      // codes (including 403) to 500. Use its supported conflict response so
      // rejected cross-site form posts are reported as a client error.
      this->send_error_(request, 409, "missing sprinkler request marker");
      return;
    }
  }
  auto job = std::make_shared<WebRequestJob>();
  job->post = request->method() == HTTP_POST;
  job->path = request->url_to(buffer);
  job->id = request->arg("id");
  job->data = request->arg("data");
  job->action = request->arg("action");
  job->setting = request->arg("setting");
  job->value = request->arg("value");
  job->index = request->arg("index");
  job->key = request->arg("key");
  job->type = request->arg("type");
  job->offset = request->arg("offset");

#ifdef USE_ESP32
  if (job->done == nullptr) {
    this->send_error_(request, 500, "unable to allocate request synchronization");
    return;
  }
  // ESP-IDF invokes custom handlers in its HTTP task. All ESPHome component,
  // entity, relay and preference access belongs to the main loop, so marshal
  // the complete operation there and keep only the response send in this task.
  this->defer([this, job]() {
    uint8_t queued = 0;
    if (job->state.compare_exchange_strong(queued, 1)) this->process_web_request_(*job);
    xSemaphoreGive(job->done);
  });
  if (xSemaphoreTake(job->done, pdMS_TO_TICKS(4000)) != pdTRUE) {
    uint8_t queued = 0;
    if (job->state.compare_exchange_strong(queued, 2)) {
      this->send_error_(request, 500, "controller main loop did not accept request in time");
      return;
    }
    // Processing already began in the main loop. Do not report a false
    // failure while that mutation can still complete; POST clients allow a
    // matching 15-second timeout.
    if (xSemaphoreTake(job->done, pdMS_TO_TICKS(10000)) != pdTRUE) {
      this->send_error_(request, 500, "controller operation did not finish in time");
      return;
    }
  }
#else
  this->process_web_request_(*job);
#endif
  this->send_json_(request, job->response_code, job->response_body);
}

void DynamicSprinkler::process_web_request_(WebRequestJob &job) {
  auto send_json = [&](int code, std::string body) {
    job.response_code = code;
    job.response_body = std::move(body);
  };
  auto send_error = [&](int code, const std::string &message) {
    send_json(code, json::build_json([&](JsonObject root) { root["error"] = message; }));
  };
  auto parse_u32 = [](const std::string &text, uint32_t &value) {
    char *end = nullptr;
    const unsigned long long parsed = strtoull(text.c_str(), &end, 10);
    if (end == text.c_str() || *end != '\0' || parsed == 0 || parsed > UINT32_MAX) return false;
    value = (uint32_t) parsed;
    return true;
  };

  if (!job.post && job.path == "/sprinkler/api/schedules") {
    send_json(200, this->schedules_json_());
    return;
  }
  if (!job.post && job.path == "/sprinkler/api/schedule") {
    uint32_t id = 0;
    if (!parse_u32(job.id, id)) {
      send_error(409, "invalid schedule id");
      return;
    }
    const int slot = this->find_schedule_slot_(id);
    if (slot < 0) send_error(404, "schedule not found");
    else send_json(200, this->schedule_json_(this->schedules_[slot]));
    return;
  }
  if (!job.post && job.path == "/sprinkler/api/status") {
    send_json(200, this->status_json_());
    return;
  }
  if (!job.post && job.path == "/sprinkler/api/history") {
    send_json(200, this->history_json_());
    return;
  }
  if (!job.post && job.path == "/sprinkler/api/system") {
    send_json(200, this->system_json_());
    return;
  }
  if (!job.post && job.path == "/sprinkler/api/solar") {
    TriggerType type;
    if (job.type == "sunrise") type = TriggerType::SUNRISE;
    else if (job.type == "sunset") type = TriggerType::SUNSET;
    else {
      send_error(409, "solar type must be sunrise or sunset");
      return;
    }
    char *end = nullptr;
    const long offset = strtol(job.offset.c_str(), &end, 10);
    if (end == job.offset.c_str() || *end != '\0' || offset < -360 || offset > 360) {
      send_error(409, "solar offset must be between -360 and 360 minutes");
      return;
    }
    ESPTime base = this->time_ == nullptr ? ESPTime{} : this->time_->now();
    ESPTime resolved{};
    const StartTrigger trigger{type, (int16_t) offset};
    send_json(200, json::build_json([&](JsonObject root) {
      if (base.is_valid() && this->resolve_trigger_(trigger, base, resolved))
        root["resolved"] = str_sprintf("%02u:%02u", resolved.hour, resolved.minute);
      else
        root["resolved"] = nullptr;
    }));
    return;
  }
  if (!job.post) {
    send_error(409, "method not allowed");
    return;
  }
  if (job.path == "/sprinkler/api/save") {
    ScheduleRecord incoming{};
    std::string error;
    if (!this->parse_program_(job.data, incoming, true, error)) {
      send_error(409, error);
      return;
    }
    int slot = incoming.id ? this->find_schedule_slot_(incoming.id) : this->free_schedule_slot_();
    if (slot < 0) {
      send_error(incoming.id ? 404 : 409, incoming.id ? "schedule not found" : "schedule capacity reached");
      return;
    }
    const ScheduleRecord previous = this->schedules_[slot];
    const StoreMeta previous_meta = this->meta_;
    if (incoming.id == 0) incoming.id = this->meta_.next_schedule_id++;
    else if (incoming.revision != this->schedules_[slot].revision) {
      send_error(409, "schedule changed; reload before saving");
      return;
    }
    incoming.revision++;
    for (uint8_t next_index = 0; next_index < incoming.start_time_count; next_index++) {
      for (uint8_t previous_index = 0; previous_index < previous.start_time_count; previous_index++) {
        if (incoming.start_triggers[next_index].type == previous.start_triggers[previous_index].type &&
            incoming.start_triggers[next_index].value == previous.start_triggers[previous_index].value) {
          const int64_t previous_key = previous.last_run_keys[previous_index];
          incoming.last_run_keys[next_index] =
              previous_key < 0 ? -1 : (previous_key / 100LL) * 100LL + next_index;
          break;
        }
      }
    }
    this->schedules_[slot] = incoming;
    this->invalidate_next_schedule_cache_();
    bool queued = this->save_schedule_(slot);
    queued = this->save_meta_() && queued;
    const bool persisted = this->sync_preferences_();
    if (!queued || !persisted) {
      this->schedules_[slot] = previous;
      this->meta_ = previous_meta;
      const bool rollback_queued = this->save_schedule_(slot) && this->save_meta_();
      if (!rollback_queued || !this->sync_preferences_())
        ESP_LOGE(TAG, "Failed to restore schedule state after persistence error");
      this->invalidate_next_schedule_cache_();
      this->publish_selected_enabled_();
      send_error(500, "schedule could not be persisted");
      return;
    }
    this->publish_selected_enabled_();
    send_json(200, this->schedule_json_(incoming));
    return;
  }
  if (job.path == "/sprinkler/api/validate") {
    ScheduleRecord incoming{};
    std::string error;
    if (!this->parse_program_(job.data, incoming, true, error)) send_error(409, error);
    else send_json(200, "{\"ok\":true}");
    return;
  }
  if (job.path == "/sprinkler/api/delete") {
    uint32_t id = 0;
    if (!parse_u32(job.id, id)) {
      send_error(409, "invalid schedule id");
      return;
    }
    const int slot = this->find_schedule_slot_(id);
    if (slot < 0) send_error(404, "schedule not found");
    else {
      const ScheduleRecord previous = this->schedules_[slot];
      this->schedules_[slot] = ScheduleRecord{};
      this->invalidate_next_schedule_cache_();
      if (!this->save_schedule_(slot) || !this->sync_preferences_()) {
        this->schedules_[slot] = previous;
        if (!this->save_schedule_(slot) || !this->sync_preferences_())
          ESP_LOGE(TAG, "Failed to restore deleted schedule after persistence error");
        this->invalidate_next_schedule_cache_();
        this->publish_selected_enabled_();
        send_error(500, "schedule deletion could not be persisted");
        return;
      }
      this->publish_selected_enabled_();
      send_json(200, "{\"ok\":true}");
    }
    return;
  }
  if (job.path == "/sprinkler/api/run") {
    uint32_t id = 0;
    if (!parse_u32(job.id, id)) {
      send_error(409, "invalid schedule id");
      return;
    }
    const int slot = this->find_schedule_slot_(id);
    if (slot < 0) send_error(404, "schedule not found");
    else if (!this->start_schedule_(slot, false)) send_error(409, "controller is busy");
    else send_json(200, "{\"ok\":true}");
    return;
  }
  if (job.path == "/sprinkler/api/manual") {
    ScheduleRecord program{};
    std::string error;
    if (!this->parse_program_(job.data, program, false, error)) send_error(409, error);
    else if (!this->start_program_(program, true, false)) send_error(409, "controller is busy");
    else send_json(200, "{\"ok\":true}");
    return;
  }
  if (job.path == "/sprinkler/api/control") {
    bool ok = false;
    if (job.action == "pause") ok = this->pause();
    else if (job.action == "resume") ok = this->resume();
    else if (job.action == "skip") ok = this->skip_row();
    else if (job.action == "stop") {
      this->emergency_stop("Manual emergency stop");
      ok = true;
    }
    if (ok) send_json(200, "{\"ok\":true}");
    else send_error(409, "action is not available");
    return;
  }
  if (job.path == "/sprinkler/api/settings") {
    const auto previous_zone_store = this->zone_name_store_;
    const auto previous_zone_names = this->zone_names_;
    const auto previous_weather_settings = this->weather_settings_;
    const float previous_manual_duration = this->manual_duration_ == nullptr ? NAN : this->manual_duration_->state;
    const std::string previous_timezone =
        this->timezone_select_ == nullptr ? "" : this->timezone_select_->current_option().str();
    auto restore_settings = [&]() {
      this->zone_name_store_ = previous_zone_store;
      this->zone_names_ = previous_zone_names;
      this->weather_settings_ = previous_weather_settings;
      this->update_zone_button_names_();
      bool queued = this->save_serialized_zone_names_();
      queued = this->save_weather_settings_() && queued;
      if (this->manual_duration_ != nullptr && std::isfinite(previous_manual_duration) &&
          this->manual_duration_->state != previous_manual_duration) {
        auto call = this->manual_duration_->make_call();
        call.set_value(previous_manual_duration);
        call.perform();
      }
      if (this->timezone_select_ != nullptr && !previous_timezone.empty() &&
          this->timezone_select_->current_option().str() != previous_timezone) {
        auto call = this->timezone_select_->make_call();
        call.set_option(previous_timezone);
        call.perform();
      }
      this->invalidate_next_schedule_cache_();
      if (!queued || !this->sync_preferences_()) ESP_LOGE(TAG, "Failed to restore settings after persistence error");
    };
    if (job.setting == "zone_name") {
      uint32_t zone_value = 0;
      if (!parse_u32(job.index, zone_value) || zone_value > MAX_ZONES) {
        send_error(409, "invalid zone id");
        return;
      }
      const uint8_t zone = (uint8_t) zone_value;
      if (this->zone_index_(zone) < 0 || job.value.empty() ||
          job.value.size() >= sizeof(this->zone_name_store_.names[0])) {
        send_error(409, "zone name must be 1-31 UTF-8 bytes");
        return;
      }
      if (!this->set_zone_name_(zone, job.value)) {
        restore_settings();
        send_error(500, "zone name could not be queued");
        return;
      }
    } else if (job.setting == "weather_protection") {
      if ((job.value != "on" && job.value != "off") ||
          !this->set_weather_protection_(job.key, job.value == "on")) {
        restore_settings();
        send_error(409, "invalid or unavailable weather protection");
        return;
      }
    } else if (job.setting == "disable_weather_protections") {
      this->weather_settings_.enabled_mask = 0;
      if (!this->save_weather_settings_()) {
        restore_settings();
        send_error(500, "weather settings could not be queued");
        return;
      }
    } else if (job.setting == "manual_duration" && this->manual_duration_ != nullptr) {
      char *end = nullptr;
      const float duration = strtof(job.value.c_str(), &end);
      if (end == job.value.c_str() || *end != '\0' || !std::isfinite(duration) || duration < 1.0f ||
          duration > 60.0f) {
        send_error(409, "manual duration must be between 1 and 60 minutes");
        return;
      }
      auto call = this->manual_duration_->make_call();
      call.set_value(duration);
      call.perform();
    } else if (job.setting == "timezone" && this->timezone_select_ != nullptr &&
               this->timezone_select_->has_option(job.value)) {
      auto call = this->timezone_select_->make_call();
      call.set_option(job.value);
      call.perform();
      this->invalidate_next_schedule_cache_();
    } else {
      send_error(409, "invalid setting or value");
      return;
    }
    if (!this->sync_preferences_()) {
      restore_settings();
      send_error(500, "setting could not be persisted");
      return;
    }
    send_json(200, this->system_json_());
    return;
  }
  send_error(404, "endpoint not found");
}

void DynamicSprinkler::send_json_(AsyncWebServerRequest *request, int code, const std::string &body) const {
  request->send(code, "application/json", body.c_str());
}

void DynamicSprinkler::send_error_(AsyncWebServerRequest *request, int code, const std::string &message) const {
  auto body = json::build_json([&](JsonObject root) { root["error"] = message; });
  this->send_json_(request, code, body);
}

}  // namespace esphome::dynamic_sprinkler
