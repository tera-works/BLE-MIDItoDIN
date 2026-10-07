cmake_minimum_required(VERSION 3.16)
project(ble_midi_parser_test C)
set(CMAKE_C_STANDARD 11)
add_executable(test_parser
    test_parser.c
    ../../components/ble_midi_parser/ble_midi_parser.c)
target_include_directories(test_parser PRIVATE
    ../../components/ble_midi_parser/include
    ../../components/midi_uart/include)
target_compile_options(test_parser PRIVATE -Wall -Wextra)
enable_testing()
add_test(NAME parser COMMAND test_parser)

add_executable(test_tx_queue test_tx_queue.c)
target_include_directories(test_tx_queue PRIVATE ../../components/midi_uart/include)
target_compile_options(test_tx_queue PRIVATE -Wall -Wextra -Werror)
add_test(NAME tx_queue COMMAND test_tx_queue)
add_executable(test_oled_frame test_oled_frame.c ../../components/oled_ui/oled_frame.c)
target_include_directories(test_oled_frame PRIVATE ../../components/oled_ui/include)
target_compile_options(test_oled_frame PRIVATE -Wall -Wextra -Werror)
add_test(NAME oled_frame COMMAND test_oled_frame)

add_executable(test_peer_policy test_peer_policy.c)
target_include_directories(test_peer_policy PRIVATE ../../components/ble_midi_client/include)
target_compile_options(test_peer_policy PRIVATE -Wall -Wextra -Werror)
add_test(NAME peer_policy COMMAND test_peer_policy)

add_executable(test_oled_transfer test_oled_transfer.c ../../components/oled_ui/oled_transfer.c)
target_include_directories(test_oled_transfer PRIVATE ../../components/oled_ui/include)
target_compile_options(test_oled_transfer PRIVATE -Wall -Wextra -Werror)
add_test(NAME oled_transfer COMMAND test_oled_transfer)

add_executable(test_device_controls test_device_controls.c)
target_include_directories(test_device_controls PRIVATE ../../components/ble_midi_client/include)
target_compile_options(test_device_controls PRIVATE -Wall -Wextra -Werror)
add_test(NAME device_controls COMMAND test_device_controls)

add_executable(test_midi_merge test_midi_merge.c)
target_include_directories(test_midi_merge PRIVATE ../../components/ble_midi_client/include ../../components/ble_midi_parser/include)
target_compile_options(test_midi_merge PRIVATE -Wall -Wextra -Werror)
add_test(NAME midi_merge COMMAND test_midi_merge)

add_executable(test_serial_command test_serial_command.c)
target_include_directories(test_serial_command PRIVATE ../../components/ble_midi_client/include)
target_compile_options(test_serial_command PRIVATE -Wall -Wextra -Werror)
add_test(NAME serial_command COMMAND test_serial_command)
