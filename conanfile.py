from conan import ConanFile
from conan.tools.cmake import CMake, CMakeToolchain, CMakeDeps, cmake_layout
from conan.tools.build import check_min_cppstd


class OSGatewayConan(ConanFile):
    name = "os-gateway"
    version = "2.0.0"
    license = "MIT"
    author = "OS Gateway Team"
    url = "https://github.com/os-gateway/os-gateway"
    description = "Enterprise-grade C++ API Gateway"
    topics = ("api-gateway", "reverse-proxy", "http", "http2", "load-balancer")

    settings = "os", "compiler", "build_type", "arch"
    options = {
        "shared": [True, False],
        "fPIC": [True, False],
        "with_grpc": [True, False],
        "with_opentelemetry": [True, False],
        "with_prometheus": [True, False],
    }
    default_options = {
        "shared": False,
        "fPIC": True,
        "with_grpc": True,
        "with_opentelemetry": True,
        "with_prometheus": True,
    }

    exports_sources = "CMakeLists.txt", "src/*", "include/*", "config/*"

    def validate(self):
        check_min_cppstd(self, "20")

    def config_options(self):
        if self.settings.os == "Windows":
            del self.options.fPIC

    def configure(self):
        if self.options.shared:
            self.options.rm_safe("fPIC")

    def requirements(self):
        # Core dependencies
        self.requires("openssl/3.2.0")
        self.requires("hiredis/1.2.0")
        self.requires("libnghttp2/1.58.0")
        self.requires("nlohmann_json/3.11.3")
        self.requires("spdlog/1.12.0")
        self.requires("jwt-cpp/0.7.0")
        self.requires("zlib/1.3")
        self.requires("brotli/1.1.0")

        # Optional dependencies
        if self.options.with_grpc:
            self.requires("grpc/1.54.3")

        if self.options.with_opentelemetry:
            self.requires("opentelemetry-cpp/1.12.0")

        if self.options.with_prometheus:
            self.requires("prometheus-cpp/1.1.0")

    def build_requirements(self):
        self.test_requires("gtest/1.14.0")
        self.test_requires("benchmark/1.8.3")

    def layout(self):
        cmake_layout(self)

    def generate(self):
        deps = CMakeDeps(self)
        deps.generate()

        tc = CMakeToolchain(self)
        tc.variables["GATEWAY_BUILD_TESTS"] = True
        tc.variables["GATEWAY_BUILD_BENCHMARKS"] = True
        tc.generate()

    def build(self):
        cmake = CMake(self)
        cmake.configure()
        cmake.build()

    def package(self):
        cmake = CMake(self)
        cmake.install()

    def package_info(self):
        self.cpp_info.libs = ["gateway_core"]
        self.cpp_info.set_property("cmake_file_name", "os-gateway")
        self.cpp_info.set_property("cmake_target_name", "os-gateway::gateway_core")
