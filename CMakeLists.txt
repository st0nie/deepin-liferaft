cmake_minimum_required(VERSION 3.16)
project(deepin-liferaft CXX)

# The Debian changelog is the single source of truth for the version.
find_program(DPKG_PARSECHANGELOG dpkg-parsechangelog)
if(DPKG_PARSECHANGELOG)
  execute_process(
    COMMAND ${DPKG_PARSECHANGELOG} -l ${CMAKE_CURRENT_SOURCE_DIR}/debian/changelog --show-field Version
    OUTPUT_VARIABLE LIFERAFT_VERSION
    OUTPUT_STRIP_TRAILING_WHITESPACE)
endif()
if(NOT LIFERAFT_VERSION)
  set(LIFERAFT_VERSION "unknown")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

find_package(Dtk6Widget REQUIRED)
find_package(Qt6 REQUIRED COMPONENTS LinguistTools)

add_executable(deepin-liferaft main.cpp resources.qrc)
target_compile_definitions(deepin-liferaft PRIVATE LIFERAFT_VERSION="${LIFERAFT_VERSION}")
target_link_libraries(deepin-liferaft PRIVATE Dtk6::Widget)

set(LIFERAFT_TS translations/deepin-liferaft_zh_CN.ts
              translations/deepin-liferaft_en_US.ts
              translations/deepin-liferaft_ja_JP.ts
              translations/deepin-liferaft_ko_KR.ts)
qt6_add_lrelease(deepin-liferaft TS_FILES ${LIFERAFT_TS}
                  QM_FILES_OUTPUT_VARIABLE liferaft_qm_files)

enable_testing()
add_test(NAME self-test COMMAND deepin-liferaft --self-test)

install(TARGETS deepin-liferaft RUNTIME DESTINATION bin)
install(FILES ${liferaft_qm_files} DESTINATION share/deepin-liferaft/translations)
install(FILES deepin-liferaft.service DESTINATION lib/systemd/user)
# Enable kernel PSI (memory.pressure) on CONFIG_PSI_DEFAULT_DISABLED=y kernels.
# Absolute DESTINATION: install under /etc, not /usr/etc.
install(FILES data/grub/90_deepin_liferaft.cfg DESTINATION /etc/default/grub.d)
install(FILES deepin-liferaft.desktop DESTINATION share/applications)
install(FILES data/icons/deepin-liferaft.svg DESTINATION share/icons/hicolor/scalable/apps)
install(FILES data/whitelist DESTINATION share/deepin-liferaft)
install(FILES deepin-liferaft.1 DESTINATION share/man/man1)
install(FILES README.md DESTINATION share/doc/deepin-liferaft)
