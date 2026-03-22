# Add C++ JWT library (jwt-cpp) installation script
sudo apt-get update && sudo apt-get install -y nlohmann-json3-dev
# jwt-cpp is header-only, so we fetch it
if [ ! -d "jwt-cpp" ]; then
  git clone --depth 1 https://github.com/Thalhammer/jwt-cpp.git
fi
