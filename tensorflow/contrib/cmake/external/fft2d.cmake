include (ExternalProject)

set(fft2d_URL http://www.kurims.kyoto-u.ac.jp/~ooura/fft.tgz)
set(fft2d_HASH SHA256=52bb637c70b971958ec79c9c8752b1df5ff0218a4db4510e60826e0cb79b5296)
set(fft2d_BUILD ${CMAKE_CURRENT_BINARY_DIR}/fft2d/)
set(fft2d_INSTALL ${CMAKE_CURRENT_BINARY_DIR}/fft2d/src)

if(WIN32)
  set(fft2d_STATIC_LIBRARIES ${fft2d_BUILD}/src/lib/fft2d.lib)

  ExternalProject_Add(fft2d
      PREFIX fft2d
      URL ${fft2d_URL}
      URL_HASH ${fft2d_HASH}
      DOWNLOAD_DIR "${DOWNLOAD_LOCATION}"
      BUILD_IN_SOURCE 1
      PATCH_COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_CURRENT_SOURCE_DIR}/patches/fft2d/CMakeLists.txt ${fft2d_BUILD}/src/fft2d/CMakeLists.txt
      INSTALL_DIR ${fft2d_INSTALL}
      CMAKE_CACHE_ARGS
          -DCMAKE_BUILD_TYPE:STRING=Release
          -DCMAKE_VERBOSE_MAKEFILE:BOOL=OFF
          -DCMAKE_INSTALL_PREFIX:STRING=${fft2d_INSTALL})
else()
  set(fft2d_STATIC_LIBRARIES ${fft2d_BUILD}/src/fft2d/libfft2d.a)

  ExternalProject_Add(fft2d
      PREFIX fft2d
      URL ${fft2d_URL}
      URL_HASH ${fft2d_HASH}
      DOWNLOAD_DIR "${DOWNLOAD_LOCATION}"
      BUILD_IN_SOURCE 1
      PATCH_COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_CURRENT_SOURCE_DIR}/patches/fft2d/CMakeLists.txt ${fft2d_BUILD}/src/fft2d/CMakeLists.txt
      INSTALL_DIR $(fft2d_INSTALL)
      INSTALL_COMMAND echo
      BUILD_COMMAND $(MAKE))
    
endif()
