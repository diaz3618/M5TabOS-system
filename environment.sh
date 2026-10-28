# Prepara o terminal para compilar o M5tabOS com o ESP-IDF 5.5.
#
#   source environment.sh
#   idf.py build
#
# Instalacao padrao do ESP-IDF: defina IDF_PATH (ou instale em ~/esp/esp-idf)
# e este script carrega o export.sh dele. Para um ambiente fora do padrao
# (outra pasta, IDF de outra ferramenta), crie environment.local.sh ao lado deste
# arquivo: se ele existir, e usado no lugar do export.sh. Ele nao vai para o
# repositorio (.gitignore).

AQUI="$(cd "$(dirname "${BASH_SOURCE[0]:-$0}")" && pwd)"

if [ -f "$AQUI/environment.local.sh" ]; then
    source "$AQUI/environment.local.sh"
else
    : "${IDF_PATH:=$HOME/esp/esp-idf}"
    if [ ! -f "$IDF_PATH/export.sh" ]; then
        echo "ESP-IDF nao encontrado em $IDF_PATH - instale o ESP-IDF 5.5 ou defina IDF_PATH."
        return 1 2>/dev/null || exit 1
    fi
    source "$IDF_PATH/export.sh" > /dev/null
fi

# CRITICO PARA O WIFI. O Kconfig do esp_wifi_remote faz
#   orsource "./Kconfig.idf_v$ESP_IDF_VERSION.in"
# e o orsource IGNORA EM SILENCIO um arquivo que nao existe. Sem esta variavel
# em "5.5", as opcoes CONFIG_WIFI_RMT_* nao existem e a compilacao morre com
# "CONFIG_WIFI_RMT_STATIC_RX_BUFFER_NUM undeclared".
export ESP_IDF_VERSION=5.5
