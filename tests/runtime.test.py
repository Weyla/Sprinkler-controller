"""Host regression tests executing extracted production runtime functions."""
from pathlib import Path
import subprocess
import tempfile
import unittest

SOURCE = (Path(__file__).resolve().parents[1] / "components/dynamic_sprinkler/dynamic_sprinkler.cpp").read_text()


def function(marker):
    start = SOURCE.index(marker)
    end = SOURCE.index("{", start) + 1
    depth = 1
    while depth:
        depth += (SOURCE[end] == "{") - (SOURCE[end] == "}")
        end += 1
    return SOURCE[start:end]


class RuntimeTests(unittest.TestCase):
    def test_per_valve_time_and_weather(self):
        methods = "\n".join(function(marker) for marker in [
            "uint32_t DynamicSprinkler::row_remaining_ms_", "void DynamicSprinkler::account_zone_time_",
            "DynamicSprinkler::WeatherResult DynamicSprinkler::prepare_weather_",
            "bool DynamicSprinkler::pause()", "bool DynamicSprinkler::resume()",
        ])
        source = r'''
#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <string>
uint32_t clock_ms=1000;
uint32_t millis() { return clock_ms; }
namespace sensor { struct Sensor { float state=0; }; }
enum class RunState { IDLE, STARTING, RUNNING, PAUSED, WEATHER_WAIT };
enum class ActivityStopReason { COMPLETED, PAUSED };
enum { WEATHER_RAIN_RATE, WEATHER_WIND, WEATHER_GUST, WEATHER_RAIN_24H, WEATHER_TEMPERATURE };
struct Relay { bool state=false; void turn_off() {state=false;} };
struct DynamicSprinkler {
  static constexpr int MAX_ZONES_PER_ROW=3;
  enum class WeatherResult { READY, WAIT, SKIP };
  struct Row { uint16_t duration_seconds=10, max_wind_deci=100, max_gust_deci=150, rain_target_deci=100;
    uint8_t zone_count=3, zones[3]{0,1,2}; };
  struct Program { Row rows[2]; uint8_t rounds=1, row_count=1; uint16_t max_wait_minutes=5;
    int16_t temp_adjust_deci=0, base_temp_deci=200; } active_program_;
  uint8_t active_row_index_=0, active_round_index_=0;
  uint32_t zone_remaining_ms_[3]{}, zone_delivered_ms_[3]{}, zone_accounted_ms_[3]{};
  bool zone_running_[3]{}, row_initialized_=false;
  uint32_t row_target_ms_=0, paused_remaining_ms_=0, run_started_ms_=1000, weather_retry_ms_=0;
  uint32_t state_deadline_ms_=0, weather_blocked_since_ms_=0; bool weather_wait_active_=false;
  uint32_t weather_updated_ms_[5]{1000,1000,1000,1000,1000}, watered_seconds_=0;
  uint64_t valve_watered_ms_=0;
  bool manual_run_=false, enforce_window_=true, resuming_=false;
  RunState state_=RunState::RUNNING;
  sensor::Sensor wind, gust, rain, rate, temp;
  sensor::Sensor *wind_speed_=&wind, *wind_gust_=&gust, *rain_24h_=&rain, *rain_rate_=&rate, *daily_temp_=&temp;
  Relay relay[3]; Relay *relays_[3]{&relay[0],&relay[1],&relay[2]};
  bool weather_enabled_(int) { return true; }
  int zone_index_(uint8_t z) { return z; }
  void stop_zone_activity_(uint8_t,ActivityStopReason) {}
  void set_decision_(const std::string &) {}
  void publish_status_(bool=true) {}
  void turn_all_off_(ActivityStopReason) { account_zone_time_(millis()); for(int i=0;i<3;i++){zone_running_[i]=false;relay[i].turn_off();} }
  void begin_row_() { state_=RunState::STARTING; }
  void prepare_next_row_() { state_=RunState::IDLE; }
  uint32_t row_remaining_ms_() const;
  void account_zone_time_(uint32_t);
  WeatherResult prepare_weather_();
  bool pause(); bool resume();
  void open(int i,uint32_t now) { zone_running_[i]=true;zone_accounted_ms_[i]=now;relay[i].state=true; }
};
''' + methods + r'''
int main() {
  DynamicSprinkler c;
  assert(c.prepare_weather_()==DynamicSprinkler::WeatherResult::READY);
  c.open(0,1000); c.open(1,1250); c.open(2,1500);
  c.account_zone_time_(11000);
  assert(!c.relay[0].state && c.relay[1].state && c.relay[2].state);
  c.account_zone_time_(11250); assert(!c.relay[1].state && c.relay[2].state);
  c.account_zone_time_(11500); assert(!c.relay[2].state);
  for(auto ms:c.zone_delivered_ms_) assert(ms==10000);
  assert(c.watered_seconds_==30);

  DynamicSprinkler p; clock_ms=1000; p.prepare_weather_();
  p.open(0,1000); p.open(1,1250); p.open(2,1500);
  clock_ms=2000; assert(p.pause());
  assert(p.zone_remaining_ms_[0]==9000 && p.zone_remaining_ms_[2]==9500);
  clock_ms=60000; assert(p.resume());
  assert(p.zone_remaining_ms_[0]==9000 && p.zone_remaining_ms_[2]==9500);
  // Rain reduction uses the new TOTAL target minus time already delivered.
  p.rain.state=5; assert(p.prepare_weather_()==DynamicSprinkler::WeatherResult::READY);
  assert(p.zone_remaining_ms_[0]==4000 && p.zone_remaining_ms_[2]==4500);
  p.rain.state=0; p.prepare_weather_(); assert(p.zone_remaining_ms_[0]==4000);
  p.rate.state=1; assert(p.prepare_weather_()==DynamicSprinkler::WeatherResult::WAIT);
  p.rate.state=0; p.wind.state=11; assert(p.prepare_weather_()==DynamicSprinkler::WeatherResult::WAIT);
  p.wind.state=0; assert(p.prepare_weather_()==DynamicSprinkler::WeatherResult::READY);
  p.rate.state=1; clock_ms=301001; assert(p.prepare_weather_()==DynamicSprinkler::WeatherResult::WAIT);
  clock_ms=601002; assert(p.prepare_weather_()==DynamicSprinkler::WeatherResult::SKIP);
  p.manual_run_=true; assert(p.prepare_weather_()==DynamicSprinkler::WeatherResult::READY);
  // Unsigned elapsed arithmetic must also work over millis rollover.
  DynamicSprinkler wrap; clock_ms=1000; wrap.prepare_weather_();
  wrap.open(0,0xFFFFFF00U); wrap.account_zone_time_(0x100U);
  assert(wrap.zone_remaining_ms_[0]==9488);
}
'''
        with tempfile.TemporaryDirectory(prefix="sprinkler-runtime-") as directory:
            binary = str(Path(directory) / "test")
            result = subprocess.run(["g++", "-std=c++17", "-Wall", "-Wextra", "-x", "c++", "-o", binary, "-"],
                                    input=source, text=True, capture_output=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            subprocess.run([binary], check=True)


if __name__ == "__main__":
    unittest.main()
