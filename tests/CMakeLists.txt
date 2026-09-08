# Copyright 2026 Summon Software Labs.
# Licensed under the Apache License, Version 2.0.
#
# The test executables. No test carries a timeout: a hang is a defect to
# diagnose, never something to terminate.

set(RACK_FABRIC_TEST_SOURCES
  test_main.cpp
  test_core.cpp
  test_membership.cpp
  test_topology.cpp
  test_envelopes.cpp
  test_snapshots.cpp
  test_authority.cpp
  test_protocol.cpp
  test_persistence.cpp
  test_recovery.cpp
  test_concurrency.cpp
  test_property.cpp
  test_synthetic.cpp)

add_executable(rack_fabric_tests ${RACK_FABRIC_TEST_SOURCES})
target_include_directories(rack_fabric_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(rack_fabric_tests PRIVATE rack_fabric rack_fabric_warnings)
if(RACK_FABRIC_CUDA_AVAILABLE)
  target_link_libraries(rack_fabric_tests PRIVATE rack_fabric_cuda)
  target_compile_definitions(rack_fabric_tests PRIVATE RACK_FABRIC_HAS_CUDA=1)
endif()

add_test(NAME rack_fabric_unit COMMAND rack_fabric_tests)

# A second run of the whole suite in reverse-ordered repeats proves that the
# runtime starts and stops deterministically and that no state leaks between
# instances.
add_test(NAME rack_fabric_unit_repeat COMMAND rack_fabric_tests --repeat 3)

if(WIN32)
  add_executable(rack_fabric_multiprocess_tests test_multiprocess.cpp test_main.cpp)
  target_include_directories(rack_fabric_multiprocess_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  target_link_libraries(rack_fabric_multiprocess_tests PRIVATE rack_fabric rack_fabric_warnings)
  add_test(NAME rack_fabric_multiprocess COMMAND rack_fabric_multiprocess_tests)
  set_tests_properties(rack_fabric_multiprocess PROPERTIES
    ENVIRONMENT "RACK_FABRIC_COORDINATOR=$<TARGET_FILE:rack_fabric_coordinator>;RACK_FABRIC_AGENT=$<TARGET_FILE:rack_fabric_agent>")
endif()
