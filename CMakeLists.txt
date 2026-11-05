cmake_minimum_required(VERSION 3.25)
project(SystemInfo VERSION 1.5.11.5 LANGUAGES CXX RC)

if(NOT WIN32 OR NOT MSVC OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "SystemInfo requires Windows, MSVC and x64 (configure with -A x64).")
endif()
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

set(SYSTEMINFO_AUTHOR "sclliang")
set(SYSTEMINFO_COMPANY "sixunited")
set(SYSTEMINFO_COPYRIGHT "Copyright (c) 2026 sixunited. Author: sclliang")
option(SYSTEMINFO_REQUIRE_ADMIN "Match FactoryTestCases administrator execution level" ON)
if(SYSTEMINFO_REQUIRE_ADMIN)
    set(SYSTEMINFO_EXECUTION_LEVEL "requireAdministrator")
else()
    set(SYSTEMINFO_EXECUTION_LEVEL "asInvoker")
endif()
set(SYSTEMINFO_FLUENTUI_SDK "${CMAKE_CURRENT_SOURCE_DIR}/../FluentUI/sdk"
    CACHE PATH "Local native Win32 FluentUI SDK")
find_package(FluentUI 0.2 CONFIG REQUIRED HINTS "${SYSTEMINFO_FLUENTUI_SDK}")

function(systeminfo_compile_settings target)
    target_compile_definitions(${target} PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN
        NOMINMAX WINVER=0x0A00 _WIN32_WINNT=0x0A00)
    target_compile_options(${target} PRIVATE
        "$<$<COMPILE_LANGUAGE:CXX>:/W4;/WX;/EHsc;/permissive-;/utf-8;/guard:cf>")
endfunction()

add_library(HardwareInfo STATIC src/hardware_info/HardwareInfo.cpp)
systeminfo_compile_settings(HardwareInfo)
target_link_libraries(HardwareInfo PRIVATE advapi32 ole32 oleaut32 setupapi wbemuuid)

set(ATA_CORE "${CMAKE_CURRENT_SOURCE_DIR}/third_party/AtaSmartCore")
add_library(AtaSmartCore STATIC
    "${ATA_CORE}/AtaSmart.cpp" "${ATA_CORE}/NVMeInterpreter.cpp" "${ATA_CORE}/SlotSpeedGetter.cpp"
    "${ATA_CORE}/Priscilla/md5.cpp" "${ATA_CORE}/Priscilla/OsInfoFx.cpp" "${ATA_CORE}/Priscilla/UtilityFx.cpp")
set_target_properties(AtaSmartCore PROPERTIES CXX_STANDARD 14 CXX_STANDARD_REQUIRED YES)
target_compile_definitions(AtaSmartCore PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(AtaSmartCore PRIVATE /W0 /EHsc /utf-8 /guard:cf)
target_include_directories(AtaSmartCore PRIVATE "${ATA_CORE}" "${ATA_CORE}/Priscilla")
target_link_libraries(AtaSmartCore PRIVATE advapi32 comsuppw setupapi shlwapi)

add_library(SsdInfo STATIC src/storage/DiskCollector.cpp src/storage/Smart.cpp
    src/storage/Wmi.cpp src/storage/SsdInfo.cpp)
systeminfo_compile_settings(SsdInfo)
target_include_directories(SsdInfo PRIVATE "${ATA_CORE}" "${ATA_CORE}/Priscilla")
target_link_libraries(SsdInfo PRIVATE AtaSmartCore cfgmgr32 ole32 oleaut32 wbemuuid)

add_library(EventLogCollectorCore STATIC
    src/eventlog/Collector.cpp src/eventlog/Diagnostics.cpp src/eventlog/EventLogService.cpp
    src/eventlog/HtmlReport.cpp src/eventlog/SystemSnapshot.cpp src/eventlog/UserSources.cpp)
systeminfo_compile_settings(EventLogCollectorCore)
target_link_libraries(EventLogCollectorCore PRIVATE wevtapi wbemuuid setupapi cfgmgr32 powrprof
    advapi32 shell32 ole32 oleaut32)

set(AMI_RESOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/resources/ami")
set(AMI_FILES AMIDEWINx64.EXE amifldrv64.sys amigendrv64.sys amifldrv32.sys)
set(AMI_HASHES
    ae8c870c64e72c1cf88f120d8d754ba60238dc2911c3ca36cc2c16138c326798
    e7cbfb16261de1c7f009431d374d90e9eb049ba78246e38bc4c8b9e06f324b6f
    ffc72f0bde21ba20aa97beE99d9e96870e5aa40cce9884e44c612757f939494f
    45045dd6c9f7cbcd438ef7b730703ab10eaed402d49e6ec83c074872636861e2)
foreach(index RANGE 0 3)
    list(GET AMI_FILES ${index} name)
    list(GET AMI_HASHES ${index} expected)
    string(TOLOWER "${expected}" expected)
    file(SHA256 "${AMI_RESOURCE_DIR}/${name}" actual)
    if(NOT actual STREQUAL expected)
        message(FATAL_ERROR "Embedded AMI resource does not match the supplied original: ${name}")
    endif()
endforeach()
add_library(AmiFirmware STATIC src/firmware/AmiParser.cpp src/firmware/EmbeddedAmi.cpp
    src/firmware/AmiProcess.cpp src/firmware/AmiCleanup.cpp src/firmware/AmiReader.cpp)
systeminfo_compile_settings(AmiFirmware)
target_link_libraries(AmiFirmware PRIVATE HardwareInfo advapi32 ole32)

configure_file(resources/app.rc.in "${CMAKE_CURRENT_BINARY_DIR}/app.rc" @ONLY)
configure_file(resources/log-template.rc.in "${CMAKE_CURRENT_BINARY_DIR}/log-template.rc" @ONLY)
configure_file(resources/app.manifest.in "${CMAKE_CURRENT_BINARY_DIR}/app.manifest" @ONLY)
add_executable(SystemInfo WIN32 src/main.cpp src/InformationView.cpp src/Collection.cpp src/LogCollection.cpp src/Report.cpp
    "${CMAKE_CURRENT_BINARY_DIR}/app.rc" "${CMAKE_CURRENT_BINARY_DIR}/log-template.rc" resources/ami/resources.rc)
systeminfo_compile_settings(SystemInfo)
target_compile_definitions(SystemInfo PRIVATE "SYSTEMINFO_VERSION=L\"${PROJECT_VERSION}\"")
target_include_directories(SystemInfo PRIVATE "${CMAKE_CURRENT_BINARY_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}/resources" "${AMI_RESOURCE_DIR}")
target_link_libraries(SystemInfo PRIVATE AmiFirmware SsdInfo EventLogCollectorCore fluent::FluentUI)
target_link_options(SystemInfo PRIVATE /MANIFEST:NO /guard:cf /DYNAMICBASE /NXCOMPAT
    "$<$<CONFIG:Release>:/LTCG;/OPT:REF;/OPT:ICF>")

include(CTest)
if(BUILD_TESTING)
    # Keep the reference project's EDID regression cases alongside its unchanged collector.
    set(SYSTEMINFO_TEST_NAME "MonitorInfoTests")
    configure_file(resources/tests.rc.in "${CMAKE_CURRENT_BINARY_DIR}/tests.rc" @ONLY)
    add_executable(MonitorInfoTests src/hardware_info/tests/MonitorInfoTests.cpp
        "${CMAKE_CURRENT_BINARY_DIR}/tests.rc")
    systeminfo_compile_settings(MonitorInfoTests)
    target_link_libraries(MonitorInfoTests PRIVATE advapi32 ole32 oleaut32 setupapi wbemuuid)
    add_test(NAME MonitorInfoTests COMMAND MonitorInfoTests)

    set(SYSTEMINFO_TEST_NAME "AmiFirmwareTests")
    configure_file(resources/tests.rc.in "${CMAKE_CURRENT_BINARY_DIR}/ami-tests.rc" @ONLY)
    add_executable(AmiFirmwareTests tests/AmiFirmwareTests.cpp "${CMAKE_CURRENT_BINARY_DIR}/ami-tests.rc")
    systeminfo_compile_settings(AmiFirmwareTests)
    target_link_libraries(AmiFirmwareTests PRIVATE AmiFirmware)
    add_test(NAME AmiFirmwareParsingAndFallback COMMAND AmiFirmwareTests)
    add_test(NAME AmiProcessAndDriverOwnership COMMAND AmiFirmwareTests --process)
    add_test(NAME EmbeddedAmiResources COMMAND AmiFirmwareTests --resources
        "$<TARGET_FILE:SystemInfo>" "${AMI_RESOURCE_DIR}")
    set(SYSTEMINFO_TEST_NAME "SsdInfoTests")
    configure_file(resources/tests.rc.in "${CMAKE_CURRENT_BINARY_DIR}/ssd-tests.rc" @ONLY)
    add_executable(SsdInfoTests tests/SsdInfoTests.cpp src/Collection.cpp "${CMAKE_CURRENT_BINARY_DIR}/ssd-tests.rc")
    systeminfo_compile_settings(SsdInfoTests)
    target_link_libraries(SsdInfoTests PRIVATE SsdInfo)
    add_test(NAME SsdSmartAndProgress COMMAND SsdInfoTests)
    set(SYSTEMINFO_TEST_NAME "LogCollectionTests")
    configure_file(resources/tests.rc.in "${CMAKE_CURRENT_BINARY_DIR}/log-tests.rc" @ONLY)
    add_executable(LogCollectionTests tests/LogCollectionTests.cpp src/LogCollection.cpp
        "${CMAKE_CURRENT_BINARY_DIR}/log-tests.rc" "${CMAKE_CURRENT_BINARY_DIR}/log-template.rc")
    systeminfo_compile_settings(LogCollectionTests)
    target_link_libraries(LogCollectionTests PRIVATE EventLogCollectorCore)
    add_test(NAME LogCollectionCancelAndRetry COMMAND LogCollectionTests)
    set_tests_properties(LogCollectionCancelAndRetry PROPERTIES TIMEOUT 60)
endif()

install(TARGETS SystemInfo RUNTIME DESTINATION .)
install(FILES "${ATA_CORE}/LICENSE.txt" DESTINATION licenses RENAME AtaSmartCore-LICENSE.txt)
