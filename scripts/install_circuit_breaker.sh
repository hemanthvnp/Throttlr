# Circuit breaker C++ library (header-only) installation script
if [ ! -d "cpp-circuit-breaker" ]; then
  git clone --depth 1 https://github.com/Netflix/Hystrix.git cpp-circuit-breaker
fi
