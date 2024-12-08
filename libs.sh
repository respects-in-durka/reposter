sudo pacman -S asio

mkdir libs
#crow
git clone https://github.com/CrowCpp/Crow.git --depth=1
cd Crow
mkdir build && cd build
cmake -DCROW_BUILD_EXAMPLES=OFF -DCROW_BUILD_TESTS=OFF -DCMAKE_INSTALL_PREFIX=../../libs/crow ../
make install

# rabbit
git clone https://github.com/CopernicaMarketingSoftware/AMQP-CPP.git --depth=1
cd AMQP-CPP
mkdir build && cd build
cmake -DAMQP-CPP_BUILD_SHARED=ON -DAMQP-CPP_LINUX_TCP=ON -DCMAKE_INSTALL_PREFIX=../../libs/amqp ../
cmake --build . --target install

# boost
wget https://archives.boost.io/release/1.82.0/source/boost_1_82_0.tar.gz
tar xf boost_1_82_0.tar.gz
cd boost_1_82_0
./bootstrap.sh --prefix=../libs/boost