"""Execute the actual import API branches against in-memory clock/NVS stubs."""
from pathlib import Path
import subprocess
import tempfile
import unittest


class ImportTests(unittest.TestCase):
    def test_lock_ownership_recovery_and_claims(self):
        source = (Path(__file__).resolve().parents[1] / "components/dynamic_sprinkler/dynamic_sprinkler.cpp").read_text()
        start = source.index('  if (job.path == "/sprinkler/api/import/begin")')
        end = source.index('  if (job.path == "/sprinkler/api/save")', start)
        actual = source[start:end]
        cpp = r'''
#include <cassert>
#include <cstdint>
#include <string>
uint32_t clock_ms=1000;uint32_t millis(){return clock_ms;}
struct ESPTime { int64_t timestamp=10000; uint8_t day_of_week=1; bool valid=true; bool is_valid()const{return valid;} };
ESPTime local_date_offset(ESPTime t,int day){t.timestamp+=day*86400;return t;}
struct Clock {ESPTime now(){return {};}};
struct Job {std::string path,import_token,action;};
struct Controller {
 static constexpr int MAX_SCHEDULES=2,SCHEDULE_MARKER=42,TRIGGER_GRACE_SECONDS=300;
 bool import_locked_=false,active=false,persist_ok=true;
 std::string import_token_;
 uint32_t import_touched_ms_=0;
 struct Preference {bool stored=false;bool save(const bool*v){stored=*v;return true;}} import_pref_;
 Clock clock;Clock *time_=&clock;
 struct Schedule {int marker=42;uint8_t days_mask=127,start_time_count=1;int start_triggers[1]{0};int64_t last_run_keys[1]{-1};} schedules_[2];
 bool is_active(){return active;}
 bool sync_preferences_(){return persist_ok;}
 bool save_schedule_(uint8_t){return persist_ok;}
 bool resolve_trigger_(int,ESPTime base,ESPTime &target){target=base;target.timestamp-=10;return true;}
 int64_t trigger_run_key_(ESPTime base,uint8_t i){return base.timestamp*100+i;}
 void invalidate_next_schedule_cache_(){}
 int code=0;
 void request(Job job){
 auto send_error=[&](int c,const std::string&){code=c;};
 auto send_json=[&](int c,const std::string&){code=c;};
''' + actual + r'''
 code=200; // Authorized ordinary mutation.
 }
};
int main(){
 Controller c;c.active=true;c.request({"/sprinkler/api/import/begin","owner",""});assert(c.code==409&&!c.import_locked_);
 c.active=false;c.request({"/sprinkler/api/import/begin","owner",""});assert(c.code==200&&c.import_locked_&&c.import_pref_.stored);
 c.request({"/sprinkler/api/settings","other",""});assert(c.code==409);
 c.request({"/sprinkler/api/validate","",""});assert(c.code==200); // Retry preflight is read-only.
 c.request({"/sprinkler/api/import/begin","other",""});assert(c.code==409);
 c.request({"/sprinkler/api/settings","owner",""});assert(c.code==200);
 c.request({"/sprinkler/api/control","","stop"});assert(c.code==200&&c.import_locked_);
 clock_ms=62000;c.request({"/sprinkler/api/import/begin","retry",""});assert(c.code==200&&c.import_locked_);
 c.request({"/sprinkler/api/import/commit","owner",""});assert(c.code==409);
 c.persist_ok=false;c.request({"/sprinkler/api/import/commit","retry",""});assert(c.code==500&&c.import_locked_);
 c.persist_ok=true;c.request({"/sprinkler/api/import/commit","retry",""});assert(c.code==200&&!c.import_locked_&&!c.import_pref_.stored);
 for(const auto&s:c.schedules_)assert(s.last_run_keys[0]==1000000);
 // On reboot the persisted flag is restored, but an abandoned owner is not.
 Controller reboot;reboot.import_locked_=true;reboot.import_pref_.stored=true;
 reboot.request({"/sprinkler/api/import/begin","new-browser",""});assert(reboot.code==200&&reboot.import_locked_);
}
'''
        with tempfile.TemporaryDirectory(prefix="sprinkler-import-") as directory:
            binary = str(Path(directory) / "test")
            result = subprocess.run(["g++", "-std=c++17", "-Wall", "-Wextra", "-x", "c++", "-o", binary, "-"],
                                    input=cpp, text=True, capture_output=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            subprocess.run([binary], check=True)


if __name__ == "__main__":
    unittest.main()
