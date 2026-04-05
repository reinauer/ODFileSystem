/*
 * fuzz_hfsplus.c — forced HFS+ parser smoke target
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "fuzz_common.h"

int main(int argc, char **argv)
{
    return fuzz_main_paths(argc, argv, ODFS_BACKEND_HFSPLUS, "fuzz_hfsplus");
}
