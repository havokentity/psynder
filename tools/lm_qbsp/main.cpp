// SPDX-License-Identifier: MIT
// lm_qbsp — CLI driver. Implementation in Qbsp.cpp.

#include "Qbsp.h"

int main(int argc, char** argv) {
    return psynder::tools::qbsp::cli_main(argc, argv);
}
