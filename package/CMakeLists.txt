if(WIN32)
set(_os "Windows")
else()
set(_os "Linux")
endif()

set(ARCH "x64")

set(US_CMAKE_MINIMUM_REQUIRED_VERSION 3.15)
cmake_minimum_required(VERSION ${US_CMAKE_MINIMUM_REQUIRED_VERSION})
cmake_policy( VERSION ${US_CMAKE_MINIMUM_REQUIRED_VERSION})

set(CMAKE_CXX_STANDARD 17)
project(ileo)

set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

#Versioning format - Major.Minor.builddate(yymmdd)
set(MAJOR_MINOR "3.0")#update major and minor version manually
set(BUG_FIX "0") #For major bug fix information
set(_desc "application to optimize IA platform [set epp value dynamically]")
string(TIMESTAMP buildDate "%y%m%d")
set(_builddate ${buildDate})

set(CMAKE_LIBRARY_OUTPUT_DIRECTORY_NATIVE ${CMAKE_LIBRARY_OUTPUT_DIRECTORY})
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_NATIVE ${CMAKE_RUNTIME_OUTPUT_DIRECTORY})

# set binary name with system details
string(CONCAT BINARY_OUTPUT ${PROJECT}"_" ${CMAKE_SYSTEM_NAME} "_" ${CMAKE_SYSTEM_PROCESSOR})
string(TOLOWER ${BINARY_OUTPUT} BINARY_OUTPUT_LOWER)
set(BINARY_OUTPUT_FILE ${BINARY_OUTPUT_LOWER})

#find_package(OpenSSL REQUIRED)
#find_package(PkgConfig REQUIRED)
#pkg_check_modules(DBUS REQUIRED dbus-1 dbus-glib-1) # This calls pkgconfig with appropriate arguments

# A list of source code files
add_executable(
  ${BINARY_OUTPUT_FILE}
  	../src/lpmd_main.c \
	../src/lpmd_proc.c \
	../src/lpmd_dbus_server.c \
	../src/lpmd_config.c \
	../src/lpmd_cpu.c \
	../src/lpmd_helpers.c \
	../src/lpmd_hfi.c \
	../src/lpmd_irq.c \
	../src/lpmd_socket.c \
	../src/lpmd_util.c	\
	../lpmd-resource.c
  #../src/main.cpp
  ../src/helper.cpp
  #src/msrInterface.cpp
  #src/cgroupInterface.cpp
  #src/raplInterface.cpp
  ../src/permissions.cpp	

)

include_directories(
    ${CMAKE_CURRENT_BINARY_DIR}
    src/include/
)

if(CMAKE_COMPILER_IS_GNUCC OR CMAKE_COMPILER_IS_GNUCXX)
   #add -Wextra later due to errors
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall -Werror -Wconversion -pedantic -Wno-unknown-pragmas -Wformat-security -Wformat -Wformat-security -D_FORTIFY_SOURCE=2")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fno-strict-overflow -fstack-protector-strong -fPIE -fPIC -z noexecstack -pie")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fno-delete-null-pointer-checks -flto -fvisibility=hidden")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fwrapv")
endif()

target_link_libraries( ${BINARY_OUTPUT_FILE} m cap)
