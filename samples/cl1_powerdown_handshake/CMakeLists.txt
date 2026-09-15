cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(cl1_powerdown_handshake)
target_sources(app PRIVATE src/main.c src/power_permission.S)
