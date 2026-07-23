##
# @file CMakeLists.txt
# @brief TuyaOpen window_broker 应用构建配置
#
# 智能开窗器 LoRa 网关桥接器：内置 mosquitto MQTT Broker 接收 LoRa 网关上报，
# 协议桥接层将 $SH 协议与涂鸦 DP 双向转换，通过 TuyaOpen SDK 接入涂鸦云。
#
# 构建方式：通过 TuyaOpen CLI (tos.py) 构建，自动编译 TuyaOpen/src 为静态库并链接。
# mosquitto broker 组件放在 esp_components/ 下，由 ESP-IDF 构建系统自动发现编译。
# 这里仅添加 include 路径，让 TuyaOpen CMake 步骤能找到 mosq_broker.h 等头文件。
#/

# APP_PATH
set(APP_PATH ${CMAKE_CURRENT_LIST_DIR})

# APP_NAME
get_filename_component(APP_NAME ${APP_PATH} NAME)

# APP_SRCS - 自动收集 src 目录下的所有 .c 源文件
aux_source_directory(${APP_PATH}/src APP_SRCS)

# APP_INC - 应用头文件 + mosquitto broker 头文件 + ESP-IDF esp_tls 头文件
set(MOSQ_DIR ${APP_PATH}/esp_components/espressif__mosquitto)
set(APP_INC
    ${APP_PATH}/src
    ${MOSQ_DIR}/port/include
    ${MOSQ_DIR}/mosquitto/include
)

# 添加 ESP-IDF 的 esp_tls include 路径（mosq_broker.h 包含 esp_tls.h）
# IDF_PATH 由 platform_config.cmake 设置；esp_tls.h 直接在 esp-tls/ 目录下
if(DEFINED IDF_PATH)
    list(APPEND APP_INC ${IDF_PATH}/components/esp-tls)
    list(APPEND APP_INC ${IDF_PATH}/components/esp_common/include)
    list(APPEND APP_INC ${IDF_PATH}/components/log/include)
    # mDNS 头文件（matter-broker.local 局域网寻址）
    list(APPEND APP_INC ${IDF_PATH}/components/mdns/include)
endif()

########################################
# Target Configure
########################################
add_library(${EXAMPLE_LIB})

target_sources(${EXAMPLE_LIB}
    PRIVATE
        ${APP_SRCS}
    )

target_include_directories(${EXAMPLE_LIB}
    PRIVATE
        ${APP_INC}
    )
