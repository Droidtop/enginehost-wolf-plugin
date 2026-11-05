# The Wolf core's native code. The Android build (plugin-core) adds this directory; the tests
# build and run on the host.
cmake_minimum_required(VERSION 3.18)
project(enginehost_wolf_core C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

add_library(wolfcore STATIC src/dxa.c src/wolf_cmd.c src/vm.c src/values.c src/calc.c src/flow.c)
target_link_libraries(wolfcore PRIVATE m)
target_include_directories(wolfcore PUBLIC src)
target_compile_options(wolfcore PRIVATE -Wall -Wextra -Werror)

if(NOT ANDROID)
  enable_testing()
  add_executable(test_dxa test/test_dxa.c)
  target_link_libraries(test_dxa PRIVATE wolfcore)
  target_compile_options(test_dxa PRIVATE -Wall -Wextra -Werror)
  add_test(NAME dxa COMMAND test_dxa)
  add_executable(test_vm test/test_vm.c)
  target_link_libraries(test_vm PRIVATE wolfcore)
  target_compile_options(test_vm PRIVATE -Wall -Wextra -Werror)
  add_test(NAME vm COMMAND test_vm)
endif()
