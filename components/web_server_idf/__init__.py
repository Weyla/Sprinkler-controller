# Modified for Sprinkler Controller on 2026-07-12: allow bounded 32-row forms.
from esphome.components.esp32 import add_idf_sdkconfig_option
import esphome.config_validation as cv

CODEOWNERS = ["@dentra"]

CONFIG_SCHEMA = cv.All(
    cv.Schema({}),
    cv.only_on_esp32,
)


async def to_code(config):
    # ESPHome also uses this as the maximum form-body size. Dynamic sprinkler
    # schedules can contain 32 rows and encode to roughly 6.4 KB.
    add_idf_sdkconfig_option("CONFIG_HTTPD_MAX_REQ_HDR_LEN", 8192)
