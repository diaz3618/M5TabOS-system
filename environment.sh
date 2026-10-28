# Prepares the terminal to build M5tabOS with ESP-IDF 5.5.
#
#   source environment.sh
#   idf.py build
#
# Standard ESP-IDF install: set IDF_PATH (or install it at ~/esp/esp-idf)
# and this script loads its export.sh. For a non-standard setup (a
# different folder, an IDF from another tool), create environment.local.sh
# next to this file: if it exists, it's used instead of export.sh. It is
# not tracked in the repository (.gitignore).

HERE="$(cd "$(dirname "${BASH_SOURCE[0]:-$0}")" && pwd)"

if [ -f "$HERE/environment.local.sh" ]; then
    source "$HERE/environment.local.sh"
else
    : "${IDF_PATH:=$HOME/esp/esp-idf}"
    if [ ! -f "$IDF_PATH/export.sh" ]; then
        echo "ESP-IDF not found at $IDF_PATH - install ESP-IDF 5.5 or set IDF_PATH."
        return 1 2>/dev/null || exit 1
    fi
    source "$IDF_PATH/export.sh" > /dev/null
fi

# CRITICAL FOR WIFI. esp_wifi_remote's Kconfig does
#   orsource "./Kconfig.idf_v$ESP_IDF_VERSION.in"
# and orsource SILENTLY IGNORES a file that doesn't exist. Without this
# variable set to "5.5", the CONFIG_WIFI_RMT_* options don't exist and the
# build dies with "CONFIG_WIFI_RMT_STATIC_RX_BUFFER_NUM undeclared".
export ESP_IDF_VERSION=5.5
