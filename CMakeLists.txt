# The following lines of boilerplate have to be in your project's CMakeLists
# in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.5)

# IDF 工程最重要的入口，会定义 project() 命令的特殊行为:
# 自动扫描 main/ 目录和 components/ 目录
# 调用每个组件的 CMakeLists.txt 来添加源码、头文件、依赖等。
# 处理 sdkconfig、依赖锁文件等 IDF 特有功能。
include($ENV{IDF_PATH}/tools/cmake/project.cmake)

project(camera_test)
