# The following lines of boilerplate have to be in your project's CMakeLists
# in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.5)

# 加载 ESP-ADF 框架的 CMake 构建逻辑（比如它的依赖管理、组件搜索规则等）
include($ENV{ADF_PATH}/CMakeLists.txt)

# IDF 工程最重要的入口，会定义 project() 命令的特殊行为:
# 自动扫描 main/ 目录和 components/ 目录
# 调用每个组件的 CMakeLists.txt 来添加源码、头文件、依赖等。
# 处理 sdkconfig、依赖锁文件等 IDF 特有功能。
include($ENV{IDF_PATH}/tools/cmake/project.cmake)

# set(EXTRA_COMPONENT_DIRS
# 	"components"
# 	"offline_components"
# 	"$ENV{ADF_PATH}/components"  # 显式添加 ESP-ADF 组件路径, 上面未生效
# )

project(camera_test)
