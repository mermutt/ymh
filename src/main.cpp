#include <exception>
#include <iostream>

#include "ymh/cli/cli.hpp"
#include "ymh/config/config.hpp"

int main(int argc, char** argv) {
    try {
        return ymh::run_cli(argc, argv);
    } catch (const ymh::ConfigError& error) {
        std::cerr << "ymh: " << error.what() << '\n';
        return 2;
    }
}
