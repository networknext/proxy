sudo apt update -y
sudo apt upgrade -y
sudo apt install golang build-essential -y
sudo apt autoremove -y
wget https://download.libsodium.org/libsodium/releases/libsodium-1.0.18-stable.tar.gz
tar -zxf libsodium-1.0.18-stable.tar.gz
cd libsodium-stable
./configure
make -j32
sudo make install
sudo ldconfig
cd ..
wget https://github.com/premake/premake-core/releases/download/v5.0.0-beta1/premake-5.0.0-beta1-linux.tar.gz
tar -zxf premake-5.0.0-beta1-linux.tar.gz
sudo mv premake5 /usr/local/bin
git clone https://github.com/networknext/proxy.git
cd proxy
premake5 gmake
make -j32
./bin/proxy test