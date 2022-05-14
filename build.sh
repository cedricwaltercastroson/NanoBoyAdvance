rm -rf build
cmake \
            -B build \
            -G "Unix Makefiles" \
            -DCMAKE_BUILD_TYPE=Release \
            -DCMAKE_CXX_FLAGS="-s" \
            -DPLATFORM_QT_STATIC=ON \
            -DSDL2_STATIC=ON \
            -DGLEW_USE_STATIC_LIBS=ON \
            -DQT5_STATIC_DIR="C:\msys64\mingw64\qt5-static"
          cd build
          make -j$NUMBER_OF_PROCESSORS