from conan import ConanFile
from conan.tools.cmake import CMakeToolchain, CMake, cmake_layout, CMakeDeps
from os import environ

class PayPerCallConan (ConanFile):
    name = "pay-per-call"
    version = "v0.0.1"
    license = "Open BSV"
    author = "Daniel Krawisz"
    url = "https://github.com/DanielKrawisz/pay-per-call"
    description = "call whatsonchain without a subscription and pay to call"
    topics = ("Bitcoin", "BSV", "API")
    settings = "os", "compiler", "build_type", "arch"
    options = {"shared": [True, False], "fPIC": [True, False]}
    default_options = {"shared": False, "fPIC": True}
    exports_sources = "CMakeLists.txt", "Cosmos.cpp", "include/*", "source/*", "test/*"
    requires = [
        "boost/1.80.0",
        "openssl/1.1.1t",
        "cryptopp/8.5.0",
        "nlohmann_json/3.11.2",
        "gmp/6.2.1",
        "argh/1.3.2",
        "secp256k1/0.3@proofofwork/stable",
        "data/v0.0.27@proofofwork/stable",
        "gigamonkey/v0.0.15@proofofwork/stable",
        "cosmos_lib/v0.0.1@proofofwork/stable"
    ]

    def set_version (self):
        if "CIRCLE_TAG" in environ:
            self.version = environ.get ("CIRCLE_TAG")[1:]
        if "CURRENT_VERSION" in environ:
            self.version = environ['CURRENT_VERSION']
        else:
            self.version = "v0.0.1"

    def config_options (self):
        if self.settings.os == "Windows":
            del self.options.fPIC

    def configure_cmake (self):
        cmake = CMake (self)
        cmake.configure (variables={"PACKAGE_TESTS":"Off"})
        return cmake

    def layout (self):
        cmake_layout(self)

    def generate (self):
        deps = CMakeDeps (self)
        deps.generate ()
        tc = CMakeToolchain (self)
        tc.generate ()

    def build (self):
        cmake = self.configure_cmake ()
        cmake.build ()

    def package (self):
        cmake = CMake (self)
        cmake.install ()

    def package_info (self):
#        self.cpp_info.libdirs = ["lib"]  # Default value is 'lib'
        self.cpp_info.libs = ["pay-per-call"]


