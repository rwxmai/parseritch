if(EXISTS "/Users/zefiepie/Documents/GitHub/parseritch/build-dev/itch_tests")
  if(NOT EXISTS "/Users/zefiepie/Documents/GitHub/parseritch/build-dev/itch_tests[1]_tests.cmake" OR
     NOT "/Users/zefiepie/Documents/GitHub/parseritch/build-dev/itch_tests[1]_tests.cmake" IS_NEWER_THAN "/Users/zefiepie/Documents/GitHub/parseritch/build-dev/itch_tests" OR
     NOT "/Users/zefiepie/Documents/GitHub/parseritch/build-dev/itch_tests[1]_tests.cmake" IS_NEWER_THAN "${CMAKE_CURRENT_LIST_FILE}")
    include("/Applications/CMake.app/Contents/share/cmake-3.28/Modules/GoogleTestAddTests.cmake")
    gtest_discover_tests_impl(
      TEST_EXECUTABLE [==[/Users/zefiepie/Documents/GitHub/parseritch/build-dev/itch_tests]==]
      TEST_EXECUTOR [==[]==]
      TEST_WORKING_DIR [==[/Users/zefiepie/Documents/GitHub/parseritch/build-dev]==]
      TEST_EXTRA_ARGS [==[]==]
      TEST_PROPERTIES [==[]==]
      TEST_PREFIX [==[x86-64-v2.]==]
      TEST_SUFFIX [==[]==]
      TEST_FILTER [==[]==]
      NO_PRETTY_TYPES [==[FALSE]==]
      NO_PRETTY_VALUES [==[FALSE]==]
      TEST_LIST [==[itch_tests_TESTS]==]
      CTEST_FILE [==[/Users/zefiepie/Documents/GitHub/parseritch/build-dev/itch_tests[1]_tests.cmake]==]
      TEST_DISCOVERY_TIMEOUT [==[60]==]
      TEST_XML_OUTPUT_DIR [==[]==]
    )
  endif()
  include("/Users/zefiepie/Documents/GitHub/parseritch/build-dev/itch_tests[1]_tests.cmake")
else()
  add_test(itch_tests_NOT_BUILT itch_tests_NOT_BUILT)
endif()
