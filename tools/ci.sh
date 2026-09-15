#!/bin/bash

if which nproc > /dev/null; then
    MAKEOPTS="-j$(nproc)"
else
    MAKEOPTS="-j$(sysctl -n hw.ncpu)"
fi
########################################################################################
# code formatting

function ci_code_formatting_setup {
    sudo apt-get install uncrustify
    pip3 install black
    uncrustify --version
    black --version
}

function ci_code_formatting_run {
    tools/codeformat.py -v
}

########################################################################################
# code spelling

function ci_code_spell_setup {
    pip3 install codespell
}

function ci_code_spell_run {
    # src/ and tests/ arrive with the driver; spell-check whatever is present.
    codespell README.md $(test -d src && echo src) $(test -d tests && echo tests)
}

########################################################################################
# host tests

function ci_tests_setup {
    sudo apt-get update
    sudo apt-get install gcc-multilib
}

function ci_tests_run {
    make $MAKEOPTS -C tests/host
    make $MAKEOPTS -C tests/host asan
}

########################################################################################
# host tests with clang

function ci_tests_clang_setup {
    sudo apt-get update
    sudo apt-get install clang gcc-multilib
}

function ci_tests_clang_run {
    make $MAKEOPTS -C tests/host CC=clang
}

########################################################################################
# qemu tests

# The Cortex-M55 target needs GCC >= 14; the runner's default arm-none-eabi-gcc
# is older, so fetch a current Arm GNU toolchain.
CI_GCC_ARM_VER=14.2.rel1
CI_GCC_ARM_DIR=$HOME/gcc-arm
CI_GCC_ARM=$CI_GCC_ARM_DIR/bin/arm-none-eabi-gcc

function ci_gcc_arm_setup {
    curl -sL -o gcc-arm.tar.xz \
        "https://developer.arm.com/-/media/Files/downloads/gnu/${CI_GCC_ARM_VER}/binrel/arm-gnu-toolchain-${CI_GCC_ARM_VER}-x86_64-arm-none-eabi.tar.xz"
    mkdir -p "$CI_GCC_ARM_DIR"
    tar -xf gcc-arm.tar.xz -C "$CI_GCC_ARM_DIR" --strip-components=1
}

function ci_tests_qemu_setup {
    sudo apt-get update
    sudo apt-get install qemu-system-arm
    ci_gcc_arm_setup
}

function ci_tests_qemu_run {
    make $MAKEOPTS -C tests/qemu CC="$CI_GCC_ARM"
}

# clang cross-compiles the sources (using the GCC toolchain's sysroot), then
# links with GCC; this puts clang's eyes on mm_halow_sched.c, which the host
# tests do not build.
function ci_tests_qemu_clang_setup {
    sudo apt-get update
    sudo apt-get install clang qemu-system-arm
    ci_gcc_arm_setup
}

function ci_tests_qemu_clang_run {
    make $MAKEOPTS -C tests/qemu CC=clang GCC="$CI_GCC_ARM"
}
