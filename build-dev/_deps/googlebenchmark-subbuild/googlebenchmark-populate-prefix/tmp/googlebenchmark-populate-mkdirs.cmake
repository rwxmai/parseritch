# Distributed under the OSI-approved BSD 3-Clause License.  See accompanying
# file Copyright.txt or https://cmake.org/licensing for details.

cmake_minimum_required(VERSION 3.5)

file(MAKE_DIRECTORY
  "/Users/zefiepie/Documents/GitHub/parseritch/build-dev/_deps/googlebenchmark-src"
  "/Users/zefiepie/Documents/GitHub/parseritch/build-dev/_deps/googlebenchmark-build"
  "/Users/zefiepie/Documents/GitHub/parseritch/build-dev/_deps/googlebenchmark-subbuild/googlebenchmark-populate-prefix"
  "/Users/zefiepie/Documents/GitHub/parseritch/build-dev/_deps/googlebenchmark-subbuild/googlebenchmark-populate-prefix/tmp"
  "/Users/zefiepie/Documents/GitHub/parseritch/build-dev/_deps/googlebenchmark-subbuild/googlebenchmark-populate-prefix/src/googlebenchmark-populate-stamp"
  "/Users/zefiepie/Documents/GitHub/parseritch/build-dev/_deps/googlebenchmark-subbuild/googlebenchmark-populate-prefix/src"
  "/Users/zefiepie/Documents/GitHub/parseritch/build-dev/_deps/googlebenchmark-subbuild/googlebenchmark-populate-prefix/src/googlebenchmark-populate-stamp"
)

set(configSubDirs )
foreach(subDir IN LISTS configSubDirs)
    file(MAKE_DIRECTORY "/Users/zefiepie/Documents/GitHub/parseritch/build-dev/_deps/googlebenchmark-subbuild/googlebenchmark-populate-prefix/src/googlebenchmark-populate-stamp/${subDir}")
endforeach()
if(cfgdir)
  file(MAKE_DIRECTORY "/Users/zefiepie/Documents/GitHub/parseritch/build-dev/_deps/googlebenchmark-subbuild/googlebenchmark-populate-prefix/src/googlebenchmark-populate-stamp${cfgdir}") # cfgdir has leading slash
endif()
