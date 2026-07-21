##
# @file CMakeLists.txt
# @brief TuyaOpen window_broker 应用构建配置
#
# 智能开窗器 LoRa 网关桥接器：内置 mosquitto MQTT Broker 接收 LoRa 网关上报，
# 协议桥接层将 $SH 协议与涂鸦 DP 双向转换，通过 TuyaOpen SDK 接入涂鸦云。
#
# 构建方式：通过 TuyaOpen CLI (tos.py) 构建，自动编译 TuyaOpen/src 为静态库并链接。
#/

# APP_PATH
set(APP_PATH ${CMAKE_CURRENT_LIST_DIR})

# APP_NAME
get_filename_component(APP_NAME ${APP_PATH} NAME)

# APP_SRCS - 自动收集 src 目录下的所有 .c 源文件
aux_source_directory(${APP_PATH}/src APP_SRCS)

# APP_INC
set(APP_INC
    ${APP_PATH}/src
)

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
