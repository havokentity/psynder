// SPDX-License-Identifier: MIT
// lm_pak — CLI driver. Real work lives in Lmpak.cpp; this file is just a
// thin shell around lmpak::cli_main so the same code runs from tests too.

#include "Lmpak.h"

int main(int argc, char** argv) {
    return psynder::tools::lmpak::cli_main(argc, argv);
}
