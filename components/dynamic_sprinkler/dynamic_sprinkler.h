#pragma once

#include "esphome/core/component.h"
#include "esphome/core/preferences.h"
#include "esphome/components/json/json_util.h"
#include "esphome/components/button/button.h"
#include "esphome/components/number/number.h"
#include "esphome/components/select/select.h"
#include "esphome/components/sensor/sensor.h"
#include "esphome/components/sun/sun.h"
#include "esphome/components/switch/switch.h"
#include "esphome/components/text_sensor/text_sensor.h"
#include "esphome/components/time/real_time_clock.h"
#include "esphome/components/web_server_base/web_server_base.h"
#include "esphome/components/wifi/wifi_component.h"

#include <array>
#include <string>
#include <vector>

namespace esphome::dynamic_sprinkler {

class DynamicSprinkler;
struct WebRequestJob;

class DynamicWeatherSensor : public sensor::Sensor, public Component {
 public:
  void set_entity_id(const std::string &entity_id) { entity_id_ = entity_id; }
  void setup() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::AFTER_CONNECTION; }

 protected:
  std::string entity_id_;
};

class DynamicZoneButton : public button::Button {
 public:
  DynamicZoneButton(DynamicSprinkler *parent, uint8_t zone) : parent_(parent), zone_(zone) {}
  void set_dynamic_name(const std::string &name) {
    this->dynamic_name_ = name + " Manual Run";
    this->name_ = StringRef(this->dynamic_name_);
  }

 protected:
  void press_action() override;
  DynamicSprinkler *parent_;
  uint8_t zone_;
  std::string dynamic_name_;
};

static constexpr uint8_t MAX_SCHEDULES = 32;
static constexpr uint8_t MAX_ROWS = 32;
static constexpr uint8_t MAX_ZONES = 32;
static constexpr uint8_t MAX_ZONES_PER_ROW = 3;
static constexpr uint8_t MAX_START_TIMES = 8;
static constexpr uint8_t HISTORY_SIZE = 32;
static constexpr uint8_t ACTIVITY_SIZE = 64;

enum class TriggerType : uint8_t { CLOCK = 0, SUNRISE = 1, SUNSET = 2 };
enum class ActivityStopReason : uint8_t { NONE = 0, COMPLETED = 1, PAUSED = 2, SKIPPED = 3, STOPPED = 4 };

struct StartTrigger {
  TriggerType type{TriggerType::CLOCK};
  int16_t value{360};  // Clock minute-of-day or signed solar offset in minutes.
};

struct RowRecord {
  uint8_t zone_count{0};
  uint8_t zones[MAX_ZONES_PER_ROW]{0, 0, 0};
  uint16_t duration_seconds{60};
  uint16_t delay_after_seconds{0};
  int16_t max_wind_deci{0};
  int16_t max_gust_deci{0};
  int16_t rain_target_deci{0};
};

struct ScheduleRecord {
  uint32_t marker{0};
  uint32_t id{0};
  uint32_t revision{0};
  int64_t last_run_keys[MAX_START_TIMES]{-1, -1, -1, -1, -1, -1, -1, -1};
  char name[32]{};
  uint8_t enabled{0};
  uint8_t start_time_count{1};
  StartTrigger start_triggers[MAX_START_TIMES]{};
  uint8_t rounds{1};
  uint8_t days_mask{0x7F};
  uint16_t max_wait_minutes{240};
  int16_t base_temp_deci{220};
  int16_t temp_adjust_deci{0};
  uint8_t row_count{0};
  RowRecord rows[MAX_ROWS]{};
};

struct HistoryRecord {
  uint32_t marker{0};
  uint32_t run_id{0};
  uint32_t schedule_id{0};
  uint32_t started_at{0};
  uint32_t ended_at{0};
  uint32_t watered_seconds{0};
  uint8_t manual{0};
  char name[32]{};
  char result[48]{};
};

struct ActivityRecord {
  uint32_t marker{0};
  uint32_t run_id{0};
  uint32_t schedule_id{0};
  uint32_t started_at{0};
  uint32_t ended_at{0};
  uint32_t watered_ms{0};
  uint32_t commanded_on{0};
  uint32_t commanded_off{0};
  uint8_t zone_id{0};
  uint8_t round{0};
  uint8_t row{0};
  uint8_t manual{0};
  ActivityStopReason reason{ActivityStopReason::NONE};
};

struct ActivityStore {
  uint32_t marker{0};
  uint8_t head{0};
  uint8_t count{0};
  ActivityRecord records[ACTIVITY_SIZE]{};
};

struct StoreMeta {
  uint32_t marker{0};
  uint32_t next_schedule_id{1};
  uint32_t next_run_id{1};
  uint8_t history_head{0};
};

struct ZoneNameStore {
  uint32_t marker{0};
  char names[MAX_ZONES][32]{};
};

struct WeatherSettingsStore {
  uint32_t marker{0};
  uint8_t enabled_mask{0};
};

static_assert(sizeof(ScheduleRecord) <= 688, "Schedule persistence record unexpectedly grew");

class DynamicSprinkler : public Component, public AsyncWebHandler {
 public:
  void setup() override;
  void loop() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::DATA; }

  void set_time(time::RealTimeClock *value) { time_ = value; }
  void set_sun(sun::Sun *value) { sun_ = value; }
  void add_zone(switch_::Switch *relay, const std::string &name, uint8_t zone_id) {
    relays_.push_back(relay);
    zone_names_.push_back(name);
    zone_ids_.push_back(zone_id);
    zone_buttons_.push_back(nullptr);
  }
  void set_wind_speed(sensor::Sensor *value) { wind_speed_ = value; }
  void set_wind_gust(sensor::Sensor *value) { wind_gust_ = value; }
  void set_rain_24h(sensor::Sensor *value) { rain_24h_ = value; }
  void set_rain_rate(sensor::Sensor *value) { rain_rate_ = value; }
  void set_daily_temp(sensor::Sensor *value) { daily_temp_ = value; }
  void set_status_text(text_sensor::TextSensor *value) { status_text_ = value; }
  void set_decision_text(text_sensor::TextSensor *value) { decision_text_ = value; }
  void set_remaining_sensor(sensor::Sensor *value) { remaining_sensor_ = value; }
  void set_timezone_select(select::Select *value) { timezone_select_ = value; }
  void set_selected_enabled_switch(switch_::Switch *value) { selected_enabled_switch_ = value; }
  void set_manual_duration(number::Number *value) { manual_duration_ = value; }
  void set_zone_button(uint8_t index, DynamicZoneButton *value) {
    if (index < zone_buttons_.size()) zone_buttons_[index] = value;
  }

  bool canHandle(AsyncWebServerRequest *request) const override;
  void handleRequest(AsyncWebServerRequest *request) override;

  void emergency_stop(const std::string &reason);
  bool pause();
  bool resume();
  bool skip_row();
  bool start_single_zone(uint8_t zone, uint16_t seconds);
  void check_safety();

  bool is_active() const { return state_ != RunState::IDLE; }
  bool is_paused() const { return state_ == RunState::PAUSED; }
  uint32_t remaining_seconds() const;
  uint32_t elapsed_seconds() const;
  uint8_t active_row_number() const { return active_row_index_ + (is_active() ? 1 : 0); }
  uint32_t active_schedule_id() const { return active_schedule_id_; }
  uint8_t schedule_count() const;
  void select_schedule_ordinal(uint8_t ordinal);
  bool run_selected_schedule();
  bool selected_schedule_enabled() const;
  void set_selected_schedule_enabled(bool enabled);
  std::string selected_schedule_name() const;
  std::string selected_schedule_details() const;
  std::string next_schedule_name() const;
  std::string next_schedule_start() const;
  uint32_t next_schedule_seconds() const;
  uint16_t quick_manual_seconds() const;

 protected:
  enum class RunState : uint8_t { IDLE, WEATHER_WAIT, STARTING, RUNNING, DELAY, PAUSED };
  enum class WeatherResult : uint8_t { READY, WAIT, SKIP };

  struct NextScheduleInfo {
    int slot{-1};
    uint32_t seconds{0};
    char start[24]{};
  };

  static constexpr uint32_t SCHEDULE_MARKER = 0x53505234;
  static constexpr uint32_t HISTORY_MARKER = 0x48535432;
  static constexpr uint32_t ACTIVITY_MARKER = 0x41435431;
  static constexpr uint32_t ACTIVITY_STORE_MARKER = 0x41435331;
  static constexpr uint32_t META_MARKER = 0x4D455432;
  static constexpr uint32_t ZONE_NAMES_MARKER = 0x5A4E4D33;
  static constexpr uint32_t WEATHER_SETTINGS_MARKER = 0x57454131;
  static constexpr uint32_t PREF_BASE = 0xD5100000;
  static constexpr uint32_t SERIALIZED_SCHEDULE_PREF_BASE = PREF_BASE + 0x500;
  static constexpr uint16_t SCHEDULE_FORMAT_VERSION = 2;
  static constexpr uint16_t SCHEDULE_BLOB_SIZE = 768;
  static constexpr uint16_t META_BLOB_SIZE = 32;
  static constexpr uint16_t HISTORY_BLOB_SIZE = 128;
  static constexpr uint16_t ACTIVITY_BLOB_SIZE = 2432;
  static constexpr uint16_t ZONE_NAMES_BLOB_SIZE = 1088;
  static constexpr uint16_t WEATHER_BLOB_SIZE = 32;
  static constexpr int32_t TRIGGER_GRACE_SECONDS = 300;
  static constexpr uint8_t WEATHER_WIND = 1U << 0;
  static constexpr uint8_t WEATHER_GUST = 1U << 1;
  static constexpr uint8_t WEATHER_RAIN_24H = 1U << 2;
  static constexpr uint8_t WEATHER_RAIN_RATE = 1U << 3;
  static constexpr uint8_t WEATHER_TEMPERATURE = 1U << 4;

  void load_store_();
  bool save_meta_();
  bool save_schedule_(uint8_t slot);
  bool save_history_(uint8_t slot);
  bool save_activity_();
  bool save_weather_settings_();
  bool sync_preferences_();
  bool set_zone_name_(uint8_t zone_id, const std::string &name);
  bool load_serialized_schedule_(uint8_t slot, ScheduleRecord &schedule);
  bool save_serialized_schedule_(uint8_t slot, const ScheduleRecord &schedule);
  bool load_serialized_meta_();
  bool load_serialized_history_(uint8_t slot);
  bool load_serialized_activity_();
  bool load_serialized_zone_names_();
  bool load_serialized_weather_();
  bool save_serialized_meta_();
  bool save_serialized_history_(uint8_t slot);
  bool save_serialized_activity_();
  bool save_serialized_zone_names_();
  bool save_serialized_weather_();
  int zone_index_(uint8_t zone_id) const;
  bool set_weather_protection_(const std::string &key, bool enabled);
  uint8_t weather_capabilities_() const;
  bool weather_enabled_(uint8_t flag) const;
  int find_schedule_slot_(uint32_t id) const;
  int free_schedule_slot_() const;
  int schedule_slot_by_ordinal_(uint8_t ordinal) const;
  bool schedule_is_usable_(const ScheduleRecord &schedule) const;
  NextScheduleInfo next_schedule_info_() const;
  void invalidate_next_schedule_cache_();
  void publish_selected_enabled_();
  void update_zone_button_names_();
  bool resolve_trigger_(const StartTrigger &trigger, ESPTime base_date, ESPTime &target) const;
  int64_t trigger_run_key_(const ESPTime &base_date, uint8_t trigger_index) const;
  std::string trigger_label_(const StartTrigger &trigger, const ESPTime *base_date = nullptr) const;

  void evaluate_schedules_();
  bool start_schedule_(uint8_t slot, bool enforce_window);
  bool start_program_(const ScheduleRecord &program, bool manual, bool enforce_window);
  void prepare_next_row_();
  WeatherResult prepare_weather_();
  void begin_row_();
  void finish_row_();
  void finish_program_(const char *result);
  void turn_all_off_(ActivityStopReason reason = ActivityStopReason::STOPPED, bool force = false);
  void turn_zone_on_(uint8_t zone);
  void start_zone_activity_(uint8_t zone);
  void stop_zone_activity_(uint8_t zone, ActivityStopReason reason);
  uint32_t commanded_zone_bitmap_() const;
  const HistoryRecord *history_for_run_(uint32_t run_id) const;
  bool relay_expected_(uint8_t zone) const;
  void publish_status_(bool force = false);
  void set_decision_(const std::string &message);
  void add_history_(const char *result);

  bool parse_program_(const std::string &json_data, ScheduleRecord &out, bool require_identity, std::string &error);
  std::string schedules_json_() const;
  std::string schedule_json_(const ScheduleRecord &schedule) const;
  std::string status_json_() const;
  std::string history_json_(uint8_t offset, uint8_t limit) const;
  std::string activity_json_(uint8_t offset, uint8_t limit) const;
  std::string system_json_() const;
  void process_web_request_(WebRequestJob &job);
  void send_json_(AsyncWebServerRequest *request, int code, const std::string &body) const;
  void send_error_(AsyncWebServerRequest *request, int code, const std::string &message) const;

  time::RealTimeClock *time_{nullptr};
  sun::Sun *sun_{nullptr};
  std::vector<switch_::Switch *> relays_;
  std::vector<std::string> zone_names_;
  std::vector<uint8_t> zone_ids_;
  sensor::Sensor *wind_speed_{nullptr};
  sensor::Sensor *wind_gust_{nullptr};
  sensor::Sensor *rain_24h_{nullptr};
  sensor::Sensor *rain_rate_{nullptr};
  sensor::Sensor *daily_temp_{nullptr};
  text_sensor::TextSensor *status_text_{nullptr};
  text_sensor::TextSensor *decision_text_{nullptr};
  sensor::Sensor *remaining_sensor_{nullptr};
  select::Select *timezone_select_{nullptr};
  switch_::Switch *selected_enabled_switch_{nullptr};
  number::Number *manual_duration_{nullptr};
  std::vector<DynamicZoneButton *> zone_buttons_;

  uint32_t weather_updated_ms_[5]{};
  ScheduleRecord schedules_[MAX_SCHEDULES]{};
  HistoryRecord history_[HISTORY_SIZE]{};
  ActivityStore activity_store_{};
  uint32_t activity_started_ms_[ACTIVITY_SIZE]{};
  StoreMeta meta_{};
  ESPPreferenceObject serialized_schedule_prefs_[MAX_SCHEDULES];
  ZoneNameStore zone_name_store_{};
  WeatherSettingsStore weather_settings_{};
  ESPPreferenceObject serialized_meta_pref_;
  ESPPreferenceObject serialized_history_prefs_[HISTORY_SIZE];
  ESPPreferenceObject serialized_activity_pref_;
  ESPPreferenceObject serialized_zone_names_pref_;
  ESPPreferenceObject serialized_weather_pref_;

  RunState state_{RunState::IDLE};
  ScheduleRecord active_program_{};
  uint32_t active_schedule_id_{0};
  uint8_t active_row_index_{0};
  uint8_t active_round_index_{0};
  uint8_t starting_zone_index_{0};
  uint32_t run_started_ms_{0};
  uint32_t run_sequence_{0};
  uint32_t active_run_id_{0};
  uint32_t state_started_ms_{0};
  uint32_t state_deadline_ms_{0};
  uint32_t paused_remaining_ms_{0};
  uint32_t watered_seconds_{0};
  uint32_t last_evaluation_ms_{0};
  uint32_t last_status_ms_{0};
  std::string published_status_;
  uint32_t published_remaining_seconds_{UINT32_MAX};
  uint32_t weather_retry_ms_{0};
  uint32_t last_loop_call_ms_{0};
  uint32_t loop_window_started_ms_{0};
  uint32_t current_max_loop_ms_{0};
  uint32_t reported_max_loop_ms_{0};
  mutable NextScheduleInfo next_schedule_cache_{};
  mutable uint32_t next_schedule_cache_ms_{0};
  mutable bool next_schedule_cache_valid_{false};
  uint8_t selected_schedule_ordinal_{1};
  bool manual_run_{false};
  bool enforce_window_{false};
  bool resuming_{false};
};

}  // namespace esphome::dynamic_sprinkler
