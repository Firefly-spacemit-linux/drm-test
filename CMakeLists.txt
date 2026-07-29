# Copyright (C) 2023 Spacemit Co., Ltd.

cmake_minimum_required (VERSION 3.0)
project (k1x-drm)

find_package(PkgConfig REQUIRED)
pkg_check_modules(DRM REQUIRED libdrm)

find_path(LINUX_HEADERS_DIR linux/dma-heap.h
    PATHS /usr/include
    REQUIRED
)

include_directories(${DRM_INCLUDE_DIRS})

add_executable(modeset-single-buffer modeset-single-buffer.c)
target_link_libraries(modeset-single-buffer ${DRM_LIBRARIES})

add_executable(modeset-double-buffer modeset-double-buffer.c)
target_link_libraries(modeset-double-buffer ${DRM_LIBRARIES})

add_executable(modeset-atomic modeset-atomic.c)
target_link_libraries(modeset-atomic ${DRM_LIBRARIES})

add_executable(modeset-vsync modeset-vsync.c)
target_link_libraries(modeset-vsync ${DRM_LIBRARIES})

add_executable(modeset-rdma modeset-rdma.c)
target_link_libraries(modeset-rdma ${DRM_LIBRARIES})

add_executable(modeset-test modeset-test.c)
target_link_libraries(modeset-test ${DRM_LIBRARIES})

add_executable(modeset-wb modeset-wb.c)
target_link_libraries(modeset-wb ${DRM_LIBRARIES})

add_executable(modeset-dmabuf modeset-dmabuf.c)
target_include_directories(modeset-dmabuf PRIVATE 
    ${DRM_INCLUDE_DIRS}
    ${LINUX_HEADERS_DIR}
)
target_link_libraries(modeset-dmabuf ${DRM_LIBRARIES})

install(TARGETS modeset-single-buffer modeset-double-buffer RUNTIME DESTINATION "${CMAKE_INSTALL_PREFIX}/bin")
install(TARGETS modeset-atomic modeset-vsync modeset-rdma modeset-test modeset-wb RUNTIME DESTINATION "${CMAKE_INSTALL_PREFIX}/bin")
install(TARGETS modeset-dmabuf RUNTIME DESTINATION "${CMAKE_INSTALL_PREFIX}/bin")
install(DIRECTORY res/
        DESTINATION share/drm
        FILES_MATCHING
        PATTERN "*.yuv"
        PATTERN "*.rgb")