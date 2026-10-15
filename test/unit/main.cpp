// pjfetch 单元测试入口（CppUTest）；运行：pjfetch_tests [-v] [-g<group>] [-n<name>] [-ojunit]
#include "CppUTest/CommandLineTestRunner.h"

int main(int ac, char** av) {
    return CommandLineTestRunner::RunAllTests(ac, av);
}
