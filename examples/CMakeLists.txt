# Copyright 2026 Summon Software Labs.
# Licensed under the Apache License, Version 2.0.

add_executable(rack_fabric_example_quick_start quick_start.cpp)
target_link_libraries(rack_fabric_example_quick_start PRIVATE rack_fabric rack_fabric_warnings)

add_executable(rack_fabric_example_multiprocess multiprocess_demo.cpp)
target_link_libraries(rack_fabric_example_multiprocess PRIVATE rack_fabric rack_fabric_warnings)
