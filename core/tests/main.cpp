#include <chrono>
#include <cstring>

#include "test.hpp"

int main(int argc, char** argv) {
    const char* filter = argc > 1 ? argv[1] : nullptr;
    int run = 0;
    for (auto& c : t::registry()) {
        if (filter && !std::strstr(c.name, filter)) continue;
        const int before = t::failures();
        const auto start = std::chrono::steady_clock::now();
        c.fn();
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now() - start)
                            .count();
        std::printf("%s %s (%lld ms)\n", t::failures() == before ? "ok  " : "FAIL", c.name,
                    static_cast<long long>(ms));
        ++run;
    }
    std::printf("\n%d tests, %d failed checks\n", run, t::failures());
    return t::failures() == 0 ? 0 : 1;
}
