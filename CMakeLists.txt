add_definitions(
  -D_FILE_OFFSET_BITS=64
  )


include (FindPkgConfig)

pkg_check_modules (DRM libdrm)
pkg_check_modules (GBM gbm)
pkg_check_modules (EGL egl)
pkg_check_modules (GLESV2 glesv2)

set(LIBS ${LIBS} ${DRM_LIBRARIES} ${GBM_LIBRARIES} ${EGL_LIBRARIES} ${GLESV2_LIBRARIES})

add_executable(test_driver main.cc)
include_directories(/usr/include/libdrm)
link_directories(/usr/lib/i386-linux-gnu/mesa-egl)
target_link_libraries(test_driver ${LIBS})
set_property(TARGET test_driver PROPERTY CXX_STANDARD 14)
